//
// Created by quartzy on 7/4/22.
//

#include "cli.h"
#include "dbus-client.h"
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include "util.h"
#include "spotify.h"

void
playback(int argc, char **argv) {
    if (argc == 1) {
        if (init_dbus_client())
            return;
        PlayerProperties s;
        s.playback_status = 100;
        if (get_player_properties(&s)) {
            fprintf(stderr, "[dbus-client] Error while getting player status\n");
            return;
        }
        print_properties(stdout, &s);
        return;
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
                {"shuffle",  no_argument,       0, 's'},
                {"loop",     no_argument,       0, 'l'},
                {"stop",     no_argument,       0, 'S'},
                {0, 0,                          0, 0}
        };

        c = getopt_long(argc, argv, "d?stpPbnv:Sls",
                        long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 't':
                if (!init_dbus_client())
                    dbus_client_pause_play();
                break;
            case 'p':
                if (!init_dbus_client())
                    dbus_client_play();
                break;
            case 'P':
                if (!init_dbus_client())
                    dbus_client_pause();
                break;
            case 'n':
                if (!init_dbus_client())
                    dbus_client_next();
                break;
            case 'b':
                if (!init_dbus_client())
                    dbus_client_previous();
                break;
            case 'v':
                if (init_dbus_client())
                    break;
                if (optarg) {
                    char *end = NULL;
                    double d = strtod(optarg, &end);
                    dbus_client_set_volume(d);
                }
                printf("%lf\n", dbus_client_get_volume());
                break;
            case 's':
                if (init_dbus_client())
                    break;
                bool shuffle_mode = dbus_client_get_shuffle_mode();
                dbus_client_set_shuffle_mode(!shuffle_mode);
                printf("Shuffle: %s\n", shuffle_mode ? "off" : "on");
                break;
            case 'l':
                if (init_dbus_client())
                    break;
                LoopMode loop_mode1 = dbus_client_get_loop_mode();
                loop_mode1 = (loop_mode1 + 1) % LOOP_MODE_LAST;
                dbus_client_set_loop_mode(loop_mode1);
                char *loop_mode_string;
                switch (loop_mode1) {
                    case LOOP_MODE_TRACK:
                        loop_mode_string = "Track";
                        break;
                    case LOOP_MODE_PLAYLIST:
                        loop_mode_string = "Playlist";
                        break;
                    default:
                        loop_mode_string = "None";
                        break;
                }
                printf("%s\n", loop_mode_string);
                break;
            case 'S':
                if (init_dbus_client())
                    break;
                dbus_client_call_method("org.mpris.MediaPlayer2.Player", "Stop");
                break;
            case '?':
                printf(HELP_TXT_PLAYBACK);
                exit(EXIT_SUCCESS);
            default:
                printf("?? getopt returned character code 0%o ??\n", c);
        }
    }
}

void
track(int argc, char **argv) {
    if (argc == 1) {
        if (init_dbus_client())
            return;
        PlayerProperties s;
        s.playback_status = 100;
        if (get_player_properties(&s)) {
            fprintf(stderr, "[dbus-client] Error while getting player status\n");
            return;
        }
        print_properties(stdout, &s);
        return;
    }
    int c;

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
                {"help", no_argument, 0, '?'},
                {"list", no_argument, 0, 'l'},
                {0, 0,                0, 0}
        };

        c = getopt_long(argc, argv, "?l",
                        long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'l':
                if (init_dbus_client())
                    return;
                char **tracks = NULL;
                int count = 0;
                dbus_client_tracklist_get_tracks(&tracks, &count);
                if (count != 0) {
                    Metadata *metadata = calloc(count, sizeof(*metadata));
                    dbus_client_get_tracks_metadata(tracks, count, metadata);
                    for (int i = 0; i < count; ++i) {
                        printf("[%d] '%s' by '%s' (%s)\n", i + 1, metadata[i].title, metadata[i].artist,
                               metadata[i].url);
                    }
                    for (int i = 0; i < count; ++i) {
                        free(tracks[i]);
                        free_metadata(&metadata[i]);
                    }
                    free(metadata);
                } else {
                    printf("No tracks loaded\n");
                }
                free(tracks);
                break;
            case '?':
                printf(HELP_TXT_TRACK);
                exit(EXIT_SUCCESS);
            default:
                printf("?? getopt returned character code 0%o ??\n", c);
        }
    }
}

