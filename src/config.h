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
extern char *track_info_path;
extern size_t track_info_path_len;
extern char *playlist_save_path;
extern size_t playlist_save_path_len;
extern double initial_volume;

extern char **piped_api_instances;
extern size_t piped_api_instance_count;
//Download and api instances are separate because the urls of the api's can be fetched dynamically while the urls of the
//ones to download from can not. https://piped-instances.kavin.rocks/ provides only the api urls, not the urls of the actual
//piped sites
extern char **download_instances;
extern size_t download_instance_count;

#define random_api_instance (piped_api_instances[(int) (((float) rand()/(float) RAND_MAX) * (float) piped_api_instance_count)])
#define random_download_instance (download_instances[(int) (((float) rand()/(float) RAND_MAX) * (float) download_instance_count)])

int load_config();

void clean_config();

#endif //SMP_CONFIG_H
