//
// Created by quartzy on 7/22/23.
//

#ifndef SMP_CTRL_H
#define SMP_CTRL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "dbus-util.h"
#include "util.h"

struct smp_context;

struct search_params {
    bool tracks, artists, albums, playlists;
    char *query;
    dbus_method_call *call;
    dbus_bus *bus;
};

struct smp_context *ctrl_create_context(struct event_base *base);

void
ctrl_set_dbus_ifaces(struct smp_context *ctx, dbus_interface *player, dbus_interface *playlists, dbus_interface *tracks,
                     dbus_interface *smp, dbus_bus *bus);

void ctrl_init_audio(struct smp_context *ctx, double initial_volume);

void ctrl_quit(struct smp_context *ctx);

void ctrl_free(struct smp_context *ctx);

void ctrl_play_album(struct smp_context *ctx, const char *id);

void ctrl_play_playlist(struct smp_context *ctx, const char *id);

void ctrl_play_track(struct smp_context *ctx, const char *id);

void ctrl_play_uri(struct smp_context *ctx, const char *id);

void ctrl_pause(struct smp_context *ctx);

void ctrl_play(struct smp_context *ctx);

void ctrl_playpause(struct smp_context *ctx);

void ctrl_stop(struct smp_context *ctx);

void ctrl_change_track_index(struct smp_context *ctx, int32_t i);

void ctrl_set_track_index(struct smp_context *ctx, int32_t i);

void ctrl_search(struct smp_context *ctx, struct search_params *params);

void ctrl_seek(struct smp_context *ctx, int64_t position);

void ctrl_seek_to(struct smp_context *ctx, int64_t position);

struct audio_context *ctrl_get_audio_context(struct smp_context *ctx);

struct audio_info *ctrl_get_audio_info(struct smp_context *ctx);

struct audio_info *ctrl_get_audio_info_prev(struct smp_context *ctx);

struct spotify_state *ctrl_get_spotify_state(struct smp_context *ctx);

size_t ctrl_get_track_index(struct smp_context *ctx);

void ctrl_set_shuffle(struct smp_context *ctx, bool shuffle);

bool ctrl_get_shuffle(struct smp_context *ctx);

#endif //SMP_CTRL_H
