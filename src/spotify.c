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
#include "config.h"
#include "downloader.h"

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

    if (auth_header){
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

int
track_by_id(const char *id, Track **track) {
    ensure_token();
    int attempts = 3;
    retry_req:;

    char *url = malloc((34 + SPOTIFY_ID_LEN) * sizeof(*id) + 1);
    snprintf(url, (34 + SPOTIFY_ID_LEN) * sizeof(*id) + 1, "https://api.spotify.com/v1/tracks/%s", id);

    Response response;
    read_url(url, &response, auth_header);
    free(url);

    if (!response.size) {
        fprintf(stderr, "[spotify] Got empty response when trying to get playlist info\n");
        free(response.data);
        return 1;
    }

    cJSON *root = cJSON_ParseWithLength(response.data, response.size);

    if (cJSON_HasObjectItem(root, "error")) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        int status = cJSON_GetObjectItem(err, "status")->valueint;
        free(response.data);
        if (status == 401 && attempts-- != 0) {
            get_token();
            goto retry_req;
        }
        fprintf(stderr, "[spotify] Error occurred when trying to get track info: %s\n", cJSON_GetStringValue(
                cJSON_GetObjectItem(err, "message")));
        cJSON_Delete(root);
        return 1;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "is_local"))) {
        fprintf(stderr, "[spotify] Local tracks cannot be downloaded. (ID: %s)\n", id);
        cJSON_Delete(root);
        free(response.data);
        return 1;
    }

    (*track) = malloc(sizeof(**track));
    memcpy((*track)->spotify_id, id, SPOTIFY_ID_LEN_NULL);
    (*track)->spotify_name = strdup(cJSON_GetStringValue(cJSON_GetObjectItem(root, "name")));
    sanitize(&(*track)->spotify_name);
    (*track)->spotify_name_escaped = urlencode((*track)->spotify_name);
    memcpy((*track)->spotify_uri, cJSON_GetStringValue(cJSON_GetObjectItem(root, "uri")), SPOTIFY_URI_LEN_NULL);
    (*track)->spotify_album_art = strdup(cJSON_GetStringValue(
            cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "album"), "images"),
                                                   0), "url")));
    (*track)->download_state = DS_NOT_DOWNLOADED;
    (*track)->duration_ms = cJSON_GetObjectItem(root, "duration_ms")->valueint;

    cJSON *artist = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "artists"), 0);
    (*track)->artist = strdup(cJSON_GetStringValue(cJSON_GetObjectItem(artist, "name")));

    memcpy((*track)->spotify_artist_id, cJSON_GetStringValue(cJSON_GetObjectItem(artist, "id")), SPOTIFY_ID_LEN_NULL);
    sanitize(&(*track)->artist);
    (*track)->artist_escaped = urlencode((*track)->artist);

    cJSON_Delete(root);
    free(response.data);
    return 0;
}

