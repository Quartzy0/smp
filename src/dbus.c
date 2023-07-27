//
// Created by quartzy on 5/1/22.
//

#include "dbus.h"
#include <dbus/dbus.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dbus-util.h>
#include "audio.h"
#include "util.h"
#include "spotify.h"
#include "introspection_xml.h"
#include "ctrl.h"

#define CHECKERR(x) do{int ret = (x);if(ret != 0){printf("Assert fail in %s:%d with %d\n", __FILE__, __LINE__, ret);dbus_util_free_bus(dbus_state->bus);exit(1);}}while(0)

static const char mpris_name[] = "org.mpris.MediaPlayer2.smp";

static int
compare_alphabetical(const void *a, const void *b) {
    return strcmp(((const PlaylistInfo *) a)->name, ((const PlaylistInfo *) b)->name);
}

static int
compare_alphabetical_reverse(const void *a, const void *b) {
    return -strcmp(((const PlaylistInfo *) a)->name, ((const PlaylistInfo *) b)->name);
}

static int
compare_last_played(const void *a, const void *b) {
    return (int) (((const PlaylistInfo *) b)->last_played - ((const PlaylistInfo *) a)->last_played);
}

static int
compare_last_played_reverse(const void *a, const void *b) {
    return (int) (((const PlaylistInfo *) a)->last_played - ((const PlaylistInfo *) b)->last_played);
}

static void add_track_metadata(struct audio_context *audio_ctx, dbus_message_context *ctx, Track *track) {
    if (!track || !audio_started(audio_ctx)){
        dbus_util_message_context_enter_array(&ctx, "{sv}");
        dbus_util_message_context_exit_array(&ctx);
        return;
    }

    char *obj_path = malloc((strlen(track->spotify_id) + 46 + 1) * sizeof(*obj_path));
    snprintf(obj_path, (strlen(track->spotify_id) + 46 + 1) * sizeof(*obj_path),
             "/org/mpris/MediaPlayer2/smp/track/%s", track->spotify_id);

    dbus_util_message_context_enter_array(&ctx, "{sv}");

    dbus_util_message_context_enter_dict_entry(&ctx);
    dbus_util_message_context_add_string(ctx, "mpris:trackid");
    dbus_util_message_context_add_object_path_variant(ctx, obj_path);
    dbus_util_message_context_exit_dict_entry(&ctx);

    dbus_util_message_context_enter_dict_entry(&ctx);
    dbus_util_message_context_add_string(ctx, "mpris:length");
    dbus_util_message_context_add_int64_variant(ctx, track->duration_ms * 1000);
    dbus_util_message_context_exit_dict_entry(&ctx);

    dbus_util_message_context_enter_dict_entry(&ctx);
    dbus_util_message_context_add_string(ctx, "mpris:artUrl");
    dbus_util_message_context_add_string_variant(ctx, track->spotify_album_art);
    dbus_util_message_context_exit_dict_entry(&ctx);

    dbus_util_message_context_enter_dict_entry(&ctx);
    dbus_util_message_context_add_string(ctx, "xesam:artist");
    dbus_util_message_context_enter_variant(&ctx, "as");
    dbus_util_message_context_enter_array(&ctx, "s");
    dbus_util_message_context_add_string(ctx, track->artist);
    dbus_util_message_context_exit_array(&ctx);
    dbus_util_message_context_exit_variant(&ctx);
    dbus_util_message_context_exit_dict_entry(&ctx);

    dbus_util_message_context_enter_dict_entry(&ctx);
    dbus_util_message_context_add_string(ctx, "xesam:url");
    dbus_util_message_context_add_string_variant(ctx, track->spotify_uri);
    dbus_util_message_context_exit_dict_entry(&ctx);

    dbus_util_message_context_enter_dict_entry(&ctx);
    dbus_util_message_context_add_string(ctx, "xesam:title");
    dbus_util_message_context_add_string_variant(ctx, track->spotify_name);
    dbus_util_message_context_exit_dict_entry(&ctx);

    dbus_util_message_context_exit_array(&ctx);

    free(obj_path);
}

