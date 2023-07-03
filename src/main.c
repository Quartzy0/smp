#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "spotify.h"
#include "audio.h"
#include "dbus.h"
#include "util.h"
#include <unistd.h>
#include "config.h"
#include "cli.h"
#include <event2/event.h>

size_t track_index = -1;
Track *tracks = NULL;
size_t track_count = -1;
size_t track_size = -1;
Track *next_tracks = NULL;
size_t next_track_count = 0;

void
tracks_loaded_cb(struct spotify_state *spotify, void *userp) {
    if (userp) free(userp);
    printf("[ctrl] Track list loaded\n");
    if (play_track(spotify, &tracks[track_index], &spotify->smp_ctx->audio_buf, NULL)) {
        fprintf(stderr, "[ctrl] Error when trying to play track\n");
        Action a = {.type = ACTION_POSITION_RELATIVE, .position = 1};
        write(spotify->smp_ctx->action_fd[1], &a, sizeof(a));
    }
}

void
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

    Action a = {.type = ACTION_POSITION_RELATIVE, .position = 1};
    write(conn->spotify->smp_ctx->action_fd[1], &a, sizeof(a)); // Try playing next track on failure
    printf("[ctrl] Error occurred on connection, trying to play next track\n");
}

void
handle_action(int fd, short what, void *arg) {
    struct smp_context *ctx = (struct smp_context *) arg;
    Action a;
    read(fd, &a, sizeof(a));
    switch (a.type) {
        case ACTION_QUIT: {
            stop();
            for (int i = 0; i < ctx->spotify->connections_len; ++i) {
                struct connection *conn = &ctx->spotify->connections[i];
                if (conn->bev) bufferevent_free(conn->bev);
                if (conn->cache_fp) fclose(conn->cache_fp);
                if (conn->cache_path) remove(conn->cache_path); // Remove unfinished file
                if (conn->params.path) free(conn->params.path);
            }
            event_base_loopbreak(ctx->base);
            break;
        }
        case ACTION_ALBUM: {
            printf("[ctrl] Starting album with id %s\n", a.id);
            clear_tracks(tracks, &track_count, &track_size);
            track_index = 0;
            add_playlist(ctx->spotify, a.id, &tracks, &track_size, &track_count, true, tracks_loaded_cb);
            break;
        }
        case ACTION_PLAYLIST: {
            printf("[ctrl] Starting playlist with id %s\n", a.id);
            clear_tracks(tracks, &track_count, &track_size);
            track_index = 0;
            add_playlist(ctx->spotify, a.id, &tracks, &track_size, &track_count, false, tracks_loaded_cb);
            break;
        }
        case ACTION_PAUSE: {
            if (started) pause();
            break;
        }
        case ACTION_PLAY: {
            if (started) play();
            break;
        }
        case ACTION_PLAYPAUSE: {
            if (started) {
                if (status) pause();
                else play();
            }
            break;
        }
        case ACTION_STOP: {
            pause();
            stop();
            clear_tracks(tracks, &track_count, &track_size);
            free_tracks(next_tracks, next_track_count);
            next_tracks = NULL;
            next_track_count = 0;
            break;
        }
        case ACTION_TRACK_OVER:
        case ACTION_POSITION_RELATIVE: {
            if (track_index == -1 || track_size == -1 || track_count == -1 || track_count == 0) break;
            if (a.type == ACTION_TRACK_OVER && loop_mode == LOOP_MODE_TRACK) {
                //Do nothing
            } else if (shuffle) {
                size_t newI;
                while ((newI = (size_t) ((float) track_count *
                                         ((float) rand() / (float) RAND_MAX))) == track_index &&
                       track_count > 1) {}
                track_index = newI;
            } else {
                if (track_index + a.position >= track_count || track_index + a.position < 0) {
                    if (loop_mode == LOOP_MODE_PLAYLIST) {
                        track_index = 0;
                        if (play_track(ctx->spotify, &tracks[track_index], &ctx->audio_buf, NULL)) {
                            fprintf(stderr, "[ctrl] Error when trying to play track\n");
                            Action a1 = {.type = ACTION_POSITION_RELATIVE, .position = 1};
                            write(ctx->action_fd[1], &a1, sizeof(a1));
                        }
                        break;
                    }
                    //Continue playing recommendations
                    add_recommendations_from_tracks(ctx->spotify, &tracks, &track_size, &track_count,
                                                    tracks_loaded_cb);
                    track_index++;
                    break;
                } else {
                    track_index += a.position;
                }
            }
            if (play_track(ctx->spotify, &tracks[track_index], &ctx->audio_buf, NULL)) {
                fprintf(stderr, "[ctrl] Error when trying to play track\n");
                Action a1 = {.type = ACTION_POSITION_RELATIVE, .position = 1};
                write(ctx->action_fd[1], &a1, sizeof(a1));
            }
            break;
        }
        case ACTION_POSITION_ABSOLUTE: {
            if (!started) break;
            pause();
            stop();
            if (a.position >= track_count || a.position < 0) break;
            track_index = a.position;
            break;
        }
        case ACTION_TRACK: {
            printf("[ctrl] Starting track with id %s\n", a.id);

            clear_tracks(tracks, &track_count, &track_size);
            track_index = 0;

            add_track_info(ctx->spotify, a.id, &tracks, &track_size, &track_count, tracks_loaded_cb);
            break;
        }
        case ACTION_SEEK: {
            if (!started) break;
            seek = a.position;
            printf("[ctrl] Seek to: %ld\n", seek);
            break;
        }
        case ACTION_SET_POSITION: {
            if (!started) break;
            if (ctx->audio_info.finished_reading &&
                (a.position > (int64_t) (ctx->audio_info.total_frames / ctx->audio_info.sample_rate) * 1000000 ||
                 a.position < 0))
                break;
            seek = -((int64_t) (((double) ctx->audio_buf.offset / (double) ctx->audio_info.sample_rate) *
                                1000000.0) -
                     a.position);
            printf("[ctrl] Set position to: %ld (seek: %ld)\n", a.position, seek);
            break;
        }
        case ACTION_SEARCH: {
            struct search_params *params = malloc(sizeof(*params));
            memcpy(params, &a.search_params, sizeof(*params));
            search(ctx->spotify, params->query, search_complete_cb, params->tracks, params->artists, params->albums,
                   params->playlists, params);
            break;
        }
        case ACTION_NONE: {
            break;
        }
    }
}