int
get_album(char *albumId, PlaylistInfo *playlistOut, Track **tracksOut) {
    ensure_token();
    char *file = NULL;
    playlist_filepath(albumId, &file, true);
    if (!access(file, R_OK)) {
        printf("[spotify] Album info found on disk. Reading...\n");
        read_playlist_from_file(file, playlistOut, tracksOut);
        free(file);
        playlistOut->album = true;
        printf("[spotify] Done reading album from disk\n");
        return 0;
    }

    printf("[spotify] Getting info for album %s\n", albumId);
    redo:;

    char *url = malloc((34 + strlen(albumId) + 1) * sizeof(char));
    snprintf(url, 34 + strlen(albumId) + 1,
             "https://api.spotify.com/v1/albums/%s",
             albumId);

    Response response;
    read_url(url, &response, auth_header);
    free(url);

    if (!response.size) {
        fprintf(stderr, "[spotify] Got empty response when trying to get album info\n");
        free(file);
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
        return 1;
    }

    playlistOut->not_empty = true;
    playlistOut->album = true;
    playlistOut->name = strdup(cJSON_GetObjectItem(root, "name")->valuestring);
    cJSON *tracks_array = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "tracks"), "items");
    (*tracksOut) = malloc(sizeof(Track) * cJSON_GetArraySize(tracks_array));
    memcpy(playlistOut->spotify_id, cJSON_GetObjectItem(root, "id")->valuestring, 23);
    playlistOut->image_url = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "images"), 0),
                                                        "url")->valuestring);

    cJSON *track;
    size_t i = 0;
    cJSON_ArrayForEach(track, tracks_array) {
        memcpy((*tracksOut)[i].spotify_id, cJSON_GetObjectItem(track, "id")->valuestring, 23);
        (*tracksOut)[i].spotify_name = strdup(cJSON_GetObjectItem(track, "name")->valuestring);
        sanitize(&(*tracksOut)[i].spotify_name);
        (*tracksOut)[i].spotify_name_escaped = urlencode((*tracksOut)[i].spotify_name);
        memcpy((*tracksOut)[i].spotify_uri, cJSON_GetObjectItem(track, "uri")->valuestring, 37);
        (*tracksOut)[i].artist = strdup(
                cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "name")->valuestring);
        sanitize(&(*tracksOut)[i].artist);
        (*tracksOut)[i].artist_escaped = urlencode((*tracksOut)[i].artist);
        memcpy((*tracksOut)[i].spotify_artist_id,
               cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "id")->valuestring,
               SPOTIFY_ID_LEN_NULL);
        (*tracksOut)[i].spotify_album_art = strdup(playlistOut->image_url);
        (*tracksOut)[i].download_state = DS_NOT_DOWNLOADED;
        (*tracksOut)[i].duration_ms = cJSON_GetObjectItem(track, "duration_ms")->valueint;
        i++;
    }
    playlistOut->track_count = i;
    cJSON_Delete(root);
    printf("[spotify] Done getting info form API. Saving to disk...\n");
    save_playlist_to_file(file, playlistOut, *tracksOut);
    free(file);
    printf("[spotify] Done writing album to disk.\n");
    return 0;
}

