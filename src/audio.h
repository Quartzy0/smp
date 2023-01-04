//
// Created by quartzy on 4/30/22.
//

#ifndef SMP_AUDIO_H
#define SMP_AUDIO_H

#include <stdbool.h>
#include <stdio.h>

extern int status;
extern bool started;
extern double volume;
extern int64_t seek;

#define FRAMES_PER_BUFFER   (512)

void init(struct smp_context *ctx, struct evbuffer *audio_buf);

int set_file(const char *filename);

int set_pcm_stream(FILE *fp);

int start(struct audio_info *info, struct audio_info *previous);

int stop();

int pause();

int play();

int clean_audio();

#endif //SMP_AUDIO_H
