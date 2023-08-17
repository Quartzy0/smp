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
#include <errno.h>

#include <immintrin.h>

#define ERR_NULL(p, r) if (!(p)){ fprintf(stderr, "Error occurred in " __FILE__ ":%d : %s\n", __LINE__, strerror(errno)); return r; }

static const enum AudioThreadSignal TRACK_OVER_SIG = AUDIO_THREAD_SIGNAL_TRACK_OVER;
static const enum AudioThreadSignal SEEKED_SIG = AUDIO_THREAD_SIGNAL_SEEKED;

struct audio_context {
    bool status: 1;
    bool started: 1;
    double volume;
    int64_t seek;

    PaStream *stream;
    struct buffer *audio_buf;

    int track_over_fd;

    struct audio_info {
        size_t sample_rate;
        size_t bitrate;
        size_t total_frames;
        int channels;
        bool finished_reading;
    } audio_info;
    struct audio_info previous;
};

static
int
callback(const void *input, void *output, unsigned long frameCount, const PaStreamCallbackTimeInfo *timeInfo,
         PaStreamCallbackFlags statusFlags, void *userData) {
    float *out = (float *) output;
    struct audio_context *ctx = (struct audio_context *) userData;

    /* clear output buffer */
    if (ctx->audio_info.channels) memset(out, 0, sizeof(short) * frameCount * ctx->audio_info.channels);

    if (!ctx->started || !ctx->status || !ctx->audio_buf->len || !ctx->audio_info.channels ||
        (ctx->audio_info.finished_reading &&
         ctx->audio_info.total_frames <=
         ctx->audio_buf->offset))
        return paContinue;

    if (ctx->seek != 0) {
        int64_t seek_offset = (int64_t) ((((double) ctx->seek) * 0.000001) *
                                         (double) ctx->audio_info.sample_rate);
        if (seek_offset < 0) {
            ctx->audio_buf->offset =
                    -seek_offset > ctx->audio_buf->offset ? 0 : ctx->audio_buf->offset + seek_offset;
        } else if (ctx->audio_info.finished_reading) {
            ctx->audio_buf->offset += seek_offset;
            if (ctx->audio_buf->offset >= ctx->audio_info.total_frames) {
                write(ctx->track_over_fd, &SEEKED_SIG, sizeof(SEEKED_SIG));
                write(ctx->track_over_fd, &TRACK_OVER_SIG, sizeof(TRACK_OVER_SIG));
                ctx->seek = 0;
                return paContinue;
            }
        } else {
            int64_t possible_seek = ctx->audio_info.total_frames - ctx->audio_buf->offset;
            if (possible_seek < seek_offset) {
                ctx->audio_buf->offset += possible_seek;
                ctx->seek = (int64_t) (
                        (double) ((seek_offset - possible_seek) / (double) ctx->audio_info.sample_rate) /
                        0.000001);
                goto nozero;
            } else {
                ctx->audio_buf->offset += seek_offset;
            }
        }
        ctx->seek = 0;
        nozero:
        write(ctx->track_over_fd, &SEEKED_SIG, sizeof(SEEKED_SIG));
    }

    size_t num_read =
            frameCount < (ctx->audio_info.total_frames - ctx->audio_buf->offset)
            ? frameCount : (ctx->audio_info.total_frames - ctx->audio_buf->offset);
    num_read *= ctx->audio_info.channels;
    float *s_buf = &ctx->audio_buf->buf[ctx->audio_buf->offset * ctx->audio_info.channels];

    ctx->audio_buf->offset += num_read / ctx->audio_info.channels;
    const double volumeDb = -6.0;
    const float volumeMultiplier = (float) (ctx->volume * pow(10.0, (volumeDb / 20.0)));
    uint32_t i = 0;

#ifdef __AVX__
    {
        const __m256 multiplier = _mm256_set1_ps(volumeMultiplier);
        for (i = 0; i < num_read >> 3; ++i) {
            __m256 x = _mm256_loadu_ps(s_buf + (i << 3));
            __m256 res = _mm256_mul_ps(x, multiplier);
            _mm256_storeu_ps(out + (i << 3), res);
        }
    }
#endif
#ifdef __SSE__
    {
        const __m128 multiplier = _mm_set1_ps(volumeMultiplier);
        for (i <<= 3; i < num_read >> 2; ++i) {
            __m128 x = _mm_loadu_ps(s_buf + (i << 2));
            __m128 res = _mm_mul_ps(x, multiplier);
            _mm_storeu_ps(out + (i << 2), res);
        }
    }
#endif

    for (i <<= 2; i < num_read; ++i) {
        out[i] = s_buf[i] * volumeMultiplier;
    }
    if (ctx->audio_info.finished_reading &&
        ctx->audio_info.total_frames <= ctx->audio_buf->offset) {
        write(ctx->track_over_fd, &TRACK_OVER_SIG, sizeof(TRACK_OVER_SIG));
    }

    return paContinue;
}