int
get_playlist(char *playlistId, PlaylistInfo *playlistOut, Track **tracksOut) {
    ensure_token();
    char *file = NULL;
    playlist_filepath(playlistId, &file, false);
    if (!access(file, R_OK)) {
        printf("[spotify] Playlist info found on disk. Reading...\n");
        read_playlist_from_file(file, playlistOut, tracksOut);
        free(file);
        playlistOut->album = false;
        printf("[spotify] Done reading playlist from disk\n");
        return 0;
    }

    printf("[spotify] Getting playlist info for playlist %s\n", playlistId);
    redo:;

    char *url = malloc((253 + strlen(playlistId) + 1) * sizeof(char));
    snprintf(url, 253 + strlen(playlistId) + 1,
             "https://api.spotify.com/v1/playlists/%s?fields=id,name,tracks.items.track.album.images,tracks.items.track.id,tracks.items.track.name,tracks.items.track.artists.name,tracks.items.track.artists.id,tracks.items.track.uri,tracks.items.track.duration_ms,images",
             playlistId);

    Response response;
    read_url(url, &response, auth_header);

    if (!response.size) {
        fprintf(stderr, "[spotify] Got empty response when trying to get playlist info\n");
        free(file);
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
        fprintf(stderr, "[spotify] Error occurred when trying to get playlist info: %s\n", error);
        cJSON_Delete(root);
        free(file);
        return 1;
    }

    playlistOut->not_empty = true;
    playlistOut->album = false;
    playlistOut->name = strdup(cJSON_GetObjectItem(root, "name")->valuestring);
    playlistOut->image_url = strdup(
            cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "images"), 0), "url")->valuestring);
    cJSON *tracks_array = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "tracks"), "items");
    *tracksOut = malloc(sizeof(Track) * cJSON_GetArraySize(tracks_array));
    memcpy(playlistOut->spotify_id, cJSON_GetObjectItem(root, "id")->valuestring, 23);

    cJSON *element;
    size_t i = 0;
    cJSON_ArrayForEach(element, tracks_array) {
        cJSON *track = cJSON_GetObjectItem(element, "track");
        if (cJSON_IsNull(track) || cJSON_IsTrue(cJSON_GetObjectItem(track, "is_local"))) {
            continue;
        }
        memcpy((*tracksOut)[i].spotify_id, cJSON_GetObjectItem(track, "id")->valuestring, 23);
        (*tracksOut)[i].spotify_name = strdup(cJSON_GetObjectItem(track, "name")->valuestring);
        sanitize(&(*tracksOut)[i].spotify_name);
        (*tracksOut)[i].spotify_name_escaped = urlencode((*tracksOut)[i].spotify_name);
        memcpy((*tracksOut)[i].spotify_uri, cJSON_GetObjectItem(track, "uri")->valuestring, 37);
        (*tracksOut)[i].artist = strdup(
                cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "name")->valuestring);
        sanitize(&(*tracksOut)[i].artist);
        (*tracksOut)[i].artist_escaped = urlencode((*tracksOut)[i].artist);
        memcpy((*tracksOut)[i].spotify_artist_id,
               cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "id")->valuestring,
               SPOTIFY_ID_LEN_NULL);
        (*tracksOut)[i].spotify_album_art = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(
                cJSON_GetObjectItem(track, "album"), "images"), 0), "url")->valuestring);
        (*tracksOut)[i].download_state = DS_NOT_DOWNLOADED;
        (*tracksOut)[i].duration_ms = cJSON_GetObjectItem(track, "duration_ms")->valueint;
        i++;
    }
    playlistOut->track_count = i;
    cJSON_Delete(root);
    printf("[spotify] Done getting info form API. Saving to disk...\n");
    save_playlist_to_file(file, playlistOut, *tracksOut);
    free(file);
    printf("[spotify] Done writing playlist to disk.\n");
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
    (*out) = malloc((track_save_path_len + SPOTIFY_URI_LEN + 4 + 1) * sizeof(char));
    snprintf((*out), track_save_path_len + SPOTIFY_URI_LEN + 4 + 1, "%s%s.ogg", track_save_path,
             track->spotify_id);
    out[track_save_path_len + strlen(track->spotify_id) + 4] = 0;
}

void
playlist_filepath(char *id, char **out, bool album) {
    (*out) = malloc(strlen(id) * sizeof(char) + playlist_save_path_len + 2);
    memcpy(*out, playlist_save_path, playlist_save_path_len);
    memcpy(*out + playlist_save_path_len, id, strlen(id));
    (*out)[strlen(id) + playlist_save_path_len] = album ? 'a' : 'p';
    (*out)[strlen(id) + playlist_save_path_len + 1] = 0;
}

/*
 * Playlist file structure
 *
 * bool album (1 byte)
 * last played: clock_t
 * name: null terminated string
 * spotify id: 22 byte string; not null terminated because the length is always the same
 * playlist image: null terminated string
 * unsigned int: track count (4 bytes)
 * Track array:
 *  (each item)
 *      spotify id (same as before)
 *      spotify uri: 36 bytes; not null terminated because the length is always the same
 *      duration in ms: uint32_t - 4 bytes
 *      artist id: 22 byte string
 *      name: null terminated string
 *      album art uri: null terminated string
 *      artist name: null terminated string
 */
