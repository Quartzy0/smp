//
// Created by quartzy on 7/22/23.
//

#include <stdlib.h>
#include <event2/event.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include "ctrl.h"
#include "audio.h"
#include "dbus.h"
#include "spotify.h"

struct smp_context {
    struct event_base *base;
    struct event *audio_next_event;
    int audio_next_fd[2];
    struct buffer audio_buf;
    struct spotify_state *spotify;
    struct audio_context *audio_ctx;

    dbus_bus *bus;
    dbus_interface *player_iface;
    dbus_interface *playlist_iface;
    dbus_interface *tracks_iface;
    dbus_interface *smp_iface;

    bool shuffle;

    int64_t track_index;
    int64_t *shuffle_table;
    int64_t shuffle_table_size;
    int64_t shuffle_index;
};

bool recommendations_loading = false;
struct connection *currently_streaming;
PlaylistInfo *previous_playlist = NULL;

static int64_t
random_int(int64_t max){
    return (int64_t) round(((double) (rand())/ (double) (RAND_MAX)) * (double) max);
}

static void
update_shuffle_table(struct smp_context *ctx, int64_t from, int64_t size) {
    if (!size || (size == ctx->shuffle_table_size && ctx->shuffle_table) || from >= size) return;

    if (ctx->shuffle_table_size != size){
        int64_t *tmp = realloc(ctx->shuffle_table, size * sizeof(*ctx->shuffle_table));
        if (!tmp){
            perror("Error when calling realloc");
            exit(EXIT_FAILURE);
        }
        ctx->shuffle_table = tmp;
        ctx->shuffle_table_size = size;
    }

    for (int64_t i = from; i < size; ++i) {
        ctx->shuffle_table[i] = i;
    }

    int64_t i1, i2, tmp;
    for (int64_t i = from; i < size; ++i) {
        i1 = from + random_int(size-from-1);
        i2 = from + random_int(size-from-1);

        tmp = ctx->shuffle_table[i1];
        ctx->shuffle_table[i1] = ctx->shuffle_table[i2];
        ctx->shuffle_table[i2] = tmp;
    }
}

static void
wrapped_play_track(struct smp_context *ctx) {
    cancel_track_transfer(currently_streaming);
    if (ctx->shuffle) ctx->track_index = ctx->shuffle_table[ctx->shuffle_index];
    Track *track = &ctx->spotify->tracks[ctx->track_index];
    if(play_track(ctx->spotify, track, &ctx->audio_buf, &currently_streaming)){
        fprintf(stderr, "[ctrl] Error occurred while trying to play track, playing next one.\n");
        static const enum AudioThreadSignal NEXT_SIG = AUDIO_THREAD_SIGNAL_TRACK_OVER;
        write(ctx->audio_next_fd[1], &NEXT_SIG, sizeof(NEXT_SIG));
        return;
    }
    if (track->playlist != previous_playlist){
        deref_playlist(previous_playlist);
        previous_playlist = track->playlist;
        if (previous_playlist) previous_playlist->reference_count++;
        dbus_util_invalidate_property(ctx->playlist_iface, "ActivePlaylist");
    }
    dbus_util_invalidate_property(ctx->player_iface, "Metadata");
}

static void
wrapped_update_shuffle_table(struct spotify_state *spotify, void *userp){
    struct smp_context *ctx = (struct smp_context*) userp;
    bool should_add = audio_started(ctx->audio_ctx) && ctx->shuffle_table_size > ctx->shuffle_index;
    update_shuffle_table(ctx, ctx->shuffle_index + ((int64_t) should_add), (int64_t) spotify->track_count);
}

static void
tracks_loaded_cb(struct spotify_state *spotify, void *userp) {
    struct smp_context *ctx = (struct smp_context*) userp;
    recommendations_loading = false;
    printf("[ctrl] Track list loaded\n");
    bool should_add = audio_started(ctx->audio_ctx) && ctx->shuffle_table_size > ctx->shuffle_index;
    update_shuffle_table(ctx, ctx->shuffle_index + ((int64_t) should_add), (int64_t) ctx->spotify->track_count);
    wrapped_play_track(ctx);
    dbus_util_invalidate_property(ctx->tracks_iface, "Tracks");
}

