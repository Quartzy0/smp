#include "spotify.h"
#include <unistd.h>
#include <curl/curl.h>
#include "util.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../lib/cjson/cJSON.h"
#include <errno.h>
#include <dirent.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include "config.h"
#include "downloader.h"
#include "audio.h"

const char recommendations_base_url[] = "https://api.spotify.com/v1/recommendations?seed_tracks=";
const size_t recommendations_base_url_len = sizeof(recommendations_base_url) - 1;
const char recommendations_seed_artists[] = "&seed_artists=";
const size_t recommendations_seed_artists_len = sizeof(recommendations_seed_artists) - 1;
const char recommendations_seed_genre[] = "&seed_genres=";
const size_t recommendations_seed_genre_len = sizeof(recommendations_seed_genre) - 1;
const char recommendations_limit[] = "&limit=";
const size_t recommendations_limit_len = sizeof(recommendations_limit) - 1;

uint64_t token_expiration = 0;
struct curl_slist *auth_header = NULL;

int
parse_playlist_info(const char *data, size_t len, PlaylistInfo *playlist);

void
get_token() {
    Response response;
    read_url("https://open.spotify.com/", &response, NULL);

    char *jsonStart = strstr(response.data, "<script id=\"config\" data-testid=\"config\" type=\"application/json\">");
    if (!jsonStart) {
        fprintf(stderr, "[spotify] Auth token not found\n");
        free(response.data);
        return;
    }
    jsonStart += 65; // Length of search string
    char *jsonEnd = strstr(jsonStart, "</script>");

    cJSON *root = cJSON_ParseWithLength(jsonStart, jsonEnd - jsonStart);
    if (!root) {
        fprintf(stderr, "[spotify] Error while parsing json\n");
        free(response.data);
        return;
    }
    char *token = cJSON_GetStringValue(cJSON_GetObjectItem(root, "accessToken"));
    if (!token) {
        fprintf(stderr, "[spotify] Auth token not found\n");
        free(response.data);
        cJSON_Delete(root);
        return;
    }
    printf("[spotify] Got token: %s\n", token);
    char token_header[strlen(token) + 22 + 1]; //Authorization: Bearer
    strcpy(token_header, "Authorization: Bearer ");
    strcpy(&token_header[22], token);

    if (auth_header) {
        curl_slist_free_all(auth_header);
        auth_header = NULL;
    }
    auth_header = curl_slist_append(auth_header, token_header);
    token_expiration = cJSON_GetObjectItem(root, "accessTokenExpirationTimestampMs")->valueint;

    cJSON_Delete(root);
    free(response.data);
}

void
ensure_token() {
    if (!token_expiration ||
        token_expiration <= (uint64_t) ((((double) clock()) / ((double) CLOCKS_PER_SEC)) * 1000.0)) {
        get_token();
    }
}

