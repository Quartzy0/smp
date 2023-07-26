//
// Created by quartzy on 7/23/22.
//

#ifdef PIPEWIRE_BACKEND

#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>
#include <math.h>
#include "dbus.h"
#include "util.h"
#include <errno.h>
#include <unistd.h>

#define ERR_NULL(p, r) if (!(p)){ fprintf(stderr, "Error occurred in " __FILE__ ":%d : %s\n", __LINE__, strerror(errno)); return r; }

static const uint8_t TRACK_OVER_SIG = 1;

struct audio_context {
    bool status: 1;
    bool started: 1;
    double volume;
    int64_t seek;

    struct pw_thread_loop *loop;
    struct pw_stream *stream;
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

static void on_process(void *userdata) {
    struct audio_context *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *dst;

    if (!data->started || !data->status || !data->audio_buf->len || !data->audio_info.channels ||
        (data->audio_info.finished_reading &&
         data->audio_info.total_frames <=
         data->audio_buf->offset))
        return;

    if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    if ((dst = buf->datas[0].data) == NULL)
        return;

    if (data->seek != 0) {
        int64_t seek_offset = (int64_t) ((((double) data->seek) * 0.000001) *
                                         (double) data->audio_info.sample_rate);
        if (seek_offset < 0) {
            data->audio_buf->offset =
                    -seek_offset > data->audio_buf->offset ? 0 : data->audio_buf->offset + seek_offset;
        } else if (data->audio_info.finished_reading) {
            data->audio_buf->offset += seek_offset;
            if (data->audio_buf->offset >= data->audio_info.total_frames) {
                write(data->track_over_fd, &TRACK_OVER_SIG, sizeof(TRACK_OVER_SIG));
                data->seek = 0;
                goto finish;
            }
        } else {
            int64_t possible_seek = data->audio_info.total_frames - data->audio_buf->offset;
            if (possible_seek < seek_offset) {
                data->audio_buf->offset += possible_seek;
                data->seek = (int64_t) (
                        (double) ((seek_offset - possible_seek) / (double) data->audio_info.sample_rate) /
                        0.000001);
                goto nozero;
            } else {
                data->audio_buf->offset += seek_offset;
            }
        }
        data->seek = 0;
        nozero:;
    }

    size_t max_frames = (buf->datas[0].maxsize / sizeof(*data->audio_buf->buf)) / data->audio_info.channels;
    size_t num_read =
            max_frames < (data->audio_info.total_frames - data->audio_buf->offset)
            ? max_frames : (data->audio_info.total_frames - data->audio_buf->offset);
    num_read *= data->audio_info.channels;
    memcpy(dst, &data->audio_buf->buf[data->audio_buf->offset * data->audio_info.channels],
           num_read * sizeof(*data->audio_buf->buf));
    data->audio_buf->offset += num_read / data->audio_info.channels;
    const double volumeDb = -6.0;
    const float volumeMultiplier = (float) (data->volume * pow(10.0, (volumeDb / 20.0)));
    for (uint32_t i = 0; i < num_read; ++i) {
        dst[i] *= volumeMultiplier;
    }
    if (data->audio_info.finished_reading &&
        data->audio_info.total_frames <= data->audio_buf->offset) {
        write(data->track_over_fd, &TRACK_OVER_SIG, sizeof(TRACK_OVER_SIG));
    }

    finish:
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = (int) sizeof(*dst) * data->audio_info.channels;
    buf->datas[0].chunk->size = buf->datas[0].maxsize;

    pw_stream_queue_buffer(data->stream, b);
}

static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .process = on_process,
};

struct audio_context *
audio_init(struct buffer *audio_buf, int track_over_fd) {
    pw_init(NULL, NULL);
    struct audio_context *data = calloc(1, sizeof(*data));
    data->audio_buf = audio_buf;
    data->track_over_fd = track_over_fd;
    return data;
}

int
audio_pause(struct audio_context *ctx) {
    ctx->status = false;
    return 0;
}

int audio_play(struct audio_context *ctx) {
    ctx->status = true && ctx->started;
    return 0;
}

int audio_stop(struct audio_context *ctx) {
    if (!ctx->started) return 0;
    ctx->status = false;

    pw_thread_loop_stop(ctx->loop);
    pw_stream_destroy(ctx->stream);
    pw_thread_loop_destroy(ctx->loop);

    ctx->audio_buf->offset = 0;
    ctx->audio_buf->len = 0;

    ctx->started = false;
    return 0;
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
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    ctx->loop = pw_thread_loop_new("smp-audio-loop", NULL);
    ERR_NULL(ctx->loop, 1);

    ctx->stream = pw_stream_new_simple(
            pw_thread_loop_get_loop(ctx->loop),
            "audio-src",
            pw_properties_new(
                    PW_KEY_MEDIA_TYPE, "Audio",
                    PW_KEY_MEDIA_CATEGORY, "Playback",
                    PW_KEY_MEDIA_ROLE, "Music",
                    NULL),
            &stream_events,
            ctx);
    ERR_NULL(ctx->stream, 1);

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
                                           &SPA_AUDIO_INFO_RAW_INIT(
                                                   .format = SPA_AUDIO_FORMAT_F32,
                                                   .channels = info->channels,
                                                   .rate = info->sample_rate));

    pw_stream_connect(ctx->stream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS |
                      PW_STREAM_FLAG_RT_PROCESS,
                      params, 1);

    pw_thread_loop_start(ctx->loop);
    ctx->started = true;

    return audio_play(ctx);
}

int audio_clean(struct audio_context *ctx) {
    audio_stop(ctx);
    pw_deinit();
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
    int64_t seek;
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
