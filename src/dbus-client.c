//
// Created by quartzy on 6/30/22.
//

#include "dbus-client.h"
#include <dbus/dbus.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

static const char mpris_name[] = "org.mpris.MediaPlayer2.smp";

DBusConnection *conn;
DBusError err;

bool inited = false;

int
init_dbus_client() {
    if (inited)return 0;
    inited = true;
    dbus_error_init(&err);
    conn = dbus_bus_get(DBUS_BUS_SESSION, &err);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "[dbus] Connection Error (%s)\n", err.message);
        dbus_error_free(&err);
        return 1;
    }
    if (NULL == conn) {
        fprintf(stderr, "[dbus] Connection Null\n");
        return 1;
    }

    if (!dbus_bus_name_has_owner(conn, mpris_name, &err)) {
        if (dbus_error_is_set(&err)) {
            fprintf(stderr, "[dbus-client] Error when checking for name %s: %s\n", mpris_name, err.message);
            return 1;
        }
        fprintf(stderr,
                "[dbus-client] The smp daemon is not running or could not be reached. Make sure there is an instance of this program running with the -d option\n");
        return 1;
    }

    dbus_connection_read_write_dispatch(conn, 0); // Idk why, but this has to be here
                                                                                 // Probably some dbus protocol stuff
    char *identity = NULL;
    dbus_client_get_property("org.mpris.MediaPlayer2", "Identity", DBUS_TYPE_STRING, &identity);

    if (strcmp(identity, "smp")!=0) {
        printf("[dbus-client] Warning: Received identity of different media player (expected: smp got: %s). Some features might be broken\n",
               identity);
    }
    return 0;
}

void
dbus_client_parse_metadata(Metadata *out, DBusMessageIter *val) {
    DBusMessageIter arrIn, varIn, valIn;
    dbus_message_iter_recurse(val, &arrIn);
    int metadataCount = dbus_message_iter_get_element_count(val);
    for (int j = 0; j < metadataCount; ++j) {
        dbus_message_iter_recurse(&arrIn, &varIn);

        char *metaEntryName = NULL;
        dbus_message_iter_get_basic(&varIn, &metaEntryName);
        dbus_message_iter_next(&varIn);
        dbus_message_iter_recurse(&varIn, &valIn);

        char *s;
        if (!strcmp(metaEntryName, "mpris:trackid")) {
            dbus_message_iter_get_basic(&valIn, &s);
            out->track_id = strdup(s);
        } else if (!strcmp(metaEntryName, "mpris:length")) {
            dbus_message_iter_get_basic(&valIn, &out->length);
        } else if (!strcmp(metaEntryName, "mpris:artUrl")) {
            dbus_message_iter_get_basic(&valIn, &s);
            out->art_url = strdup(s);
        } else if (!strcmp(metaEntryName, "xesam:artist")) {
            DBusMessageIter artistArr;
            dbus_message_iter_recurse(&valIn, &artistArr);
            dbus_message_iter_get_basic(&artistArr, &s);
            out->artist = strdup(s);
        } else if (!strcmp(metaEntryName, "xesam:title")) {
            dbus_message_iter_get_basic(&valIn, &s);
            out->title = strdup(s);
        } else if (!strcmp(metaEntryName, "xesam:url")) {
            dbus_message_iter_get_basic(&valIn, &s);
            out->url = strdup(s);
        }

        dbus_message_iter_next(&arrIn);
    }
}