static void add_playlist_dbus(dbus_message_context *ctx, PlaylistInfo *playlistInfo){
    dbus_util_message_context_enter_struct(&ctx);

    if (playlistInfo){
        char *obj_path = malloc((strlen(playlistInfo->spotify_id) + 50 + 1) * sizeof(*obj_path));
        snprintf(obj_path, (strlen(playlistInfo->spotify_id) + 50 + 1) * sizeof(*obj_path),
                 "/org/mpris/MediaPlayer2/smp/playlist/%s%s", playlistInfo->spotify_id, playlistInfo->album ? "a" : "p");

        dbus_util_message_context_add_object_path(ctx, obj_path);
        dbus_util_message_context_add_string(ctx, playlistInfo->name);
        dbus_util_message_context_add_string(ctx, playlistInfo->image_url);

        free(obj_path);
    } else {
        dbus_util_message_context_add_object_path(ctx, "/");
        dbus_util_message_context_add_string(ctx, "");
        dbus_util_message_context_add_string(ctx, "");
    }

    dbus_util_message_context_exit_struct(&ctx);
}

/*      Properties      */

static void SupportedUriSchemes_cb(dbus_bus *bus, dbus_message_context *ctx, void *param){
    dbus_util_message_context_enter_variant(&ctx, "as");
    dbus_util_message_context_enter_array(&ctx, "s");

    dbus_util_message_context_add_string(ctx, "spotify");

    dbus_util_message_context_exit_array(&ctx);
    dbus_util_message_context_exit_variant(&ctx);
}

static void Metadata_cb(dbus_bus *bus, dbus_message_context *ctx, void *param) {
    struct smp_context *smp_ctx = (struct smp_context*) param;
    struct spotify_state *spotify = ctrl_get_spotify_state(smp_ctx);
    struct audio_context *audio_ctx = ctrl_get_audio_context(smp_ctx);
    dbus_util_message_context_enter_variant(&ctx, "a{sv}");
    add_track_metadata(audio_ctx, ctx, &spotify->tracks[ctrl_get_track_index(smp_ctx)]);
    dbus_util_message_context_exit_variant(&ctx);
}

static void Volume_cb(dbus_bus *bus, dbus_message_context *ctx, void *param) {
    struct audio_context *audio_ctx = ctrl_get_audio_context(param);
    dbus_util_message_context_add_double_variant(ctx, audio_get_volume(audio_ctx));
}

static void Volume_set_cb(dbus_bus *bus, dbus_message_context *ctx, void *param) {
    struct audio_context *audio_ctx = ctrl_get_audio_context(param);
    double volume;
    dbus_util_message_context_enter_variant(&ctx, "d");
    dbus_util_message_context_get_double(ctx, &volume);
    dbus_util_message_context_exit_variant(&ctx);
    audio_set_volume(audio_ctx, volume);
}

static void PlaylistCount_cb(dbus_bus *bus, dbus_message_context *ctx, void *param) {
    dbus_util_message_context_add_uint32_variant(ctx, get_saved_playlist_count());
}

static void Orderings_cb(dbus_bus *bus, dbus_message_context *ctx, void *param) {
    dbus_util_message_context_enter_variant(&ctx, "as");
    dbus_util_message_context_enter_array(&ctx, "s");
    dbus_util_message_context_add_string(ctx, "Alphabetical");
    dbus_util_message_context_add_string(ctx, "Played");
    dbus_util_message_context_exit_array(&ctx);
    dbus_util_message_context_exit_variant(&ctx);
}