static void
playlist_loaded_cb(struct spotify_state *spotify, void *userp){
    struct smp_context *ctx = (struct smp_context*) userp;
    dbus_util_invalidate_property(ctx->tracks_iface, "PlaylistCount");
    tracks_loaded_cb(spotify, userp);
}

static void
spotify_conn_err(struct connection *conn, void *userp) {
    if (conn->payload) { // Remove possible left over files
        if (conn->payload[0] == MUSIC_DATA || conn->payload[0] == MUSIC_INFO) {
            char *music_info_path, *music_data_path;
            track_info_filepath_id(&conn->payload[1], &music_info_path);
            track_filepath_id(&conn->payload[1], &music_data_path);
            remove(music_info_path);
            remove(music_data_path);
        }
    }

    printf("[ctrl] Error occurred on connection, trying to play next track\n");
}

static void
next_track_cb(int fd, short t, void *param){
    struct smp_context *smp_ctx = (struct smp_context*) param;
    enum AudioThreadSignal r;
    size_t read_bytes = read(fd, &r, sizeof(r));
    if (read_bytes != sizeof(r)) return;
    switch (r) {
        case AUDIO_THREAD_SIGNAL_TRACK_OVER:
            ctrl_change_track_index(param, 1);
            break;
        case AUDIO_THREAD_SIGNAL_SEEKED: {
            dbus_method_call *call = dbus_util_new_signal("/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
                                                          "Seeked");
            dbus_message_context *ctx = dbus_util_make_write_context(call);
            dbus_util_message_context_add_int64(ctx, audio_get_position(smp_ctx->audio_ctx));
            dbus_util_message_context_free(ctx);

            dbus_util_send_method(smp_ctx->bus, call, NULL, NULL);
            break;
        }
        default:
            break;
    }
}

struct smp_context *ctrl_create_context(struct event_base *base) {
    struct smp_context *ctx = calloc(1, sizeof(*ctx));
    ctx->base = base;

    struct spotify_state *spotify_state = calloc(1, sizeof(*spotify_state));
    ctx->spotify = spotify_state;
    spotify_state->base = base;
    spotify_state->smp_ctx = ctx;
    spotify_state->err_cb = spotify_conn_err;

    pipe(ctx->audio_next_fd);

    ctx->audio_next_event = event_new(base, ctx->audio_next_fd[0], EV_READ | EV_PERSIST, next_track_cb, ctx);
    event_add(ctx->audio_next_event, NULL);

    return ctx;
}

void
ctrl_set_dbus_ifaces(struct smp_context *ctx, dbus_interface *player, dbus_interface *playlists, dbus_interface *tracks,
                     dbus_interface *smp, dbus_bus *bus) {
    ctx->player_iface = player;
    ctx->playlist_iface = playlists;
    ctx->tracks_iface = tracks;
    ctx->smp_iface = smp;
    ctx->bus = bus;
}

void ctrl_init_audio(struct smp_context *ctx, double initial_volume) {
    ctx->audio_buf.buf = calloc(12000000, sizeof(*ctx->audio_buf.buf));
    ctx->audio_buf.size = 12000000;
    ctx->audio_ctx = audio_init(&ctx->audio_buf, ctx->audio_next_fd[1]);
    audio_set_volume(ctx->audio_ctx, initial_volume);
}

void
ctrl_quit(struct smp_context *ctx) {
    audio_stop(ctx->audio_ctx);
    for (int i = 0; i < ctx->spotify->connections_len; ++i) {
        struct connection *conn = &ctx->spotify->connections[i];
        if (conn->bev) bufferevent_free(conn->bev);
        if (conn->cache_fp) fclose(conn->cache_fp);
        if (conn->cache_path) remove(conn->cache_path); // Remove unfinished file
        if (conn->params.path) free(conn->params.path);
    }
    event_base_loopbreak(ctx->base);
}

