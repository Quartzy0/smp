//
// Created by quartzy on 6/23/22.
//

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include "config.h"
#include "util.h"
#include "../lib/cjson/cJSON.h"

uint32_t preload_amount;
char *track_save_path;
size_t track_save_path_len;
char *track_info_path = "/home/quartzy/dev/smp/debug/cache/smp/info/";
size_t track_info_path_len = 43;
char *playlist_save_path;
size_t playlist_save_path_len;
double initial_volume;
char **piped_api_instances;
size_t piped_api_instance_count;
char **download_instances;
size_t download_instance_count;

const char config_home_suffix[] = "/.config/";
const size_t config_home_suffix_len = sizeof(config_home_suffix) - 1;
const char conf_file_suffix[] = "/smp/smp.json";
const size_t conf_file_suffix_len = sizeof(conf_file_suffix) - 1;

const char tracks_home_suffix[] = "/.cache/smp/tracks/";
const size_t tracks_home_suffix_len = sizeof(tracks_home_suffix) - 1;
const char tracks_file_suffix[] = "/smp/tracks/";
const size_t tracks_file_suffix_len = sizeof(tracks_file_suffix) - 1;

const char data_home_suffix[] = "/.local/share/smp/playlists/";
const size_t data_home_suffix_len = sizeof(data_home_suffix) - 1;
const char data_file_suffix[] = "/smp/playlists/";
const size_t data_file_suffix_len = sizeof(data_file_suffix) - 1;

bool appended_tracks = false, appended_playlists = false;

#define cJSON_GetDefault(object, name, type, def) (cJSON_HasObjectItem(object, name) ? cJSON_GetObjectItem(object, name)->value ## type : (def))