static void ActivePlaylist_cb(dbus_bus *bus, dbus_message_context *ctx, void *param) {
    struct smp_context *smp_ctx = (struct smp_context*) param;
    struct spotify_state *spotify = ctrl_get_spotify_state(smp_ctx);
    struct audio_context *audio_ctx = ctrl_get_audio_context(smp_ctx);
    dbus_util_message_context_enter_variant(&ctx, "(b(oss))");
    dbus_util_message_context_enter_struct(&ctx);

    size_t track_index = ctrl_get_track_index(smp_ctx);
    bool value = audio_started(audio_ctx) && spotify->tracks[track_index].playlist && spotify->tracks[track_index].playlist->not_empty;
    dbus_util_message_context_add_bool(ctx, value);

    add_playlist_dbus(ctx, value ? spotify->tracks[track_index].playlist : NULL);

    dbus_util_message_context_exit_struct(&ctx);
    dbus_util_message_context_exit_variant(&ctx);
}

static void Tracks_cb(dbus_bus *bus, dbus_message_context *ctx, void *param) {
    struct smp_context *smp_ctx = (struct smp_context*) param;
    struct spotify_state *spotify = ctrl_get_spotify_state(smp_ctx);

    dbus_util_message_context_enter_variant(&ctx, "ao");
    dbus_util_message_context_enter_array(&ctx, "o");

    if (spotify->track_count != -1) {
        static const char base_path[] = "/org/mpris/MediaPlayer2/smp/track/";
        char *object = malloc(sizeof(base_path) + 22 * sizeof(*object)); //Enough space for ids
        object[sizeof(base_path) + 22 - 1] = 0;
        memcpy(object, base_path, sizeof(base_path) - 1);
        for (int i = 0; i < spotify->track_count; ++i) {
            memcpy(object + sizeof(base_path) - 1, spotify->tracks[i].spotify_id, 22);
            dbus_util_message_context_add_object_path(ctx, object);
        }
        free(object);
    }

    dbus_util_message_context_exit_array(&ctx);
    dbus_util_message_context_exit_variant(&ctx);
}

static void PlaybackStatus_cb(dbus_bus *bus, dbus_message_context *ctx, void *param){
    struct audio_context *audio_ctx = ctrl_get_audio_context(param);
    dbus_util_message_context_add_string_variant(ctx, audio_started(audio_ctx) ? (audio_playing(audio_ctx) ? "Playing" : "Paused") : "Stopped");
}

static void LoopStatus_cb(dbus_bus *bus, dbus_message_context *ctx, void *param){
    switch (loop_mode) {
        case LOOP_MODE_NONE:
            dbus_util_message_context_add_string_variant(ctx, "None");
            break;
        case LOOP_MODE_TRACK:
            dbus_util_message_context_add_string_variant(ctx, "Track");
            break;
        case LOOP_MODE_PLAYLIST:
            dbus_util_message_context_add_string_variant(ctx, "Playlist");
            break;
        default:
            dbus_util_message_context_add_string_variant(ctx, "None");
            break;
    }
}

static void LoopStatus_set_cb(dbus_bus *bus, dbus_message_context *ctx, void *param){
    const char *str;
    dbus_util_message_context_enter_variant(&ctx, "s");
    dbus_util_message_context_get_string(ctx, &str);
    dbus_util_message_context_exit_variant(&ctx);

    if (!strcmp(str, "None")) {
        loop_mode = LOOP_MODE_NONE;
        printf("[spotify] Loop mode set to None\n");
    } else if (!strcmp(str, "Track")) {
        loop_mode = LOOP_MODE_TRACK;
        printf("[spotify] Loop mode set to Track\n");
    } else if (!strcmp(str, "Playlist")) {
        loop_mode = LOOP_MODE_PLAYLIST;
        printf("[spotify] Loop mode set to Playlist\n");
    } else {
        fprintf(stderr, "[spotify] Unrecognized loop mode: %s (supported: None, Track, Playlist)\n", str);
    }
}

static void Shuffle_cb(dbus_bus *bus, dbus_message_context *ctx, void *param){
    struct smp_context *smp_ctx = (struct smp_context*) param;
    dbus_util_message_context_add_bool_variant(ctx, ctrl_get_shuffle(smp_ctx));
}