void
search_completed_cb(struct spotify_state *spotify, void *userp) {
    struct spotify_search_results *res = (struct spotify_search_results *) userp;

}

void
search_cli(int argc, char **argv) {
    if (argc < 2) {
        printf(HELP_TXT_SEARCH);
        exit(EXIT_FAILURE);
    }
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
                printf(HELP_TXT_SEARCH);
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
    for (int i = optind; i < argc; ++i) {
        if (strlen(argv[i]) + 1 >= (query_size - query_size_used)) {
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
        query[query_size_used++ - 1] = ' ';
    }
    if (str_is_empty(query)) {
        fprintf(stderr, "Query cannot be empty!\n");
        printf(HELP_TXT_SEARCH);
        free(query);
        exit(EXIT_FAILURE);
    }
    printf("Searching with query: %s\n", query);

    if (init_dbus_client())
        return;

    Track *tracks = NULL;
    PlaylistInfo *albums = NULL;
    PlaylistInfo *playlists = NULL;
    size_t track_count = 0, album_count = 0, playlist_count = 0;
    for (int i = 0; i < 3; ++i) {
        int r = dbus_client_search(t, a, false, p, query, &tracks, &track_count, &albums, &album_count, NULL, NULL,
                                   &playlists, &playlist_count);
        if (!r)break;
        if (i == 2) {
            fprintf(stderr, "[cli] Failed to search after 3 attempts\n");
            free(query);
            exit(EXIT_FAILURE);
        }
    }
    free(query);
    bool one = (t + a + p) == 1;
    if (t) {
        printf("Tracks:\n");
        for (int i = 0; i < track_count; ++i) {
            printf("\t[%d] %s by %s (%s)\n", i + 1, tracks[i].spotify_name, tracks[i].artist, tracks[i].spotify_uri);
        }
        if (one) goto choose_track;
    }
    if (a) {
        printf("Albums:\n");
        for (int i = 0; i < album_count; ++i) {
            printf("\t[%d] %s with %d tracks (spotify:album:%s)\n", i + 1, albums[i].name, albums[i].track_count,
                   albums[i].spotify_id);
        }
        if (one) goto choose_album;
    }
    if (p) {
        printf("Playlists:\n");
        for (int i = 0; i < playlist_count; ++i) {
            printf("\t[%d] %s with %d tracks (spotify:playlist:%s)\n", i + 1, playlists[i].name,
                   playlists[i].track_count,
                   playlists[i].spotify_id);
        }
        if (one) goto choose_playlist;
    }

    printf("Play anything (");
    if (t) printf(" Track");
    if (a) printf(" Album");
    if (p) printf(" Playlist");
    printf(" or Quit )?\n");
    ask_input:
    printf("> ");
    int in = getchar();
    getchar(); /* Read trailing \n character */
    switch (in) {
        case 'Q':
        case 'q':
            goto free_all;
        case 'T':
        case 't':
        choose_track:
        {
            printf("Choose track (1 - %zu or Quit)\n> ", track_count);

            char *line = NULL;
            size_t len = 0;
            if (getline(&line, &len, stdin) == -1) {
                fprintf(stderr, "[cli] Error reading line: %s\n", strerror(errno));
                free(line);
                goto free_all;
            }
            if (*line == 'q' || *line == 'Q') break;
            char *end = NULL;
            size_t index = strtol(line, &end, 10);
            free(line);
            if (init_dbus_client())
                return;
            dbus_client_open(tracks[index - 1].spotify_uri);
            break;
        }
        case 'A':
        case 'a':
        choose_album:
        {
            printf("Choose album (1 - %zu or Quit)\n> ", album_count);

            char *line = NULL;
            size_t len = 0;
            if (getline(&line, &len, stdin) == -1) {
                fprintf(stderr, "[cli] Error reading line: %s\n", strerror(errno));
                free(line);
                goto free_all;
            }
            if (*line == 'q' || *line == 'Q') break;
            char *end = NULL;
            size_t index = strtol(line, &end, 10);
            free(line);
            if (init_dbus_client())
                return;
            char uri[37] = "spotify:album:";
            memcpy(&uri[14], albums[index - 1].spotify_id, SPOTIFY_ID_LEN_NULL);
            dbus_client_open(uri);
            break;
        }
        case 'P':
        case 'p':
        choose_playlist:
        {
            printf("Choose playlist (1 - %zu or Quit)\n> ", playlist_count);

            char *line = NULL;
            size_t len = 0;
            if (getline(&line, &len, stdin) == -1) {
                fprintf(stderr, "[cli] Error reading line: %s\n", strerror(errno));
                free(line);
                goto free_all;
            }
            if (*line == 'q' || *line == 'Q') break;
            char *end = NULL;
            size_t index = strtol(line, &end, 10);
            free(line);
            if (init_dbus_client())
                return;
            char uri[40] = "spotify:playlist:";
            memcpy(&uri[17], playlists[index - 1].spotify_id, SPOTIFY_ID_LEN_NULL);
            dbus_client_open(uri);
            break;
        }
        default:
            printf("Invalid input! Must be: t, a, p or q\n");
            goto ask_input;
    }

    //Free them all
    free_all:
    free_tracks(tracks, track_count);
    free(tracks);
    for (int i = 0; i < album_count; ++i) {
        free_playlist(&albums[i]);
    }
    free(albums);
    for (int i = 0; i < playlist_count; ++i) {
        free_playlist(&playlists[i]);
    }
    free(playlists);
}

void playlists(int argc, char **argv) {
    if (argc == 1) {
        if (init_dbus_client())
            return;
        DBusPlaylistInfo playlist;
        if (dbus_client_get_active_playlist(&playlist))
            return;
        if (playlist.valid)
            printf("Currently playing playlist: %s (artUrl: %s)\n", playlist.name, playlist.icon);
        else
            printf("No playlist is playing\n");
        return;
    }
    int c;
    uint32_t index = 0;

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
                {"help", no_argument,       0, '?'},
                {"page", required_argument, 0, 'p'},
                {"list", no_argument,       0, 'l'},
                {0, 0,                      0, 0}
        };

        c = getopt_long(argc, argv, "?lp:",
                        long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'p': {
                char *end = NULL;
                index = strtol(optarg, &end, 10) - 1;
                if (end == optarg) index = 0;
                if (index < 0) index = 0;
                break;
            }
            case 'l': {
                if (init_dbus_client())
                    return;
                uint32_t playlist_count = 0;
                if (dbus_client_get_playlist_count(&playlist_count))
                    return;
                if (playlist_count <= 0){
                    printf("No playlists saved!\n");
                    return;
                }

                relist_playlists:
                if (index * PLAYLISTS_PER_PAGE >= playlist_count) {
                    printf("Page index out of range!\n");
                    return;
                }

                DBusPlaylistInfo playlists[PLAYLISTS_PER_PAGE];
                int count = 0;
                if (dbus_client_get_playlists(index * PLAYLISTS_PER_PAGE, PLAYLISTS_PER_PAGE, ORDER_ALPHABETICAL, false,
                                              playlists, &count))
                    return;
                printf("Showing %d saved playlists out of %u:\n", count, playlist_count);
                for (int i = 0; i < count; ++i) {
                    printf("\t[%d] %s\n", i + 1, playlists[i].name);
                }
                int pages = (int) ceilf((float) playlist_count / PLAYLISTS_PER_PAGE);
                printf("Page %u/%d\n", index + 1, pages);
                printf("Select playlist to play (1-%d), go to the next page (p), go to a specific page (p[PAGE]) or quit (q)\n> ",
                       count);

                char *line;
                size_t _len;
                size_t len;
                invalid_input:
                _len = 0;
                line = NULL;
                if ((len = getline(&line, &_len, stdin)) == -1) {
                    fprintf(stderr, "[cli] Error reading line: %s\n", strerror(errno));
                    free(line);
                    for (int j = 0; j < count; ++j) {
                        free_dbus_playlist(&playlists[j]);
                    }
                    break;
                }
                if (*line == 'q' || *line == 'Q') {
                    free(line);
                    for (int j = 0; j < count; ++j) {
                        free_dbus_playlist(&playlists[j]);
                    }
                    break;
                }
                if (*line == 'p' || *line == 'P') {
                    if (len == 2) { //"p\n"
                        if (++index >= pages)
                            index = 0;
                        free(line);
                        for (int j = 0; j < count; ++j) {
                            free_dbus_playlist(&playlists[j]);
                        }
                        goto relist_playlists;
                    }
                    char *end = NULL;
                    uint32_t i = strtol(&line[1], &end, 10);
                    if (end == &line[1]) {
                        index = 0;
                        free(line);
                        for (int j = 0; j < count; ++j) {
                            free_dbus_playlist(&playlists[j]);
                        }
                        goto relist_playlists;
                    }
                    free(line);
                    index = (i - 1) % pages;
                    for (int j = 0; j < count; ++j) {
                        free_dbus_playlist(&playlists[j]);
                    }
                    goto relist_playlists;
                }
                char *end = NULL;
                uint32_t i = strtol(line, &end, 10);
                if (end == line) {
                    printf("Invalid number %s> ", line);
                    free(line);
                    goto invalid_input;
                }
                free(line);
                if (i > count || i < 1) {
                    printf("Index out of range!\n> ");
                    goto invalid_input;
                }

                dbus_client_activate_playlist(playlists[i - 1].id);
                printf("Playing %s\n", playlists[i - 1].name);
                for (int j = 0; j < count; ++j) {
                    free_dbus_playlist(&playlists[j]);
                }
                break;
            }
            case '?':
                printf(HELP_TXT_PLAYLIST);
                exit(EXIT_SUCCESS);
            default:
                printf("?? getopt returned character code 0%o ??\n", c);
        }
    }
}