int
search(const char *query_in, Track **tracks, size_t *tracks_count, PlaylistInfo **playlists, size_t *playlist_count,
       PlaylistInfo **albums, size_t *album_count) {
    ensure_token();

    //playlist,album,track
    char type[21];
    char *type_p = type;
    bool prev = false;
    if (tracks) {
        *(type_p++) = 't';
        *(type_p++) = 'r';
        *(type_p++) = 'a';
        *(type_p++) = 'c';
        *(type_p++) = 'k';
        prev = true;
    }
    if (albums) {
        if (prev) {
            *(type_p++) = ',';
        }
        *(type_p++) = 'a';
        *(type_p++) = 'l';
        *(type_p++) = 'b';
        *(type_p++) = 'u';
        *(type_p++) = 'm';
        prev = true;
    }
    if (playlists) {
        if (prev) {
            *(type_p++) = ',';
        }
        *(type_p++) = 'p';
        *(type_p++) = 'l';
        *(type_p++) = 'a';
        *(type_p++) = 'y';
        *(type_p++) = 'l';
        *(type_p++) = 'i';
        *(type_p++) = 's';
        *(type_p++) = 't';
        prev = true;
    }
    *type_p = 0;

    char *query = urlencode(query_in);
    char *url = malloc((42 + strlen(type) + strlen(query) + 1) * sizeof(*url));
    snprintf(url, 42 + strlen(type) + strlen(query) + 1, "https://api.spotify.com/v1/search?type=%s&q=%s", type, query);

    redo:;

    Response response;
    int status = read_url(url, &response, auth_header);
    curl_free(query);
    if (!status) {
        fprintf(stderr, "[spotify] Error when performing request to url %s\n", url);
        free(url);
        return 1;
    }

    cJSON *root = cJSON_ParseWithLength(response.data, response.size);

    if (cJSON_HasObjectItem(root, "error")) {
        char *error = cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring;
        if (!strcmp(error, "The access token expired")) {
            get_token();
            cJSON_Delete(root);
            goto redo;
        }
        fprintf(stderr, "[spotify] Error occurred when trying to get album info: %s\n", error);
        cJSON_Delete(root);
        free(url);
        return 1;
    }
    free(url);

    if (tracks) {
        cJSON *tracks_results = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "tracks"), "items");
        *tracks_count = cJSON_GetArraySize(tracks_results);
        if (!*tracks_count) {
            goto albums;
        }
        *tracks = calloc(*tracks_count, sizeof(**tracks));

        cJSON *element;
        size_t i = 0;
        cJSON_ArrayForEach(element, tracks_results) {
            memcpy((*tracks)[i].spotify_id, cJSON_GetObjectItem(element, "id")->valuestring, SPOTIFY_ID_LEN_NULL);
            memcpy((*tracks)[i].spotify_uri, cJSON_GetObjectItem(element, "uri")->valuestring, SPOTIFY_URI_LEN_NULL);
            (*tracks)[i].spotify_name = strdup(cJSON_GetObjectItem(element, "name")->valuestring);
            sanitize(&(*tracks)[i].spotify_name);
            (*tracks)[i].spotify_name_escaped = urlencode((*tracks)[i].spotify_name);
            cJSON *artist = cJSON_GetArrayItem(cJSON_GetObjectItem(element, "artists"), 0);
            memcpy((*tracks)[i].spotify_artist_id, cJSON_GetObjectItem(artist, "id")->valuestring, SPOTIFY_ID_LEN_NULL);
            (*tracks)[i].artist = strdup(cJSON_GetObjectItem(artist, "name")->valuestring);
            sanitize(&(*tracks)[i].artist);
            (*tracks)[i].artist_escaped = urlencode((*tracks)[i].artist);
            (*tracks)[i].spotify_album_art = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(
                    cJSON_GetObjectItem(element, "album"), "images"), 0), "url")->valuestring);
            (*tracks)[i].duration_ms = cJSON_GetObjectItem(element, "duration_ms")->valueint;
            (*tracks)[i].download_state = DS_NOT_DOWNLOADED;
            i++;
        }
    }
    albums:;
    if (albums) {
        cJSON *album_results = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "albums"), "items");
        *album_count = cJSON_GetArraySize(album_results);
        if (!*album_count) {
            goto playlists;
        }
        *albums = calloc(*album_count, sizeof(**albums));

        cJSON *element;
        size_t i = 0;
        cJSON_ArrayForEach(element, album_results) {
            (*albums)[i].album = true;
            (*albums)[i].not_empty = true;
            (*albums)[i].track_count = cJSON_GetObjectItem(element, "total_tracks")->valueint;
            (*albums)[i].last_played = 0;
            (*albums)[i].name = strdup(cJSON_GetObjectItem(element, "name")->valuestring);
            memcpy((*albums)[i].spotify_id, cJSON_GetObjectItem(element, "id")->valuestring, SPOTIFY_ID_LEN_NULL);
            (*albums)[i].image_url = strdup(
                    cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(element, "images"), 0),
                                        "url")->valuestring);
            i++;
        }
    }
    playlists:;
    if (playlists) {
        cJSON *playlist_results = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "playlists"), "items");
        *playlist_count = cJSON_GetArraySize(playlist_results);
        if (!*playlist_count) {
            goto end;
        }
        *playlists = calloc(*playlist_count, sizeof(**playlists));

        cJSON *element;
        size_t i = 0;
        cJSON_ArrayForEach(element, playlist_results) {
            (*playlists)[i].album = false;
            (*playlists)[i].not_empty = true;
            (*playlists)[i].track_count = cJSON_GetObjectItem(cJSON_GetObjectItem(element, "tracks"),
                                                              "total")->valueint;
            (*playlists)[i].last_played = 0;
            (*playlists)[i].name = strdup(cJSON_GetObjectItem(element, "name")->valuestring);
            memcpy((*playlists)[i].spotify_id, cJSON_GetObjectItem(element, "id")->valuestring, SPOTIFY_ID_LEN_NULL);
            (*playlists)[i].image_url = strdup(
                    cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(element, "images"), 0),
                                        "url")->valuestring);
            i++;
        }
    }

    end:
    cJSON_Delete(root);
    free(response.data);
    return 0;
}