void
save_playlist_to_file(const char *path, PlaylistInfo *playlist, Track *tracks) {
    size_t buffer_size =
            1 + strlen(playlist->name) + 22 + sizeof(playlist->last_played) + strlen(playlist->image_url) + 1 + 4 +
            (22 + 36 + 60 + 64 + 30 /*Estimated size of single track*/) * playlist->track_count;
    char *buffer = calloc(1, buffer_size); //Allocate more memory than will probably be needed just in case
    size_t i = sizeof(i); //Leave space to write the size of the buffer at the beginning
    buffer[i++] = (char) playlist->album;
    memcpy(&buffer[i], &playlist->last_played, sizeof(playlist->last_played));
    i += sizeof(playlist->last_played);
    memcpy(&buffer[i], playlist->name, strlen(playlist->name) + 1 /*Also copy null byte*/);
    i += strlen(playlist->name) + 1;

    memcpy(&buffer[i], playlist->spotify_id, 22);
    i += 22;
    memcpy(&buffer[i], playlist->image_url, strlen(playlist->image_url) + 1);
    i += strlen(playlist->image_url) + 1;
    memcpy(&buffer[i], &playlist->track_count, sizeof(playlist->track_count));
    i += sizeof(playlist->track_count);

    for (int j = 0; j < playlist->track_count; ++j) {
        size_t name_len = strlen(tracks[j].spotify_name) + 1;
        size_t album_len = strlen(tracks[j].spotify_album_art) + 1;
        size_t artist_len = strlen(tracks[j].artist) + 1;

        if (buffer_size <= i + 22 + 36 + name_len + album_len + artist_len) {
            char *r = realloc(buffer, buffer_size + (22 + 36 + 60 + 64 + 30) * 20);
            if (!r) {
                fprintf(stderr, "[spotify] Error when allocating more memory: %s\n", strerror(errno));
                free(buffer);
                return;
            }
            buffer_size = buffer_size + (22 + 36 + 60 + 64 + 30) * 20;
        }

        memcpy(&buffer[i], tracks[j].spotify_id, 22);
        i += 22;
        memcpy(&buffer[i], tracks[j].spotify_uri, 36);
        i += 36;
        memcpy(&buffer[i], &tracks[j].duration_ms, sizeof(tracks[j].duration_ms));
        i += sizeof(tracks[j].duration_ms);
        memcpy(&buffer[i], tracks[j].spotify_artist_id, 22);
        i += 22;
        memcpy(&buffer[i], tracks[j].spotify_name, name_len);
        i += name_len;
        memcpy(&buffer[i], tracks[j].spotify_album_art, album_len);
        i += album_len;
        memcpy(&buffer[i], tracks[j].artist, artist_len);
        i += artist_len;
    }
    i -= sizeof(i);
    memcpy(buffer, &i, sizeof(i));

    FILE *fp = fopen_mkdir(path, "w");
    if (!fp) {
        fprintf(stderr, "[spotify] Error when opening file to save playlist: %s\n", strerror(errno));
        free(buffer);
        return;
    }

    fwrite(buffer, sizeof(*buffer), i + sizeof(i), fp);

    fclose(fp);
    free(buffer);
}

void
save_playlist_last_played_to_file(const char *path, PlaylistInfo *playlist) {
    FILE *fp = fopen(path, "r+");
    if (!fp) {
        fprintf(stderr, "[spotify] Error when saving playlist to file: %s\n", strerror(errno));
        return;
    }

    fseek(fp, sizeof(playlist->album) + sizeof(size_t), SEEK_SET);
    fwrite(&playlist->last_played, sizeof(playlist->last_played), 1, fp);
    fclose(fp);
}