int
get_player_properties(PlayerProperties *properties) {
    DBusMessage *msg = dbus_message_new_method_call(mpris_name, "/org/mpris/MediaPlayer2",
                                                    "org.freedesktop.DBus.Properties", "GetAll");
    char *iface_name = "org.mpris.MediaPlayer2.Player";
    dbus_message_append_args(msg,
                             DBUS_TYPE_STRING, &iface_name,
                             DBUS_TYPE_INVALID);
    dbus_uint32_t serial = 0;
    dbus_connection_send(conn, msg, &serial);

    DBusMessage *reply;
    for (int j = 0; j < 5; ++j) {
        dbus_connection_read_write(conn, 0);
        reply = dbus_connection_pop_message(conn);

        if (NULL == reply) {
            usleep(10000);
            continue;
        } else {
            break;
        }
    }
    dbus_message_unref(msg);

    if (!reply) {
        fprintf(stderr, "[dbus-client] No message was received\n");
        return 1;
    }

    DBusMessageIter iter, arr, entry, val;
    dbus_message_iter_init(reply, &iter);
    int element_count = dbus_message_iter_get_element_count(&iter);
    dbus_message_iter_recurse(&iter, &arr);
    for (int i = 0; i < element_count; ++i) {
        dbus_message_iter_recurse(&arr, &entry);
        char *name = NULL;
        dbus_message_iter_get_basic(&entry, &name);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &val);

        if (!strcmp(name, "PlaybackStatus")) {
            char *pb_status = NULL;
            dbus_message_iter_get_basic(&val, &pb_status);
            if (!strcmp(pb_status, "Playing")) {
                properties->playback_status = PBS_PLAYING;
            } else if (!strcmp(pb_status, "Paused")) {
                properties->playback_status = PBS_PAUSED;
            } else {
                properties->playback_status = PBS_STOPPED;
            }
        } else if (!strcmp(name, "LoopMode")) {
            char *l_status = NULL;
            dbus_message_iter_get_basic(&val, &l_status);
            if (!strcmp(l_status, "None")) {
                properties->loop_mode = LOOP_MODE_NONE;
            } else if (!strcmp(l_status, "Track")) {
                properties->loop_mode = LOOP_MODE_TRACK;
            } else if (!strcmp(l_status, "Playlist")) {
                properties->loop_mode = LOOP_MODE_PLAYLIST;
            }
        } else if (!strcmp(name, "Shuffle")) {
            dbus_message_iter_get_basic(&val, &properties->shuffle);
        } else if (!strcmp(name, "Volume")) {
            dbus_message_iter_get_basic(&val, &properties->volume);
        } else if (!strcmp(name, "Position")) {
            dbus_message_iter_get_basic(&val, &properties->position);
        } else if (!strcmp(name, "Metadata")) {
            dbus_client_parse_metadata(&properties->metadata, &val);
        }
        dbus_message_iter_next(&arr);
    }
    dbus_message_unref(reply);

    return 0;
}

void
dbus_client_call_method(const char *iface, const char *method) {
    DBusMessage *msg = dbus_message_new_method_call(mpris_name, "/org/mpris/MediaPlayer2",
                                                    iface, method);
    dbus_uint32_t serial = 0;
    dbus_connection_send(conn, msg, &serial);

    DBusMessage *reply;
    for (int j = 0; j < 5; ++j) { /* Make sure to get the empty reply that is sent as a response */
        dbus_connection_read_write(conn, 0);
        reply = dbus_connection_pop_message(conn);

        if (NULL == reply) {
            usleep(10000);
            continue;
        } else {
            break;
        }
    }
    dbus_message_unref(msg);
}

void
dbus_client_pause_play() {
    dbus_client_call_method("org.mpris.MediaPlayer2.Player", "PlayPause");
}

void
dbus_client_play() {
    dbus_client_call_method("org.mpris.MediaPlayer2.Player", "Play");
}

void
dbus_client_pause() {
    dbus_client_call_method("org.mpris.MediaPlayer2.Player", "Pause");
}

void
dbus_client_next() {
    dbus_client_call_method("org.mpris.MediaPlayer2.Player", "Next");
}

void
dbus_client_previous() {
    dbus_client_call_method("org.mpris.MediaPlayer2.Player", "Previous");
}

void
dbus_client_open(const char *uri) {
    DBusMessage *msg = dbus_message_new_method_call(mpris_name, "/org/mpris/MediaPlayer2",
                                                    "org.mpris.MediaPlayer2.Player", "OpenUri");
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &uri, DBUS_TYPE_INVALID);

    dbus_uint32_t serial = 0;
    dbus_connection_send(conn, msg, &serial);

    DBusMessage *reply;
    for (int j = 0; j < 5; ++j) { /* Make sure to get the empty reply that is sent as a response */
        dbus_connection_read_write(conn, 0);
        reply = dbus_connection_pop_message(conn);

        if (NULL == reply) {
            usleep(10000);
            continue;
        } else {
            break;
        }
    }
    dbus_message_unref(msg);
}

void
dbus_client_set_property(const char *iface, const char *name, int type, void *value) {
    DBusMessage *msg = dbus_message_new_method_call(mpris_name, "/org/mpris/MediaPlayer2",
                                                    "org.freedesktop.DBus.Properties", "Set");
    DBusMessageIter iter, val;
    dbus_message_iter_init_append(msg, &iter);
    char type_str[] = {type, '\0'};

    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, type_str, &val);
    dbus_message_iter_append_basic(&val, type, value);
    dbus_message_iter_close_container(&iter, &val);

    dbus_uint32_t serial = 0;
    dbus_connection_send(conn, msg, &serial);

    DBusMessage *reply;
    for (int j = 0; j < 5; ++j) { /* Make sure to get the empty reply that is sent as a response */
        dbus_connection_read_write(conn, 0);
        reply = dbus_connection_pop_message(conn);

        if (NULL == reply) {
            usleep(10000);
            continue;
        } else {
            break;
        }
    }
    dbus_message_unref(msg);
}

