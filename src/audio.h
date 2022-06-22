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
extern bool inited;

#define FRAMES_PER_BUFFER   (512)

int init();

int set_file(const char *filename);

int start();

void block();

int stop();

int pause();

int play();

void sleep_audio(int i);

int clean_audio();

#endif //SMP_AUDIO_H
