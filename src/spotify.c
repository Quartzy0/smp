#include "spotify.h"
#include <unistd.h>
#include "util.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/cjson/cJSON.h"
#include <errno.h>
#include <dirent.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include "config.h"
#include "audio.h"
#include "ctrl.h"

struct json_track_parse_params {
    Track **tracks;
    size_t *track_size;
    size_t *track_len;
};

struct backend_sort {
    struct backend_instance *inst;
    uint32_t score;
    char *fmatch;
};

struct ArtistQuantity {
    char id[23];
    size_t appearances;
};

bool
contains_regions(char *regions, size_t region_count, char *region) {
    if (!region_count) return true;
    char *ret = regions;
    while (ret) {
        ret = memchr(ret, region[0], region_count * 2 - (ret - regions));
        if (ret && ret[1] == region[1]) return true;
        if (!ret) break;
        ret++;
    }
    return false;
}

static int
compare_artist_quantities(const void *a, const void *b) {
    return (int) (((struct ArtistQuantity *) b)->appearances - ((struct ArtistQuantity *) a)->appearances);
}

static int
backend_sort_comprar(const void *a, const void *b) {
    return (int) (((struct backend_sort *) a)->score - ((struct backend_sort *) b)->score);
}

int
parse_playlist_info(const char *data, size_t len, PlaylistInfo *playlist);

void
generic_read_cb(struct bufferevent *bev, void *arg);

void
free_tracks(Track *track, size_t count) {
    if (!track || !count) return;
    for (int i = 0; i < count; ++i) {
        free(track[i].spotify_name);
        free(track[i].spotify_album_art);
        free(track[i].artist);
    }
    memset(track, 0, sizeof(*track) * count);
}

void
free_track(Track *track) {
    free(track->spotify_name);
    free(track->spotify_album_art);
    free(track->artist);
    if (track->playlist) {
        if (--track->playlist->reference_count == 0) {
            free(track->playlist->name);
            free(track->playlist->image_url);
            free(track->playlist);
        }
    }
    memset(track, 0, sizeof(*track));
}

void
clear_tracks(Track *tracks, size_t *track_len, size_t *track_size) {
    *track_size = 0;
    if (!tracks || !track_len) {
        *track_len = 0;
        return;
    }
    *track_len = 0;
    for (size_t i = 0; i < *track_len; ++i) free_track(&tracks[i]);
}

void
deref_playlist(PlaylistInfo *playlist){
    if (!playlist || !playlist->not_empty || --playlist->reference_count != 0)return;
    free_playlist(playlist);
}

void
free_playlist(PlaylistInfo *playlist) {
    if (!playlist->not_empty)return;
    free(playlist->name);
    free(playlist->image_url);
    memset(playlist, 0, sizeof(*playlist));
}

void
free_artist(Artist *artist) {
    free(artist->name);
}

void
free_connection(struct connection *conn) {
    free(conn->payload);
    conn->payload = NULL;
    conn->payload_len = 0;
    free(conn->cache_path);
    conn->cache_path = NULL;
    if (conn->cache_fp) {
        fclose(conn->cache_fp);
        conn->cache_fp = NULL;
    }
    conn->busy = false;
    conn->retries = 0;
    conn->expecting = conn->progress = 0;
    conn->error_type = ET_NO_ERROR;
}

void
track_filepath_id(const char id[SPOTIFY_ID_LEN], char **out) {
    (*out) = malloc((track_save_path_len + SPOTIFY_ID_LEN + 4 + 1) * sizeof(char));
    snprintf((*out), track_save_path_len + SPOTIFY_ID_LEN + 4 + 1, "%s%.22s.ogg", track_save_path,
             id);
}

void
track_info_filepath_id(const char id[SPOTIFY_ID_LEN], char **out) {
    (*out) = malloc((track_info_path_len + SPOTIFY_ID_LEN + 5 + 1) * sizeof(char));
    snprintf((*out), track_info_path_len + SPOTIFY_ID_LEN + 5 + 1, "%s%.22s.json", track_info_path,
             id);
}

void
album_info_filepath_id(const char id[SPOTIFY_ID_LEN], char **out) {
    (*out) = malloc((album_info_path_len + SPOTIFY_ID_LEN + 5 + 1) * sizeof(char));
    snprintf((*out), album_info_path_len + SPOTIFY_ID_LEN + 5 + 1, "%s%.22s.json", album_info_path,
             id);
}

void
playlist_info_filepath_id(const char id[SPOTIFY_ID_LEN], char **out) {
    (*out) = malloc((playlist_info_path_len + SPOTIFY_ID_LEN + 5 + 1) * sizeof(char));
    snprintf((*out), playlist_info_path_len + SPOTIFY_ID_LEN + 5 + 1, "%s%.22s.json", playlist_info_path,
             id);
}

size_t
get_saved_playlist_from_dir_count(const char *dir) {
    size_t count = 0;
    DIR *dirp = opendir(dir);
    if (!dirp) {
        fprintf(stderr, "[spotify] Error when opening playlist directory '%s': %s\n", dir,
                strerror(errno));
        return 0;
    }

    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(dirp))) {
        count += (1 - (entry->d_name[0] == '.'));
    }
    if (errno) {
        fprintf(stderr, "[spotify] Error while counting playlists in directory '%s': %s\n", dir,
                strerror(errno));
    }
    closedir(dirp);
    return count;
}

size_t
get_saved_playlist_count() {
    return get_saved_playlist_from_dir_count(playlist_info_path) + get_saved_playlist_from_dir_count(album_info_path);
}

