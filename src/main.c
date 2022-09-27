#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "spotify.h"
#include "audio.h"
#include "dbus.h"
#include "util.h"
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "config.h"
#include "cli.h"
#include <event2/event.h>

PlaylistInfo cplaylist;
size_t track_index = -1;
Track *tracks = NULL;
size_t track_count;
Track *next_tracks = NULL;
size_t next_track_count = 0;
pthread_t prepare_thread;

struct RecommendationParams {
    Track *tracks;
    size_t track_count;
    Track **next_tracks;
    size_t *next_track_count;
};

void *
prepare_recommendations(void *userp) {
    struct RecommendationParams *params = (struct RecommendationParams *) userp;
    printf("[ctrl - recommendations] Looking for recommendations\n");
    get_recommendations_from_tracks(params->tracks, params->track_count, 30, params->next_tracks,
                                    params->next_track_count);
    printf("[ctrl - recommendations] Done. Wake: %ld\n",
           syscall(SYS_futex, &next_track_count, FUTEX_WAKE, INT_MAX, NULL, 0, 0));
    free(params);
    return NULL;
}

void
redo_audio(struct smp_context *ctx) {
    printf("[ctrl] Attempting to download new tracks\n");
    FILE *pcm = NULL;
    for (int i = 0; i < preload_amount; ++i) {
        Track *track = NULL;
        if (track_index + i >= track_count) {
            if (loop_mode == LOOP_MODE_NONE) {
                if (next_track_count == 0) {
                    next_track_count = -1;
                    struct RecommendationParams *params = malloc(sizeof(*params));
                    params->next_tracks = &next_tracks;
                    params->next_track_count = &next_track_count;
                    params->tracks = tracks;
                    params->track_count = track_count;
                    pthread_create(&prepare_thread, NULL, prepare_recommendations, params);
                } else if (next_track_count != -1) {
                    track = &next_tracks[(track_index + i) - track_count];
                    goto loop;
                }
            }
            break;
        } else {
            track = &tracks[track_index + i];
        }
        loop:
        download_track(track, !i, &pcm);
    }
    while (tracks[track_index].download_state == DS_DOWNLOADING) {
        printf("[ctrl] Sleep sleep... (downloading)\n");
        if (syscall(SYS_futex, &tracks[track_index].download_state, FUTEX_WAIT, DS_DOWNLOADING, NULL,
                    0, 0) == -1) {
            fprintf(stderr, "Error when waiting: %s\n", strerror(errno));
            break;
        }
    }
    if (tracks[track_index].download_state ==
        DS_DOWNLOAD_FAILED) { //Go to next track if this track couldn't be downloaded
        Action a = {.type = ACTION_POSITION_RELATIVE};
        a.position = 1;
        write(ctx->action_fd[1], &a, sizeof(a));
        start();
        return;
    }
    printf("[ctrl] Playing '%s' by %s\n", tracks[track_index].spotify_name,
           tracks[track_index].artist);
    if (pcm){
        set_pcm_stream(pcm);
    }else{
        char *file = NULL;
        track_filepath(&tracks[track_index], &file);
        if (set_file(file)) return;
        free(file);
    }
    start();
    play();
}