void
read_playlist_from_file(const char *path, PlaylistInfo *playlistOut, Track **tracksOut) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "[spotify] Error when saving playlist to file: %s\n", strerror(errno));
        return;
    }

    size_t size = 0;
    fread(&size, sizeof(size), 1, fp);
    char *buffer = malloc(size);
    fread(buffer, sizeof(*buffer), size, fp);

    fclose(fp);

    size_t i = 0;
    playlistOut->not_empty = true;
    playlistOut->album = buffer[i++];
    memcpy(&playlistOut->last_played, &buffer[i], sizeof(playlistOut->last_played));
    i += sizeof(playlistOut->last_played);
    playlistOut->name = strdup(&buffer[i]); //String is null terminated
    i += strlen(playlistOut->name) + 1;
    memcpy(playlistOut->spotify_id, &buffer[i], 22);
    i += 22;
    playlistOut->spotify_id[22] = 0;
    playlistOut->image_url = strdup(&buffer[i]);
    i += strlen(playlistOut->image_url) + 1;
    memcpy(&playlistOut->track_count, &buffer[i], sizeof(playlistOut->track_count));
    i += sizeof(playlistOut->track_count);
    Track *tracks = *tracksOut = malloc(sizeof(*tracks) * playlistOut->track_count);
    for (int j = 0; j < playlistOut->track_count; ++j) {
        memcpy(tracks[j].spotify_id, &buffer[i], 22);
        i += 22;
        tracks[j].spotify_id[22] = 0;
        memcpy(tracks[j].spotify_uri, &buffer[i], 36);
        i += 36;
        tracks[j].spotify_uri[36] = 0;
        memcpy(&tracks[j].duration_ms, &buffer[i], sizeof(tracks[j].duration_ms));
        i += sizeof(tracks[j].duration_ms);
        memcpy(tracks[j].spotify_artist_id, &buffer[i], 22);
        i += 22;
        tracks[j].spotify_artist_id[22] = 0;
        tracks[j].spotify_name = strdup(&buffer[i]);
        i += strlen(tracks[j].spotify_name) + 1;
        tracks[j].spotify_name_escaped = urlencode(tracks[j].spotify_name);
        tracks[j].spotify_album_art = strdup(&buffer[i]);
        i += strlen(tracks[j].spotify_album_art) + 1;
        tracks[j].artist = strdup(&buffer[i]);
        i += strlen(tracks[j].artist) + 1;
        tracks[j].artist_escaped = urlencode(tracks[j].artist);
    }
    free(buffer);
}

void
read_playlist_info_from_file(const char *path, PlaylistInfo *playlistOut) {
    FILE *fp = fopen(path, "r");

    if (!fp) {
        fprintf(stderr, "[spotify] Error when saving playlist to file: %s\n", strerror(errno));
        return;
    }

    size_t size = 0;
    fread(&size, sizeof(size), 1, fp);
    char *buffer = malloc(size);
    fread(buffer, sizeof(*buffer), size, fp);

    fclose(fp);

    size_t i = 0;
    playlistOut->not_empty = true;
    playlistOut->album = buffer[i++];
    memcpy(&playlistOut->last_played, &buffer[i], sizeof(playlistOut->last_played));
    i += sizeof(playlistOut->last_played);
    playlistOut->name = strdup(&buffer[i]); //String is null terminated
    i += strlen(playlistOut->name) + 1;
    memcpy(playlistOut->spotify_id, &buffer[i], 22);
    i += 22;
    playlistOut->spotify_id[22] = 0;
    playlistOut->image_url = strdup(&buffer[i]);
    i += strlen(playlistOut->image_url) + 1;
    memcpy(&playlistOut->track_count, &buffer[i], sizeof(playlistOut->track_count));

    free(buffer);
}

size_t
get_saved_playlist_count() {
    size_t count = 0;
    DIR *dirp = opendir(playlist_save_path);
    if (!dirp) {
        fprintf(stderr, "[spotify] Error when opening playlist directory '%s': %s\n", playlist_save_path,
                strerror(errno));
        return 0;
    }

    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(dirp))) {
        count += (1 - (entry->d_name[0] == '.'));
    }
    if (errno) {
        fprintf(stderr, "[spotify] Error while counting playlists in directory '%s': %s\n", playlist_save_path,
                strerror(errno));
    }
    closedir(dirp);
    return count;
}

