//
// Created by quartzy on 7/23/22.
//

#ifdef PIPEWIRE_BACKEND

#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>
#include <stdint.h>
#include <math.h>
#include "stb_vorbis.c"
#include "util.h"
#include "dbus.h"
#include <errno.h>
#include <semaphore.h>

#define ERR_NULL(p, r) if (!p){ fprintf(stderr, "Error occurred in " __FILE__ ":%d : %s\n", __LINE__, strerror(errno)); return r; }

int status;
bool started;
double volume;
int64_t seek;
size_t offset;
size_t frames;
int audio_samplerate;
char *current_file;

typedef struct Data {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    size_t offset;
    size_t buffer_size;
    int16_t *buffer;
    int channels;
    int sample_rate;
} Data;

static void on_process(void *userdata) {
    Data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    unsigned int i, n_frames, stride;
    int16_t *dst;

    if (data->offset >= data->buffer_size) return;

    if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    if ((dst = buf->datas[0].data) == NULL)
        return;

    if (seek != 0) {
        data->offset += (int64_t) ((((double) seek) * 0.000001) * (double) data->sample_rate);
        seek = 0;
        if (data->offset < 0) data->offset = 0;
        else if (data->offset >= data->buffer_size) {
            rear.type = ACTION_TRACK_OVER;
            last_rear.position = 1;
            sem_post(&state_change_lock);
            return;
        }
    }

    stride = sizeof(int16_t) * data->channels;
    n_frames = buf->datas[0].maxsize / stride;

    size_t num_read = n_frames < data->buffer_size - data->offset ? n_frames : data->buffer_size - data->offset;
    const double volumeDb = -6.0;
    const float volumeMultiplier = (float) (volume * pow(10.0, (volumeDb / 20.0)));
    for (i = 0; i < num_read * data->channels; ++i) {
        dst[i] = (int16_t) ((float) data->buffer[data->offset * data->channels + i] * volumeMultiplier);
    }
    data->offset += num_read;
    if (data->offset >= data->buffer_size) {
        rear.type = ACTION_TRACK_OVER;
        last_rear.position = 1;
        sem_post(&state_change_lock);
        return;
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = (int) stride;
    buf->datas[0].chunk->size = n_frames * stride;

    pw_stream_queue_buffer(data->stream, b);
}

static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .process = on_process,
};

Data data = {0,};

int init() {
    pw_init(NULL, NULL);
    return 0;
}

int start() {
    if (started) return 0;
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
                                                   .format = SPA_AUDIO_FORMAT_S16,
                                                   .channels = data.channels,
                                                   .rate = data.sample_rate));

    pw_stream_connect(data.stream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS |
                      PW_STREAM_FLAG_RT_PROCESS,
                      params, 1);

    pw_thread_loop_start(data.loop);
    started = true;

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
        return 1;
    }
    frames = data.buffer_size;
    audio_samplerate = data.sample_rate;
    return 0;
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

    started = false;
    return 0;
}

int clean_audio() {
    stop();
    pw_deinit();
    free(data.buffer);
    free(current_file);
    return 0;
}

#endif