void
free_tracks(Track *track, size_t count) {
    if (!track || !count) return;
    for (int i = 0; i < count; ++i) {
        free(track[i].spotify_name);
        track[i].spotify_name = NULL;
        free(track[i].spotify_album_art);
        track[i].spotify_album_art = NULL;
        curl_free(track[i].spotify_name_escaped); //Escaped names were generated using curl and must be freed using curl
        track[i].spotify_name_escaped = NULL;
        free(track[i].artist);
        track[i].artist = NULL;
        curl_free(track[i].artist_escaped);
        track[i].artist_escaped = NULL;
    }
}

void
free_track(Track *track) {
    free(track->spotify_name);
    free(track->spotify_album_art);
    curl_free(track->spotify_name_escaped); //Escaped names were generated using curl and must be freed using curl
    free(track->artist);
    curl_free(track->artist_escaped);
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
    if (!tracks || !track_len){
        *track_len = 0;
        return;
    }
    *track_len = 0;
    for (size_t i = 0; i < *track_len; ++i) free_track(&tracks[i]);
}

void
free_playlist(PlaylistInfo *playlist) {
    if (!playlist->not_empty)return;
    free(playlist->name);
    free(playlist->image_url);
    memset(playlist, 0, sizeof(*playlist));
}

void
download_track(Track *track, bool block, FILE **pcm) {
    if (track->download_state == DS_DOWNLOAD_FAILED) return;
    if (track->download_state != DS_NOT_DOWNLOADED) {
        printf("[spotify] Track '%s' already downloaded or is downloading\n", track->spotify_name);
        return;
    }
    char *track_file = NULL;
    track_filepath(track, &track_file);
    if (!access(track_file, R_OK)) {
        track->download_state = DS_DOWNLOADED;
        printf("[spotify] Track '%s' already downloaded\n", track->spotify_name);
        free(track_file);
        return; // Track was already downloaded
    }
    free(track_file);

    track->download_state = DS_DOWNLOADING;

    printf("[spotify] Track '%s' by '%s' needs to be downloaded. Searching...\n", track->spotify_name,
           track->artist);

    DownloadParams *params = malloc(sizeof(*params));
    params->track = track;
    params->path_out = track_save_path;

    if (block) {
        *pcm = search_and_get_pcm(params);
    } else {
        pthread_t t;
        pthread_create(&t, NULL, search_and_download, params);
        pthread_detach(t);
    }
}

void
cleanup() {
    curl_slist_free_all(auth_header);
}

void
track_filepath(Track *track, char **out) {
    (*out) = malloc((track_save_path_len + SPOTIFY_ID_LEN + 4 + 1) * sizeof(char));
    snprintf((*out), track_save_path_len + SPOTIFY_ID_LEN + 4 + 1, "%s%s.ogg", track_save_path,
             track->spotify_id);
    out[track_save_path_len + SPOTIFY_ID_LEN + 4] = 0;
}