int
load_config() {
    char *home = getenv("HOME");
    char *cache_home_env = getenv("XDG_CACHE_HOME");
    char *cache_home = NULL;
    if (!cache_home_env && home) {
        cache_home = malloc(strlen(home) * sizeof(*home) + tracks_home_suffix_len + 1);
        memcpy(cache_home, home, strlen(home) * sizeof(*home));
        memcpy(cache_home + strlen(home) * sizeof(*home), tracks_home_suffix,
               tracks_home_suffix_len + 1 /* Also copy null byte */);
    } else if (cache_home_env) {
        cache_home = malloc(strlen(cache_home_env) * sizeof(*cache_home_env) + tracks_file_suffix_len + 1);
        memcpy(cache_home, cache_home_env, strlen(cache_home_env) * sizeof(*cache_home_env));
        memcpy(cache_home + strlen(cache_home_env) * sizeof(*cache_home_env), tracks_file_suffix,
               tracks_file_suffix_len + 1);
    }
    char *data_home_env = getenv("XDG_DATA_HOME");
    char *data_home = NULL;
    if (!data_home_env && home) {
        data_home = malloc(strlen(home) * sizeof(*home) + data_home_suffix_len + 1);
        memcpy(data_home, home, strlen(home) * sizeof(*home));
        memcpy(data_home + strlen(home) * sizeof(*home), data_home_suffix,
               data_home_suffix_len + 1 /* Also copy null byte */);
    } else if (data_home_env) {
        data_home = malloc(strlen(data_home_env) * sizeof(*data_home_env) + data_file_suffix_len + 1);
        memcpy(data_home, data_home_env, strlen(data_home_env) * sizeof(*data_home_env));
        memcpy(data_home + strlen(data_home_env) * sizeof(*data_home_env), data_file_suffix, data_file_suffix_len + 1);
    }

    char *config_home_tmp = getenv("XDG_CONFIG_HOME");
    char *config_home = NULL;
    if (!config_home_tmp) {
        if (!home) {
            fprintf(stderr,
                    "[config] Could not determine the location of the config file. Environment variables XDG_CONFIG_HOME and HOME are both undefined.\n");
            return 1;
        }
        config_home = malloc(strlen(home) * sizeof(*home) + config_home_suffix_len + 1);
        memcpy(config_home, home, strlen(home) * sizeof(*home));
        memcpy(config_home + strlen(home) * sizeof(*home), config_home_suffix,
               config_home_suffix_len + 1 /* Also copy null byte */);
    } else {
        config_home = config_home_tmp;
    }
    char *config_file = malloc(strlen(config_home) * sizeof(*config_home) + conf_file_suffix_len + 1);
    memcpy(config_file, config_home, strlen(config_home) * sizeof(*config_home));
    memcpy(config_file + strlen(config_home) * sizeof(*config_home), conf_file_suffix, conf_file_suffix_len + 1);
    if (config_home != config_home_tmp) free(config_home);

    cJSON *config_root = NULL;
    char *data = NULL;
    if (access(config_file, R_OK)) {
        printf("[config] No config file found, using defaults.\n");
        goto no_file;
    }
    FILE *fp = fopen_mkdir(config_file, "r");
    if (!fp){
        fprintf(stderr, "[config] Error opening file %s: %s\n", config_file, strerror(errno));
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    data = (char *) malloc(size + 1);
    fread(data, 1, size, fp);
    data[size] = '\0';
    fclose(fp);

    config_root = cJSON_ParseWithLength(data, size);
    if (!config_root){
        fprintf(stderr, "[config] Error while parsing JSON\n");
        return 1;
    }
    no_file:
    free(config_file);

    preload_amount = cJSON_GetDefault(config_root, "preload_amount", int, 3);
    char *track_save_path_tmp = cJSON_GetDefault(config_root, "track_save_path", string, cache_home);
    if (!track_save_path_tmp) {
        fprintf(stderr,
                "[config] Track save path was not set and neither were the environment variables XDG_DATA_HOME or HOME\n");
        return 1;
    }
    track_save_path_len = strlen(track_save_path_tmp);
    if (track_save_path_tmp[track_save_path_len - 1] != '/') {
        appended_tracks = true;
        track_save_path = malloc(track_save_path_len + 2);
        memcpy(track_save_path, track_save_path_tmp, track_save_path_len);
        track_save_path[track_save_path_len] = '/';
        track_save_path[track_save_path_len + 1] = 0;
        track_save_path_len = strlen(track_save_path);
    } else {
        track_save_path = strdup(track_save_path_tmp);
    }
    free(cache_home);

    char *playlist_save_path_tmp = cJSON_GetDefault(config_root, "playlist_save_path", string, data_home);
    if (!playlist_save_path_tmp) {
        fprintf(stderr,
                "[config] Playlist save path was not set and neither were the environment variables XDG_DATA_HOME or HOME\n");
        return 1;
    }
    playlist_save_path_len = strlen(playlist_save_path_tmp);
    if (playlist_save_path_tmp[playlist_save_path_len - 1] != '/') {
        appended_playlists = true;
        playlist_save_path = malloc(playlist_save_path_len + 2);
        memcpy(playlist_save_path, playlist_save_path_tmp, playlist_save_path_len);
        playlist_save_path[playlist_save_path_len] = '/';
        playlist_save_path[playlist_save_path_len + 1] = 0;
        playlist_save_path_len = strlen(playlist_save_path);
    } else {
        playlist_save_path = strdup(playlist_save_path_tmp);
    }
    free(data_home);

    initial_volume = cJSON_GetDefault(config_root, "initial_volume", double, 1.0);

    cJSON *v = NULL;
    if (!cJSON_HasObjectItem(config_root, "piped_api_instances") ||
        cJSON_GetArraySize((v = cJSON_GetObjectItem(config_root, "piped_api_instances"))) == 0) {
        /*Response res;
        read_url("https://piped-instances.kavin.rocks/", &res, NULL);

        if (!res.size) {
            fprintf(stderr,
                    "[config] Got empty response when trying to get piped instances. Defaulting to https://pipedapi.kavin.rocks\n");
            piped_api_instance_count = 1;
            piped_api_instances = calloc(piped_api_instance_count, sizeof(*piped_api_instances));
            piped_api_instances[0] = strdup("https://pipedapi.kavin.rocks");
            goto download_instances_part;
        }

        cJSON *root = cJSON_ParseWithLength(res.data, res.size);
        piped_api_instances = calloc(cJSON_GetArraySize(root), sizeof(*piped_api_instances));
        size_t i = 0;
        cJSON *element;
        cJSON_ArrayForEach(element, root) {
            piped_api_instances[i++] = strdup(cJSON_GetObjectItem(element, "api_url")->valuestring);
        }
        piped_api_instance_count = i;
        free(res.data);
        cJSON_Delete(root);*/
    } else {
        piped_api_instance_count = cJSON_GetArraySize(v);
        piped_api_instances = calloc(piped_api_instance_count, sizeof(*piped_api_instances));
        for (int i = 0; i < piped_api_instance_count; ++i) {
            piped_api_instances[i] = strdup(cJSON_GetArrayItem(v, i)->valuestring);
        }
    }

    download_instances_part:
    if (!cJSON_HasObjectItem(config_root, "download_instances") ||
        cJSON_GetArraySize((v = cJSON_GetObjectItem(config_root, "download_instances"))) == 0) {
        /*Response res;
        read_url("https://api.invidious.io/instances.json", &res, NULL);

        if (!res.size) {
            fprintf(stderr,
                    "[config] Got empty response when trying to get invidious instances. Defaulting to https://yewtu.be\n");
            download_instance_count = 1;
            download_instances = calloc(download_instance_count, sizeof(*download_instances));
            download_instances[0] = strdup("https://yewtu.be");
            free(res.data);
            goto end_config;
        }

        cJSON *root = cJSON_ParseWithLength(res.data, res.size);
        download_instances = calloc(cJSON_GetArraySize(root), sizeof(*download_instances));
        size_t i = 0;
        cJSON *element;
        cJSON_ArrayForEach(element, root) {
            if (strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(element, 1), "type")->valuestring, "https") == 0) {
                download_instances[i++] = strdup(
                        cJSON_GetObjectItem(cJSON_GetArrayItem(element, 1), "uri")->valuestring);
            }
        }
        download_instance_count = i;
        free(res.data);
        cJSON_Delete(root);*/
    } else {
        download_instance_count = cJSON_GetArraySize(v);
        download_instances = calloc(download_instance_count, sizeof(*download_instances));
        for (int i = 0; i < download_instance_count; ++i) {
            download_instances[i] = strdup(cJSON_GetArrayItem(v, i)->valuestring);
        }
    }

    end_config:
    free(data);
    cJSON_Delete(config_root);
    printf("[config] Loaded values from config:\n - preload_amount: %d\n - track_save_path: %s\n - playlist_save_path: %s\n - initial_volume: %f\n",
           preload_amount, track_save_path, playlist_save_path, initial_volume);
    printf(" - piped_api_instances: ");
    for (int i = 0; i < piped_api_instance_count; ++i) {
        if (i != 0) {
            printf(",");
        }
        printf("%s", piped_api_instances[i]);
    }
    printf("\n");
    printf(" - download_instances: ");
    for (int i = 0; i < download_instance_count; ++i) {
        if (i != 0) {
            printf(",");
        }
        printf("%s", download_instances[i]);
    }
    printf("\n");
    return 0;
}

void
clean_config() {
    free(playlist_save_path);
    free(track_save_path);
    for (int i = 0; i < piped_api_instance_count; ++i) {
        free(piped_api_instances[i]);
    }
    free(piped_api_instances);
    for (int i = 0; i < download_instance_count; ++i) {
        free(download_instances[i]);
    }
    free(download_instances);
}