void
handle_action(int fd, short what, void *arg) {
    struct smp_context *ctx = (struct smp_context*) arg;
    Action a;
    read(fd, &a, sizeof(a));
    switch (a.type) {
        case ACTION_QUIT: {
            if (started) {
                pause();
                stop();
            }
            event_base_loopbreak(ctx->base);
        }
        case ACTION_ALBUM: {
            printf("[ctrl] Starting album with id %s\n", a.id);
            if (started) {
                pause();
                stop();
                free_tracks(tracks, track_count);
                tracks = NULL;
                track_count = 0;
            }
            track_index = 0;
            if (!get_album(a.id, &cplaylist, (Track **) &tracks)) {
                track_count = cplaylist.track_count;
                struct timespec tp;
                clock_gettime(CLOCK_REALTIME, &tp);
                cplaylist.last_played = tp.tv_sec;
                redo_audio(ctx);
                char *file = NULL;
                playlist_filepath(cplaylist.spotify_id, &file, cplaylist.album);
                save_playlist_last_played_to_file(file, (PlaylistInfo *) &cplaylist);
                free(file);
            }
            break;
        }
        case ACTION_PLAYLIST: {
            printf("[ctrl] Starting playlist with id %s\n", a.id);
            if (started) {
                pause();
                stop();
                free_tracks(tracks, track_count);
                tracks = NULL;
            }
            track_index = 0;
            if (!get_playlist(a.id, &cplaylist, &tracks)) {
                track_count = cplaylist.track_count;
                struct timespec tp;
                clock_gettime(CLOCK_REALTIME, &tp);
                cplaylist.last_played = tp.tv_sec;
                redo_audio(ctx);
                char *file = NULL;
                playlist_filepath(cplaylist.spotify_id, &file, cplaylist.album);
                save_playlist_last_played_to_file(file, (PlaylistInfo *) &cplaylist);
                free(file);
            }
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
            free_tracks(tracks, track_count);
            tracks = NULL;
            free_tracks(next_tracks, next_track_count);
            next_tracks = NULL;
            next_track_count = 0;
            break;
        }
        case ACTION_TRACK_OVER:
        case ACTION_POSITION_RELATIVE: {
            if (!started) break;
            pause();
            stop();
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
                        redo_audio(ctx);
                        break;
                    }
                    //Continue playing recommendations
                    if (next_track_count == 0) {
                        if (get_recommendations_from_tracks(tracks, track_count, 30, &next_tracks,
                                                            &next_track_count))
                            break;
                    } else if (next_track_count == -1) {
                        while (next_track_count == -1) {
                            printf("[ctrl] Sleep sleep... (recommendations)\n");
                            if (syscall(SYS_futex, &next_track_count, FUTEX_WAIT, -1, NULL,
                                        0, 0) == -1) {
                                fprintf(stderr, "Error when waiting for recommendations: %s\n", strerror(errno));
                                break;
                            }
                        }
                    }
                    pthread_join(prepare_thread, NULL);
                    free_tracks(tracks, track_count);
                    tracks = NULL;
                    tracks = next_tracks;
                    track_count = next_track_count;
                    next_tracks = NULL;
                    next_track_count = 0;
                    track_index = 0;
                    free_playlist(&cplaylist);
                } else {
                    track_index += a.position;
                }
            }
            redo_audio(ctx);
            break;
        }
        case ACTION_POSITION_ABSOLUTE: {
            if (!started) break;
            pause();
            stop();
            if (a.position >= track_count || a.position < 0) break;
            track_index = a.position;
            redo_audio(ctx);
            break;
        }
        case ACTION_TRACK: {
            printf("[ctrl] Starting track with id %s\n", a.id);
            if (started) {
                pause();
                stop();
                free_tracks(tracks, track_count);
                tracks = NULL;
            }
            track_index = 0;
            track_count = 1;
            write_track(ctx->spotify, a.id, ctx->audio_buf);
//            if (!track_by_id(a.id, (Track **) &tracks))
//                redo_audio(ctx);
            start();
            play();
            break;
        }
        case ACTION_SEEK: {
            seek = a.position;
            printf("[ctrl] Seek to: %ld\n", seek);
            break;
        }
        case ACTION_SET_POSITION: {
            int64_t seek_new = -(((int64_t) (offset / audio_samplerate) * 1000000) - a.position);
            if ((int64_t) seek_new > (int64_t) (frames / audio_samplerate) * 1000000) break;
            seek = seek_new;
            printf("[ctrl] Set position to: %ld\n", seek);
            break;
        }
        case ACTION_NONE: {
            break;
        }
    }
}

int main(int argc, char **argv) {
    srand(clock());
    if(handle_cli(argc, argv)){
        return EXIT_SUCCESS;
    }

//    if (load_config()) {
//        return EXIT_FAILURE;
//    }

    struct spotify_state state;
    state.connections_len = 0;
    state.instances[0] = "127.0.0.1";
    struct smp_context ctx;
    ctx.base = event_base_new();
    ctx.spotify = &state;
    state.base = ctx.base;

//    volume = initial_volume;
    volume = 0.5;
    struct evbuffer *audio_buf = evbuffer_new();
    ctx.audio_buf = audio_buf;
    init(&ctx, audio_buf);

    pipe(ctx.action_fd);

    struct event *fd_event = event_new(ctx.base, ctx.action_fd[0], EV_READ | EV_PERSIST, handle_action, &ctx);
    event_add(fd_event, NULL);

    pthread_t dbus_thread = 0;
    int err = pthread_create(&dbus_thread, NULL, &init_dbus, &ctx);
    if (err) {
        fprintf(stderr, "[ctrl] Error occurred while trying to create downloader thread: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    event_base_dispatch(ctx.base);

    err = pthread_join(dbus_thread, NULL);
    if (err) {
        fprintf(stderr, "[ctrl] Error occurred while trying to wait for control thread to exit: %s\n", strerror(errno));
    }

    clean_audio();
    free_playlist(&cplaylist);
    cleanup();
    clean_config();
    evbuffer_free(audio_buf);

    return EXIT_SUCCESS;
}