void
ctrl_free(struct smp_context *ctx){
    audio_clean(ctx->audio_ctx);
    if (ctx->audio_next_event) event_free(ctx->audio_next_event);
    clean_vorbis_decode(&ctx->spotify->decode_ctx);
    for (int i = 0; i < sizeof(ctx->spotify->connections) / sizeof(*ctx->spotify->connections); ++i) {
        free(ctx->spotify->connections[i].cache_path);
    }
    clear_tracks(ctx->spotify->tracks, &ctx->spotify->track_count, &ctx->spotify->track_size);
    free(ctx->spotify->tracks);
    close(ctx->audio_next_fd[0]);
    close(ctx->audio_next_fd[1]);
    memset(ctx->spotify, 0, sizeof(*ctx->spotify));
    free(ctx->spotify);
    free(ctx->shuffle_table);
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}

void
ctrl_play_album(struct smp_context *ctx, const char *id) {
    printf("[ctrl] Loading album with id %s\n", id);
    if (dbus_util_get_property_bool(ctx->smp_iface, "ReplaceOld")) {
        clear_tracks(ctx->spotify->tracks, &ctx->spotify->track_count, &ctx->spotify->track_size);
        ctx->track_index = 0;
        ctx->shuffle_index = 0;
    }

    if (ctx->spotify->track_count == 0)
        add_playlist(ctx->spotify, id, &ctx->spotify->tracks, &ctx->spotify->track_size, &ctx->spotify->track_count, true,
                     playlist_loaded_cb, ctx, tracks_loaded_cb);
    else
        add_playlist(ctx->spotify, id, &ctx->spotify->tracks, &ctx->spotify->track_size, &ctx->spotify->track_count,
                     true,
                     wrapped_update_shuffle_table, ctx, NULL);
}

void
ctrl_play_playlist(struct smp_context *ctx, const char *id){
    printf("[ctrl] Loading playlist with id %s\n", id);
    if (dbus_util_get_property_bool(ctx->smp_iface, "ReplaceOld")) {
        clear_tracks(ctx->spotify->tracks, &ctx->spotify->track_count, &ctx->spotify->track_size);
        ctx->track_index = 0;
        ctx->shuffle_index = 0;
    }

    if (ctx->spotify->track_count == 0)
        add_playlist(ctx->spotify, id, &ctx->spotify->tracks, &ctx->spotify->track_size, &ctx->spotify->track_count, false,
                     playlist_loaded_cb, ctx, tracks_loaded_cb);
    else
        add_playlist(ctx->spotify, id, &ctx->spotify->tracks, &ctx->spotify->track_size, &ctx->spotify->track_count,
                     false,
                     wrapped_update_shuffle_table, ctx, NULL);
}

void
ctrl_play_track(struct smp_context *ctx, const char *id){
    printf("[ctrl] Loading track with id %s\n", id);

    if (dbus_util_get_property_bool(ctx->smp_iface, "ReplaceOld")) {
        clear_tracks(ctx->spotify->tracks, &ctx->spotify->track_count, &ctx->spotify->track_size);
        ctx->track_index = 0;
        ctx->shuffle_index = 0;
    }

    info_received_cb cb = wrapped_update_shuffle_table;
    if (ctx->spotify->track_count == 0) cb = tracks_loaded_cb;
    add_track_info(ctx->spotify, id, &ctx->spotify->tracks, &ctx->spotify->track_size, &ctx->spotify->track_count, cb, ctx);
}

void ctrl_play_uri(struct smp_context *ctx, const char *id){
    char out[23];
    switch (id_from_url(id, out)) {
        case URI_INVALID:
            return;
        case URI_TRACK:
            ctrl_play_track(ctx, out);
            break;
        case URI_ALBUM:
            ctrl_play_album(ctx, out);
            break;
        case URI_PLAYLIST:
            ctrl_play_playlist(ctx, out);
            break;
    }
}

void
ctrl_pause(struct smp_context *ctx){
    audio_pause(ctx->audio_ctx);
    dbus_util_invalidate_property(ctx->player_iface, "PlaybackStatus");
}

void
ctrl_play(struct smp_context *ctx){
    audio_play(ctx->audio_ctx);
    dbus_util_invalidate_property(ctx->player_iface, "PlaybackStatus");
}

void
ctrl_playpause(struct smp_context *ctx){
    if (audio_playing(ctx->audio_ctx)) ctrl_pause(ctx);
    else ctrl_play(ctx);
}