struct audio_context *
audio_init(struct buffer *audio_buf, int track_over_fd) {
    PaError error;
    /* init portaudio */
    error = Pa_Initialize();
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem initializing\n");
        return NULL;
    }
    struct audio_context *data = calloc(1, sizeof(*data));
    data->audio_buf = audio_buf;
    data->track_over_fd = track_over_fd;
    return data;
}

int audio_start(struct audio_context *ctx, struct audio_info *info, struct audio_info *previous) {
    ctx->audio_buf->offset = 0;
    ctx->audio_buf->len = 0;
    if (ctx->started && previous && previous->channels == info->channels &&
        previous->sample_rate == info->sample_rate) {
        printf("[audio] Using same audio stream\n");
        return audio_play(ctx);
    }

    printf("[audio] Starting audio new stream\n");
    memcpy(previous, info, sizeof(*info));
    audio_stop(ctx);

    ctx->started = true;
    return audio_play(ctx);
}

int
audio_pause(struct audio_context *ctx) {
    if (!ctx->status || !ctx->started) return 0;
    ctx->status = false;
    PaError error = Pa_AbortStream(ctx->stream);
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem stopping Stream: %s\n", Pa_GetErrorText(error));
        return 1;
    }

    error = Pa_CloseStream(ctx->stream);
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem closing stream\n");
        return 1;
    }
    return 0;
}

int audio_play(struct audio_context *ctx) {
    if (ctx->status || !ctx->started) return 0;
    ctx->status = true;
    PaError error = Pa_OpenDefaultStream(&ctx->stream, 0, ctx->audio_info.channels, paFloat32,
                                         (double) ctx->audio_info.sample_rate, FRAMES_PER_BUFFER, callback,
                                         ctx);
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem opening Default Stream\n");
        return 1;
    }

    error = Pa_StartStream(ctx->stream);
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem starting Stream: %s\n", Pa_GetErrorText(error));
        return 1;
    }
    return 0;
}

int audio_stop(struct audio_context *ctx) {
    if (!ctx->started) return 0;
    int ret = audio_pause(ctx);
    ctx->started = false;

    return ret;
}

int audio_clean(struct audio_context *ctx) {
    audio_stop(ctx);

    PaError error = Pa_Terminate();
    if (error != paNoError) {
        fprintf(stderr, "[audio] Problem terminating\n");
        return 1;
    }
    free(ctx->audio_buf->buf);
    memset(ctx->audio_buf, 0, sizeof(*ctx->audio_buf));
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
    return 0;
}

void audio_seek(struct audio_context *ctx, int64_t position) {
    ctx->seek = position;
}

void audio_seek_to(struct audio_context *ctx, int64_t position) {
    if (!ctx->started) return;
    if (ctx->audio_info.finished_reading &&
        (position > (int64_t) (ctx->audio_info.total_frames / ctx->audio_info.sample_rate) * 1000000 ||
         position < 0))
        return;
    ctx->seek = -((int64_t) (((double) ctx->audio_buf->offset / (double) ctx->audio_info.sample_rate) *
                             1000000.0) -
                  position);
}

int64_t audio_get_position(struct audio_context *ctx) {
    return (int64_t) (((double) ctx->audio_buf->offset / (double) ctx->audio_info.sample_rate) *
                      1000000.0);;
}

bool audio_started(struct audio_context *ctx) {
    return ctx->started;
}

bool audio_playing(struct audio_context *ctx) {
    return ctx->status;
}

double audio_get_volume(struct audio_context *ctx) {
    return ctx->volume;
}

void audio_set_volume(struct audio_context *ctx, double volume) {
    ctx->volume = volume;
}

void audio_info_set(struct audio_info *info, size_t sample_rate, size_t bitrate, int channels) {
    memset(info, 0, sizeof(*info));
    info->sample_rate = sample_rate;
    info->bitrate = bitrate;
    info->channels = channels;
}

void audio_info_set_finished(struct audio_info *info) {
    info->finished_reading = true;
}

void audio_info_add_frames(struct audio_info *info, size_t frames) {
    info->total_frames += frames;
}

struct audio_info *audio_get_info(struct audio_context *ctx) {
    return &ctx->audio_info;
}

struct audio_info *audio_get_info_prev(struct audio_context *ctx) {
    return &ctx->previous;
}

#endif