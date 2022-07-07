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
    if (search_for_track(random_api_instance, &video_url, query, params->track->spotify_name, params->track->artist)) {
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
find_match(cJSON *items, const char *title, const char *artist, float *score_out) {
    char *ar_o = strdup(artist);
    char *artist_words[100];
    size_t artist_word_count = 0;
    {
        char *ar = strtok(ar_o, " ");
        while (ar) {
            artist_words[artist_word_count] = ar;
            ar = strtok(NULL, " ");
            artist_word_count++;
        }
    }

    char *ti_o = strdup(title);
    char *title_words[100];
    size_t title_word_count = 0;
    {
        char *ti = strtok(ti_o, " ");
        while (ti) {
            title_words[title_word_count] = ti;
            ti = strtok(NULL, " ");
            title_word_count++;
        }
    }

    float best_match_score = 0;
    size_t best_match_index = 0;
    size_t i = 0;
    cJSON *element;
    cJSON_ArrayForEach(element, items) {
        float artist_score = 0;
        float title_score = 0;
        float score = 0;

        {
            size_t artist_match_count = 0;
            size_t artist_in_words = 1;
            char *artist_in = cJSON_GetObjectItem(element, "uploaderName")->valuestring;
            char *ar = strtok(artist_in, " ");
            while (ar) {
                for (int j = 0; j < artist_word_count; ++j) {
                    if (!strcasecmp(artist_words[j], ar)) {
                        artist_match_count++;
                        break;
                    }
                }
                ar = strtok(NULL, " ");
                if (ar)
                    artist_in_words++;
            }
            artist_score = (float) artist_match_count / (float) artist_in_words;
        }
        {
            size_t title_match_count = 0;
            size_t title_in_words = 1;
            char *title_in = cJSON_GetObjectItem(element, "title")->valuestring;
            char *ti = strtok(title_in, " ");
            while (ti) {
                for (int j = 0; j < title_word_count; ++j) {
                    if (!strcasecmp(title_words[j], ti)) {
                        title_match_count++;
                        break;
                    }
                }
                ti = strtok(NULL, " ");
                if (ti)
                    title_in_words++;
            }
            title_score = (float) title_match_count / (float) title_in_words;
        }
        score = (title_score + artist_score) / 2.0f;


        if (best_match_score < score) {
            best_match_score = score;
            best_match_index = i;
        }
        i++;
    }
    free(ar_o);
    free(ti_o);

    if (score_out) {
        *score_out = best_match_score;
    }

    return (int) best_match_index;
}

int
search_for_track(char *instance, char **url_out, char *query, char *title, char *artist) {
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

    cJSON *root = cJSON_ParseWithLength(response.data, response.size);
    cJSON *searchResults = cJSON_GetObjectItem(root, "items");
    float music_score = 0.f;
    int music_index = -1;
    if (!cJSON_GetArraySize(searchResults)) {
        goto search_regular;
    }
    music_index = find_match(searchResults, title, artist, &music_score);
    if (music_score < 0.5f)
        goto search_regular;

    free(url);
    *url_out = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(searchResults, music_index), "url")->valuestring);
    cJSON_Delete(root);
    free(response.data);
    return 0;

    search_regular:;
    char *url_temp = NULL;
    if (music_index != -1)
        url_temp = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(searchResults, music_index), "url")->valuestring);
    cJSON_Delete(root);
    free(response.data);
    snprintf(url, 24 + strlen(instance) + strlen(query) + 1,
             "%s/search?q=%s&filter=videos",
             instance, query);

    printf("[downloader] Searching with url: %s\n", url);
    read_url(url, &response, NULL);

    if (!response.size) {
        fprintf(stderr, "[downloader] Error when looking for song on instance: empty response received\n");
        free(url);
        if (url_temp) free(url_temp);
        return 1;
    }

    root = cJSON_ParseWithLength(response.data, response.size);
    searchResults = cJSON_GetObjectItem(root, "items");

    float videos_score = 0.f;
    int videos_index = find_match(searchResults, title, artist, &videos_score);

    if (videos_score < 0.05f && music_score < 0.05f) {
        fprintf(stderr,
                "[downloader] No matching youtube video was found (very wierd) (url: %s)\n",
                url);
        cJSON_Delete(root);
        free(url);
        url = NULL;
        free(response.data);
        free(url_temp);
        return 1;
    }

    if (url_temp && music_score >= videos_score) {
        *url_out = url_temp;
    } else {
        if (url_temp) free(url_temp);
        *url_out = strdup(cJSON_GetObjectItem(cJSON_GetArrayItem(searchResults, videos_index), "url")->valuestring);
    }

    free(url);
    cJSON_Delete(root);
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