static void Shuffle_set_cb(dbus_bus *bus, dbus_message_context *ctx, void *param){
    struct smp_context *smp_ctx = (struct smp_context*) param;
    bool shuffle;
    dbus_util_message_context_enter_variant(&ctx, "b");
    dbus_util_message_context_get_bool(ctx, &shuffle);
    dbus_util_message_context_exit_variant(&ctx);
    ctrl_set_shuffle(smp_ctx, shuffle);
}

static void Position_cb(dbus_bus *bus, dbus_message_context *ctx, void *param){
    struct smp_context *smp_ctx = (struct smp_context*) param;
    dbus_util_message_context_add_int64_variant(ctx, audio_get_position(ctrl_get_audio_context(smp_ctx)));
}

/*      Methods         */

static void Quit_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                    void *param){
    struct smp_context *ctx = (struct smp_context*) param;
    ctrl_quit(ctx);

    dbus_util_send_empty_reply(call);
}

static void Next_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                        void *param){
    struct smp_context *ctx = (struct smp_context*) param;
    ctrl_change_track_index(ctx, 1);

    dbus_util_send_empty_reply(call);
}

static void Previous_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                    void *param){
    struct smp_context *ctx = (struct smp_context*) param;
    ctrl_change_track_index(ctx, -1);

    dbus_util_send_empty_reply(call);
}

static void PlayPause_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                    void *param){
    struct smp_context *ctx = (struct smp_context*) param;
    ctrl_playpause(ctx);

    dbus_util_send_empty_reply(call);
}

static void Pause_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                    void *param){
    struct smp_context *ctx = (struct smp_context*) param;
    ctrl_pause(ctx);

    dbus_util_send_empty_reply(call);
}

static void Stop_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                    void *param){
    struct smp_context *ctx = (struct smp_context*) param;
    ctrl_stop(ctx);

    dbus_util_send_empty_reply(call);
}

static void Play_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                    void *param){
    struct smp_context *ctx = (struct smp_context*) param;
    ctrl_play(ctx);

    dbus_util_send_empty_reply(call);
}

static void Seek_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                    void *param){
    struct smp_context *ctx = (struct smp_context*) param;
    int64_t pos;
    dbus_message_context *rctx = dbus_util_make_read_context(call);
    dbus_util_message_context_get_int64(rctx, &pos);
    dbus_util_message_context_free(rctx);

    ctrl_seek(ctx, pos);

    dbus_util_send_empty_reply(call);
}

static void SetPosition_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                    void *param){
    struct smp_context *ctx = (struct smp_context*) param;
    int64_t pos;
    dbus_message_context *rctx = dbus_util_make_read_context(call);
    dbus_util_message_context_get_int64(rctx, &pos);
    dbus_util_message_context_free(rctx);

    ctrl_seek_to(ctx, pos);

    dbus_util_send_empty_reply(call);
}

static void OpenUri_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                           void *param){
    struct smp_context *ctx = (struct smp_context*) param;
    const char *uri = NULL;
    if (dbus_util_get_method_arguments(bus, call, DBUS_TYPE_STRING, &uri, DBUS_TYPE_INVALID)) return;

    ctrl_play_uri(ctx, uri);

    dbus_util_send_empty_reply(call);
}

