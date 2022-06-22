//
// Created by quartzy on 5/1/22.
//

#ifndef SMP_DBUS_H
#define SMP_DBUS_H

#include "spotify.h"
#include <semaphore.h>

extern sem_t state_change_lock;
extern PlaylistInfo cplaylist;
extern size_t track_index;
extern Track *tracks;
extern size_t track_count;

int init_dbus();

void handle_message();

#endif //SMP_DBUS_H
