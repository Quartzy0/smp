//
// Created by quartzy on 4/30/22.
//

#ifndef SMP_AUDIO_H
#define SMP_AUDIO_H

#include <stdbool.h>

extern int status;
extern bool started;
extern double volume;
extern int64_t seek;
extern size_t offset;
extern size_t frames;
extern int audio_samplerate;

#define FRAMES_PER_BUFFER   (512)

int init();

int set_file(const char *filename);

int start();

int stop();

int pause();

int play();

int clean_audio();

#endif //SMP_AUDIO_H
