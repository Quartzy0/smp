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
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <errno.h>
#include <dirent.h>

char *authHeader;
const char *searchUrl1 = "https://api-partner.spotify.com/pathfinder/v1/query?operationName=searchDesktop&variables=%7B%22searchTerm%22%3A%22";
const size_t searchUrl1Size = 115 * sizeof(*searchUrl1);
const char *searchUrl2 = "%22%2C%22offset%22%3A0%2C%22limit%22%3A10%2C%22numberOfTopResults%22%3A5%2C%22includeAudiobooks%22%3Afalse%7D&extensions=%7B%22persistedQuery%22%3A%7B%22version%22%3A1%2C%22sha256Hash%22%3A%2219967195df75ab8b51161b5ac4586eab9cf73b51b35a03010073533c33fd11ae%22%7D%7D";
const size_t searchUrl2Size = 265 * sizeof(*searchUrl2);
const char *authHeaderStart = "authorization: Bearer ";

const char getTrackUrl1[] = "https://api-partner.spotify.com/pathfinder/v1/query?operationName=getTrack&variables=%7B%22uri%22%3A%22spotify%3Atrack%3A";
const size_t getTrackUrl1Size = sizeof(getTrackUrl1) - 1;
const char getTrackUrl2[] = "%22%7D&extensions=%7B%22persistedQuery%22%3A%7B%22version%22%3A1%2C%22sha256Hash%22%3A%22c1dba8d326cc06a6ebc960dabc4fd79fd73d774edaced6dc3fc5f9d9a6f7f64f%22%7D%7D";
const size_t getTrackUrl2Size = sizeof(getTrackUrl2) - 1;

const char *exts_template = ".%(ext)s";
const size_t exts_template_len = 8;

const char *track_save_path = "tracks/";
const size_t track_save_path_len = 7;
const char *track_ext = ".ogg";
const size_t track_ext_len = 4;
const char playlist_save_path[] = "playlists/";
const size_t playlist_save_path_len = sizeof(playlist_save_path) - 1;

const char recommendations_base_url[] = "https://api.spotify.com/v1/recommendations?seed_tracks=";
const size_t recommendations_base_url_len = sizeof(recommendations_base_url) - 1;
const char recommendations_seed_artists[] = "&seed_artists=";
const size_t recommendations_seed_artists_len = sizeof(recommendations_seed_artists) - 1;
const char recommendations_seed_genre[] = "&seed_genres=";
const size_t recommendations_seed_genre_len = sizeof(recommendations_seed_genre) - 1;
const char recommendations_limit[] = "&limit=";
const size_t recommendations_limit_len = sizeof(recommendations_limit) - 1;

uint64_t token_expiration = 0;