void
track_filepath_id(const char id[SPOTIFY_ID_LEN], char **out) {
    (*out) = malloc((track_save_path_len + SPOTIFY_ID_LEN + 4 + 1) * sizeof(char));
    snprintf((*out), track_save_path_len + SPOTIFY_ID_LEN + 4 + 1, "%s%s.ogg", track_save_path,
             id);
    out[track_save_path_len + SPOTIFY_ID_LEN + 4] = 0;
}

void
track_info_filepath_id(const char id[SPOTIFY_ID_LEN], char **out) {
    (*out) = malloc((track_info_path_len + SPOTIFY_ID_LEN + 5 + 1) * sizeof(char));
    snprintf((*out), track_info_path_len + SPOTIFY_ID_LEN + 5 + 1, "%s%s.json", track_info_path,
             id);
    out[track_info_path_len + SPOTIFY_ID_LEN + 5] = 0;
}

void
album_info_filepath_id(const char id[SPOTIFY_ID_LEN], char **out) {
    (*out) = malloc((album_info_path_len + SPOTIFY_ID_LEN + 5 + 1) * sizeof(char));
    snprintf((*out), album_info_path_len + SPOTIFY_ID_LEN + 5 + 1, "%s%s.json", album_info_path,
             id);
    out[album_info_path_len + SPOTIFY_ID_LEN + 5] = 0;
}

void
playlist_info_filepath_id(const char id[SPOTIFY_ID_LEN], char **out) {
    (*out) = malloc((playlist_info_path_len + SPOTIFY_ID_LEN + 5 + 1) * sizeof(char));
    snprintf((*out), playlist_info_path_len + SPOTIFY_ID_LEN + 5 + 1, "%s%s.json", playlist_info_path,
             id);
    out[playlist_info_path_len + SPOTIFY_ID_LEN + 5] = 0;
}

void
playlist_filepath(char *id, char **out, bool album) {
    (*out) = malloc(strlen(id) * sizeof(char) + playlist_save_path_len + 2);
    memcpy(*out, playlist_save_path, playlist_save_path_len);
    memcpy(*out + playlist_save_path_len, id, strlen(id));
    (*out)[strlen(id) + playlist_save_path_len] = album ? 'a' : 'p';
    (*out)[strlen(id) + playlist_save_path_len + 1] = 0;
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
    char *name = malloc(PLAYLIST_NAME_LEN_NULL + dir_len + 5);
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
        ERROR_ENTRY(ET_HTTP),
        ERROR_ENTRY(ET_FULL),
};