int
get_all_playlists_from_dir(PlaylistInfo *playlistInfo, size_t count, const char *dir) {
    DIR *dirp = opendir(dir);
    if (!dirp) {
        fprintf(stderr, "[spotify] Error when opening playlist directory '%s': %s\n", dir,
                strerror(errno));
        return 1;
    }
    size_t dir_len = strlen(dir);
    char *name = calloc(PLAYLIST_NAME_LEN_NULL + dir_len + 5, sizeof(*name));
    memcpy(name, dir, dir_len);

    errno = 0;
    size_t i = 0;
    struct dirent *entry;
    while ((entry = readdir(dirp)) && i < count) {
        if (entry->d_name[0] == '.')continue;
        memcpy(name + dir_len, entry->d_name, PLAYLIST_NAME_LEN_NULL + 5); // +5 for .json extension

        FILE *fp = fopen(name, "r");
        if (!fp)continue;
        fseek(fp, 0L, SEEK_END);
        size_t len = ftell(fp);
        rewind(fp);
        char buf[len];
        fread(buf, sizeof(*buf), len, fp);
        fclose(fp);
        parse_playlist_info(buf, len, &playlistInfo[i++]);
    }
    free(name);
    if (errno) {
        fprintf(stderr, "[spotify] Error while counting playlists in directory '%s': %s\n", dir,
                strerror(errno));
        return 1;
    }
    closedir(dirp);
    return 0;
}

int
get_all_playlist_info(PlaylistInfo **playlistInfo, size_t *countOut) {
    size_t album_count = get_saved_playlist_from_dir_count(album_info_path);
    size_t playlist_count = get_saved_playlist_from_dir_count(playlist_info_path);

    *playlistInfo = calloc(album_count + playlist_count, sizeof(**playlistInfo));
    *countOut = album_count + playlist_count;

    return get_all_playlists_from_dir(*playlistInfo, album_count, album_info_path) +
           get_all_playlists_from_dir(&(*playlistInfo)[album_count], playlist_count, playlist_info_path);
}

#define ERROR_ENTRY(x) [x]=#x

static const char *err_c[] = {
        ERROR_ENTRY(ET_NO_ERROR),
        ERROR_ENTRY(ET_SPOTIFY),
        ERROR_ENTRY(ET_SPOTIFY_INTERNAL),
        ERROR_ENTRY(ET_HTTP),
        ERROR_ENTRY(ET_FULL),
};

void
spotify_reconnect(struct connection *conn);

void spotify_bufferevent_cb(struct bufferevent *bev, short what, void *ctx) {
    struct connection *conn = (struct connection *) ctx;
    if (what & BEV_EVENT_ERROR || what & BEV_EVENT_EOF) {
        if (conn->busy && conn->retries < 3) {
            fprintf(stderr, "[spotify] Error occurred on connection. Retrying because was busy.\n");
            spotify_reconnect(conn);
            conn->retries++;
            if (bufferevent_write(conn->bev, conn->payload, conn->payload_len) != 0) return;
            bufferevent_setcb(conn->bev, generic_read_cb, NULL, spotify_bufferevent_cb, conn);
        } else {
            if (conn->retries >= 3) {
                fprintf(stderr, "[spotify] Error occurred on connection. Closing because failed after 3 retries.\n");
                if (conn->spotify->err_cb) conn->spotify->err_cb(conn, conn->spotify->err_userp);
                conn->inst->disabled = true;
            } else {
                printf("[spotify] Error occurred on connection. Closing because was idle.\n");
            }
            free_connection(conn);
            bufferevent_free(bev);
            conn->bev = NULL;
        }
    }
}