static void GetPlaylists_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                    void *param){
    uint32_t index, max_count;
    char *order;
    dbus_bool_t reverse_order;
    if (dbus_util_get_method_arguments(bus, call, DBUS_TYPE_UINT32, &index, DBUS_TYPE_UINT32, &max_count, DBUS_TYPE_STRING,
                                       &order, DBUS_TYPE_BOOLEAN, &reverse_order, DBUS_TYPE_INVALID))
        return;

    PlaylistInfo *playlists;
    size_t count;
    int func_ret = get_all_playlist_info(&playlists, &count);

    int (*compar)(const void *, const void *) = compare_alphabetical;
    if (reverse_order) {
        if (order[0] == 'P' || order[0] == 'p') compar = compare_last_played_reverse;
        else compar = compare_alphabetical_reverse;
    } else {
        if (order[0] == 'P' || order[0] == 'p') compar = compare_last_played;
    }
    qsort(playlists, count, sizeof(*playlists), compar);

    dbus_message_context *ctx = dbus_util_make_reply_context(call);

    dbus_util_message_context_enter_array(&ctx, "(oss)");
    for (uint32_t i = index; i < count && i < index + max_count; ++i) {
        add_playlist_dbus(ctx, &playlists[i]);
    }
    dbus_util_message_context_exit_array(&ctx);

    dbus_util_message_context_free(ctx);
    for (int i = 0; i < count; ++i) {
        free_playlist(&playlists[i]);
    }
    free(playlists);
}

static void ActivatePlaylist_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                            void *param){
    struct smp_context *ctx = (struct smp_context*) param;

    char *obj;
    if (dbus_util_get_method_arguments(bus, call, DBUS_TYPE_OBJECT_PATH, &obj, DBUS_TYPE_INVALID))
        return;

    size_t objLen = strlen(obj);

    if (obj[objLen - 1] == 'a'){
        ctrl_play_album(ctx, &obj[objLen - 23]);
    } else{
        ctrl_play_playlist(ctx, &obj[objLen - 23]);
    }

    dbus_util_send_empty_reply(call);
}

static void Goto_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                                void *param){
    struct smp_context *ctx = (struct smp_context*) param;

    char *obj;
    if (dbus_util_get_method_arguments(bus, call, DBUS_TYPE_OBJECT_PATH, &obj, DBUS_TYPE_INVALID))
        return;

    struct spotify_state *spotify = ctrl_get_spotify_state(ctx);
    for (int32_t i = 0; i < spotify->track_count; ++i) {
        const char *id = strrchr(obj, '/');
        if (!id || strcmp(++id, spotify->tracks[i].spotify_id) != 0)continue;
        ctrl_set_track_index(ctx, i);
        break;
    }
}

static void GetTracksMetadata_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                    void *param){
    struct smp_context *smp_ctx = (struct smp_context*) param;
    struct audio_context *audio_ctx = ctrl_get_audio_context(param);
    size_t len = 20;
    char **objs = NULL;
    if (dbus_util_get_method_arguments(bus, call, DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &objs, &len, DBUS_TYPE_INVALID))
        return;
    struct spotify_state *spotify = ctrl_get_spotify_state(smp_ctx);

    dbus_message_context *ctx = dbus_util_make_reply_context(call);
    dbus_util_message_context_enter_array(&ctx, "a{sv}");
    const size_t id_offset = strrchr(objs[0], '/') - objs[0] + 1;
    for (size_t i = 0; i < spotify->track_count; ++i) {
        for (size_t j = 0; j < len; ++j) {
            if (strcmp(objs[j] + id_offset, spotify->tracks[i].spotify_id) != 0)continue;
            add_track_metadata(audio_ctx, ctx, &spotify->tracks[i]);
        }
    }
    dbus_util_message_context_exit_array(&ctx);

    dbus_util_message_context_free(ctx);
}

static void Search_cb(dbus_bus *bus, dbus_object *object, dbus_interface *interface, dbus_method_call *call,
                                 void *param){
    struct smp_context *ctx = (struct smp_context*) param;

    dbus_bool_t tracks, albums, artists, playlists;
    char *query;
    if (dbus_util_get_method_arguments(bus, call, DBUS_TYPE_BOOLEAN, &tracks, DBUS_TYPE_BOOLEAN, &albums, DBUS_TYPE_BOOLEAN,
                               &artists, DBUS_TYPE_BOOLEAN, &playlists, DBUS_TYPE_STRING, &query,
                               DBUS_TYPE_INVALID)) {
        return;
    }
    struct search_params *params = malloc(sizeof(*params));
    params->tracks = tracks;
    params->artists = artists;
    params->albums = albums;
    params->playlists = playlists;
    params->query = strdup(query);
    params->call = dbus_util_make_reply_call(call);
    params->bus = bus;
    ctrl_search(ctx, params);
}