void
get_all_playlist_info(PlaylistInfo **playlistInfo, size_t *countOut) {
    *countOut = get_saved_playlist_count();
    DIR *dirp = opendir(playlist_save_path);
    if (!dirp) {
        fprintf(stderr, "[spotify] Error when opening playlist directory '%s': %s\n", playlist_save_path,
                strerror(errno));
        return;
    }
    *playlistInfo = calloc(*countOut, sizeof(**playlistInfo));
    char *name = malloc(PLAYLIST_NAME_LEN_NULL + strlen(playlist_save_path));
    memcpy(name, playlist_save_path, strlen(playlist_save_path));

    errno = 0;
    size_t i = 0;
    struct dirent *entry;
    while ((entry = readdir(dirp))) {
        if (entry->d_name[0] == '.')continue;
        memcpy(name + strlen(playlist_save_path), entry->d_name, PLAYLIST_NAME_LEN_NULL);

        read_playlist_info_from_file(name, &((*playlistInfo)[i++]));
    }
    free(name);
    if (errno) {
        fprintf(stderr, "[spotify] Error while counting playlists in directory '%s': %s\n", playlist_save_path,
                strerror(errno));
    }
    closedir(dirp);
}

int
get_recommendations(char **seed_tracks, size_t seed_track_count, char **seed_artists, size_t seed_artist_count,
                    char **seed_genres, size_t genre_count, size_t limit, Track **tracksOut, size_t *track_count) {
    ensure_token();
    printf("[spotify] Getting recommendations\n");

    //Base url: https://api.spotify.com/v1/recommendations?seed_tracks=&seed_artists=&seed_genres=&limit=
    char *url = malloc((89 + 23 * 5 * 2 + genre_count * 12 /*Probably big enough for genres*/) *
                       sizeof(char)); //Might be too big but thats fine
    memcpy(url, recommendations_base_url, recommendations_base_url_len);
    char *urlb = url + recommendations_base_url_len;
    bool first = true;
    for (int i = 0; seed_tracks && i < seed_track_count; ++i) {
        if (!first) {
            *urlb++ = ',';
        }
        memcpy(urlb, seed_tracks[i], 22);
        free(seed_tracks[i]);
        urlb += 22;
        first = false;
    }
    memcpy(urlb, recommendations_seed_artists, recommendations_seed_artists_len);
    urlb += recommendations_seed_artists_len;
    first = true;
    for (int i = 0; seed_artists && i < seed_artist_count; ++i) {
        if (!first) {
            *urlb++ = ',';
        }
        memcpy(urlb, seed_artists[i], 22);
        free(seed_artists[i]);
        urlb += 22;
        first = false;
    }
    memcpy(urlb, recommendations_seed_genre, recommendations_seed_genre_len);
    urlb += recommendations_seed_genre_len;
    first = true;
    for (int i = 0; seed_genres && i < genre_count; ++i) {
        if (!seed_genres[i])continue;
        if (!first) {
            *urlb++ = ',';
        }
        memcpy(urlb, seed_genres[i], strlen(seed_genres[i]));
        urlb += strlen(seed_genres[i]);
        free(seed_genres[i]);
        first = false;
    }
    memcpy(urlb, recommendations_limit, recommendations_limit_len);
    urlb += recommendations_limit_len;
    char *limit_str = malloc(10 * sizeof(*limit_str));
    snprintf(limit_str, 10, "%zu", limit ? limit : 30);
    memcpy(urlb, limit_str, strlen(limit_str) + 1);
    free(limit_str);

    printf("[spotify] Fetching recommendations using url: %s\n", url);

    redo:;

    Response response;
    read_url(url, &response, auth_header);

    if (!response.size) {
        fprintf(stderr, "[spotify] Got empty response when trying to get recommendations\n");
        free(url);
        free(response.data);
        return 1;
    }

    cJSON *root = cJSON_ParseWithLength(response.data, response.size);

    if (cJSON_HasObjectItem(root, "error")) {
        char *error = cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring;
        if (!strcmp(error, "The access token expired")) {
            get_token();
            cJSON_Delete(root);
            free(response.data);
            goto redo;
        }
        free(url);
        fprintf(stderr, "[spotify] Error occurred when trying to get recommendations: %s\n", cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring);
        cJSON_Delete(root);
        free(response.data);
        return 1;
    }
    free(url);
    cJSON *tracks_array = cJSON_GetObjectItem(root, "tracks");
    *track_count = cJSON_GetArraySize(tracks_array);
    *tracksOut = malloc(*track_count * sizeof(**tracksOut));
    Track *tracks = *tracksOut;

    cJSON *track;
    size_t i = 0;
    cJSON_ArrayForEach(track, tracks_array) {
        if (cJSON_IsNull(track)) {
            continue;
        }
        memcpy(tracks[i].spotify_id, cJSON_GetObjectItem(track, "id")->valuestring, 23);
        tracks[i].spotify_name = strdup(cJSON_GetObjectItem(track, "name")->valuestring);
        sanitize(&tracks[i].spotify_name);
        tracks[i].spotify_name_escaped = urlencode(tracks[i].spotify_name);
        memcpy(tracks[i].spotify_uri, cJSON_GetObjectItem(track, "uri")->valuestring, 37);
        tracks[i].artist = strdup(
                cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "name")->valuestring);
        sanitize(&tracks[i].artist);
        memcpy(tracks[i].spotify_artist_id,
               cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(track, "artists"), 0), "id")->valuestring,
               SPOTIFY_ID_LEN_NULL);
        tracks[i].artist_escaped = urlencode(tracks[i].artist);
        tracks[i].spotify_album_art = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(
                cJSON_GetObjectItem(track, "album"), "images"), 0), "url")->valuestring);
        tracks[i].download_state = DS_NOT_DOWNLOADED;
        (*tracksOut)[i].duration_ms = cJSON_GetObjectItem(track, "duration_ms")->valueint;
        i++;
    }
    cJSON_Delete(root);
    free(response.data);
    printf("[spotify] Done getting recommendations\n");
    return 0;
}