struct connection *
spotify_connect(struct spotify_state *spotify) {
    printf("[spotify] Creating spotify connection\n");
    size_t i;
    for (i = 0; i < spotify->connections_len; ++i) {
        if (spotify->connections[i].bev && !spotify->connections[i].busy){
            printf("[spotify] Found existing free connection\n");
            return &spotify->connections[i];
        }
    }
    if (spotify->connections_len >= CONNECTION_POOL_MAX) return NULL;
    size_t inst = (size_t) (((float) rand() / (float) RAND_MAX) * (float) (spotify->instances_len - 1));
    printf("[spotify] Creating new connection to %s\n", spotify->instances[inst]);
    spotify->connections_len++;
    spotify->connections[i].bev = bufferevent_socket_new(spotify->base, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_enable(spotify->connections[i].bev, EV_READ | EV_WRITE);
    if (bufferevent_socket_connect_hostname(spotify->connections[i].bev, NULL, AF_UNSPEC, spotify->instances[inst],
                                            SPOTIFY_PORT) != 0) {
        bufferevent_free(spotify->connections[i].bev);
        return NULL;
    }
    return &spotify->connections[i];
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
            conn->error_buffer = calloc(conn->expecting + 1, sizeof(*conn->error_buffer));
            free(conn->cache_path);
            conn->cache_path = NULL; // Not caching errors
        } else {
            conn->error_buffer = NULL;
            if (conn->cache_path) conn->cache_fp = fopen(conn->cache_path, "w");
            else conn->cache_fp = NULL;
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
            conn->busy = false;
        }
    } else {
        if (conn->cache_fp) {
            uint8_t *data = evbuffer_pullup(input, -1);
            fwrite(data, 1, evbuffer_get_length(input), conn->cache_fp);
        }
        if (conn->cb) conn->cb(bev, conn, conn->cb_arg);
        if (conn->expecting == conn->progress) {
            conn->busy = false;
            if (conn->cache_fp) fclose(conn->cache_fp);
            free(conn->cache_path);
            conn->cache_path = NULL;
            conn->cache_fp = NULL;
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

        if (params->path){
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

        params->func(buf, conn->progress, params->tracks, params->track_size, params->track_len);
        if (params->func1) params->func1(conn->spotify, *(params->tracks));
        printf("[spotify] Parsed JSON info\n");
        conn->busy = false;
    }
}

int
make_and_parse_generic_request(struct spotify_state *spotify, char *payload, size_t payload_len, char *cache_path,
                               json_parse_func func, info_received_cb func1, Track **tracks, size_t *track_size,
                               size_t *track_len) {
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
        if ((ret = func(file_buf, file_len + 1, tracks, track_size, track_len)) != 0) return ret;
        if (func1) func1(spotify, *tracks);
        return ret;
    }
    //Make request and download data otherwise
    remote:
    {
        struct connection *conn = spotify_connect(spotify);
        if (!conn) return -1;

        if (cache_path){
            conn->params.path = calloc(strlen(cache_path) + 1, sizeof(*cache_path));
            strncpy(conn->params.path, cache_path, strlen(cache_path) + 1);
        }else {
            conn->params.path = NULL;
        }
        conn->params.track_size = track_size;
        conn->params.track_len = track_len;
        conn->params.tracks = tracks;
        conn->params.func = func;
        conn->params.func1 = func1;

        conn->progress = 0;
        conn->expecting = 0;
        conn->busy = true;
        conn->spotify = spotify;
        conn->cache_path = NULL;
        conn->cb = generic_proxy_cb;
        conn->cb_arg = &conn->params;

        if (bufferevent_write(conn->bev, payload, payload_len) != 0) return -1;
        bufferevent_setcb(conn->bev, generic_read_cb, NULL, NULL, conn);
    };
    return 0;
}

void
track_data_read_cb(struct bufferevent *bev, struct connection *conn, void *arg) {
    if (!arg) return;
    struct evbuffer *input = bufferevent_get_input(bev);
    if (!decode_vorbis(input, arg, &conn->spotify->decode_ctx, &conn->progress,
                       &conn->spotify->smp_ctx->audio_info, &conn->spotify->smp_ctx->previous,
                       (audio_info_cb) start)) { // Stream is over
        conn->busy = false;
        memset(&conn->spotify->decode_ctx, 0, sizeof(conn->spotify->decode_ctx));
        printf("[spotify] End of stream\n");
    }
    if (conn->expecting == conn->progress) {
        printf("[spotify] All data received\n");
    }
}

int
read_remote_track(struct spotify_state *spotify, const char id[SPOTIFY_ID_LEN], struct evbuffer *buf) {
    struct connection *conn = spotify_connect(spotify);
    if (!conn) return -1;

    char data[23];
    data[0] = MUSIC_DATA;
    memcpy(&data[1], id, sizeof(data) - 1);
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
    if (!conn->cache_path) {
        free(conn->cache_path);
        conn->cache_path = NULL;
    }
    track_filepath_id(id, &conn->cache_path);

    if (bufferevent_write(conn->bev, data, sizeof(data)) != 0) return -1;
    bufferevent_setcb(conn->bev, generic_read_cb, NULL, NULL, conn);
    return 0;
}

