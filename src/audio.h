//
// Created by quartzy on 4/30/22.
//

#ifndef SMP_AUDIO_H
#define SMP_AUDIO_H

#include <stdbool.h>
#include <stdio.h>
#include "util.h"

extern int status;
extern bool started;
extern double volume;
extern int64_t seek;

#define FRAMES_PER_BUFFER   (512)

int init(struct smp_context *ctx, struct buffer *audio_buf);

int start(struct audio_info *info, struct audio_info *previous);

int stop();

int pause();

int play();

int clean_audio();

#endif //SMP_AUDIO_H
