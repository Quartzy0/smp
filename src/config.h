//
// Created by quartzy on 6/23/22.
//

#ifndef SMP_CONFIG_H
#define SMP_CONFIG_H

#include <stdint.h>
#include <stddef.h>

extern uint32_t preload_amount;
extern char *track_save_path;
extern size_t track_save_path_len;
extern char *playlist_save_path;
extern size_t playlist_save_path_len;
extern double initial_volume;

int load_config();

void clean_config();

#endif //SMP_CONFIG_H
