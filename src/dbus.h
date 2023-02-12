//
// Created by quartzy on 5/1/22.
//

#ifndef SMP_DBUS_H
#define SMP_DBUS_H

#include "spotify.h"

extern size_t track_index;
extern Track *tracks;
extern size_t track_count;
extern size_t track_size;

void * init_dbus(void *arg);

void handle_message(struct smp_context *ctx);

void search_complete_cb(struct spotify_state *spotify, void *userp);

#endif //SMP_DBUS_H