int
read_local_track(struct spotify_state *spotify, const char id[SPOTIFY_ID_LEN], struct evbuffer *buf) {
    char *path = NULL;
    track_filepath_id(id, &path);
    FILE *fp = fopen(path, "r");
    free(path);
    if (!fp) return 1;
    struct evbuffer *file_buf = evbuffer_new();
    evbuffer_add_file(file_buf, fileno(fp), 0, -1);
    size_t p = 0;
    while (decode_vorbis(file_buf, buf, &spotify->decode_ctx, &p, &spotify->smp_ctx->audio_info,
                         &spotify->smp_ctx->previous,
                         (audio_info_cb) start)) {}
    evbuffer_free(file_buf);
    return 0;
}

int
play_track(struct spotify_state *spotify, const char id[SPOTIFY_ID_LEN], struct evbuffer *buf) {
    if (!spotify || !id || !buf) return 1;
    memset(&spotify->decode_ctx, 0, sizeof(spotify->decode_ctx));
    if (read_local_track(spotify, id, buf))
        return read_remote_track(spotify, id, buf);
    return 0;
}

int
ensure_track(struct spotify_state *spotify, const char id[SPOTIFY_ID_LEN]) {
    if (!spotify || !id) return 1;
    char *path = NULL;
    track_filepath_id(id, &path);
    if (!access(path, R_OK)) {
        free(path);
        return 0;
    }
    free(path);
    return read_remote_track(spotify, id, NULL);
}

int
parse_track_json(const char *data, size_t len, Track **tracks, size_t *track_size, size_t *track_len) {
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
    char *id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "is_local"))) {
        fprintf(stderr, "[spotify] Local spotify tracks cannot be downloaded. (ID: %s)\n", id);
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

    track->playlist = NULL;
    memcpy(track->spotify_id, id, SPOTIFY_ID_LEN_NULL);
    track->spotify_name = strdup(cJSON_GetStringValue(cJSON_GetObjectItem(root, "name")));
    sanitize(&track->spotify_name);
    track->spotify_name_escaped = urlencode(track->spotify_name);
    memcpy(track->spotify_uri, cJSON_GetStringValue(cJSON_GetObjectItem(root, "uri")), SPOTIFY_URI_LEN_NULL);
    track->spotify_album_art = strdup(cJSON_GetStringValue(
            cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "album"), "images"),
                                                   0), "url")));
    track->download_state = DS_NOT_DOWNLOADED;
    track->duration_ms = cJSON_GetObjectItem(root, "duration_ms")->valueint;

    cJSON *artist = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "artists"), 0);
    track->artist = strdup(cJSON_GetStringValue(cJSON_GetObjectItem(artist, "name")));

    memcpy(track->spotify_artist_id, cJSON_GetStringValue(cJSON_GetObjectItem(artist, "id")), SPOTIFY_ID_LEN_NULL);
    sanitize(&track->artist);
    track->artist_escaped = urlencode(track->artist);

    cJSON_Delete(root);
    return 0;
}

int
add_track_info(struct spotify_state *spotify, const char id[SPOTIFY_ID_LEN], Track **tracks, size_t *track_size,
               size_t *track_len,
               info_received_cb func) {
    char payload[SPOTIFY_ID_LEN + 1];
    payload[0] = MUSIC_INFO;
    memcpy(&payload[1], id, SPOTIFY_ID_LEN);
    char *path = NULL;
    track_info_filepath_id(id, &path);
    make_and_parse_generic_request(spotify, payload, sizeof(payload), path, parse_track_json, func, tracks, track_size,
                                   track_len);
    free(path);
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

    playlist->not_empty = true;
    playlist->album = cJSON_HasObjectItem(root, "album_type");
    playlist->name = strdup(cJSON_GetObjectItem(root, "name")->valuestring);
    memcpy(playlist->spotify_id, cJSON_GetObjectItem(root, "id")->valuestring, 23);
    playlist->image_url = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "images"), 0),
                                                     "url")->valuestring);
    if (playlist->album) {
        playlist->track_count = cJSON_GetObjectItem(root, "total_tracks")->valueint;
    } else {
        playlist->track_count = cJSON_GetArraySize(cJSON_GetObjectItem(root, "tracks"));
    }
    cJSON_Delete(root);
    return 0;
}

