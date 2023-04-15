//
// Created by quartzy on 6/23/22.
//

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "config.h"
#include "util.h"
#include "../lib/cjson/cJSON.h"

uint32_t preload_amount;
char *track_save_path;
size_t track_save_path_len;
char *track_info_path;
size_t track_info_path_len;
char *album_info_path;
size_t album_info_path_len;
char *playlist_info_path;
size_t playlist_info_path_len;
double initial_volume;
struct backend_instance *backend_instances;
size_t backend_instance_count;

const char config_home_suffix[] = "/.config/";
const size_t config_home_suffix_len = sizeof(config_home_suffix) - 1;
const char conf_file_suffix[] = "/smp/smp.json";
const size_t conf_file_suffix_len = sizeof(conf_file_suffix) - 1;

const char smp_home_suffix[] = "/.cache/smp/";
const size_t tracks_home_suffix_len = sizeof(smp_home_suffix) - 1;
const char smp_file_suffix[] = "/smp/";
const size_t tracks_file_suffix_len = sizeof(smp_file_suffix) - 1;

const char tracks_path_suffix[] = "tracks/";
const size_t tracks_path_suffix_len = sizeof(tracks_path_suffix) - 1;
const char playlist_info_path_suffix[] = "playlist_info/";
const size_t playlist_info_path_suffix_len = sizeof(playlist_info_path_suffix) - 1;
const char album_info_path_suffix[] = "album_info/";
const size_t album_info_path_suffix_len = sizeof(album_info_path_suffix) - 1;
const char track_info_path_suffix[] = "track_info/";
const size_t track_info_path_suffix_len = sizeof(track_info_path_suffix) - 1;

#define cJSON_GetDefault(object, name, type, def) (cJSON_HasObjectItem(object, name) ? cJSON_GetObjectItem(object, name)->value ## type : (def))

void
get_path_config_value(char **out, cJSON *obj) {
    if (!obj) return;
    char *conf_value = cJSON_GetStringValue(obj);
    size_t conf_value_len = strlen(conf_value);
    if (conf_value[conf_value_len - 1] != '/') {
        *out = calloc(conf_value_len + 2, sizeof(*out));
        (*out)[conf_value_len] = '/';
    } else {
        *out = calloc(conf_value_len + 1, sizeof(*out));
    }
    memcpy(*out, conf_value, conf_value_len);
}

int
load_config() {
    char *home = getenv("HOME");
    char *cache_home_env = getenv("XDG_CACHE_HOME");
    char *cache_home = NULL;
    if (!cache_home_env && home) {
        cache_home = malloc(strlen(home) * sizeof(*home) + tracks_home_suffix_len + 1);
        memcpy(cache_home, home, strlen(home) * sizeof(*home));
        memcpy(cache_home + strlen(home) * sizeof(*home), smp_home_suffix,
               tracks_home_suffix_len + 1 /* Also copy null byte */);
    } else if (cache_home_env) {
        cache_home = malloc(strlen(cache_home_env) * sizeof(*cache_home_env) + tracks_file_suffix_len + 1);
        memcpy(cache_home, cache_home_env, strlen(cache_home_env) * sizeof(*cache_home_env));
        memcpy(cache_home + strlen(cache_home_env) * sizeof(*cache_home_env), smp_file_suffix,
               tracks_file_suffix_len + 1);
    }
    size_t cache_home_len = strlen(cache_home);

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
    if (!fp) {
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
    if (!config_root) {
        fprintf(stderr, "[config] Error while parsing JSON\n");
        free(cache_home);
        return 1;
    }
    no_file:
    free(config_file);

    preload_amount = cJSON_GetDefault(config_root, "preload_amount", int, 3);
    if (cJSON_HasObjectItem(config_root, "track_save_path")) {
        get_path_config_value(&track_save_path, cJSON_GetObjectItem(config_root, "track_save_path"));
    } else {
        track_save_path = calloc(cache_home_len + tracks_path_suffix_len + 1, sizeof(*track_save_path));
        memcpy(track_save_path, cache_home, cache_home_len);
        memcpy(&track_save_path[cache_home_len], tracks_path_suffix, tracks_path_suffix_len);
    }
    track_save_path_len = strlen(track_save_path);

    if (cJSON_HasObjectItem(config_root, "playlist_info_path")) {
        get_path_config_value(&playlist_info_path, cJSON_GetObjectItem(config_root, "playlist_info_path"));
    } else {
        playlist_info_path = calloc(cache_home_len + playlist_info_path_suffix_len + 1, sizeof(*playlist_info_path));
        memcpy(playlist_info_path, cache_home, cache_home_len);
        memcpy(&playlist_info_path[cache_home_len], playlist_info_path_suffix, playlist_info_path_suffix_len);
    }
    playlist_info_path_len = strlen(playlist_info_path);

    if (cJSON_HasObjectItem(config_root, "album_info_path")) {
        get_path_config_value(&album_info_path, cJSON_GetObjectItem(config_root, "album_info_path"));
    } else {
        album_info_path = calloc(cache_home_len + album_info_path_suffix_len + 1, sizeof(*album_info_path));
        memcpy(album_info_path, cache_home, cache_home_len);
        memcpy(&album_info_path[cache_home_len], album_info_path_suffix, album_info_path_suffix_len);
    }
    album_info_path_len = strlen(album_info_path);

    if (cJSON_HasObjectItem(config_root, "track_info_path")) {
        get_path_config_value(&track_info_path, cJSON_GetObjectItem(config_root, "track_info_path"));
    } else {
        track_info_path = calloc(cache_home_len + track_info_path_suffix_len + 1, sizeof(*track_info_path));
        memcpy(track_info_path, cache_home, cache_home_len);
        memcpy(&track_info_path[cache_home_len], track_info_path_suffix, track_info_path_suffix_len);
    }
    track_info_path_len = strlen(track_info_path);

    initial_volume = cJSON_GetDefault(config_root, "initial_volume", double, 1.0);

    cJSON *v = NULL;
    if (!cJSON_HasObjectItem(config_root, "backend_instances") ||
        cJSON_GetArraySize((v = cJSON_GetObjectItem(config_root, "backend_instances"))) == 0) {

    } else {
        backend_instance_count = cJSON_GetArraySize(v);
        backend_instances = calloc(backend_instance_count, sizeof(*backend_instances));
        for (int i = 0; i < backend_instance_count; ++i) {
            backend_instances[i].host = strdup(cJSON_GetArrayItem(v, i)->valuestring);
        }
    }

    free(data);
    cJSON_Delete(config_root);
    free(cache_home);
    printf("[config] Loaded values from config:\n - preload_amount: %d\n - track_save_path: %s\n - playlist_info_path: %s\n - album_info_path: %s\n - track_info_path: %s\n - initial_volume: %f\n",
           preload_amount, track_save_path, playlist_info_path, album_info_path, track_info_path, initial_volume);
    printf(" - backend_instances: ");
    for (int i = 0; i < backend_instance_count; ++i) {
        if (i != 0) {
            printf(",");
        }
        printf("%s", backend_instances[i].host);
    }
    printf("\n");
    return 0;
}

void
clean_config() {
    free(playlist_info_path);
    free(album_info_path);
    free(track_info_path);
    free(track_save_path);
    for (int i = 0; i < backend_instance_count; ++i) {
        free(backend_instances[i].regions);
        free(backend_instances[i].host);
    }
    free(backend_instances);
}