int
get_recommendations_from_tracks(Track *tracks, size_t track_count, size_t limit, Track **tracksOut,
                                size_t *track_count_out) {
    char *seed_tracks[5]; //Last 5 tracks
    char *seed_artists[5];
    struct ArtistQuantity *artists = calloc(track_count, sizeof(*artists));
    for (int i = 0; i < track_count; ++i) {
        for (int j = 0; j < track_count; ++j) {
            if (strcmp(tracks[i].spotify_artist_id, artists[j].id) != 0) continue;
            artists[j].appearances++;
            goto end_loop;
        }
        memcpy(artists[i].id, tracks[i].spotify_artist_id, SPOTIFY_ID_LEN_NULL);
        artists[i].appearances = 1;
        end_loop:;
    }
    qsort(artists, track_count, sizeof(*artists), compare_artist_quantities);
    size_t artist_count = 0;
    for (int i = 0; i < track_count; ++i) {
        if (artists[i].appearances)artist_count++;
        if (artist_count >= 3)break; //Don't need more than 3
    }
    for (int i = 0; i < artist_count; ++i) {
        seed_artists[i] = malloc(SPOTIFY_ID_LEN_NULL);
        memcpy(seed_artists[i], artists[i].id, SPOTIFY_ID_LEN_NULL);
        seed_artists[i][SPOTIFY_ID_LEN] = 0;
        printf("[spotify] Using artist: %s\n", artists[i].id);
    }
    size_t track_amount = track_count >= 5 - artist_count ? 5 - artist_count : track_count;
    for (int i = 0; i < track_amount; ++i) {
        seed_tracks[i] = malloc(SPOTIFY_ID_LEN_NULL);
        memcpy(seed_tracks[i], tracks[track_count - (i + 1)].spotify_id, SPOTIFY_ID_LEN_NULL);
        seed_tracks[i][SPOTIFY_ID_LEN] = 0;
        printf("[spotify] Using track: %s\n", tracks[track_count - (i + 1)].spotify_id);
    }
    free(artists);
    return get_recommendations(seed_tracks, track_amount, seed_artists, artist_count, NULL, 0, limit, tracksOut,
                               track_count_out);
}