int
parse_album_json(const char *data, size_t len, Track **tracks, size_t *track_size, size_t *track_len) {
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
    *track_len += track_array_len;
    memcpy(playlist->spotify_id, cJSON_GetObjectItem(root, "id")->valuestring, 23);
    playlist->image_url = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "images"), 0),
                                                     "url")->valuestring);

    cJSON *track;
    cJSON_ArrayForEach(track, tracks_array) {
        memcpy((*tracks)[i].spotify_id, cJSON_GetObjectItem(track, "id")->valuestring, 23);
        (*tracks)[i].spotify_name = strdup(cJSON_GetObjectItem(track, "name")->valuestring);
        sanitize(&(*tracks)[i].spotify_name);
        (*tracks)[i].spotify_name_escaped = urlencode((*tracks)[i].spotify_name);
        memcpy((*tracks)[i].spotify_uri, cJSON_GetObjectItem(track, "uri")->valuestring, 37);
        (*tracks)[i].artist = strdup(
                cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "name")->valuestring);
        sanitize(&(*tracks)[i].artist);
        (*tracks)[i].artist_escaped = urlencode((*tracks)[i].artist);
        memcpy((*tracks)[i].spotify_artist_id,
               cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "id")->valuestring,
               SPOTIFY_ID_LEN_NULL);
        (*tracks)[i].spotify_album_art = strdup(playlist->image_url);
        (*tracks)[i].download_state = DS_NOT_DOWNLOADED;
        (*tracks)[i].duration_ms = cJSON_GetObjectItem(track, "duration_ms")->valueint;
        (*tracks)[i].playlist = playlist;
        playlist->reference_count++;
        i++;
    }
    playlist->track_count = cJSON_GetArraySize(tracks_array);
    cJSON_Delete(root);
    return 0;
}

int
parse_playlist_json(const char *data, size_t len, Track **tracks, size_t *track_size,
                    size_t *track_len) {
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
    *track_len += track_array_len;

    playlist->image_url = strdup(
            cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "images"), 0), "url")->valuestring);
    memcpy(playlist->spotify_id, cJSON_GetObjectItem(root, "id")->valuestring, 23);

    cJSON *element;
    cJSON_ArrayForEach(element, tracks_array) {
        cJSON *track = cJSON_GetObjectItem(element, "track");
        if (cJSON_IsNull(track) || cJSON_IsTrue(cJSON_GetObjectItem(track, "is_local"))) {
            continue;
        }
        memcpy((*tracks)[i].spotify_id, cJSON_GetObjectItem(track, "id")->valuestring, 23);
        (*tracks)[i].spotify_name = strdup(cJSON_GetObjectItem(track, "name")->valuestring);
        sanitize(&(*tracks)[i].spotify_name);
        (*tracks)[i].spotify_name_escaped = urlencode((*tracks)[i].spotify_name);
        memcpy((*tracks)[i].spotify_uri, cJSON_GetObjectItem(track, "uri")->valuestring, 37);
        (*tracks)[i].artist = strdup(
                cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "name")->valuestring);
        sanitize(&(*tracks)[i].artist);
        (*tracks)[i].artist_escaped = urlencode((*tracks)[i].artist);
        memcpy((*tracks)[i].spotify_artist_id,
               cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "id")->valuestring,
               SPOTIFY_ID_LEN_NULL);
        (*tracks)[i].spotify_album_art = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(
                cJSON_GetObjectItem(track, "album"), "images"), 0), "url")->valuestring);
        (*tracks)[i].download_state = DS_NOT_DOWNLOADED;
        (*tracks)[i].duration_ms = cJSON_GetObjectItem(track, "duration_ms")->valueint;
        (*tracks)[i].playlist = playlist;
        playlist->reference_count++;
        i++;
    }
    playlist->track_count = track_array_len;
    cJSON_Delete(root);
    return 0;
}