struct connection *
spotify_connect_with_backend(struct backend_instance *inst, struct spotify_state *spotify) {
    printf("[spotify] Creating spotify connection\n");
    size_t avail = -1;
    for (size_t i = 0; i < spotify->connections_len; ++i) {
        if (spotify->connections[i].inst != inst) continue;
        if (spotify->connections[i].bev && !spotify->connections[i].busy) {
            printf("[spotify] Found existing free connection\n");
            return &spotify->connections[i];
        } else if (!spotify->connections[i].busy && avail == -1) {
            avail = i;
        }
    }
    if (spotify->connections_len >= CONNECTION_POOL_MAX && avail == -1) return NULL;
    printf("[spotify] Creating new connection to %s\n", inst->host);
    if (avail == -1) avail = spotify->connections_len++;
    spotify->connections[avail].inst = inst;
    spotify->connections[avail].spotify = spotify;
    spotify->connections[avail].bev = bufferevent_socket_new(spotify->base, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_enable(spotify->connections[avail].bev, EV_READ | EV_WRITE);
    bufferevent_setcb(spotify->connections[avail].bev, generic_read_cb, NULL, spotify_bufferevent_cb,
                      &spotify->connections[avail]);
    if (bufferevent_socket_connect_hostname(spotify->connections[avail].bev, NULL, AF_UNSPEC, inst->host,
                                            SPOTIFY_PORT) != 0) {
        bufferevent_free(spotify->connections[avail].bev);
        return NULL;
    }
    return &spotify->connections[avail];
}

struct connection *
spotify_connect(struct spotify_state *spotify) {
    printf("[spotify] Creating spotify connection\n");
    size_t avail = -1;
    for (size_t i = 0; i < spotify->connections_len; ++i) {
        if (spotify->connections[i].bev && !spotify->connections[i].busy) {
            printf("[spotify] Found existing free connection\n");
            return &spotify->connections[i];
        } else if (!spotify->connections[i].busy && avail == -1) {
            avail = i;
        }
    }
    if (spotify->connections_len >= CONNECTION_POOL_MAX && avail == -1) return NULL;

    size_t avail_inst = -1;
    for (int i = 0; i < backend_instance_count; ++i) {
        if (!backend_instances[i].disabled) {
            if (avail_inst == -1) avail_inst = i;
            else avail_inst = -2;
        }
    }

    struct backend_instance *inst;

    if (avail_inst == -2) {
        inst = random_backend_instance;
        while (inst->disabled) {
            inst = random_backend_instance;
        }
    } else if (avail_inst == -1) {
        fprintf(stderr,
                "[spotify] All backend instances have been marked as disabled. To retry, restart the program.\n");
        return NULL;
    } else {
        inst = &backend_instances[avail_inst];
    }

    printf("[spotify] Creating new connection to %s\n", inst->host);
    if (avail == -1) avail = spotify->connections_len++;
    spotify->connections[avail].inst = inst;
    spotify->connections[avail].bev = bufferevent_socket_new(spotify->base, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_enable(spotify->connections[avail].bev, EV_READ | EV_WRITE);
    bufferevent_setcb(spotify->connections[avail].bev, generic_read_cb, NULL, spotify_bufferevent_cb,
                      &spotify->connections[avail]);
    if (bufferevent_socket_connect_hostname(spotify->connections[avail].bev, NULL, AF_UNSPEC, inst->host,
                                            SPOTIFY_PORT) != 0) {
        bufferevent_free(spotify->connections[avail].bev);
        return NULL;
    }
    return &spotify->connections[avail];
}

void
spotify_reconnect(struct connection *conn) {
    printf("[spotify] Recreating spotify connection\n");
    struct spotify_state *spotify = conn->spotify;

    bufferevent_setcb(conn->bev, NULL, NULL, NULL, NULL);
    bufferevent_free(conn->bev);

    struct backend_instance *inst = random_backend_instance;
    printf("[spotify] Creating new connection to %s\n", inst->host);
    conn->bev = bufferevent_socket_new(spotify->base, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_enable(conn->bev, EV_READ | EV_WRITE);
    bufferevent_setcb(conn->bev, generic_read_cb, NULL, spotify_bufferevent_cb, conn);
    if (bufferevent_socket_connect_hostname(conn->bev, NULL, AF_UNSPEC, inst->host,
                                            SPOTIFY_PORT) != 0) {
        bufferevent_free(conn->bev);
    }
}

void
generic_read_cb(struct bufferevent *bev, void *arg) {
    struct connection *conn = (struct connection *) arg;
    struct evbuffer *input = bufferevent_get_input(bev);
    if (!conn->expecting) {
        uint8_t *data = evbuffer_pullup(input, 9);
        if (!data) return; // Not enough data yet
        conn->expecting = *((size_t *) &data[1]);
        if (data[0] != ET_NO_ERROR) { // Error occurred
            switch (data[0]) {
                case ET_SPOTIFY:
                case ET_HTTP:
                case ET_SPOTIFY_INTERNAL:
                case ET_NO_ERROR:
                    break;
                default: // Unknown error?
                    fprintf(stderr, "[spotify] Received unknown error code, closing connection (possibly data corruption)\n");
                    conn->error_buffer = NULL;
                    conn->error_type = -1;
                    if (conn->spotify->err_cb) conn->spotify->err_cb(conn, conn->spotify->err_userp);
                    free_connection(conn);
                    conn->busy = false;
                    return;
            }
            conn->error_buffer = calloc(conn->expecting + 1, sizeof(*conn->error_buffer));
            free(conn->cache_path);
            conn->cache_path = NULL; // Not caching errors
        } else {
            conn->error_buffer = NULL;
            if (conn->cache_path) {
                conn->cache_fp = fopen(conn->cache_path, "w");
                fwrite(&conn->expecting, sizeof(conn->expecting), 1, conn->cache_fp);
            } else {
                conn->cache_fp = NULL;
            }
        }
        conn->error_type = data[0];
        printf("[spotify] Receiving data: %s\n", err_c[conn->error_type]);
        evbuffer_drain(input, 9);
    }
    if (conn->error_buffer) {
        conn->progress += evbuffer_remove(input, &conn->error_buffer[conn->progress], conn->expecting - conn->progress);
        if (conn->expecting == conn->progress) {
            fprintf(stderr, "[spotify] Received error from smp backend %s: %s\n", err_c[conn->error_type],
                    conn->error_buffer);
            free(conn->error_buffer);
            conn->error_buffer = NULL;
            if (conn->error_type == ET_SPOTIFY || conn->retries > 3) {
                if (conn->spotify->err_cb) conn->spotify->err_cb(conn, conn->spotify->err_userp);
                free_connection(conn);
                conn->busy = false;
                return; // ET_SPOTIFY means an error with the query (invalid track id, etc.) so reconnecting won't help
            }
            spotify_reconnect(conn);
            conn->retries++;
            conn->expecting = conn->progress = 0;
            conn->error_type = ET_NO_ERROR;
            if (bufferevent_write(conn->bev, conn->payload, conn->payload_len) != 0) return;
            bufferevent_setcb(conn->bev, generic_read_cb, NULL, spotify_bufferevent_cb, conn);
        }
    } else {
        if (conn->cache_fp) {
            uint8_t *data = evbuffer_pullup(input, -1);
            fwrite(data, 1, evbuffer_get_length(input), conn->cache_fp);
        }
        if (conn->cb) conn->cb(bev, conn, conn->cb_arg);
        if (conn->expecting == conn->progress) {
            free_connection(conn);
        }
    }
}

void
generic_proxy_cb(struct bufferevent *bev, struct connection *conn, void *arg) {
    struct evbuffer *input = bufferevent_get_input(bev);
    conn->progress = evbuffer_get_length(input);
    if (conn->progress == conn->expecting) { // All data is in the buffer, now parse all at once
        struct parse_func_params *params = (struct parse_func_params *) arg;
        char buf[conn->progress];
        evbuffer_remove(input, buf, conn->progress);

        if (params->path) {
            FILE *fp = fopen(params->path, "w");
            if (fp) {
                fwrite(buf, 1, conn->progress, fp);
                fclose(fp);
            } else {
                fprintf(stderr, "[spotify] Error when trying to open/create cache file '%s': %s\n", params->path,
                        strerror(errno));
            }
            free(params->path);
            params->path = NULL;
        }

        params->func(buf, conn->progress, params->func_userp);
        printf("[spotify] Parsed JSON info\n");
        if (params->func1) params->func1(conn->spotify, params->func1_userp);
    }
}

int
make_and_parse_generic_request_with_conn(struct connection *conn, char *payload, size_t payload_len,
                                         json_parse_func func, void *userp, info_received_cb func1, void *userp1) {
    if (!conn) return -1;

    conn->params.func_userp = userp;
    conn->params.func = func;
    conn->params.func1_userp = userp1;
    conn->params.func1 = func1;

    conn->progress = 0;
    conn->expecting = 0;
    conn->busy = true;
    if (conn->cache_path) {
        free(conn->cache_path);
        conn->cache_path = NULL;
    }
    conn->cb = generic_proxy_cb;
    conn->cb_arg = &conn->params;

    conn->payload = malloc(payload_len);
    memcpy(conn->payload, payload,
           payload_len); // Payload copied in case of error when the request has to be resent
    conn->payload_len = payload_len;

    if (bufferevent_write(conn->bev, payload, payload_len) != 0) return -1;
    bufferevent_setcb(conn->bev, generic_read_cb, NULL, spotify_bufferevent_cb, conn);
    return 0;
}

int
make_and_parse_generic_request(struct spotify_state *spotify, char *payload, size_t payload_len, char *cache_path,
                               json_parse_func func, void *userp, info_received_cb func1, void *userp1,
                               info_received_cb read_local_cb) {
    //Try to read from file
    if (cache_path) {
        FILE *fp = fopen(cache_path, "r");
        if (!fp) goto remote;
        fseek(fp, 0L, SEEK_END);
        size_t file_len = ftell(fp);
        rewind(fp);
        char file_buf[file_len + 1];
        size_t read = fread(file_buf, 1, file_len, fp);
        fclose(fp);
        if (read != file_len) goto remote;
        file_buf[file_len] = 0;
        int ret;
        if ((ret = func(file_buf, file_len + 1, userp)) != 0) return ret;
        if (read_local_cb) read_local_cb(spotify, userp1);
        else if (func1) func1(spotify, userp1);
        return ret;
    }
    //Make request and download data otherwise
    remote:
    {
        struct connection *conn = spotify_connect(spotify);
        if (!conn) return -1;

        if (cache_path) {
            conn->params.path = calloc(strlen(cache_path) + 1, sizeof(*cache_path));
            strncpy(conn->params.path, cache_path, strlen(cache_path) + 1);
        } else {
            conn->params.path = NULL;
        }
        conn->spotify = spotify;
        return make_and_parse_generic_request_with_conn(conn, payload, payload_len, func, userp, func1, userp1);
    };
}

void
track_data_read_cb(struct bufferevent *bev, struct connection *conn, void *arg) {
    if (!arg) return;
    struct evbuffer *input = bufferevent_get_input(bev);
    if (!decode_vorbis(input, arg, &conn->spotify->decode_ctx, &conn->progress,
                       ctrl_get_audio_info(conn->spotify->smp_ctx), ctrl_get_audio_info_prev(conn->spotify->smp_ctx),
                       (audio_info_cb) audio_start, ctrl_get_audio_context(conn->spotify->smp_ctx))) { // Stream is over
        clean_vorbis_decode(&conn->spotify->decode_ctx);
        printf("[spotify] End of stream\n");
    }
    if (conn->expecting == conn->progress) {
        printf("[spotify] All data received\n");
    }
}

int
read_remote_track(struct spotify_state *spotify, const Track *track, struct buffer *buf,
                  struct connection **conn_out) {
    struct connection *conn;

    if (track->region_count > 0){
        // Choose best instance based on available regions
        struct backend_sort scores[backend_instance_count];
        memset(scores, 0, sizeof(scores));
        for (int i = 0; i < backend_instance_count; ++i) {
            uint32_t incl = 0;
            for (int j = 0; j < backend_instances[i].region_count; ++j) {
                incl += contains_regions(track->regions, track->region_count, &backend_instances[i].regions[j * 2]);
                if (!scores[i].fmatch) scores[i].fmatch = &backend_instances[i].regions[j * 2];
            }
            scores[i].inst = &backend_instances[i];
            scores[i].score = (uint32_t) (((float) incl / (float) backend_instances[i].region_count) * 1000);
        }
        qsort(scores, backend_instance_count, sizeof(*scores), backend_sort_comprar);

        size_t fzero = backend_instance_count;
        for (int i = 0; i < backend_instance_count; ++i) {
            if (scores[i].score == 0) {
                fzero = i;
                break;
            }
        }
        if (fzero == 0) {
            fprintf(stderr,
                    "[spotify] No backends with support for any regions of track with id '%s'. Needed one of following regions: ",
                    track->spotify_id);
            for (int i = 0; i < track->region_count; ++i) {
                if (i == track->region_count - 1)
                    fprintf(stderr, "%.2s\n", &track->regions[i * 2]);
                else
                    fprintf(stderr, "%.2s,", &track->regions[i * 2]);
            }
            return 1;
        }
        struct backend_sort *inst = &scores[((uint32_t) (((float) rand() / (float) RAND_MAX) * (float) (fzero / 2 + 1)))];
        conn = spotify_connect_with_backend(inst->inst, spotify);
        if (!conn) return 1;
        *conn_out = conn;

        conn->payload = malloc(25);
        conn->payload_len = 25;
        if (inst->score != 1000) {
            conn->payload[23] = inst->fmatch[0];
            conn->payload[24] = inst->fmatch[1];
        } else {
            conn->payload[23] = conn->payload[24] = 0;
        }
    }else{
        conn = spotify_connect(spotify);
        if (!conn) return 1;
        *conn_out = conn;

        conn->payload = malloc(25);
        conn->payload_len = 25;
        conn->payload[23] = conn->payload[24] = 0;
    }


    conn->payload[0] = MUSIC_DATA;
    memcpy(&conn->payload[1], track->spotify_id,
           22); // Payload stored for later in case of error if it has to be resent

    conn->progress = 0;
    conn->expecting = 0;
    conn->busy = true;
    conn->spotify = spotify;
    if (buf) {
        conn->cb = track_data_read_cb;
        conn->cb_arg = buf;
    } else {
        conn->cb = NULL;
        conn->cb_arg = NULL;
    }
    if (conn->cache_path) {
        free(conn->cache_path);
        conn->cache_path = NULL;
    }
    track_filepath_id(track->spotify_id, &conn->cache_path);

    if (bufferevent_write(conn->bev, conn->payload, conn->payload_len) != 0) return 1;
    bufferevent_setcb(conn->bev, generic_read_cb, NULL, spotify_bufferevent_cb, conn);
    return 0;
}

int
read_local_track(struct spotify_state *spotify, const char id[SPOTIFY_ID_LEN], struct buffer *buf) {
    char *path = NULL;
    track_filepath_id(id, &path);
    FILE *fp = fopen(path, "r");
    free(path);
    path = NULL;
    if (!fp) return 1;
    fseek(fp, 0L, SEEK_END);
    size_t file_len = ftell(fp);
    rewind(fp);

    struct evbuffer *file_buf = evbuffer_new();
    evbuffer_add_file(file_buf, fileno(fp), 0, -1);

    size_t expected_len = 0;
    evbuffer_remove(file_buf, &expected_len, sizeof(expected_len));
    if (expected_len != file_len - sizeof(expected_len)) goto fail;

    size_t p = 0;
    int ret, fails = 0;
    while ((ret = decode_vorbis(file_buf, buf, &spotify->decode_ctx, &p, ctrl_get_audio_info(spotify->smp_ctx), ctrl_get_audio_info_prev(spotify->smp_ctx),
                                (audio_info_cb) audio_start, ctrl_get_audio_context(spotify->smp_ctx))) != 0) {
        if (ret >= 2) fails += ret - 1;
        if (fails >= 3 || ret == -1) goto fail;
    }
    evbuffer_free(file_buf);
    fclose(fp);
    printf("[spotify] Finished decoding the audio data\n");
    return 0;


    fail:
    fclose(fp);
    evbuffer_free(file_buf);
    clean_vorbis_decode(&spotify->decode_ctx);
    printf("[spotify] Encountered error while reading local file, fetching from remote.\n");
    track_filepath_id(id, &path);
    remove(path);
    free(path);
    path = NULL;
    return 1;
}

int
play_track(struct spotify_state *spotify, const Track *track, struct buffer *buf, struct connection **conn_out) {
    if (!spotify || !track || !buf) return 0;
    clean_vorbis_decode(&spotify->decode_ctx);
    if (read_local_track(spotify, track->spotify_id, buf))
        return read_remote_track(spotify, track, buf, conn_out); // TODO: Handle audio corruption on remote track
    return 0;
}

int
ensure_track(struct spotify_state *spotify, const Track *track, char *region, struct connection **conn_out) {
    if (!spotify || !track) return 0;
    char *path = NULL;
    track_filepath_id(track->spotify_id, &path);
    if (!access(path, R_OK)) {
        free(path);
        return 0;
    }
    free(path);
    return read_remote_track(spotify, track, NULL, conn_out);
}

int
parse_available_regions(const char *data, size_t len, void *userp) {
    struct backend_instance *inst = (struct backend_instance *) userp;
    if (inst->regions) {
        free(inst->regions);
        inst->regions = NULL;
    }
    inst->region_count = len / 2;
    inst->regions = calloc(inst->region_count, 2);
    memcpy(inst->regions, data, len);
    printf("[spotify] Got available regions for '%s': ", inst->host);
    for (int i = 0; i < inst->region_count; ++i) {
        if (i == inst->region_count - 1)
            printf("%.2s\n", &inst->regions[i * 2]);
        else
            printf("%.2s,", &inst->regions[i * 2]);
    }
    return 0;
}

int
parse_track_cjson(cJSON *track_json, Track *track) {
    if (cJSON_IsNull(track_json)) {
        return 1;
    }
    char *id = cJSON_GetStringValue(cJSON_GetObjectItem(track_json, "id"));
    if (cJSON_IsTrue(cJSON_GetObjectItem(track_json, "is_local"))) {
        fprintf(stderr, "[spotify] Local spotify tracks cannot be downloaded. (ID: %s)\n", id);
        return 1;
    }
    if (cJSON_IsFalse(cJSON_GetObjectItem(track_json, "is_playable"))){
        return 1; // TODO: Handle this when the reason is regional
    }

    track->playlist = NULL;
    memcpy(track->spotify_id, id, SPOTIFY_ID_LEN_NULL);
    track->spotify_name = strdup(cJSON_GetStringValue(cJSON_GetObjectItem(track_json, "name")));
    sanitize(&track->spotify_name);
    memcpy(track->spotify_uri, cJSON_GetStringValue(cJSON_GetObjectItem(track_json, "uri")), SPOTIFY_URI_LEN_NULL);

    char *album_cover = cJSON_GetStringValue(
            cJSON_GetObjectItem(
                    cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(track_json, "album"), "images"),
                                       0), "url"));
    track->spotify_album_art = album_cover ? strdup(album_cover) : NULL; // Null when parsing album's tracks
    track->download_state = DS_NOT_DOWNLOADED;
    track->duration_ms = cJSON_GetObjectItem(track_json, "duration_ms")->valueint;

    cJSON *artist = cJSON_GetArrayItem(cJSON_GetObjectItem(track_json, "artists"), 0);
    track->artist = strdup(cJSON_GetStringValue(cJSON_GetObjectItem(artist, "name")));

    memcpy(track->spotify_artist_id, cJSON_GetStringValue(cJSON_GetObjectItem(artist, "id")), SPOTIFY_ID_LEN_NULL);
    sanitize(&track->artist);

    cJSON *markets = cJSON_GetObjectItem(track_json, "available_markets");
    track->region_count = cJSON_GetArraySize(markets);
    track->regions = calloc(track->region_count, 2);

    int i = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, markets) {
        char *s = cJSON_GetStringValue(e);
        track->regions[i * 2] = s[0];
        track->regions[i * 2 + 1] = s[1];
        i++;
    }
    return 0;
}