void
ctrl_stop(struct smp_context *ctx){
    audio_stop(ctx->audio_ctx);
    clear_tracks(ctx->spotify->tracks, &ctx->spotify->track_count, &ctx->spotify->track_size);
    previous_playlist = NULL;
    cancel_track_transfer(currently_streaming);
    currently_streaming = NULL;
    recommendations_loading = false;
    ctx->track_index = 0;
    ctx->shuffle_index = 0;
    free(ctx->shuffle_table);
    ctx->shuffle_table = NULL;
    ctx->shuffle_table_size = 0;
    ctx->shuffle = false;
    dbus_util_invalidate_property(ctx->player_iface, "PlaybackStatus");
    dbus_util_invalidate_property(ctx->player_iface, "Shuffle");
    dbus_util_invalidate_property(ctx->tracks_iface, "Tracks");
}

void
ctrl_change_track_index(struct smp_context *ctx, int32_t i){
    if (ctx->spotify->track_count == 0 || !audio_started(ctx->audio_ctx)) return;
    if (loop_mode != LOOP_MODE_TRACK)  {
        if (ctx->shuffle){
            if (ctx->shuffle_index >= ctx->spotify->track_count) return;
            ctx->shuffle_index += i;
        } else {
            if (ctx->track_index >= ctx->spotify->track_count) return;
            ctx->track_index += i;
        }
        if ((!ctx->shuffle && (ctx->track_index >= ctx->spotify->track_count || ctx->track_index < 0)) ||
              ctx->shuffle && (ctx->shuffle_index >= ctx->spotify->track_count || ctx->shuffle_index < 0)) {
            if (loop_mode == LOOP_MODE_PLAYLIST) {
                ctx->track_index = 0;
                ctx->shuffle_index = 0;
                wrapped_play_track(ctx);
                return;
            }
            //Continue playing recommendations
            if(!recommendations_loading) {
                recommendations_loading = true;
                add_recommendations_from_tracks(ctx->spotify, &ctx->spotify->tracks, &ctx->spotify->track_size, &ctx->spotify->track_count,
                                                tracks_loaded_cb, ctx);
            }
            return;
        }
    }
    wrapped_play_track(ctx);
}

void ctrl_set_track_index(struct smp_context *ctx, int32_t i){
    if (!audio_started(ctx->audio_ctx)) return;
    if (i >= ctx->spotify->track_count || i < 0) return;
    ctx->track_index = i;
}

void ctrl_seek(struct smp_context *ctx, int64_t position){
    audio_seek(ctx->audio_ctx, position);
}

void ctrl_seek_to(struct smp_context *ctx, int64_t position){
    audio_seek_to(ctx->audio_ctx, position);
}

static void
search_cb(struct spotify_state *state, void *userp){
    handle_search_response(userp);
}

void ctrl_search(struct smp_context *ctx, struct search_params *params){
    search(ctx->spotify, params->query, search_cb, params->tracks, params->artists, params->albums, params->playlists, params);
}

struct audio_context *ctrl_get_audio_context(struct smp_context *ctx) {
    return ctx->audio_ctx;
}

struct audio_info *ctrl_get_audio_info(struct smp_context *ctx){
    return audio_get_info(ctx->audio_ctx);
}

struct audio_info *ctrl_get_audio_info_prev(struct smp_context *ctx){
    return audio_get_info_prev(ctx->audio_ctx);
}

struct spotify_state *ctrl_get_spotify_state(struct smp_context *ctx){
    return ctx->spotify;
}

size_t ctrl_get_track_index(struct smp_context *ctx){
    return ctx->track_index;
}

void ctrl_set_shuffle(struct smp_context *ctx, bool shuffle){
    if (!ctx->shuffle && shuffle && audio_started(ctx->audio_ctx)){ // Make it so tracks which have already been played aren't played in the future
        update_shuffle_table(ctx, 0, ctx->track_index + 1);
        ctx->shuffle_index = ctx->track_index;
        update_shuffle_table(ctx, ctx->track_index + 1, (int64_t) ctx->spotify->track_count);
    }
    ctx->shuffle = shuffle;
}

bool ctrl_get_shuffle(struct smp_context *ctx){
    return ctx->shuffle;
}