struct dbus_state *
init_dbus(struct smp_context *ctx) {
    struct dbus_state *dbus_state = malloc(sizeof(*dbus_state));

    CHECKERR(dbus_util_create_bus_with_name(&dbus_state->bus, mpris_name));
    CHECKERR(dbus_util_set_introspectable_xml(dbus_state->bus, (const char *) introspection_xml_data));
    CHECKERR(dbus_util_parse_introspection(dbus_state->bus));

    dbus_state->mpris_obj = dbus_util_find_object(dbus_state->bus, "/org/mpris/MediaPlayer2");
    dbus_util_add_property_interface(dbus_state->mpris_obj);
    dbus_util_add_introspectable_interface(dbus_state->mpris_obj);

    dbus_object *base_obj = dbus_util_find_object(dbus_state->bus, "/");
    dbus_util_add_introspectable_interface(base_obj);

    dbus_state->mp_iface = dbus_util_find_interface(dbus_state->mpris_obj, "org.mpris.MediaPlayer2");
    dbus_util_set_property_bool(dbus_state->mp_iface, "CanQuit", true);
    dbus_util_set_property_bool(dbus_state->mp_iface, "Fullscreen", false);
    dbus_util_set_property_bool(dbus_state->mp_iface, "CanFullScreen", false);
    dbus_util_set_property_bool(dbus_state->mp_iface, "CanRaise", false);
    dbus_util_set_property_bool(dbus_state->mp_iface, "HasTrackList", true);
    dbus_util_set_property_string(dbus_state->mp_iface, "Identity", "smp");
    dbus_util_set_property_cb(dbus_state->mp_iface, "SupportedUriSchemes", SupportedUriSchemes_cb, NULL, ctx);
    dbus_util_set_method_cb(dbus_state->mp_iface, "Quit", Quit_cb, ctx);

    dbus_state->mplayer_iface = dbus_util_find_interface(dbus_state->mpris_obj, "org.mpris.MediaPlayer2.Player");
    dbus_util_set_property_cb(dbus_state->mplayer_iface, "PlaybackStatus", PlaybackStatus_cb, NULL, ctx);
    dbus_util_set_property_cb(dbus_state->mplayer_iface, "LoopStatus", LoopStatus_cb, LoopStatus_set_cb, ctx);
    dbus_util_set_property_double(dbus_state->mplayer_iface, "Rate", 1.0);
    dbus_util_set_property_cb(dbus_state->mplayer_iface, "Shuffle", Shuffle_cb, Shuffle_set_cb, ctx);
    dbus_util_set_property_cb(dbus_state->mplayer_iface, "Metadata", Metadata_cb, NULL, ctx);
    dbus_util_set_property_cb(dbus_state->mplayer_iface, "Volume", Volume_cb, Volume_set_cb, ctx);
    dbus_util_set_property_cb(dbus_state->mplayer_iface, "Position", Position_cb, NULL, ctx);
    dbus_util_set_property_double(dbus_state->mplayer_iface, "MinimumRate", 1.0);
    dbus_util_set_property_double(dbus_state->mplayer_iface, "MaximumRate", 1.0);
    dbus_util_set_property_bool(dbus_state->mplayer_iface, "CanGoNext", true);
    dbus_util_set_property_bool(dbus_state->mplayer_iface, "CanGoPrevious", true);
    dbus_util_set_property_bool(dbus_state->mplayer_iface, "CanPlay", true);
    dbus_util_set_property_bool(dbus_state->mplayer_iface, "CanPause", true);
    dbus_util_set_property_bool(dbus_state->mplayer_iface, "CanSeek", true);
    dbus_util_set_property_bool(dbus_state->mplayer_iface, "CanControl", true);
    dbus_util_set_method_cb(dbus_state->mplayer_iface, "Next", Next_cb, ctx);
    dbus_util_set_method_cb(dbus_state->mplayer_iface, "Previous", Previous_cb, ctx);
    dbus_util_set_method_cb(dbus_state->mplayer_iface, "PlayPause", PlayPause_cb, ctx);
    dbus_util_set_method_cb(dbus_state->mplayer_iface, "Pause", Pause_cb, ctx);
    dbus_util_set_method_cb(dbus_state->mplayer_iface, "Stop", Stop_cb, ctx);
    dbus_util_set_method_cb(dbus_state->mplayer_iface, "Play", Play_cb, ctx);
    dbus_util_set_method_cb(dbus_state->mplayer_iface, "Seek", Seek_cb, ctx);
    dbus_util_set_method_cb(dbus_state->mplayer_iface, "SetPosition", SetPosition_cb, ctx);
    dbus_util_set_method_cb(dbus_state->mplayer_iface, "OpenUri", OpenUri_cb, ctx);

    dbus_state->mplaylist_iface = dbus_util_find_interface(dbus_state->mpris_obj, "org.mpris.MediaPlayer2.Playlists");
    dbus_util_set_property_cb(dbus_state->mplaylist_iface, "PlaylistCount", PlaylistCount_cb, NULL, ctx);
    dbus_util_set_property_cb(dbus_state->mplaylist_iface, "Orderings", Orderings_cb, NULL, ctx);
    dbus_util_set_property_cb(dbus_state->mplaylist_iface, "ActivePlaylist", ActivePlaylist_cb, NULL, ctx);
    dbus_util_set_method_cb(dbus_state->mplaylist_iface, "GetPlaylists", GetPlaylists_cb, ctx);
    dbus_util_set_method_cb(dbus_state->mplaylist_iface, "ActivatePlaylist", ActivatePlaylist_cb, ctx);

    dbus_state->mtracks_iface = dbus_util_find_interface(dbus_state->mpris_obj, "org.mpris.MediaPlayer2.TrackList");
    dbus_util_set_property_cb(dbus_state->mtracks_iface, "Tracks", Tracks_cb, NULL, ctx);
    dbus_util_set_property_bool(dbus_state->mtracks_iface, "CanEditTracks", false);
    dbus_util_set_method_cb(dbus_state->mtracks_iface, "Goto", Goto_cb, ctx);
    dbus_util_set_method_cb(dbus_state->mtracks_iface, "GetTracksMetadata", GetTracksMetadata_cb, ctx);

    dbus_state->smp_iface = dbus_util_find_interface(dbus_state->mpris_obj, "me.quartzy.smp");
    dbus_util_set_method_cb(dbus_state->smp_iface, "Search", Search_cb, ctx);
    dbus_util_set_property_bool(dbus_state->smp_iface, "ReplaceOld", false);

    return dbus_state;
}