void
parse_playlist_info_cjson(cJSON *root, PlaylistInfo *playlist) {
    playlist->not_empty = true;
    playlist->album = cJSON_HasObjectItem(root, "album_type");
    playlist->name = strdup(cJSON_GetObjectItem(root, "name")->valuestring);
    memcpy(playlist->spotify_id, cJSON_GetObjectItem(root, "id")->valuestring, 23);
    playlist->image_url = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "images"), 0),
                                                     "url")->valuestring);
    if (playlist->album) {
        playlist->track_count = cJSON_GetObjectItem(root, "total_tracks")->valueint;
    } else {
        if (cJSON_HasObjectItem(cJSON_GetObjectItem(root, "tracks"), "total")) {
            playlist->track_count = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "tracks"), "total")->valueint;
        } else {
            playlist->track_count = cJSON_GetArraySize(
                    cJSON_GetObjectItem(cJSON_GetObjectItem(root, "tracks"), "items"));
        }
    }
}

void
parse_artist_cjson(cJSON *root, Artist *artist) {
    artist->name = strdup(cJSON_GetStringValue(cJSON_GetObjectItem(root, "name")));
    memcpy(artist->spotify_id, cJSON_GetObjectItem(root, "id")->valuestring, 23);
    artist->followers = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "followers"), "total")->valueint;
}

