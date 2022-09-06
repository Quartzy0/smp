//
// Created by quartzy on 6/24/22.
//

#ifndef SMP_DOWNLOADER_H
#define SMP_DOWNLOADER_H

#include <stdbool.h>
#include <stdio.h>
#include "spotify.h"

typedef struct DownloadParams {
    char *path_out;
    Track *track;
} DownloadParams;

void *search_and_download(void *userp);

FILE *search_and_get_pcm(DownloadParams *params);

int search_for_track(char *instance, char **url_out, char *query, char *title, char *artist);

int download_from_id(char *instance, char *path_out, char *id, char *save_name);

FILE *get_pcm_stream_from_id(char *instance, char *path_out, char *id, char *save_name);

#endif //SMP_DOWNLOADER_H