void
get_token() {
    struct curl_slist *list = NULL;
    Response response;

    list = curl_slist_append(list, "Host: open.spotify.com");
    list = curl_slist_append(list, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    list = curl_slist_append(list, "Accept-Language: en-US,en;q=0.5");
    list = curl_slist_append(list, "Connection: keep-alive");
    list = curl_slist_append(list, "Upgrade-Insecure-Requests: 1");
    list = curl_slist_append(list, "Sec-Fetch-Dest: document");
    list = curl_slist_append(list, "Sec-Fetch-Mode: navigate");
    list = curl_slist_append(list, "Sec-Fetch-Site: none");
    list = curl_slist_append(list, "Sec-Fetch-User: ?1");

    read_url("https://open.spotify.com/", &response, list);

    char *jsonStart = strstr(response.data, "<script id=\"config\" data-testid=\"config\" type=\"application/json\">");
    if (!jsonStart) {
        fprintf(stderr, "[spotify] Auth token not found\n");
        return;
    }
    jsonStart += 65; // Length of search string
    char *jsonEnd = strstr(jsonStart, "</script>");

    cJSON *root = cJSON_ParseWithLength(jsonStart, jsonEnd - jsonStart);
    if (!root) {
        fprintf(stderr, "[spotify] Error while parsing json\n");
        free(response.data);
        curl_slist_free_all(list);
        return;
    }
    char *token = strdup(cJSON_GetObjectItem(root, "accessToken")->valuestring);
    printf("[spotify] Got token: %s\n", token);
    authHeader = malloc((22 + strlen(token) + 1) * sizeof(char));
    memcpy(authHeader, authHeaderStart, 22);
    memcpy(authHeader + 22 * sizeof(char), token, strlen(token));
    authHeader[22 + strlen(token)] = 0;
    token_expiration = cJSON_GetObjectItem(root, "accessTokenExpirationTimestampMs")->valueint;

    free(token);
    cJSON_Delete(root);
    free(response.data);
    curl_slist_free_all(list);
}

void
ensure_token() {
    if (!token_expiration || token_expiration <= ((uint64_t) (((double) clock()) / ((double) CLOCKS_PER_SEC)) * 1000)) {
        get_token();
    }
}

void
search(const char *query, Track **tracks, size_t *count) {
    ensure_token();
    struct curl_slist *list = NULL;

    list = curl_slist_append(list, "Host: api-partner.spotify.com");
    list = curl_slist_append(list, "Accept: application/json");
    list = curl_slist_append(list, "Accept-Language: en");
    list = curl_slist_append(list, "Referer: https://open.spotify.com/");
    list = curl_slist_append(list, authHeader);
    list = curl_slist_append(list, "app-platform: WebPlayer");
    list = curl_slist_append(list, "spotify-app-version: 1.1.85.257.g92a47121");
    list = curl_slist_append(list, "content-type: application/json;charset=UTF-8");
    list = curl_slist_append(list, "Origin: https://open.spotify.com");
    list = curl_slist_append(list, "Connection: keep-alive");
    list = curl_slist_append(list, "Sec-Fetch-Dest: empty");
    list = curl_slist_append(list, "Sec-Fetch-Mode: cors");
    list = curl_slist_append(list, "Sec-Fetch-Site: same-site");
    list = curl_slist_append(list, "TE: trailers");

    char *url = malloc((380 + strlen(query)) * sizeof(char));
    memcpy(url, searchUrl1, searchUrl1Size);
    memcpy(url + searchUrl1Size, query, strlen(query));
    memcpy(url + searchUrl1Size + strlen(query), searchUrl2, searchUrl2Size);

    Response response;
    int status = read_url(url, &response, list);
    curl_slist_free_all(list);
    if (!status) {
        fprintf(stderr, "[spotify] Error when performing request to url %s\n", url);
        free(url);
        return;
    }
    free(url);

    cJSON *result = cJSON_ParseWithLength(response.data, response.size);
    cJSON *data = cJSON_GetObjectItem(result, "data");
    cJSON *searchV2 = cJSON_GetObjectItem(data, "searchV2");
    cJSON *tracksJson = cJSON_GetObjectItem(searchV2, "tracks");
    cJSON *tracksArray = cJSON_GetObjectItem(tracksJson, "items");

    (*tracks) = calloc(cJSON_GetArraySize(tracksArray), sizeof(**tracks));
    cJSON *element;
    int i = 0;

    cJSON_ArrayForEach(element, tracksArray) {
        cJSON *data = cJSON_GetObjectItem(element, "data");
        memcpy((*tracks)[i].spotify_id, cJSON_GetObjectItem(data, "id")->valuestring, 23);
        memcpy((*tracks)[i].spotify_uri, cJSON_GetObjectItem(data, "uri")->valuestring, 37);
        (*tracks)[i].spotify_name = strdup(cJSON_GetObjectItem(data, "name")->valuestring);
        sanitize(&(*tracks)[i].spotify_name);
        (*tracks)[i].spotify_name_escaped = urlencode((*tracks)[i].spotify_name);
        cJSON *album = cJSON_GetObjectItem(data, "albumOfTrack");
        cJSON *coverArt = cJSON_GetObjectItem(album, "coverArt");
        cJSON *sources = cJSON_GetObjectItem(coverArt, "sources");
        cJSON *source = cJSON_GetArrayItem(sources, 0);
        (*tracks)[i].spotify_album_art = strdup(cJSON_GetObjectItem(source, "url")->valuestring);

        (*tracks)[i].artist = strdup(cJSON_GetObjectItem(cJSON_GetObjectItem(
                                                                 cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(data, "artists"), "items"), 0), "profile"),
                                                         "name")->valuestring);
        (*tracks)[i].artist_escaped = urlencode((*tracks)[i].artist);
        sanitize(&(*tracks)[i].artist);

        i++;
    }
    cJSON_Delete(result);
    *count = i;
}