int
dbus_client_get_property_reply(const char *iface, const char *name, DBusMessage **reply) {
    DBusMessage *msg = dbus_message_new_method_call(mpris_name, "/org/mpris/MediaPlayer2",
                                                    "org.freedesktop.DBus.Properties", "Get");
    dbus_message_append_args(msg,
                             DBUS_TYPE_STRING, &iface,
                             DBUS_TYPE_STRING, &name,
                             DBUS_TYPE_INVALID);
    dbus_uint32_t serial = 0;
    dbus_connection_send(conn, msg, &serial);
    dbus_message_unref(msg);

    for (int j = 0; j < 5; ++j) { /* Get reply */
        dbus_connection_read_write(conn, 0);
        *reply = dbus_connection_pop_message(conn);

        if (NULL == *reply) {
            usleep(10000);
            continue;
        } else {
            break;
        }
    }

    if (!*reply) {
        fprintf(stderr, "[dbus-client] No message was received\n");
        return 1;
    }
    return 0;
}

int
dbus_client_get_property(const char *iface, const char *name, int type, void *value) {
    DBusMessage *msg = dbus_message_new_method_call(mpris_name, "/org/mpris/MediaPlayer2",
                                                    "org.freedesktop.DBus.Properties", "Get");
    dbus_message_append_args(msg,
                             DBUS_TYPE_STRING, &iface,
                             DBUS_TYPE_STRING, &name,
                             DBUS_TYPE_INVALID);
    dbus_uint32_t serial = 0;
    dbus_connection_send(conn, msg, &serial);
    dbus_message_unref(msg);

    DBusMessage *reply;
    for (int j = 0; j < 5; ++j) { /* Get reply */
        dbus_connection_read_write(conn, 0);
        reply = dbus_connection_pop_message(conn);

        if (NULL == reply) {
            usleep(10000);
            continue;
        } else {
            break;
        }
    }

    if (!reply) {
        fprintf(stderr, "[dbus-client] No message was received\n");
        return 1;
    }

    DBusMessageIter iter, val;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &val);
    int type_got;
    if ((type_got = dbus_message_iter_get_arg_type(&val)) != type) {
        fprintf(stderr, "[dbus-client] Got unexpected type as response for option '%s' (expected '%c' got '%c')\n",
                name, type, type_got);
        dbus_message_unref(reply);
        return 1;
    }
    dbus_message_iter_get_basic(&val, value);


    dbus_message_unref(reply);
    return 0;
}

void
dbus_client_set_volume(double volume) {
    dbus_client_set_property("org.mpris.MediaPlayer2.Player", "Volume", DBUS_TYPE_DOUBLE, &volume);
}

double
dbus_client_get_volume() {
    double v = 0.0;
    dbus_client_get_property("org.mpris.MediaPlayer2.Player", "Volume", DBUS_TYPE_DOUBLE, &v);
    return v;
}

LoopMode
dbus_client_get_loop_mode() {
    char *s = NULL;
    dbus_client_get_property("org.mpris.MediaPlayer2.Player", "LoopStatus", DBUS_TYPE_STRING, &s);
    if (!strcmp(s, "None")) {
        return LOOP_MODE_NONE;
    } else if (!strcmp(s, "Track")) {
        return LOOP_MODE_TRACK;
    } else {
        return LOOP_MODE_PLAYLIST;
    }
}

void
dbus_client_set_loop_mode(LoopMode mode) {
    char *s = "None";
    switch (mode) {
        case LOOP_MODE_PLAYLIST:
            s = "Playlist";
            break;
        case LOOP_MODE_TRACK:
            s = "Track";
            break;
        case LOOP_MODE_NONE:
            s = "None";
            break;
    }
    dbus_client_set_property("org.mpris.MediaPlayer2.Player", "LoopStatus", DBUS_TYPE_STRING, &s);
}

bool
dbus_client_get_shuffle_mode() {
    uint32_t s = 0;
    dbus_client_get_property("org.mpris.MediaPlayer2.Player", "Shuffle", DBUS_TYPE_BOOLEAN, &s);
    return (bool) s;
}

