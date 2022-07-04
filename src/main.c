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
#include <getopt.h>
#include "config.h"
#include "dbus-client.h"

#define HELP_TEXT   ("Usage: smp [-dstpPbnov] [OPTIONS...]\n" \
                    "\n"\
                    "\t-d, --daemon\n"\
                    "\t\tStart the music player service. This will not detach the process from the parent process.\n\n"\
                    "\t-?, --help\n"\
                    "\t\tShow this menu.\n\n"\
                    "\t-s, --status\n"\
                    "\t\tShow the player's status. This includes information about the track's title, artist, uri and"\
                    " album art. Information about the player's mode is also show, like the volume, loop mode and shuffle status\n\n"\
                    "\t-t, --toggle\n"\
                    "\t\tToggle the current playback state. If the player is paused, it will start playing and if it is"\
                    " playing it will pause. If the player is stopped and is not playing anything this will have no effect.\n\n"\
                    "\t-p, --play\n"\
                    "\t\tContinue playing the track. This will have no effect if the player is stopped.\n\n"\
                    "\t-P, --pause\n"\
                    "\t\tPause the current track. This will have no effect if the player is stopped.\n\n"\
                    "\t-n, --next\n"\
                    "\t\tWill play the next track in the playlist. The behaviour of this command varies based on the "\
                    "loop/shuffle modes. If shuffle is enabled, it will not play the next track in the playlist but will"\
                    " play a random one. It will also not play recommendations from spotify but will endlessly play songs from"\
                    " the current playlist. If loop mode is set to track, it will still play the next track. If loop mode"\
                    " is set to playlist, it will start playing the first track once the end is reached.\n\n"\
                    "\t-b, --previous\n"\
                    "\t\tThis option behaves the same way as the --next option, except it plays the previous track.\n\n"\
                    "\t-o, --open\n"\
                    "\t\tStarts playing the track/album/playlist described by the uri. The accepted uri scheme is "\
                    "'spotify:<track/album/playlist>:<22 character id>'\n\n"\
                    "\t-v, --volume\n"\
                    "\t\tSet the volume. The volume goes from 1 to 0.\n\n")
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
    next_track_count = -1;
    get_recommendations_from_tracks(params->tracks, params->track_count, 30, params->next_tracks,
                                    params->next_track_count);
    printf("[ctrl - recommendations] Done. Wake: %ld\n",
           syscall(SYS_futex, &next_track_count, FUTEX_WAKE, INT_MAX, NULL, 0, 0));
    free(params);
    return NULL;
}

