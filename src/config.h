//
// Created by quartzy on 6/23/22.
//

#ifndef SMP_CONFIG_H
#define SMP_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern uint32_t preload_amount;
extern char *track_save_path;
extern size_t track_save_path_len;
extern char *track_info_path;
extern size_t track_info_path_len;
extern char *album_info_path;
extern size_t album_info_path_len;
extern char *playlist_info_path;
extern size_t playlist_info_path_len;
extern double initial_volume;

extern struct backend_instance{
    char *host;
    char *regions;
    size_t region_count;
    bool disabled;
} *backend_instances;

extern size_t backend_instance_count;

#define random_backend_instance (&backend_instances[(int) (((float) rand()/(float) RAND_MAX) * (float) backend_instance_count)])

int load_config();

void clean_config();

#endif //SMP_CONFIG_H
