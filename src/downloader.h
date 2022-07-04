//
// Created by quartzy on 6/24/22.
//

#ifndef SMP_DOWNLOADER_H
#define SMP_DOWNLOADER_H

#include <stdbool.h>
#include "spotify.h"

typedef struct DownloadParams {
    char *path_out;
    Track *track;
} DownloadParams;

void *search_and_download(void *userp);

int search_for_track(char *instance, char **id_out, char *query);

int download_from_id(char *instance, char *id, char *path_out_in, char *file_name);

#endif //SMP_DOWNLOADER_H
