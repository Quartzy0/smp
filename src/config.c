//
// Created by quartzy on 6/23/22.
//

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "config.h"
#include "confuse.h"

uint32_t preload_amount;
char *track_save_path;
size_t track_save_path_len;
char *playlist_save_path;
size_t playlist_save_path_len;
double initial_volume;

const char config_home_suffix[] = "/.config/";
const size_t config_home_suffix_len = sizeof(config_home_suffix) - 1;
const char conf_file_suffix[] = "/smp/smp.conf";
const size_t conf_file_suffix_len = sizeof(conf_file_suffix) - 1;

const char tracks_home_suffix[] = "/.cache/smp/tracks/";
const size_t tracks_home_suffix_len = sizeof(tracks_home_suffix) - 1;
const char tracks_file_suffix[] = "/smp/tracks/";
const size_t tracks_file_suffix_len = sizeof(tracks_file_suffix) - 1;

const char data_home_suffix[] = "/.local/share/smp/playlists/";
const size_t data_home_suffix_len = sizeof(data_home_suffix) - 1;
const char data_file_suffix[] = "/smp/playlists/";
const size_t data_file_suffix_len = sizeof(data_file_suffix) - 1;

cfg_t *cfg;
bool appended_tracks = false, appended_playlists = false;

int
load_config() {
    char *home = getenv("HOME");
    char *cache_home_env = getenv("XDG_CACHE_HOME");
    char *cache_home = NULL;
    if (!cache_home_env && home) {
        cache_home = malloc(strlen(home) * sizeof(*home) + tracks_home_suffix_len);
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
        data_home = malloc(strlen(home) * sizeof(*home) + data_home_suffix_len);
        memcpy(data_home, home, strlen(home) * sizeof(*home));
        memcpy(data_home + strlen(home) * sizeof(*home), data_home_suffix,
               data_home_suffix_len + 1 /* Also copy null byte */);
    } else if (data_home_env) {
        data_home = malloc(strlen(data_home_env) * sizeof(*data_home_env) + data_file_suffix_len + 1);
        memcpy(data_home, data_home_env, strlen(data_home_env) * sizeof(*data_home_env));
        memcpy(data_home + strlen(data_home_env) * sizeof(*data_home_env), data_file_suffix, data_file_suffix_len + 1);
    }

    cfg_opt_t opts[] =
            {
                    CFG_INT("preload_amount", 3, CFGF_NONE),
                    CFG_STR("track_save_path", cache_home, CFGF_NONE),
                    CFG_STR("playlist_save_path", data_home, CFGF_NONE),
                    CFG_FLOAT("initial_volume", 0.1f, CFGF_NONE),
                    CFG_END()
            };

    cfg = cfg_init(opts, CFGF_NONE);
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
    }else{
        config_home = config_home_tmp;
    }
    char *config_file = malloc(strlen(config_home) * sizeof(*config_home) + conf_file_suffix_len + 1);
    memcpy(config_file, config_home, strlen(config_home) * sizeof(*config_home));
    memcpy(config_file + strlen(config_home) * sizeof(*config_home), conf_file_suffix, conf_file_suffix_len + 1);
    if (config_home!=config_home_tmp) free(config_home);

    if (cfg_parse(cfg, config_file) == CFG_PARSE_ERROR) {
        fprintf(stderr, "[config] Error when parsing config file.\n");
        free(config_file);
        free(cache_home);
        free(data_home);
        return 1;
    }
    free(config_file);

    preload_amount = cfg_getint(cfg, "preload_amount");
    char *track_save_path_tmp = cfg_getstr(cfg, "track_save_path");
    if (!track_save_path_tmp) {
        fprintf(stderr,
                "[config] Track save path was not set and neither were the environment variables XDG_DATA_HOME or HOME\n");
        return 1;
    }
    track_save_path_len = strlen(track_save_path_tmp);
    if (track_save_path_tmp[track_save_path_len-1]!='/') {
        appended_tracks = true;
        track_save_path = malloc(track_save_path_len + 2);
        memcpy(track_save_path, track_save_path_tmp, track_save_path_len);
        track_save_path[track_save_path_len] = '/';
        track_save_path[track_save_path_len + 1] = 0;
        track_save_path_len = strlen(track_save_path);
    }else{
        if (track_save_path_tmp!=cache_home) free(cache_home);
        track_save_path = track_save_path_tmp;
    }

    char *playlist_save_path_tmp = cfg_getstr(cfg, "playlist_save_path");
    if (!playlist_save_path_tmp) {
        fprintf(stderr,
                "[config] Playlist save path was not set and neither were the environment variables XDG_DATA_HOME or HOME\n");
        return 1;
    }
    playlist_save_path_len = strlen(playlist_save_path_tmp);
    if (playlist_save_path_tmp[playlist_save_path_len-1]!='/'){
        appended_playlists = true;
        playlist_save_path = malloc(playlist_save_path_len + 2);
        memcpy(playlist_save_path, playlist_save_path_tmp, playlist_save_path_len);
        playlist_save_path[playlist_save_path_len] = '/';
        playlist_save_path[playlist_save_path_len + 1] = 0;
        playlist_save_path_len = strlen(playlist_save_path);
    }else{
        if (playlist_save_path_tmp!=data_home) free(data_home);
        playlist_save_path = playlist_save_path_tmp;
    }
    initial_volume = cfg_getfloat(cfg, "initial_volume");
    printf("[config] Loaded values from config:\n - preload_amount: %d\n - track_save_path: %s\n - playlist_save_path: %s\n - initial_volume: %f\n",
           preload_amount, track_save_path, playlist_save_path, initial_volume);
    return 0;
}

void
clean_config(){
    cfg_free(cfg);
    if (appended_playlists) free(playlist_save_path);
    if (appended_tracks) free(track_save_path);
}