void
redo_audio() {
    printf("[ctrl] Attempting to download new tracks\n");
    for (int i = 0; i < preload_amount; ++i) {
        Track *track = NULL;
        if (track_index + i >= track_count) {
            if (loop_mode == LOOP_MODE_NONE) {
                if (next_track_count == 0) {
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
        while (track->download_state == DS_DOWNLOADING) {
            printf("[ctrl] Sleep sleep... (downloading)\n");
            if (syscall(SYS_futex, &track->download_state, FUTEX_WAIT, DS_DOWNLOADING, NULL,
                        0, 0) == -1) {
                fprintf(stderr, "Error when waiting: %s\n", strerror(errno));
                break;
            }
        }
        download_track(track, !i);
    }
    if (tracks[track_index].download_state ==
        DS_DOWNLOAD_FAILED) { //Go to next track if this track couldn't be downloaded
        rear.type = ACTION_POSITION_RELATIVE;
        last_rear.position = 1;
        sem_post(&state_change_lock);
    }
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
                if (started) {
                    pause();
                    stop();
                }
                goto end;
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
                    tracks = NULL;
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
                    size_t newI;
                    while ((newI = (size_t) (((float) track_count) *
                                             (((float) rand()) / ((float) RAND_MAX)))) != track_index &&
                           track_count > 1) {}
                    track_index = newI;
                } else {
                    if (track_index + a.position >= track_count || track_index + a.position < 0) {
                        if (loop_mode == LOOP_MODE_PLAYLIST) {
                            track_index = 0;
                            redo_audio();
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
                    tracks = NULL;
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
    int c;

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
                {"daemon",   no_argument,       0, 'd'},
                {"help",     no_argument,       0, '?'},
                {"status",   no_argument,       0, 's'},
                {"toggle",   no_argument,       0, 't'},
                {"play",     no_argument,       0, 'p'},
                {"pause",    no_argument,       0, 'P'},
                {"next",     no_argument,       0, 'n'},
                {"previous", no_argument,       0, 'b'},
                {"open",     required_argument, 0, 'o'},
                {"volume",   required_argument, 0, 'v'},
                {"shuffle",  no_argument,       0, 'S'},
                {"loop",     no_argument,       0, 'l'},
                {0, 0,                          0, 0}
        };

        c = getopt_long(argc, argv, "d?stpPbno:v:Sl",
                        long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'd':
                printf("[smp] Starting as daemon\n");
                goto player_section;
            case 's':
                init_dbus_client();
                PlayerProperties s;
                s.playback_status = 100;
                if (get_player_properties(&s)) {
                    fprintf(stderr, "[dbus-client] Error while getting player status\n");
                    break;
                }
                print_properties(stdout, &s);
                break;
            case 't':
                init_dbus_client();
                dbus_client_pause_play();
                break;
            case 'p':
                init_dbus_client();
                dbus_client_play();
                break;
            case 'P':
                init_dbus_client();
                dbus_client_pause();
                break;
            case 'n':
                init_dbus_client();
                dbus_client_next();
                break;
            case 'b':
                init_dbus_client();
                dbus_client_previous();
                break;
            case 'o':
                init_dbus_client();
                dbus_client_open(optarg);
                break;
            case 'v':
                init_dbus_client();
                if (optarg) {
                    char *end = NULL;
                    double d = strtod(optarg, &end);
                    dbus_client_set_volume(d);
                }
                printf("%lf\n", dbus_client_get_volume());
                break;
            case 'S':
                init_dbus_client();
                bool shuffle_mode = dbus_client_get_shuffle_mode();
                dbus_client_set_shuffle_mode(!shuffle_mode);
                printf("%s", shuffle_mode ? "false" : "true");
                break;
            case 'l':
                init_dbus_client();
                LoopMode loop_mode = dbus_client_get_loop_mode();
                loop_mode = (loop_mode + 1) % LOOP_MODE_LAST;
                dbus_client_set_loop_mode(loop_mode);
                char *loop_mode_string = "None";
                switch (loop_mode) {
                    case LOOP_MODE_TRACK:
                        loop_mode_string = "Track";
                        break;
                    case LOOP_MODE_PLAYLIST:
                        loop_mode_string = "Playlist";
                        break;
                }
                printf("%s\n", loop_mode_string);
                break;
            case '?':
                printf(HELP_TEXT);
                exit(EXIT_SUCCESS);
            default:
                printf("?? getopt returned character code 0%o ??\n", c);
        }
    }

    exit(EXIT_SUCCESS);

    player_section:
    if (load_config()) {
        return EXIT_FAILURE;
    }

    get_token();
    sem_init(&state_change_lock, 0, 0);

    volume = initial_volume;
    init();

    pthread_t download_thread = 0;
    int err = pthread_create(&download_thread, NULL, &download_checks, NULL);
    if (err) {
        fprintf(stderr, "[ctrl] Error occurred while trying to create downloader thread: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    init_dbus();
    handle_message();

    err = pthread_join(download_thread, NULL);
    if (err) {
        fprintf(stderr, "[ctrl] Error occurred while trying to wait for control thread to exit: %s\n", strerror(errno));
    }

    sem_destroy(&state_change_lock);
    clean_audio();
    free_playlist(&cplaylist);
    cleanup();
    clean_config();

    return EXIT_SUCCESS;
}