int
parse_track_json(const char *data, size_t len, void *userp) {
    struct json_track_parse_params *params = (struct json_track_parse_params *) userp;
    struct Track **tracks = params->tracks;
    size_t *track_size = params->track_size;
    size_t *track_len = params->track_len;
    free(params);

    cJSON *root = cJSON_ParseWithLength(data, len);

    if (!root) {
        fprintf(stderr, "[spotify] Error when paring track JSON: %s\n", cJSON_GetErrorPtr());
        return 1;
    }

    if (cJSON_HasObjectItem(root, "error")) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        int response_status = cJSON_GetObjectItem(err, "status")->valueint;
        fprintf(stderr, "[spotify] Error occurred when trying to get track info (%d): %s\n", response_status,
                cJSON_GetStringValue(
                        cJSON_GetObjectItem(err, "message")));
        cJSON_Delete(root);
        return 1;
    }

    if (*tracks) {
        if (*track_size < *track_len + 1) {
            Track *tmp = realloc(*tracks, sizeof(*tmp) * (*track_size + 10));
            if (!tmp) perror("[spotify] Error when calling realloc to expand track list");
            *tracks = tmp;
            *track_size += 10;
        }
    } else {
        *tracks = calloc(sizeof(**tracks), 30);
        *track_size = 30;
        *track_len = 0;
    }
    Track *track = &(*tracks)[*track_len];
    *track_len += 1;

    if (parse_track_cjson(root, track)) return 1;

    cJSON_Delete(root);
    return 0;
}

