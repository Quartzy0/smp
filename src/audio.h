//
// Created by quartzy on 4/30/22.
//

#ifndef SMP_AUDIO_H
#define SMP_AUDIO_H

#include <stdbool.h>
#include <stdio.h>
#include "dbus-util.h"

struct audio_context;
struct audio_info;
struct buffer;

#define FRAMES_PER_BUFFER   (512)

struct audio_context *audio_init(struct buffer *audio_buf, dbus_interface *player_iface, int track_over_fd);

int audio_start(struct audio_context *ctx, struct audio_info *info, struct audio_info *previous);

int audio_stop(struct audio_context *ctx);

int audio_pause(struct audio_context *ctx);

int audio_play(struct audio_context *ctx);

int audio_clean(struct audio_context *ctx);

void audio_seek(struct audio_context *ctx, int64_t position);

void audio_seek_to(struct audio_context *ctx, int64_t position);

int64_t audio_get_position(struct audio_context *ctx);

bool audio_started(struct audio_context *ctx);

bool audio_playing(struct audio_context *ctx);

double audio_get_volume(struct audio_context *ctx);

void audio_set_volume(struct audio_context *ctx, double volume);

void audio_info_set(struct audio_info *info, size_t sample_rate, size_t bitrate, int channels);

void audio_info_set_finished(struct audio_info *info);

void audio_info_add_frames(struct audio_info *info, size_t frames);

struct audio_info *audio_get_info(struct audio_context *ctx);

struct audio_info *audio_get_info_prev(struct audio_context *ctx);

#endif //SMP_AUDIO_H
