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

    char *video_id = strrchr(video_url, '=')+1;
    rek_mkdir(params->path_out);
    download_part:
    if (download_from_id(random_download_instance, params->path_out, video_id, params->track->spotify_id)) {
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

FILE *
search_and_get_pcm(DownloadParams *params){
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

    char *video_id = strrchr(video_url, '=')+1;
    FILE *pcm;
    download_part:
    if (!(pcm = get_pcm_stream_from_id(random_download_instance, params->path_out, video_id, params->track->spotify_id))) {
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
    return pcm;
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
    if (music_score < 0.6f)
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
download_from_id(char *instance, char *path_out, char *id, char *save_name) {
    const size_t instance_len = strlen(instance);
    const size_t path_out_len = strlen(path_out);
    const size_t id_len = strlen(id);
    const size_t save_name_len = strlen(save_name);

    printf("[downloader] Downloading from youtube with id: %s\n", id);

    char *cmd = malloc((172 + instance_len + id_len + save_name_len + path_out_len + 1) * sizeof(char));
    snprintf(cmd, (172 + instance_len + id_len + save_name_len + path_out_len + 1) * sizeof(char),
             "ffmpeg -headers \"Accept-Language: en-US,en;q=0.5\\r\\nAccept-Encoding: gzip, deflate, br\\r\\n\" -i \"%s/latest_version?id=%s&itag=249&local=true\" -loglevel error -ac 2 -f ogg \"%s%s.ogg\"",
             instance, id, path_out, save_name);
    int status = system(cmd);
    free(cmd);
    printf("[downloader] Finished downloading\n");
    return WEXITSTATUS(status);
}

FILE*
get_pcm_stream_from_id(char *instance, char *path_out, char *id, char *save_name) {
    const size_t instance_len = strlen(instance);
    const size_t path_out_len = strlen(path_out);
    const size_t id_len = strlen(id);
    const size_t save_name_len = strlen(save_name);

    char *cmd = malloc((215 + instance_len + path_out_len + save_name_len + id_len + 1) * sizeof(char));
    snprintf(cmd, (215 + instance_len + path_out_len + save_name_len + id_len + 1) * sizeof(char),
             "ffmpeg -headers \"Accept-Language: en-US,en;q=0.5\\r\\nAccept-Encoding: gzip, deflate, br\\r\\n\" -i \"%s/latest_version?id=%s&itag=249&local=true\" -loglevel error -acodec pcm_s16le -ac 2 -f s16le \"pipe:\" -ac 2 -f ogg \"%s%s.ogg\"",
             instance, id, path_out, save_name);
    printf("[downloader] Getting PCM stream from id: %s with command: %s\n", id, cmd);
    FILE *fp = popen(cmd, "r");
    free(cmd);
    return fp;
}