int
parse_playlist_info(const char *data, size_t len, PlaylistInfo *playlist) {
    cJSON *root = cJSON_ParseWithLength(data, len);

    if (!root) {
        fprintf(stderr, "[spotify] Error when parsing JSON: %s\n", cJSON_GetErrorPtr());
        return 1;
    }

    if (cJSON_HasObjectItem(root, "error")) {
        char *error = cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring;
        fprintf(stderr, "[spotify] Error occurred when trying to get album info: %s\n", error);
        cJSON_Delete(root);
        return 1;
    }

    parse_playlist_info_cjson(root, playlist);
    cJSON_Delete(root);
    return 0;
}

int
parse_album_json(const char *data, size_t len, void *userp) {
    struct json_track_parse_params *params = (struct json_track_parse_params *) userp;
    struct Track **tracks = params->tracks;
    size_t *track_size = params->track_size;
    size_t *track_len = params->track_len;
    free(params);
    cJSON *root = cJSON_ParseWithLength(data, len);

    if (!root) {
        fprintf(stderr, "[spotify] Error when parsing JSON: %s\n", cJSON_GetErrorPtr());
        return 1;
    }

    if (cJSON_HasObjectItem(root, "error")) {
        char *error = cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring;
        fprintf(stderr, "[spotify] Error occurred when trying to get album info: %s\n", error);
        cJSON_Delete(root);
        return 1;
    }

    PlaylistInfo *playlist = malloc(sizeof(*playlist));
    playlist->not_empty = true;
    playlist->album = true;
    playlist->name = strdup(cJSON_GetObjectItem(root, "name")->valuestring);
    cJSON *tracks_array = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "tracks"), "items");
    size_t track_array_len = cJSON_GetArraySize(tracks_array);

    if (*track_size - *track_len < track_array_len) {
        Track *tmp = realloc(*tracks, (*track_size + track_array_len) * sizeof(*tmp));
        if (!tmp) perror("[spotify] Error when calling realloc to expand track array");
        *tracks = tmp;
        *track_size += track_array_len;
    }
    size_t i = *track_len;
    memcpy(playlist->spotify_id, cJSON_GetObjectItem(root, "id")->valuestring, 23);
    playlist->image_url = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "images"), 0),
                                                     "url")->valuestring);

    cJSON *track;
    cJSON_ArrayForEach(track, tracks_array) {
        if (parse_track_cjson(track, &(*tracks)[i])) continue;
        (*tracks)[i].spotify_album_art = strdup(playlist->image_url);
        (*tracks)[i].playlist = playlist;
        playlist->reference_count++;
        i++;
    }
    playlist->track_count = i - *track_len;
    *track_len = i;
    cJSON_Delete(root);
    return 0;
}

int
parse_playlist_json(const char *data, size_t len, void *userp) {
    struct json_track_parse_params *params = (struct json_track_parse_params *) userp;
    struct Track **tracks = params->tracks;
    size_t *track_size = params->track_size;
    size_t *track_len = params->track_len;
    free(params);
    cJSON *root = cJSON_ParseWithLength(data, len);

    if (cJSON_HasObjectItem(root, "error")) {
        char *error = cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring;
        fprintf(stderr, "[spotify] Error occurred when trying to get playlist info: %s\n", error);
        cJSON_Delete(root);
        return 1;
    }

    PlaylistInfo *playlist = malloc(sizeof(*playlist));
    playlist->not_empty = true;
    playlist->album = false;
    playlist->name = strdup(cJSON_GetObjectItem(root, "name")->valuestring);
    cJSON *tracks_array = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "tracks"), "items");
    size_t track_array_len = cJSON_GetArraySize(tracks_array);

    if (*track_size - *track_len < track_array_len) {
        Track *tmp = realloc(*tracks, (*track_size + track_array_len) * sizeof(*tmp));
        if (!tmp) perror("[spotify] Error when calling realloc to expand track array");
        *tracks = tmp;
        *track_size += track_array_len;
    }
    size_t i = *track_len;

    playlist->image_url = strdup(
            cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "images"), 0), "url")->valuestring);
    memcpy(playlist->spotify_id, cJSON_GetObjectItem(root, "id")->valuestring, 23);

    cJSON *element;
    cJSON_ArrayForEach(element, tracks_array) {
        if (parse_track_cjson(cJSON_GetObjectItem(element, "track"), &(*tracks)[i])) continue;
        (*tracks)[i].playlist = playlist;
        playlist->reference_count++;
        i++;
    }
    playlist->track_count = i - *track_len;
    *track_len = i;
    cJSON_Delete(root);
    return 0;
}

