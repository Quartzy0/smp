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
#include <unistd.h>

#define ERR_NULL(p, r) if (!(p)){ fprintf(stderr, "Error occurred in " __FILE__ ":%d : %s\n", __LINE__, strerror(errno)); return r; }

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
    struct evbuffer *audio_buf;
    struct smp_context *ctx;
} Data;

static void on_process(void *userdata) {
    Data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    unsigned int i, n_frames, stride;
    float *dst;

    if (!evbuffer_get_length(data->audio_buf)) return;

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
            Action a = {.type = ACTION_TRACK_OVER};
            a.position = 1;
            write(data->ctx->action_fd[1], &a, sizeof(a));
            goto finish;
        }
    }

    stride = sizeof(*dst) * data->channels;
    n_frames = buf->datas[0].maxsize / stride;

    size_t num_read = evbuffer_remove(data->audio_buf, dst, buf->datas[0].maxsize);
    const double volumeDb = -6.0;
    const float volumeMultiplier = (float) (volume * pow(10.0, (volumeDb / 20.0)));
    for (i = 0; i < num_read / sizeof(*dst); ++i) {
        dst[i] *= volumeMultiplier;
    }


    /*if (data->fp){
        int16_t buffer[n_frames * data->channels];
        num_read = fread(buffer, stride, n_frames, data->fp);
        if (!num_read){
            if (ferror(data->fp)){
                fprintf(stderr, "[audio] Error occurred while reading audio stream: %s\n", strerror(errno));
                rear.type = ACTION_TRACK_OVER;
                last_rear.position = 1;
                sem_post(&state_change_lock);
                pclose(data->fp);
                goto finish;
            } else if (feof(data->fp)){
                rear.type = ACTION_TRACK_OVER;
                last_rear.position = 1;
                sem_post(&state_change_lock);
                pclose(data->fp);
                goto finish;
            }
        }
        const double volumeDb = -6.0;
        const float volumeMultiplier = (float) (volume * pow(10.0, (volumeDb / 20.0)));
        for (i = 0; i < num_read * data->channels; ++i) {
            dst[i] = (int16_t) ((float) buffer[i] * volumeMultiplier);
        }
    }else {
        num_read = n_frames < data->buffer_size - data->offset ? n_frames : data->buffer_size - data->offset;
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
            goto finish;
        }
    }*/

    finish:
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

void init(struct smp_context *ctx, struct evbuffer *audio_buf) {
    pw_init(NULL, NULL);
    data.ctx = ctx;
    data.channels = 2;
    data.sample_rate = 44100;
    data.audio_buf = audio_buf;
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
                                                   .format = SPA_AUDIO_FORMAT_F32,
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
set_pcm_stream(FILE *fp) {
    if (current_file) free(current_file);
    current_file = NULL;

    printf("[audio] Playing from PCM stream\n");
    free(data.buffer);
    memset(&data, 0, sizeof(data));

//    data.fp = fp;
    data.channels = 2;
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
    if (!data.buffer_size || !data.buffer) {
        fprintf(stderr, "[audio] Couldn't open %s", filename);
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