int main(int argc, char **argv) {
    srand(clock());
    if (handle_cli(argc, argv)) {
        return EXIT_SUCCESS;
    }

    if (load_config()) {
        return EXIT_FAILURE;
    }

    struct spotify_state state;
    memset(&state, 0, sizeof(state));
    state.connections_len = 0;
    state.err_cb = spotify_conn_err;
    struct smp_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.base = event_base_new();
    ctx.spotify = &state;
    state.base = ctx.base;
    state.smp_ctx = &ctx;

    volume = initial_volume;
    memset(&ctx.audio_buf, 0, sizeof(ctx.audio_buf));
    ctx.audio_buf.buf = calloc(12000000, sizeof(*ctx.audio_buf.buf));
    ctx.audio_buf.size = 12000000;
    init(&ctx, &ctx.audio_buf);

    pipe(ctx.action_fd);

    ctx.action_event = event_new(ctx.base, ctx.action_fd[0], EV_READ | EV_PERSIST, handle_action, &ctx);
    if (!ctx.action_event) {
        fprintf(stderr, "[ctrl] Error when creating action event\n");
        close(ctx.action_fd[0]);
        close(ctx.action_fd[1]);
        return 1;
    }
    event_add(ctx.action_event, NULL);

    pthread_t dbus_thread = 0;
    int err = pthread_create(&dbus_thread, NULL, &init_dbus, &ctx);
    if (err) {
        fprintf(stderr, "[ctrl] Error occurred while trying to create downloader thread: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    refresh_available_regions(&state);

    event_base_dispatch(ctx.base);

    err = pthread_join(dbus_thread, NULL);
    if (err) {
        fprintf(stderr, "[ctrl] Error occurred while trying to wait for control thread to exit: %s\n", strerror(errno));
    }

    clean_audio();
    cleanup();
    clean_config();
    if (ctx.action_event) event_free(ctx.action_event);
    event_base_free(ctx.base);
    clean_vorbis_decode(&ctx.spotify->decode_ctx);
    for (int i = 0; i < sizeof(ctx.spotify->connections) / sizeof(*ctx.spotify->connections); ++i) {
        free(ctx.spotify->connections[i].cache_path);
    }
    clear_tracks(tracks, &track_count, &track_size);
    close(ctx.action_fd[0]);
    close(ctx.action_fd[1]);

    return EXIT_SUCCESS;
}
