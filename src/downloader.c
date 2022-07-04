//
// Created by quartzy on 6/24/22.
//

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "downloader.h"
#include "util.h"
#include "config.h"
#include "../lib/cjson/cJSON.h"
#include <linux/futex.h>
#include <sys/syscall.h>

void *
search_and_download(void *userp) {
    DownloadParams *params = (DownloadParams *) userp;
    char *video_url = NULL;

    const size_t artist_len = strlen(params->track->artist_escaped);
    const size_t name_len = strlen(params->track->spotify_name_escaped);
    char *query = malloc(artist_len + 3 + name_len + 1);
    memcpy(query, params->track->artist_escaped, artist_len);
    query[artist_len] = '%';
    query[artist_len + 1] = '2';
    query[artist_len + 2] = '0';
    memcpy(query + 3 + artist_len, params->track->spotify_name_escaped, name_len);
    query[artist_len + 3 + name_len] = 0;

    int attempts = 3;
    search_part:
    if (search_for_track(random_api_instance, &video_url, query)) {
        attempts--;
        if (attempts <= 0) {
            fprintf(stderr, "[downloader] Could not find result after 3 retries.\n");
            params->track->download_state = DS_DOWNLOAD_FAILED;
            syscall(SYS_futex, &params->track->download_state, FUTEX_WAKE, INT_MAX, NULL, 0, 0);
            return NULL;
        }
        fprintf(stderr, "[downloader] Retrying. %d attempts left\n", attempts);
        goto search_part;
    }
    download_part:
    if (download_from_id(random_download_instance, video_url, params->path_out, params->track->spotify_id)) {
        attempts--;
        if (attempts <= 0) {
            fprintf(stderr, "[downloader] Could not download after 3 retries.\n");
            params->track->download_state = DS_DOWNLOAD_FAILED;
            syscall(SYS_futex, &params->track->download_state, FUTEX_WAKE, INT_MAX, NULL, 0, 0);
            return NULL;
        }
        fprintf(stderr, "[downloader] Error when downloading. Retrying. %d attempts left\n", attempts);
        goto download_part;
    }
    params->track->download_state = DS_DOWNLOADED;
    syscall(SYS_futex, &params->track->download_state, FUTEX_WAKE, INT_MAX, NULL, 0, 0);
    free(video_url);
    free(params);
    return NULL;
}

int
search_for_track(char *instance, char **url_out, char *query) {
    char *url = malloc(
            (29 + strlen(instance) + strlen(query) + 1) *
            sizeof(char));
    snprintf(url, 29 + strlen(instance) + strlen(query) + 1,
             "%s/search?q=%s&filter=music_songs",
             instance, query);

    printf("[downloader] Searching with url: %s\n", url);
    Response response;
    read_url(url, &response, NULL);

    if (!response.size) {
        fprintf(stderr, "[downloader] Error when looking for song on instance: empty response received\n");
        free(url);
        return 1;
    }

    cJSON *searchResults = cJSON_ParseWithLength(response.data, response.size);
    if (!cJSON_GetArraySize(searchResults)){
        cJSON_Delete(searchResults);
        free(response.data);
        goto search_regular;
    }
    cJSON *element = cJSON_GetArrayItem(cJSON_GetObjectItem(searchResults, "items"), 0);

    free(url);
    *url_out = strdup(cJSON_GetObjectItem(element, "url")->valuestring);
    cJSON_Delete(searchResults);
    free(response.data);
    return 0;

    search_regular:
    snprintf(url, 24 + strlen(instance) + strlen(query) + 1,
             "%s/search?q=%s&filter=videos",
             instance, query);

    printf("[downloader] Searching with url: %s\n", url);
    read_url(url, &response, NULL);

    if (!response.size) {
        fprintf(stderr, "[downloader] Error when looking for song on instance: empty response received\n");
        free(url);
        return 1;
    }

    searchResults = cJSON_ParseWithLength(response.data, response.size);

    if (!cJSON_GetArraySize(searchResults)){
        fprintf(stderr,
                "[downloader] No matching youtube video was found (very wierd) (url: %s)\n",
                url);
        cJSON_Delete(searchResults);
        free(url);
        url = NULL;
        free(response.data);
        return 1;
    }

    element = cJSON_GetArrayItem(searchResults, 0);
    free(url);
    *url_out = strdup(cJSON_GetObjectItem(element, "url")->valuestring);
    cJSON_Delete(searchResults);
    free(response.data);
    return 0;
}

int
download_from_id(char *instance, char *url_in, char *path_out, char *file_name) {
    const size_t instance_len = strlen(instance);
    const size_t url_in_len = strlen(url_in);
    const size_t path_out_len = strlen(path_out);
    const size_t file_name_len = strlen(file_name);

    size_t url_len = instance_len + url_in_len;
    char *url = malloc(instance_len + url_in_len + 1);
    memcpy(url, instance, instance_len);
    memcpy(url + instance_len, url_in, url_in_len);
    url[instance_len + url_in_len] = 0;

    printf("[downloader] Downloading from url %s\n", url);

    char *cmd = malloc((101 + file_name_len + path_out_len + url_len + 1) * sizeof(char));
    snprintf(cmd, (101 + file_name_len + path_out_len + url_len + 1) * sizeof(char),
             "yt-dlp --no-warnings --quiet -x --no-keep-video --audio-format vorbis --output \"%s.%%(ext)s\" --paths %s \"%s\"",
             file_name, path_out, url);
    int status = system(cmd);
    free(url);
    free(cmd);
    printf("[downloader] Finished downloading\n");
    return WEXITSTATUS(status);
}