int
track_by_id(const char *id, Track **track) {
    ensure_token();
    struct curl_slist *list = NULL;

    list = curl_slist_append(list, "Host: api-partner.spotify.com");
    list = curl_slist_append(list, "Accept: application/json");
    list = curl_slist_append(list, "Accept-Language: en");
    list = curl_slist_append(list, "Referer: https://open.spotify.com/");
    list = curl_slist_append(list, authHeader);
    list = curl_slist_append(list, "app-platform: WebPlayer");
    list = curl_slist_append(list, "spotify-app-version: 1.1.85.257.g92a47121");
    list = curl_slist_append(list, "content-type: application/json;charset=UTF-8");
    list = curl_slist_append(list, "Origin: https://open.spotify.com");
    list = curl_slist_append(list, "Connection: keep-alive");
    list = curl_slist_append(list, "Sec-Fetch-Dest: empty");
    list = curl_slist_append(list, "Sec-Fetch-Mode: cors");
    list = curl_slist_append(list, "Sec-Fetch-Site: same-site");
    list = curl_slist_append(list, "TE: trailers");

    char *url = malloc(getTrackUrl1Size + getTrackUrl2Size + strlen(id) * sizeof(*id) + 1);
    memcpy(url, getTrackUrl1, getTrackUrl1Size);
    memcpy(url + getTrackUrl1Size, id, strlen(id));
    memcpy(url + getTrackUrl1Size + strlen(id) * sizeof(*id), getTrackUrl2, getTrackUrl2Size);
    url[getTrackUrl1Size + getTrackUrl2Size + strlen(id) * sizeof(*id)] = 0;

    Response response;
    read_url(url, &response, list);
    free(url);
    curl_slist_free_all(list);

    if (!response.size) {
        fprintf(stderr, "[spotify] Got empty response when trying to get playlist info\n");
        return 1;
    }

    cJSON *root = cJSON_ParseWithLength(response.data, response.size);

    if (cJSON_HasObjectItem(root, "errors")) {
        fprintf(stderr, "[spotify] Error occurred when trying to get track info\n");
        cJSON_Delete(root);
        return 1;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *trackJson = cJSON_GetObjectItem(data, "trackUnion");

    if (!strcmp(cJSON_GetObjectItem(trackJson, "__typename")->valuestring, "NotFound")) {
        fprintf(stderr, "[spotify] Error occurred when trying to get track info: %s\n",
                cJSON_GetObjectItem(trackJson, "message")->valuestring);
        cJSON_Delete(root);
        return 1;
    }

    memcpy((*track)->spotify_id, cJSON_GetObjectItem(trackJson, "id")->valuestring, 23);
    (*track)->spotify_name = strdup(cJSON_GetObjectItem(trackJson, "name")->valuestring);
    sanitize(&(*track)->spotify_name);
    (*track)->spotify_name_escaped = urlencode((*track)->spotify_name);
    memcpy((*track)->spotify_uri, cJSON_GetObjectItem(trackJson, "uri")->valuestring, 37);
    (*track)->spotify_album_art = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(
            cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(trackJson, "album"), "coverArt"), "sources"),
            0), "url")->valuestring);
    (*track)->download_state = DS_NOT_DOWNLOADED;
    (*track)->duration_ms = cJSON_GetObjectItem(cJSON_GetObjectItem(trackJson, "duration"),
                                                "totalMilliseconds")->valueint;

    cJSON *artists = cJSON_GetObjectItem(cJSON_GetObjectItem(trackJson, "artistsWithRoles"), "items");
    cJSON *element;
    cJSON_ArrayForEach(element, artists) {
        if (!strcmp(cJSON_GetObjectItem(element, "role")->valuestring, "MAIN")) {
            break;
        }
    }
    (*track)->artist = strdup(
            cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(element, "artist"), "profile"),
                                "name")->valuestring);
    sanitize(&(*track)->artist);
    (*track)->artist_escaped = urlencode((*track)->artist);

    cJSON_Delete(root);
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
    struct curl_slist *list = NULL;

    list = curl_slist_append(list, authHeader);

    char *url = malloc((34 + strlen(albumId) + 1) * sizeof(char));
    snprintf(url, 34 + strlen(albumId) + 1,
             "https://api.spotify.com/v1/albums/%s",
             albumId);

    Response response;
    read_url(url, &response, list);
    free(url);
    curl_slist_free_all(list);

    if (!response.size) {
        fprintf(stderr, "[spotify] Got empty response when trying to get playlist info\n");
        free(file);
        return 1;
    }

    cJSON *root = cJSON_ParseWithLength(response.data, response.size);

    if (cJSON_HasObjectItem(root, "error")) {
        fprintf(stderr, "[spotify] Error occurred when trying to get playlist info: %s\n", cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring);
        cJSON_Delete(root);
        free(file);
        return 1;
    }

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
    struct curl_slist *list = NULL;

    list = curl_slist_append(list, authHeader);

    //url: https://api.spotify.com/v1/playlists/<playlist_id>?fields=name,tracks_array.items.track.album.images,tracks_array.items.track.id,tracks_array.items.track.name,tracks_array.items.track.artists.name,tracks_array.items.track.uri
    char *url = malloc((37 + 155 + strlen(playlistId) + 1) * sizeof(char));
    snprintf(url, 37 + 155 + strlen(playlistId) + 1,
             "https://api.spotify.com/v1/playlists/%s?fields=id,name,tracks_array.items.track.album.images,tracks_array.items.track.id,tracks_array.items.track.name,tracks_array.items.track.artists.name,tracks_array.items.track.uri,images",
             playlistId);

    Response response;
    read_url(url, &response, list);
    free(url);
    curl_slist_free_all(list);

    if (!response.size) {
        fprintf(stderr, "[spotify] Got empty response when trying to get playlist info\n");
        free(file);
        return 1;
    }

    cJSON *root = cJSON_ParseWithLength(response.data, response.size);

    if (cJSON_HasObjectItem(root, "error")) {
        fprintf(stderr, "[spotify] Error occurred when trying to get playlist info: %s\n", cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring);
        cJSON_Delete(root);
        free(file);
        return 1;
    }

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
        if (cJSON_IsNull(track)) {
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
free_track(Track *track) {
    free(track->spotify_name);
    free(track->spotify_album_art);
    free(track->spotify_name_escaped);
    free(track->artist);
    free(track);
}

void
free_tracks(Track *track, size_t count) {
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
    playlist->track_count = 0;
    free(playlist->name);
    playlist->name = NULL;
    free(playlist->image_url);
    playlist->image_url = NULL;
}

typedef struct DownloadParams {
    Track *track;
    char *output;
    char *url;
} DownloadParams;

void *
download_async(void *arg) {
    DownloadParams p = *((DownloadParams *) arg);
    pid_t pid = fork();
    if (pid == 0) {
        const char *cmd[] = {"yt-dlp", "--quiet", "--ppa", "Metadata:-metadata testing=Epicc", "--embed-metadata",
                             "-x",
                             "--no-keep-video", "--audio-format", "vorbis", "--output", p.output, "--paths",
                             track_save_path, p.url, NULL};
        execvp(cmd[0], cmd);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    p.track->download_state = DS_DOWNLOADED;
    printf("[spotify] Wake: %ld\n", syscall(SYS_futex, &p.track->download_state, FUTEX_WAKE, INT_MAX, NULL, 0, 0));
    printf("[spotify] Download finished for '%s'\n", p.track->spotify_name);
    free(p.output);
    free(p.url);
    free(arg);
}

void
download_track(Track *track, bool block) {
    if (track->download_state == DS_DOWNLOADED) {
        printf("[spotify] Track '%s' already downloaded\n", track->spotify_name);
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

    printf("[spotify] Track '%s' by '%s' needs to be downloaded. Searching on youtube...\n", track->spotify_name,
           track->artist);

    char *url = malloc((76 + strlen(track->spotify_name_escaped) + strlen(track->artist_escaped) + 1) * sizeof(char));
    snprintf(url, 76 + strlen(track->spotify_name_escaped) + strlen(track->artist_escaped),
             "https://yt.artemislena.eu/api/v1/search?q=%s%%20%s&sort_by=relevance&type=video",
             track->artist_escaped, track->spotify_name_escaped);

    printf("[spotify] Searching with url: %s\n", url);
    int attempts = 3;
    fetch:
    attempts--;
    Response response;
    read_url(url, &response, NULL);

    if (!response.size) {
        fprintf(stderr, "[spotify] Error when looking for song on youtube: empty response received\n");
    }

    cJSON *searchResults = cJSON_ParseWithLength(response.data, response.size);
    cJSON *element;

    cJSON_ArrayForEach(element, searchResults) {
        cJSON *type = cJSON_GetObjectItem(element, "type");
        if (!type || !cJSON_IsString(type) ||
            (strcmp(type->valuestring, "video") && strcmp(type->valuestring, "shortVideo"))) {
            continue;
        }

        goto found;
    }
    if (!attempts) {
        fprintf(stderr,
                "[spotify] No matching youtube video was found (very wierd). Failed after 3 retries (url: %s)\n", url);
        cJSON_Delete(searchResults);
        track->download_state = DS_NOT_DOWNLOADED;
        free(url);
        return;
    }
    fprintf(stderr,
            "[spotify] No matching youtube video was found (very wierd). Trying again (%d retries left) (url: %s)\n",
            attempts, url);
    cJSON_Delete(searchResults);
    goto fetch;

    found:
    free(url);
    char *id = cJSON_GetObjectItem(element, "videoId")->valuestring;
    char *videoUrl = malloc((38 + strlen(id)) * sizeof(char));
    memcpy(videoUrl, "https://invidious.snopyta.org/watch?v=", 38);
    memcpy(videoUrl + 38, id, strlen(id));
    videoUrl[38 + strlen(id)] = 0;

    cJSON_Delete(searchResults);

    char *output = malloc((strlen(track->spotify_id) + exts_template_len + 1) * sizeof(char));
    memcpy(output, track->spotify_id, strlen(track->spotify_id) * sizeof(char));
    memcpy(output + strlen(track->spotify_id) * sizeof(char), exts_template, exts_template_len);
    output[strlen(track->spotify_id) + exts_template_len] = 0;

    printf("[spotify] Found best match for '%s'. Downloading...\n", track->spotify_name);

    if (block) {
        char *cmd = malloc((137 + strlen(output) + track_save_path_len + strlen(videoUrl) + 1) * sizeof(char));
        snprintf(cmd, (137 + strlen(output) + track_save_path_len + strlen(videoUrl) + 1) * sizeof(char),
                 "yt-dlp --quiet --ppa \"Metadata:-metadata testing=Epicc\" --embed-metadata -x --no-keep-video --audio-format vorbis --output \"%s\" --paths %s \"%s\"",
                 output, track_save_path, videoUrl);
        system(cmd);
        free(cmd);
        track->download_state = DS_DOWNLOADED;
        printf("[spotify] Wake: %ld\n", syscall(SYS_futex, &track->download_state, FUTEX_WAKE, INT_MAX, NULL, 0, 0));
        printf("[spotify] Download finished for '%s'\n", track->spotify_name);
    } else {
        DownloadParams *p = malloc(sizeof(*p));
        p->track = track;
        p->output = output;
        p->url = videoUrl;
        pthread_t t;
        pthread_create(&t, NULL, download_async, p);
        pthread_detach(t);
    }
}

void
cleanup() {
    free(authHeader);
}

void
track_filepath(Track *track, char **out) {
    (*out) = malloc((track_save_path_len + strlen(track->spotify_id) + track_ext_len + 1) * sizeof(char));
    snprintf((*out), track_save_path_len + strlen(track->spotify_id) + track_ext_len + 1, "%s%s%s", track_save_path,
             track->spotify_id, track_ext);
    out[track_save_path_len + strlen(track->spotify_id) + track_ext_len] = 0;
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
    buffer[i++] = playlist->album;
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
        memcpy(&buffer[i], tracks[j].spotify_name, name_len);
        i += name_len;
        memcpy(&buffer[i], tracks[j].spotify_album_art, album_len);
        i += album_len;
        memcpy(&buffer[i], tracks[j].artist, artist_len);
        i += artist_len;
    }
    i -= sizeof(i);
    memcpy(buffer, &i, sizeof(i));

    FILE *fp = fopen(path, "w");

    fwrite(buffer, sizeof(*buffer), i + sizeof(i), fp);

    fclose(fp);
    free(buffer);
}

void
save_playlist_last_played_to_file(const char *path, PlaylistInfo *playlist) {
    FILE *fp = fopen(path, "r+");

    fseek(fp, sizeof(playlist->album) + sizeof(size_t), SEEK_SET);
    fwrite(&playlist->last_played, sizeof(playlist->last_played), 1, fp);
    fclose(fp);
}

void
read_playlist_from_file(const char *path, PlaylistInfo *playlistOut, Track **tracksOut) {
    FILE *fp = fopen(path, "r");

    size_t size = 0;
    fread(&size, sizeof(size), 1, fp);
    char *buffer = malloc(size);
    fread(buffer, sizeof(*buffer), size, fp);

    fclose(fp);

    size_t i = 0;
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

    size_t size = 0;
    fread(&size, sizeof(size), 1, fp);
    char *buffer = malloc(size);
    fread(buffer, sizeof(*buffer), size, fp);

    fclose(fp);

    size_t i = 0;
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
    if (errno) {
        fprintf(stderr, "[spotify] Error while counting playlists in directory '%s': %s\n", playlist_save_path,
                strerror(errno));
    }
    closedir(dirp);
}

int
get_recommendations(char seed_tracks[23][5], char seed_artists[23][5], char **seed_genres, size_t genre_count,
                    size_t limit, Track **tracksOut, size_t *track_count) {
    ensure_token();
    printf("[spotify] Getting recommendations\n");

    struct curl_slist *list = NULL;

    list = curl_slist_append(list, authHeader);

    //Base url: https://api.spotify.com/v1/recommendations?seed_tracks=&seed_artists=&seed_genres=&limit=
    char *url = malloc((89 + 23 * 5 * 2 + genre_count * 12 /*Probably big enough for genres*/) *
                       sizeof(char)); //Might be too big but thats fine
    memcpy(url, recommendations_base_url, recommendations_base_url_len);
    char *urlb = url;
    bool first = true;
    for (int i = 0; seed_tracks && i < 5; ++i) {
        if (!seed_tracks[i])continue;
        if (!first) {
            *urlb++ = ',';
        }
        memcpy(urlb, seed_tracks[i], 22);
        urlb += 22;
        first = false;
    }
    memcpy(urlb, recommendations_seed_artists, recommendations_seed_artists_len);
    urlb += recommendations_seed_artists_len;
    first = true;
    for (int i = 0; seed_artists && i < 5; ++i) {
        if (!seed_artists[i])continue;
        if (!first) {
            *urlb++ = ',';
        }
        memcpy(urlb, seed_artists[i], 22);
        urlb += 22;
        first = false;
    }
    memcpy(urlb, recommendations_seed_genre, recommendations_seed_genre_len);
    urlb += recommendations_seed_genre_len;
    first = true;
    for (int i = 0; seed_genres && i < (genre_count > 5 ? 5 : genre_count); ++i) {
        if (!seed_genres[i])continue;
        if (!first) {
            *urlb++ = ',';
        }
        memcpy(urlb, seed_genres[i], strlen(seed_genres[i]));
        urlb += strlen(seed_genres[i]);
        first = false;
    }
    memcpy(urlb, recommendations_limit, recommendations_limit_len);
    urlb += recommendations_limit_len;
    char *limit_str = malloc(10 * sizeof(*limit_str));
    snprintf(limit_str, 10, "%zu", limit ? limit : 30);
    memcpy(urlb, limit_str, strlen(limit_str) + 1);

    printf("[spotify] Fetching recommendations using url: %s\n", url);

    Response response;
    read_url(url, &response, list);
    free(url);
    curl_slist_free_all(list);

    if (!response.size) {
        fprintf(stderr, "[spotify] Got empty response when trying to get recommendations\n");
        return 1;
    }

    cJSON *root = cJSON_ParseWithLength(response.data, response.size);

    if (cJSON_HasObjectItem(root, "error")) {
        fprintf(stderr, "[spotify] Error occurred when trying to get recommendations: %s\n", cJSON_GetObjectItem(
                cJSON_GetObjectItem(root, "error"), "message")->valuestring);
        cJSON_Delete(root);
        return 1;
    }
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
        tracks[i].artist_escaped = urlencode(tracks[i].artist);
        tracks[i].spotify_album_art = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(
                cJSON_GetObjectItem(track, "album"), "images"), 0), "url")->valuestring);
        tracks[i].download_state = DS_NOT_DOWNLOADED;
        i++;
    }
    cJSON_Delete(root);
    printf("[spotify] Done getting recommendations\n");
    return 0;
}