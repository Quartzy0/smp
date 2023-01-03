//
// Created by quartzy on 7/23/22.
//

#ifdef PIPEWIRE_BACKEND

#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>
#include <stdint.h>
#include <math.h>
#include "util.h"
#include "dbus.h"
#include <errno.h>
#include <unistd.h>

#define ERR_NULL(p, r) if (!(p)){ fprintf(stderr, "Error occurred in " __FILE__ ":%d : %s\n", __LINE__, strerror(errno)); return r; }

int status;
bool started;
double volume;
int64_t seek;
size_t frames;

typedef struct Data {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    struct buffer *audio_buf;
    struct smp_context *ctx;
} Data;

static void on_process(void *userdata) {
    Data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    unsigned int i, stride;
    float *dst;

    if (!data->audio_buf->len || !data->ctx->audio_info.channels || (data->ctx->audio_info.finished_reading &&
                                                                     data->ctx->audio_info.total_frames <=
                                                                     data->audio_buf->offset /
                                                                     data->ctx->audio_info.channels))
        return;

    if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    if ((dst = buf->datas[0].data) == NULL)
        return;

    if (seek != 0) {
        int64_t seek_offset = (int64_t) ((((double) seek) * 0.000001) *
                                         (double) data->ctx->audio_info.sample_rate);
        if (seek_offset < 0){
            data->ctx->audio_buf.offset = -seek_offset > data->ctx->audio_buf.offset ? 0 : data->ctx->audio_buf.offset+seek_offset;
        }else if (data->ctx->audio_info.finished_reading){
            data->ctx->audio_buf.offset += seek_offset;
            if (data->ctx->audio_buf.offset >= data->ctx->audio_buf.len){
                Action a = {.type = ACTION_TRACK_OVER};
                a.position = 1;
                write(data->ctx->action_fd[1], &a, sizeof(a));
                seek = 0;
                goto finish;
            }
        }else{
            int64_t possible_seek = data->ctx->audio_buf.len-data->ctx->audio_buf.offset;
            if (possible_seek < seek_offset){
                data->ctx->audio_buf.offset += possible_seek;
                seek = (int64_t) ((double) ((seek_offset-possible_seek) / (double) data->ctx->audio_info.sample_rate) / 0.000001);
                goto nozero;
            }else{
                data->ctx->audio_buf.offset += seek_offset;
            }
        }
        seek = 0;
        nozero:;
    }

    stride = sizeof(*dst) * data->ctx->audio_info.channels;

    size_t num_read =
            buf->datas[0].maxsize < (data->audio_buf->len - data->audio_buf->offset) * sizeof(*data->audio_buf->buf)
            ? buf->datas[0].maxsize : (data->audio_buf->len - data->audio_buf->offset) * sizeof(*data->audio_buf->buf);
    memcpy(dst, &data->audio_buf->buf[data->audio_buf->offset], num_read);
    data->audio_buf->offset += num_read / sizeof(*data->audio_buf->buf);
    const double volumeDb = -6.0;
    const float volumeMultiplier = (float) (volume * pow(10.0, (volumeDb / 20.0)));
    for (i = 0; i < num_read / sizeof(*dst); ++i) {
        dst[i] *= volumeMultiplier;
    }
    if (data->ctx->audio_info.finished_reading &&
        data->ctx->audio_info.total_frames <= data->audio_buf->offset / data->ctx->audio_info.channels) {
        Action a = {.type = ACTION_TRACK_OVER};
        a.position = 1;
        write(data->ctx->action_fd[1], &a, sizeof(a));
    }

    finish:
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = (int) stride;
    buf->datas[0].chunk->size = buf->datas[0].maxsize;

    pw_stream_queue_buffer(data->stream, b);
}

static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .process = on_process,
};

Data data = {0,};

void init(struct smp_context *ctx, struct buffer *audio_buf) {
    pw_init(NULL, NULL);
    data.ctx = ctx;
    data.audio_buf = audio_buf;
}

int pause() {
    if (!status) return 0;
    status = false;
    return pw_stream_set_active(data.stream, false);
}

int play() {
    if (status) return 0;
    status = true;
    return pw_stream_set_active(data.stream, true);
}

int stop() {
    if (!started) return 0;
    status = false;
    pw_stream_set_active(data.stream, false);

    pw_stream_destroy(data.stream);
    pw_thread_loop_destroy(data.loop);

    data.audio_buf->offset = 0;
    data.audio_buf->len = 0;

    started = false;
    return 0;
}

int start(struct audio_info *info, struct audio_info *previous) {
    if (started && previous && previous->channels == info->channels &&
        previous->sample_rate == info->sample_rate) {
        printf("[audio] Using same audio stream\n");
        data.audio_buf->offset = 0;
        data.audio_buf->len = 0;
        return play();
    }
    printf("[audio] Starting audio new stream\n");
    memcpy(previous, info, sizeof(*info));
    stop();
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    data.loop = pw_thread_loop_new("smp-audio-loop", NULL);
    ERR_NULL(data.loop, 1);

    data.stream = pw_stream_new_simple(
            pw_thread_loop_get_loop(data.loop),
            "audio-src",
            pw_properties_new(
                    PW_KEY_MEDIA_TYPE, "Audio",
                    PW_KEY_MEDIA_CATEGORY, "Playback",
                    PW_KEY_MEDIA_ROLE, "Music",
                    NULL),
            &stream_events,
            &data);
    ERR_NULL(data.stream, 1);

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
                                           &SPA_AUDIO_INFO_RAW_INIT(
                                                   .format = SPA_AUDIO_FORMAT_F32,
                                                   .channels = info->channels,
                                                   .rate = info->sample_rate));

    pw_stream_connect(data.stream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS |
                      PW_STREAM_FLAG_RT_PROCESS,
                      params, 1);

    pw_thread_loop_start(data.loop);
    started = true;

    return play();
}

int clean_audio() {
    stop();
    pw_deinit();
    free(data.audio_buf->buf);
    memset(data.audio_buf, 0, sizeof(*data.audio_buf));
    return 0;
}

int set_pcm_stream(FILE *fp) {};

int set_file(const char *filename) {};

#endif