void
dbus_add_track(Track *track, dbus_message_context *ctx) {
    dbus_util_message_context_enter_struct(&ctx);
    char *id_alloced = malloc(sizeof(track->spotify_id));
    memcpy(id_alloced, track->spotify_id, sizeof(track->spotify_id));
    dbus_util_message_context_add_string(ctx, id_alloced);
    char *uri_alloced = malloc(sizeof(track->spotify_uri));
    memcpy(uri_alloced, track->spotify_uri, sizeof(track->spotify_uri));
    dbus_util_message_context_add_string(ctx, uri_alloced);
    dbus_util_message_context_add_string(ctx, track->spotify_name);
    dbus_util_message_context_add_string(ctx, track->spotify_album_art);
    dbus_util_message_context_add_string(ctx, track->artist);
    memcpy(id_alloced, track->spotify_artist_id, sizeof(track->spotify_artist_id));
    dbus_util_message_context_add_string(ctx, id_alloced);
    dbus_util_message_context_add_uint32(ctx, track->duration_ms);
    dbus_util_message_context_exit_struct(&ctx);
    free(id_alloced);
    free(uri_alloced);
}

void
dbus_add_playlist(PlaylistInfo *playlist, dbus_message_context *ctx) {
    dbus_util_message_context_enter_struct(&ctx);
    dbus_util_message_context_add_bool(ctx, playlist->album);
    dbus_util_message_context_add_string(ctx, playlist->name);
    dbus_util_message_context_add_string(ctx, playlist->image_url);
    char *id_alloced = malloc(sizeof(playlist->spotify_id));
    memcpy(id_alloced, playlist->spotify_id, sizeof(playlist->spotify_id));
    dbus_util_message_context_add_string(ctx, id_alloced);
    dbus_util_message_context_add_uint32(ctx, playlist->track_count);
    dbus_util_message_context_exit_struct(&ctx);
    free(id_alloced);
}

