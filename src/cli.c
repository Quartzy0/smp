//
// Created by quartzy on 7/4/22.
//

#include "cli.h"
#include "dbus-client.h"
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "util.h"
#include "spotify.h"

int
playback(int argc, char **argv) {
    if (argc == 2) {
        init_dbus_client();
        PlayerProperties s;
        s.playback_status = 100;
        if (get_player_properties(&s)) {
            fprintf(stderr, "[dbus-client] Error while getting player status\n");
            return 0;
        }
        print_properties(stdout, &s);
        return 1;
    }
    int c;

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
                {"help",     no_argument,       0, '?'},
                {"toggle",   no_argument,       0, 't'},
                {"play",     no_argument,       0, 'p'},
                {"pause",    no_argument,       0, 'P'},
                {"next",     no_argument,       0, 'n'},
                {"previous", no_argument,       0, 'b'},
                {"volume",   required_argument, 0, 'v'},
                {"shuffle",  no_argument,       0, 'S'},
                {"loop",     no_argument,       0, 'l'},
                {0, 0,                          0, 0}
        };

        c = getopt_long(argc, argv, "d?stpPbnv:Sl",
                        long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
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
                printf("No help page yet\n");
                exit(EXIT_SUCCESS);
            default:
                printf("?? getopt returned character code 0%o ??\n", c);
        }
    }
    return 1;
}

int
track(int argc, char **argv) {
    if (argc == 0) {
        init_dbus_client();
        PlayerProperties s;
        s.playback_status = 100;
        if (get_player_properties(&s)) {
            fprintf(stderr, "[dbus-client] Error while getting player status\n");
            return 0;
        }
        print_properties(stdout, &s);
        return 1;
    }
    int c;

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
                {"help", no_argument,       0, '?'},
                {"open", required_argument, 0, 'o'},
                {"list", no_argument,       0, 'l'},
                {0, 0,                      0, 0}
        };

        c = getopt_long(argc, argv, "o:?l",
                        long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'o':
                init_dbus_client();
                dbus_client_open(optarg);
                break;
            case 'l':
                init_dbus_client();
                char **tracks = NULL;
                int count = 0;
                dbus_client_tracklist_get_tracks(&tracks, &count);
                Metadata *metadata = calloc(count, sizeof(*metadata));
                dbus_client_get_tracks_metadata(tracks, count, metadata);
                for (int i = 0; i < count; ++i) {
                    printf("[%d] '%s' by '%s' (%s)\n", i + 1, metadata[i].title, metadata[i].artist, metadata[i].url);
                }
                for (int i = 0; i < count; ++i) {
                    free(tracks[i]);
                    free_metadata(&metadata[i]);
                }
                free(tracks);
                free(metadata);
                break;
            case '?':
                printf("No help page yet\n");
                exit(EXIT_SUCCESS);
            default:
                printf("?? getopt returned character code 0%o ??\n", c);
        }
    }
    return 1;
}

int
search_cli(int argc, char **argv) {
    bool t = false, a = false, p = false;
    int c;

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
                {"help",      no_argument, 0, '?'},
                {"tracks",    no_argument, 0, 't'},
                {"albums",    no_argument, 0, 'a'},
                {"playlists", no_argument, 0, 'p'},
                {0, 0,                     0, 0}
        };

        c = getopt_long(argc, argv, "?tap",
                        long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 't':
                t = true;
                break;
            case 'a':
                a = true;
                break;
            case 'p':
                p = true;
                break;
            case '?':
                printf("No help page yet\n");
                exit(EXIT_SUCCESS);
            default:
                printf("?? getopt returned character code 0%o ??\n", c);
        }
    }

    if (!t && !a && !p) {
        t = true;
        a = true;
        p = true;
    }
    char *query = calloc(100, sizeof(*query));
    size_t query_size = 100;
    size_t query_size_used = 0;
    for (int i = optind + 1; i < argc; ++i) {
        if (strlen(argv[i]) >= (query_size - query_size_used)) {
            char *tmp = realloc(query, query_size + strlen(argv[i]) * 2);
            if (!tmp) {
                free(query);
                fprintf(stderr, "[cli] Error when calling realloc: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
            query = tmp;
        }
        strcat(query, argv[i]);
        query_size_used = strlen(query) + 1;
    }

    Track *tracks = NULL;
    PlaylistInfo *albums = NULL;
    PlaylistInfo *playlists = NULL;
    size_t track_count = 0, album_count = 0, playlist_count = 0;
    for (int i = 0; i < 3; ++i) {
        int r = search(query, (Track **) ((uint64_t) &tracks * t), &track_count,
                       (PlaylistInfo **) ((uint64_t) &playlists * p),
                       &playlist_count, (PlaylistInfo **) ((uint64_t) &albums * a), &album_count);
        if (!r)break;
        if (i == 2) {
            fprintf(stderr, "[cli] Failed to search after 3 attempts\n");
            free(query);
            exit(EXIT_FAILURE);
        }
    }
    free(query);
    if (t) {
        printf("Tracks:\n");
        for (int i = 0; i < track_count; ++i) {
            printf("\t[%d] %s by %s (%s)\n", i + 1, tracks[i].spotify_name, tracks[i].artist, tracks[i].spotify_uri);
        }
        free_tracks(tracks, track_count);
        free(tracks);
    }
    if (a) {
        printf("Albums:\n");
        for (int i = 0; i < album_count; ++i) {
            printf("\t[%d] %s with %d tracks (spotify:album:%s)\n", i + 1, albums[i].name, albums[i].track_count,
                   albums[i].spotify_id);
            free_playlist(&albums[i]);
        }
        free(albums);
    }
    if (p) {
        printf("Playlists:\n");
        for (int i = 0; i < playlist_count; ++i) {
            printf("\t[%d] %s with %d tracks (spotify:playlist:%s)\n", i + 1, playlists[i].name,
                   playlists[i].track_count,
                   playlists[i].spotify_id);
            free_playlist(&playlists[i]);
        }
        free(playlists);
    }

    return 1;
}

int
handle_cli(int argc, char **argv) {
    if (argc < 2) {
        printf("Incorrect usage\n");
        return 1;
    }
    if (!strcmp(argv[1], "daemon")) {
        return 0;
    } else if (!strcmp(argv[1], "playback") || !strcmp(argv[1], "pb")) {
        return playback(argc, argv);
    } else if (!strcmp(argv[1], "track") || !strcmp(argv[1], "t")) {
        return track(argc, argv);
    } else if (!strcmp(argv[1], "search") || !strcmp(argv[1], "s")) {
        return search_cli(argc, argv);
    } else {
        printf("Incorrect usage\n");
    }
    return 1;
}