int
parse_recommendations_json(const char *data, size_t len, void *userp) {
    struct json_track_parse_params *params = (struct json_track_parse_params *) userp;
    struct Track **tracks = params->tracks;
    size_t *track_size = params->track_size;
    size_t *track_len = params->track_len;
    free(params);
    cJSON *root = cJSON_ParseWithLength(data, len);

    if (cJSON_HasObjectItem(root, "error")) {
        fprintf(stderr, "[spotify] Error occurred when trying to get recommendations: %s\n", cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring);
        cJSON_Delete(root);
        return 1;
    }
    cJSON *tracks_array = cJSON_GetObjectItem(root, "tracks");
    size_t track_array_len = cJSON_GetArraySize(tracks_array);

    if (*track_size - *track_len < track_array_len) {
        Track *tmp = realloc(*tracks, (*track_size + track_array_len) * sizeof(*tmp));
        if (!tmp) perror("[spotify] Error when calling realloc to expand track array");
        *tracks = tmp;
        *track_size += track_array_len;
    }

    size_t i = *track_len;

    cJSON *track;
    cJSON_ArrayForEach(track, tracks_array) {
        if (parse_track_cjson(track, &(*tracks)[i])) continue;
        i++;
    }
    *track_len = i;
    cJSON_Delete(root);
    return 0;
}

int
parse_search_json(const char *data, size_t len, void *userp) {
    struct spotify_search_results *params = (struct spotify_search_results *) userp;

    cJSON *root = cJSON_ParseWithLength(data, len);

    if (cJSON_HasObjectItem(root, "error")) {
        fprintf(stderr, "[spotify] Error occurred when trying to search: %s\n", cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring);
        cJSON_Delete(root);
        return 1;
    }
    cJSON *e;
    int i;

    // Track parsing
    if (params->qtracks) {
        cJSON *tracks_obj = cJSON_GetObjectItem(root, "tracks");
        cJSON *tracks_arr = cJSON_GetObjectItem(tracks_obj, "items");
        params->track_len = cJSON_GetArraySize(tracks_arr);
        params->tracks = calloc(params->track_len, sizeof(*params->tracks));

        i = 0;
        cJSON_ArrayForEach(e, tracks_arr) {
            if (parse_track_cjson(e, &params->tracks[i++])) {
                params->track_len--;
                i--;
            }
        }
        if (i != params->track_len) {
            Track *tmp = realloc(params->tracks, i * sizeof(*params->tracks));
            if (!tmp)
                perror("error when rallocing tracks array");
            params->tracks = tmp;
            params->track_len = i;
        }
    }


    // Playlist parsing
    if (params->qplaylists) {
        cJSON *playlist_obj = cJSON_GetObjectItem(root, "playlists");
        cJSON *playlist_arr = cJSON_GetObjectItem(playlist_obj, "items");
        params->playlist_len = cJSON_GetArraySize(playlist_arr);
        params->playlists = calloc(params->playlist_len, sizeof(*params->playlists));

        i = 0;
        cJSON_ArrayForEach(e, playlist_arr) {
            parse_playlist_info_cjson(e, &params->playlists[i++]);
        }
    }

    // Album parsing
    if (params->qalbums) {
        cJSON *album_obj = cJSON_GetObjectItem(root, "albums");
        cJSON *album_arr = cJSON_GetObjectItem(album_obj, "items");
        params->album_len = cJSON_GetArraySize(album_arr);
        params->albums = calloc(params->album_len, sizeof(*params->albums));

        i = 0;
        cJSON_ArrayForEach(e, album_arr) {
            parse_playlist_info_cjson(e, &params->albums[i++]);
        }
    }

    // Artist parsing
    if (params->qartists) {
        cJSON *artist_obj = cJSON_GetObjectItem(root, "artists");
        cJSON *artist_arr = cJSON_GetObjectItem(artist_obj, "items");
        params->artist_len = cJSON_GetArraySize(artist_arr);
        params->artists = calloc(params->artist_len, sizeof(*params->artists));

        i = 0;
        cJSON_ArrayForEach(e, artist_arr) {
            parse_artist_cjson(e, &params->artists[i++]);
        }
    }
    cJSON_Delete(root);

    return 0;
}

int
refresh_available_regions(struct spotify_state *spotify) {
    char req = AVAILABLE_REGIONS;
    for (int i = 0; i < backend_instance_count; ++i) {
        struct backend_instance *inst = &backend_instances[i];
        struct connection *conn = spotify_connect_with_backend(inst, spotify);
        if (!conn) {
            inst->disabled = true;
            fprintf(stderr, "[spotify] Instance '%s' disabled because of failure\n", inst->host);
            continue;
        }
        int ret = make_and_parse_generic_request_with_conn(conn, &req, sizeof(req), parse_available_regions,
                                                           inst, NULL, NULL);
        if (ret) {
            fprintf(stderr, "[spotify] Instance '%s' disabled because of failure\n", inst->host);
            inst->disabled = true;
            continue;
        }
    }
}

int
add_track_info(struct spotify_state *spotify, const char id[22], Track **tracks, size_t *track_size, size_t *track_len,
               info_received_cb func, void *userp) {
    char payload[SPOTIFY_ID_LEN + 1];
    payload[0] = MUSIC_INFO;
    memcpy(&payload[1], id, SPOTIFY_ID_LEN);
    char *path = NULL;
    track_info_filepath_id(id, &path);

    struct json_track_parse_params *userp1 = malloc(sizeof(*userp1));
    userp1->track_size = track_size;
    userp1->tracks = tracks;
    userp1->track_len = track_len;

    int ret = make_and_parse_generic_request(spotify, payload, sizeof(payload), path, parse_track_json, userp1, func,
                                             userp, NULL);
    free(path);
    return ret;
}