int
handle_cli(int argc, char **argv) {
    if (argc < 2) {
        printf(HELP_TXT_GENERAL);
        exit(EXIT_FAILURE);
    }
    if (!strcmp(argv[1], "daemon") || !strcmp(argv[1], "d")) {
        if (argc > 2) {
            printf(HELP_TXT_DAEMON);
            exit(EXIT_FAILURE);
        }
        return 0;
    } else if (!strcmp(argv[1], "playback") || !strcmp(argv[1], "pb")) {
        playback(argc - 1, &argv[1]);
    } else if (!strcmp(argv[1], "track") || !strcmp(argv[1], "t")) {
        track(argc - 1, &argv[1]);
    } else if (!strcmp(argv[1], "search") || !strcmp(argv[1], "s")) {
        search_cli(argc - 1, &argv[1]);
    } else if (!strcmp(argv[1], "quit") || !strcmp(argv[1], "q")) {
        if (argc > 2) {
            printf(HELP_TXT_QUIT);
            return 1;
        }
        if (!init_dbus_client())
            dbus_client_call_method("org.mpris.MediaPlayer2", "Quit");
    } else if (!strcmp(argv[1], "open") || !strcmp(argv[1], "o")) {
        if (argc == 2 || (strncasecmp(argv[2], "http", 4) != 0 && strncasecmp(argv[2], "spotify", 7) != 0)) {
            printf(HELP_TXT_OPEN);
            return 1;
        }
        char *uri = calloc(100, sizeof(*uri));
        size_t uri_size = 100;
        size_t uri_size_used = 0;
        for (int i = 2; i < argc; ++i) {
            if (strlen(argv[i]) + 1 >= (uri_size - uri_size_used)) {
                char *tmp = realloc(uri, uri_size + strlen(argv[i]) * 2);
                if (!tmp) {
                    free(uri);
                    fprintf(stderr, "[cli] Error when calling realloc: %s\n", strerror(errno));
                    exit(EXIT_FAILURE);
                }
                uri = tmp;
            }
            strcat(uri, argv[i]);
            uri_size_used = strlen(uri) + 1;
            uri[uri_size_used++ - 1] = ' ';
        }
        if (!init_dbus_client())
            dbus_client_open(uri);
    } else if (!strcmp(argv[1], "playlist") || !strcmp(argv[1], "p")) {
        playlists(argc - 1, &argv[1]);
    } else {
        printf(HELP_TXT_GENERAL);
        exit(EXIT_FAILURE);
    }
    return 1;
}