int
parse_recommendations_json(const char *data, size_t len, Track **tracks, size_t *track_size,
                           size_t *track_len) {
    cJSON *root = cJSON_ParseWithLength(data, len);

    if (cJSON_HasObjectItem(root, "error")) {
        char *error = cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring;
        if (!strcmp(error, "The access token expired")) {
            get_token();
            cJSON_Delete(root);
            return 1;
        }
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
    *track_len += track_array_len;

    cJSON *track;
    cJSON_ArrayForEach(track, tracks_array) {
        if (cJSON_IsNull(track)) {
            continue;
        }
        memcpy((*tracks)[i].spotify_id, cJSON_GetObjectItem(track, "id")->valuestring, 23);
        (*tracks)[i].spotify_name = strdup(cJSON_GetObjectItem(track, "name")->valuestring);
        sanitize(&(*tracks)[i].spotify_name);
        (*tracks)[i].spotify_name_escaped = urlencode((*tracks)[i].spotify_name);
        memcpy((*tracks)[i].spotify_uri, cJSON_GetObjectItem(track, "uri")->valuestring, 37);
        (*tracks)[i].artist = strdup(
                cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "name")->valuestring);
        sanitize(&(*tracks)[i].artist);
        memcpy((*tracks)[i].spotify_artist_id,
               cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "id")->valuestring,
               SPOTIFY_ID_LEN_NULL);
        (*tracks)[i].artist_escaped = urlencode((*tracks)[i].artist);
        (*tracks)[i].spotify_album_art = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(
                cJSON_GetObjectItem(track, "album"), "images"), 0), "url")->valuestring);
        (*tracks)[i].download_state = DS_NOT_DOWNLOADED;
        (*tracks)[i].duration_ms = cJSON_GetObjectItem(track, "duration_ms")->valueint;
        (*tracks)[i].playlist = NULL;
        i++;
    }
    cJSON_Delete(root);
    return 0;
}

int
add_playlist(struct spotify_state *spotify, const char id[SPOTIFY_ID_LEN], Track **tracks, size_t *track_size,
             size_t *track_len,
             bool album, info_received_cb func) {
    char payload[SPOTIFY_ID_LEN + 1];
    memcpy(&payload[1], id, SPOTIFY_ID_LEN);
    char *path = NULL;
    int ret;
    if (album) {
        payload[0] = ALBUM_INFO;
        album_info_filepath_id(id, &path);
        ret = make_and_parse_generic_request(spotify, payload, sizeof(payload), path, parse_album_json, func, tracks,
                                             track_size, track_len);
    } else {
        payload[0] = PLAYLIST_INFO;
        playlist_info_filepath_id(id, &path);
        ret = make_and_parse_generic_request(spotify, payload, sizeof(payload), path, parse_playlist_json, func, tracks,
                                             track_size, track_len);
    }
    free(path);
    return ret;
}

int
add_recommendations(struct spotify_state *spotify, const char *track_ids,
                    const char *artist_ids, size_t track_count, size_t artist_count, Track **tracks,
                    size_t *track_size, size_t *track_len, info_received_cb func) {
    char payload[SPOTIFY_ID_LEN * track_count + SPOTIFY_ID_LEN * artist_count + 3];
    payload[0] = RECOMMENDATIONS;
    payload[1] = (char) track_count;
    payload[2] = (char) artist_count;
    memcpy(&payload[3], track_ids, SPOTIFY_ID_LEN * track_count);
    memcpy(&payload[3 + SPOTIFY_ID_LEN * track_count], artist_ids, SPOTIFY_ID_LEN * artist_count);
    return make_and_parse_generic_request(spotify, payload, sizeof(payload), NULL, parse_recommendations_json, func,
                                          tracks, track_size, track_len);
}

int
add_recommendations_from_tracks(struct spotify_state *spotify, Track **tracks,
                                size_t *track_size, size_t *track_len, info_received_cb func) {
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
                                  track_len, func);
    free(seed_tracks);
    free(seed_artists);
    return ret;
}