void
dbus_add_artist(Artist *artist, dbus_message_context *ctx) {
    dbus_util_message_context_enter_struct(&ctx);
    char *id_alloced = malloc(sizeof(artist->spotify_id));
    memcpy(id_alloced, artist->spotify_id, sizeof(artist->spotify_id));
    dbus_util_message_context_add_string(ctx, id_alloced);
    dbus_util_message_context_add_string(ctx, artist->name);
    dbus_util_message_context_add_uint32(ctx, artist->followers);
    dbus_util_message_context_exit_struct(&ctx);
    free(id_alloced);
}

#define ADD_DBUS_ARRAY(func, sig, len, arr, boo, ctx) { \
    dbus_bool_t b = boo;\
    dbus_util_message_context_enter_struct(&(ctx));\
    dbus_util_message_context_add_bool(ctx, b);\
    if ((b)) {\
        dbus_util_message_context_add_uint32(ctx, len);\
        dbus_util_message_context_enter_array(&(ctx), "("sig")");\
        for (int i = 0; i < (len); ++i) {\
            func(&(arr)[i], ctx);\
        }\
        dbus_util_message_context_exit_array(&(ctx));\
    }\
    dbus_util_message_context_exit_struct(&(ctx));\
}

void
handle_search_response(struct spotify_search_results *results) { // Return signature: (ba(ssssssu))(ba(bsssu))(ba(bsssu))(ba(ssu))
    struct search_params *params = (struct search_params *) results->userp;

    dbus_message_context *ctx = dbus_util_make_write_context(params->call);

    ADD_DBUS_ARRAY(dbus_add_track, "ssssssu", results->track_len, results->tracks, params->tracks, ctx);
    ADD_DBUS_ARRAY(dbus_add_playlist, "bsssu", results->album_len, results->albums, params->albums, ctx);
    ADD_DBUS_ARRAY(dbus_add_playlist, "bsssu", results->playlist_len, results->playlists, params->playlists, ctx);
    ADD_DBUS_ARRAY(dbus_add_artist, "ssu", results->artist_len, results->artists, params->artists, ctx);

    dbus_util_message_context_free(ctx);

    dbus_util_send_method(params->bus, params->call, NULL, NULL);

    // free the reply
    dbus_util_free_method_call(params->call);
    free(params->query);
    free(params);
    free_tracks(results->tracks, results->track_len);
    free(results->tracks);
    results->tracks = NULL;
    for (int i = 0; i < results->playlist_len; ++i) {
        free_playlist(&results->playlists[i]);
    }
    free(results->playlists);
    results->playlists = NULL;
    for (int i = 0; i < results->album_len; ++i) {
        free_playlist(&results->albums[i]);
    }
    free(results->albums);
    results->albums = NULL;
    for (int i = 0; i < results->artist_len; ++i) {
        free_artist(&results->artists[i]);
    }
    free(results->artists);
    results->artists = NULL;
    free(results);
}

void
handle_message(dbus_bus *bus) {
    dbus_util_poll_messages(bus);
    dbus_util_emit_signals(bus);
}