int
add_playlist(struct spotify_state *spotify, const char id[22], Track **tracks, size_t *track_size, size_t *track_len,
             bool album, info_received_cb func, void *userp, info_received_cb read_local_cb) {
    char payload[SPOTIFY_ID_LEN + 1];
    memcpy(&payload[1], id, SPOTIFY_ID_LEN);

    struct json_track_parse_params *userp_func = malloc(sizeof(*userp_func));
    userp_func->track_size = track_size;
    userp_func->tracks = tracks;
    userp_func->track_len = track_len;

    char *path = NULL;
    int ret;
    if (album) {
        payload[0] = ALBUM_INFO;
        album_info_filepath_id(id, &path);
        ret = make_and_parse_generic_request(spotify, payload, sizeof(payload), path, parse_album_json, userp_func,
                                             func, userp, read_local_cb);
    } else {
        payload[0] = PLAYLIST_INFO;
        playlist_info_filepath_id(id, &path);
        ret = make_and_parse_generic_request(spotify, payload, sizeof(payload), path, parse_playlist_json, userp_func,
                                             func, userp, read_local_cb);
    }
    free(path);
    return ret;
}

int
add_recommendations(struct spotify_state *spotify, const char *track_ids, const char *artist_ids, size_t track_count,
                    size_t artist_count, Track **tracks, size_t *track_size, size_t *track_len, info_received_cb func,
                    void *userp) {
    char payload[SPOTIFY_ID_LEN * track_count + SPOTIFY_ID_LEN * artist_count + 3];
    payload[0] = RECOMMENDATIONS;
    payload[1] = (char) track_count;
    payload[2] = (char) artist_count;
    memcpy(&payload[3], track_ids, SPOTIFY_ID_LEN * track_count);
    memcpy(&payload[3 + SPOTIFY_ID_LEN * track_count], artist_ids, SPOTIFY_ID_LEN * artist_count);

    struct json_track_parse_params *userp1 = malloc(sizeof(*userp1));
    userp1->track_size = track_size;
    userp1->tracks = tracks;
    userp1->track_len = track_len;

    return make_and_parse_generic_request(spotify, payload, sizeof(payload), NULL, parse_recommendations_json,
                                          userp1, func, userp, NULL);
}

int
add_recommendations_from_tracks(struct spotify_state *spotify, Track **tracks, size_t *track_size, size_t *track_len,
                                info_received_cb func, void *userp) {
    char *seed_tracks; //Last 5 tracks
    char *seed_artists;
    struct ArtistQuantity *artists = calloc(*track_len, sizeof(*artists));
    for (int i = 0; i < *track_len; ++i) {
        for (int j = 0; j < *track_len; ++j) {
            if (strcmp((*tracks)[i].spotify_artist_id, artists[j].id) != 0) continue;
            artists[j].appearances++;
            goto end_loop;
        }
        memcpy(artists[i].id, (*tracks)[i].spotify_artist_id, SPOTIFY_ID_LEN_NULL);
        artists[i].appearances = 1;
        end_loop:;
    }
    qsort(artists, *track_len, sizeof(*artists), compare_artist_quantities);
    size_t artist_count = 0;
    for (int i = 0; i < *track_len; ++i) {
        if (artists[i].appearances)artist_count++;
        if (artist_count >= 3)break; //Don't need more than 3
    }
    seed_artists = calloc(artist_count, SPOTIFY_ID_LEN);
    for (int i = 0; i < artist_count; ++i) {
        memcpy(&seed_artists[i * SPOTIFY_ID_LEN], artists[i].id, SPOTIFY_ID_LEN);
    }
    size_t track_amount = *track_len >= 5 - artist_count ? 5 - artist_count : *track_len;
    seed_tracks = calloc(track_amount, SPOTIFY_ID_LEN);
    for (int i = 0; i < track_amount; ++i) {
        memcpy(&seed_tracks[i * SPOTIFY_ID_LEN], (*tracks)[*track_len - (i + 1)].spotify_id, SPOTIFY_ID_LEN);
    }
    free(artists);
    int ret = add_recommendations(spotify, seed_tracks, seed_artists, track_amount, artist_count, tracks, track_size,
                                  track_len, func, userp);
    free(seed_tracks);
    free(seed_artists);
    return ret;
}

int
search(struct spotify_state *spotify, const char *query, info_received_cb cb, bool tracks, bool artists, bool albums,
       bool playlists, void *userp_in) {
    if (!tracks && !artists && !albums && !playlists) return 0;
    uint16_t q_len = strlen(query);

    uint8_t payload[2 + sizeof(q_len) + q_len];
    payload[0] = SEARCH;
    payload[1] = (tracks * 1) + (artists * 2) + (albums * 4) + (playlists * 8);
    memcpy(&payload[2], &q_len, sizeof(q_len));
    memcpy(&payload[2 + sizeof(q_len)], query, q_len);

    struct spotify_search_results *userp = calloc(1, sizeof(*userp));
    userp->qalbums = albums;
    userp->qartists = artists;
    userp->qplaylists = playlists;
    userp->qtracks = tracks;
    userp->userp = userp_in;

    return make_and_parse_generic_request(spotify, payload, sizeof(payload), NULL, parse_search_json, userp, cb, userp,
                                          NULL);
}

void
cancel_track_transfer(struct connection *conn){
    if (!conn || !conn->busy) return;
    conn->cb = NULL;
    conn->cb_arg = NULL;

    double done_percentage = ((double) conn->progress)/((double) conn->expecting);
    if (done_percentage < 0.75){
        fclose(conn->cache_fp);
        conn->cache_fp = NULL;
        remove(conn->cache_path);
        free_connection(conn);
        bufferevent_free(conn->bev);
        conn->bev = NULL;
        printf("Closing download\n");
    } else{
        printf("Leaving download\n");
    }
}
