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

#define PRELOAD_COUNT 3

PlaylistInfo cplaylist;
size_t track_index = -1;
Track *tracks;
size_t track_count;

void
redo_audio() {
    printf("[ctrl] Attempting to download new tracks\n");
    for (int i = 0; i < PRELOAD_COUNT; ++i) {
        if (track_index + i >= track_count) break;
        while (tracks[track_index + i].download_state == DS_DOWNLOADING) {
            printf("[ctrl] Sleep sleep...\n");
            if (syscall(SYS_futex, &tracks[track_index + i].download_state, FUTEX_WAIT, DS_DOWNLOADING, NULL,
                        0, 0) == -1) {
                fprintf(stderr, "Error when waiting: %s\n", strerror(errno));
                break;
            }
        }
        download_track(&tracks[track_index + i], !i);
    }
    if (!inited) init();
    printf("[ctrl] Playing '%s' by %s\n", tracks[track_index].spotify_name,
           tracks[track_index].artist);
    char *file = NULL;
    track_filepath(&tracks[track_index], &file);
    set_file(file);
    free(file);
    start();
    play();
}

void *
download_checks(void *arg) {
    srand(clock());
    while (1) {
        sem_wait(&state_change_lock);
        Action a = front;
        switch (a.type) {
            case ACTION_QUIT: {
                pause();
                stop();
                goto end;
            }
            case ACTION_ALBUM: {
                printf("[ctrl] Starting album with id %s\n", a.id);
                if (started) {
                    pause();
                    stop();
                    free_tracks(tracks, track_count);
                    track_count = 0;
                }
                track_index = 0;
                if (!get_album(a.id, &cplaylist, (Track **) &tracks)) {
                    track_count = cplaylist.track_count;
                    struct timespec tp;
                    clock_gettime(CLOCK_REALTIME, &tp);
                    cplaylist.last_played = tp.tv_sec;
                    redo_audio();
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
                }
                track_index = 0;
                if (!get_playlist(a.id, &cplaylist, &tracks)) {
                    track_count = cplaylist.track_count;
                    struct timespec tp;
                    clock_gettime(CLOCK_REALTIME, &tp);
                    cplaylist.last_played = tp.tv_sec;
                    redo_audio();
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
            case ACTION_TRACK_OVER:
            case ACTION_POSITION_RELATIVE: {
                if (started) {
                    pause();
                    stop();
                }
                if (a.type == ACTION_TRACK_OVER && loop_mode == LOOP_MODE_TRACK) {
                    //Do nothing
                } else if (shuffle) {
                    size_t newI = 0;
                    while ((newI = (size_t) (((float) track_count) *
                                             (((float) rand()) / ((float) RAND_MAX)))) != track_index &&
                           track_count > 1) {}
                    track_index = newI;
                } else {
                    if (track_index + a.position >= track_count || track_index + a.position < 0) {
                        if (loop_mode == LOOP_MODE_PLAYLIST) {
                            track_index = 0;
                        } else {
                            break;
                        }
                    } else {
                        track_index += a.position;
                    }
                }
                redo_audio();
                break;
            }
            case ACTION_POSITION_ABSOLUTE: {
                if (started) {
                    pause();
                    stop();
                }
                if (a.position >= track_count || a.position < 0) break;
                track_index = a.position;
                redo_audio();
                break;
            }
            case ACTION_TRACK: {
                printf("[ctrl] Starting track with id %s\n", a.id);
                if (started) {
                    pause();
                    stop();
                    free_tracks(tracks, track_count);
                }
                track_index = 0;
                track_count = 1;
                if (!track_by_id(a.id, (Track **) &tracks))
                    redo_audio();
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
    end:
    return NULL;
}

int main(int argc, char **argv) {
    get_token();
    sem_init(&state_change_lock, 0, 0);

    pthread_t download_thread;
    int err = pthread_create(&download_thread, NULL, &download_checks, NULL);
    if (err) {
        fprintf(stderr, "[ctrl] Error occurred while trying to create downloader thread: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    init_dbus();
    handle_message();

    pthread_join(download_thread, NULL);

    sem_destroy(&state_change_lock);
    clean_audio();
    free_playlist(&cplaylist);
    cleanup();

    return EXIT_SUCCESS;
}
