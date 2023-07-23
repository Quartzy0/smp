//
// Created by quartzy on 5/1/22.
//

#ifndef SMP_DBUS_H
#define SMP_DBUS_H

#include <stddef.h>
#include "dbus-util.h"

struct dbus_state{
    dbus_bus *bus;
    dbus_object *mpris_obj;

    dbus_interface *mp_iface;
    dbus_interface *mplayer_iface;
    dbus_interface *mplaylist_iface;
    dbus_interface *mtracks_iface;
};

struct smp_context;
struct spotify_search_results;

struct dbus_state * init_dbus(struct smp_context *ctx);

void handle_message(dbus_bus *bus);

void handle_search_response(struct spotify_search_results *results);

#endif //SMP_DBUS_H
