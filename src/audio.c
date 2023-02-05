//
// Created by quartzy on 4/30/22.
//

#ifndef PIPEWIRE_BACKEND

#include <portaudio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include "audio.h"
#include "util.h"
#include "dbus.h"
#include "stb_vorbis.c"

typedef struct {
    struct smp_context *ctx;
    struct buffer *audio_buf;
} callback_data_s;

PaStream *stream;
callback_data_s data;

int status;
bool started;
double volume;
int64_t seek;

static
int
callback(const void *input, void *output, unsigned long frameCount, const PaStreamCallbackTimeInfo *timeInfo,
         PaStreamCallbackFlags statusFlags, void *userData) {
    float *out = (float *) output;
    callback_data_s *p_data = (callback_data_s *) userData;

    /* clear output buffer */
    memset(out, 0, sizeof(short) * frameCount * p_data->ctx->audio_info.channels);

    if (!p_data->audio_buf->len || !p_data->ctx->audio_info.channels || (p_data->ctx->audio_info.finished_reading &&
                                                                         p_data->ctx->audio_info.total_frames <=
                                                                         p_data->audio_buf->offset))
        return paContinue;

    if (seek != 0) {
        int64_t seek_offset = (int64_t) ((((double) seek) * 0.000001) *
                                         (double) p_data->ctx->audio_info.sample_rate);
        if (seek_offset < 0) {
            p_data->ctx->audio_buf.offset =
                    -seek_offset > p_data->ctx->audio_buf.offset ? 0 : p_data->ctx->audio_buf.offset + seek_offset;
        } else if (p_data->ctx->audio_info.finished_reading) {
            p_data->ctx->audio_buf.offset += seek_offset;
            if (p_data->ctx->audio_buf.offset >= p_data->ctx->audio_info.total_frames) {
                Action a = {.type = ACTION_TRACK_OVER};
                a.position = 1;
                write(p_data->ctx->action_fd[1], &a, sizeof(a));
                seek = 0;
                return paContinue;
            }
        } else {
            int64_t possible_seek = p_data->ctx->audio_info.total_frames - p_data->ctx->audio_buf.offset;
            if (possible_seek < seek_offset) {
                p_data->ctx->audio_buf.offset += possible_seek;
                seek = (int64_t) (
                        (double) ((seek_offset - possible_seek) / (double) p_data->ctx->audio_info.sample_rate) /
                        0.000001);
                goto nozero;
            } else {
                p_data->ctx->audio_buf.offset += seek_offset;
            }
        }
        seek = 0;
        nozero:;
    }

    size_t num_read =
            frameCount < (p_data->ctx->audio_info.total_frames - p_data->audio_buf->offset)
            ? frameCount : (p_data->ctx->audio_info.total_frames - p_data->audio_buf->offset);
    num_read *= p_data->ctx->audio_info.channels;
    memcpy(out, &p_data->audio_buf->buf[p_data->audio_buf->offset*p_data->ctx->audio_info.channels], num_read * sizeof(*p_data->audio_buf->buf));
    p_data->audio_buf->offset += num_read / p_data->ctx->audio_info.channels;
    const double volumeDb = -6.0;
    const float volumeMultiplier = (float) (volume * pow(10.0, (volumeDb / 20.0)));
    for (uint32_t i = 0; i < num_read; ++i) {
        out[i] *= volumeMultiplier;
    }
    if (p_data->ctx->audio_info.finished_reading &&
        p_data->ctx->audio_info.total_frames <= p_data->audio_buf->offset) {
        Action a = {.type = ACTION_TRACK_OVER};
        a.position = 1;
        write(p_data->ctx->action_fd[1], &a, sizeof(a));
        return paContinue;
    }

    /*if (seek != 0) {
        p_data->offset += (int64_t) ((((double) seek) * 0.000001) * (double) p_data.sample_rate);
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

    *//*  If we couldn't read a full frameCount of samples we've reached EOF *//*
    if (num_read < frameCount) {
        rear.type = ACTION_TRACK_OVER;
        last_rear.position = 1;
        sem_post(&state_change_lock);
        return paComplete;
    }*/

    return paContinue;
}

int init(struct smp_context *ctx, struct buffer *audio_buf) {
    PaError error;
    data.ctx = ctx;
    data.audio_buf = audio_buf;
    /* init portaudio */
    error = Pa_Initialize();
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem initializing\n");
        return 1;
    }
    return 0;
}

int start(struct audio_info *info, struct audio_info *previous) {
    data.audio_buf->offset = 0;
    data.audio_buf->len = 0;
    if (started && previous && previous->channels == info->channels &&
        previous->sample_rate == info->sample_rate) {
        printf("[audio] Using same audio stream\n");
        return play();
    }

    memcpy(previous, info, sizeof(*info));
    stop();
    printf("[audio] Starting audio new stream\n");
    PaError error = Pa_OpenDefaultStream(&stream, 0, data.ctx->audio_info.channels, paFloat32,
                                         data.ctx->audio_info.sample_rate, FRAMES_PER_BUFFER, callback,
                                         &data);
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem opening Default Stream\n");
        return 1;
    }
    started = 1;
    return play();
}

int play() {
    if (status) return 0;
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
    pause();
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
    free(data.audio_buf->buf);
    memset(data.audio_buf, 0, sizeof(*data.audio_buf));
    return 0;
}

#endif