void
dbus_client_set_shuffle_mode(bool mode) {
    uint32_t s = mode;
    dbus_client_set_property("org.mpris.MediaPlayer2.Player", "Shuffle", DBUS_TYPE_BOOLEAN, (void *) &s);
}

void
dbus_client_tracklist_get_tracks(char ***out, int *count) {
    DBusMessage *reply = NULL;
    dbus_client_get_property_reply("org.mpris.MediaPlayer2.TrackList", "Tracks", &reply);

    DBusMessageIter iter, var, arr, val;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &var);
    dbus_message_iter_recurse(&var, &arr);
    *count = dbus_message_iter_get_element_count(&var);
    *out = calloc(*count, sizeof(**out));
    for (int i = 0; i < *count; ++i) {
        char *path = NULL;
        dbus_message_iter_get_basic(&arr, &path);
        (*out)[i] = strdup(path);
        dbus_message_iter_next(&arr);
    }

    dbus_message_unref(reply);
}

int
dbus_client_get_tracks_metadata(char **tracks, int count, Metadata *out) {
    DBusMessage *msg = dbus_message_new_method_call(mpris_name, "/org/mpris/MediaPlayer2",
                                                    "org.mpris.MediaPlayer2.TrackList", "GetTracksMetadata");

    {
        DBusMessageIter iter, arr;
        dbus_message_iter_init_append(msg, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH_AS_STRING, &arr);
        for (int i = 0; i < count; ++i) {
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_OBJECT_PATH, &(tracks[i]));
        }
        dbus_message_iter_close_container(&iter, &arr);
    }

    dbus_uint32_t serial = 0;
    dbus_connection_send(conn, msg, &serial);
    dbus_message_unref(msg);

    DBusMessage *reply;
    for (int j = 0; j < 5; ++j) { /* Get reply */
        dbus_connection_read_write(conn, 0);
        reply = dbus_connection_pop_message(conn);

        if (NULL == reply) {
            usleep(10000);
            continue;
        } else {
            break;
        }
    }

    if (!reply) {
        fprintf(stderr, "[dbus-client] No message was received\n");
        return 1;
    }

    DBusMessageIter iter, arr;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &arr);
    for (int i = 0; i < count; ++i) {
        dbus_client_parse_metadata(&out[i], &arr);
        dbus_message_iter_next(&arr);
    }

    dbus_message_unref(reply);
    return 0;
}

void
free_metadata(Metadata *metadata) {
    free(metadata->track_id);
    free(metadata->art_url);
    free(metadata->artist);
    free(metadata->title);
    free(metadata->url);
}

void
print_properties(FILE *stream, PlayerProperties *properties) {
    char *status_indicator = "⏸";
    switch (properties->playback_status) {
        case PBS_PAUSED:
            status_indicator = "⏸";
            break;
        case PBS_PLAYING:
            status_indicator = "▶";
            break;
        case PBS_STOPPED:
            status_indicator = "■";
            break;
    }
    char *loop_status = "No";
    switch (properties->loop_mode) {
        case LOOP_MODE_NONE:
            loop_status = "No";
            break;
        case LOOP_MODE_PLAYLIST:
            loop_status = "Playlist";
            break;
        case LOOP_MODE_TRACK:
            loop_status = "Track";
            break;
    }
    if (properties->playback_status == PBS_STOPPED) {
        fprintf(stream, "Track info:\n");
        fprintf(stream, "\tN/A (Player is stopped or hasn't been started)\n");
        fprintf(stream, "Player info:\n");
        fprintf(stream, "\tLooping: %s\n", loop_status);
        fprintf(stream, "\tShuffle: %s\n", properties->shuffle ? "true" : "false");
        fprintf(stream, "\tVolume: %lf\n", properties->volume);
        return;
    }
    fprintf(stream, "Track info:\n");
    fprintf(stream, "\t%s %s\n", status_indicator, properties->metadata.title);
    fprintf(stream, "\t\tby %s\n", properties->metadata.artist);
    fprintf(stream, "\t(%s)\n", properties->metadata.url);
    fprintf(stream, "\t(art:%s)\n", properties->metadata.art_url);
    fprintf(stream, "Player info:\n");
    fprintf(stream, "\tLooping: %s\n", loop_status);
    fprintf(stream, "\tShuffle: %s\n", properties->shuffle ? "true" : "false");
    fprintf(stream, "\tVolume: %lf%%\n", 100.0 * properties->volume);
}