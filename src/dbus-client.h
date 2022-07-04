//
// Created by quartzy on 6/30/22.
//

#ifndef SMP_DBUS_CLIENT_H
#define SMP_DBUS_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "util.h"

typedef enum PlaybackStatus{
    PBS_PLAYING,
    PBS_PAUSED,
    PBS_STOPPED
} PlaybackStatus;

typedef struct Metadata{
    const char *track_id;
    int64_t length;
    const char *art_url;
    const char *artist;
    const char *title;
    const char *url;
} Metadata;

typedef struct PlayerProperties{
    PlaybackStatus playback_status;
    LoopMode loop_mode;
    bool shuffle;
    double volume;
    int64_t position;
    Metadata metadata;
} PlayerProperties;

int init_dbus_client();

int get_player_properties(PlayerProperties *properties);

void dbus_client_pause_play();

void dbus_client_play();

void dbus_client_pause();

void dbus_client_next();

void dbus_client_previous();

void dbus_client_open(const char *uri);

void dbus_client_set_volume(double volume);

double dbus_client_get_volume();

LoopMode dbus_client_get_loop_mode();

void dbus_client_set_loop_mode(LoopMode mode);

bool dbus_client_get_shuffle_mode();

void dbus_client_set_shuffle_mode(bool mode);

void print_properties(FILE *stream, PlayerProperties *properties);

#endif //SMP_DBUS_CLIENT_H
