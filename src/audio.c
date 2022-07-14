//
// Created by quartzy on 4/30/22.
//

#include <portaudio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <semaphore.h>
#include "audio.h"
#include "util.h"
#include "dbus.h"
#include "stb_vorbis.c"

typedef struct {
    int channels;
    int sample_rate;
    short *buffer;
    size_t offset;
    size_t buffer_size;
} callback_data_s;

PaStream *stream;
callback_data_s data;
int status = 0;
bool started = false;
double volume = 0.1;
int64_t seek = 0;
size_t offset = 0;
size_t frames = 0;
int audio_samplerate = 0;
char *current_file;

static
int
callback(const void *input, void *output, unsigned long frameCount, const PaStreamCallbackTimeInfo *timeInfo,
         PaStreamCallbackFlags statusFlags, void *userData) {
    short *out;
    callback_data_s *p_data;
    size_t num_read;

    out = (short *) output;
    p_data = (callback_data_s *) userData;

    /* clear output buffer */
    memset(out, 0, sizeof(short) * frameCount * p_data->channels);

    if (!p_data->buffer) {
        return paComplete;
    }

    if (seek != 0) {
        p_data->offset += (int64_t) ((((double) seek) * 0.000001) * (double) data.sample_rate);
        seek = 0;
        if (p_data->offset < 0) p_data->offset = 0;
        else if (p_data->offset >= p_data->buffer_size) {
            rear.type = ACTION_TRACK_OVER;
            last_rear.position = 1;
            sem_post(&state_change_lock);
            return paComplete;
        }
    }

    num_read = frameCount < p_data->buffer_size - p_data->offset ? frameCount : p_data->buffer_size - p_data->offset;
    const double volumeDb = -6.0;
    const float volumeMultiplier = (float) (volume * pow(10.0, (volumeDb / 20.0)));
    for (size_t i = 0; i < num_read * p_data->channels; ++i) {
        out[i] = p_data->buffer[p_data->offset * p_data->channels + i] * volumeMultiplier;
    }

    p_data->offset += num_read;
    offset = p_data->offset;

    /*  If we couldn't read a full frameCount of samples we've reached EOF */
    if (num_read < frameCount) {
        rear.type = ACTION_TRACK_OVER;
        last_rear.position = 1;
        sem_post(&state_change_lock);
        return paComplete;
    }

    return paContinue;
}

int init() {
    PaError error;
    /* init portaudio */
    error = Pa_Initialize();
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem initializing\n");
        return 1;
    }
    return 0;
}

int
set_file(const char *filename) {
    if (current_file && !strcmp(filename, current_file)) {
        printf("[audio] File '%s' is already loaded. Reusing buffer.\n", filename);
        data.offset = 0;
        return 0;
    }
    printf("[audio] Setting file to '%s'\n", filename);
    current_file = strdup(filename);
    free(data.buffer);
    memset(&data, 0, sizeof(data));

    /* Open the soundfile */
    data.buffer_size = stb_vorbis_decode_filename(filename, &data.channels, &data.sample_rate, &data.buffer);
    if (!data.buffer_size) {
        fprintf(stderr, "Couldn't open %s", filename);
    }
    frames = data.buffer_size;
    audio_samplerate = data.sample_rate;
    return 0;
}

int start() {
    /* Open PaStream with values read from the file */
    PaError error = Pa_OpenDefaultStream(&stream, 0                     /* no input */
            , data.channels         /* stereo out */
            , paInt16             /* floating point */
            , data.sample_rate, FRAMES_PER_BUFFER, callback, &data);        /* our sndfile data struct */
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem opening Default Stream\n");
        return 1;
    }
    started = 1;
    return 0;
}

int play() {
    /* Start the stream */
    PaError error = Pa_StartStream(stream);
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem opening starting Stream\n");
        return 1;
    }
    status = 1;
    return 0;
}

int pause() {
    if (!status) return 0;
    PaError error = Pa_StopStream(stream);
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem stopping stream\n");
        return 1;
    }
    status = 0;
    return 0;
}

int stop() {
    if (!started) return 0;
    status = 0;
    started = 0;

    /*  Shut down portaudio */
    PaError error = Pa_CloseStream(stream);
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem closing stream\n");
        return 1;
    }

    return 0;
}

int clean_audio() {
    if (status) {
        pause();
        stop();
    }

    PaError error = Pa_Terminate();
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem terminating\n");
        return 1;
    }
    free(data.buffer);
    free(current_file);
    return 0;
}
