//
// Created by quartzy on 6/30/22.
//

#include "dbus-client.h"
#include "spotify.h"
#include <dbus/dbus.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#define MAX_REPLY_WAIT_TIME 1000 // in ms
#define REPLY_WAIT_TIME_CHECK_TIME 10000 // in us

#define REPLY_WAIT_LOOP_COUNT ((MAX_REPLY_WAIT_TIME*1000)/REPLY_WAIT_TIME_CHECK_TIME+1)

static const char mpris_name[] = "org.mpris.MediaPlayer2.smp";

DBusConnection *client_conn;
DBusError client_err;

bool inited = false;

int
init_dbus_client() {
    if (inited)return 0;
    inited = true;
    dbus_error_init(&client_err);
    client_conn = dbus_bus_get(DBUS_BUS_SESSION, &client_err);

    if (dbus_error_is_set(&client_err)) {
        fprintf(stderr, "[dbus] Connection Error (%s)\n", client_err.message);
        dbus_error_free(&client_err);
        return 1;
    }
    if (NULL == client_conn) {
        fprintf(stderr, "[dbus] Connection Null\n");
        return 1;
    }

    if (!dbus_bus_name_has_owner(client_conn, mpris_name, &client_err)) {
        if (dbus_error_is_set(&client_err)) {
            fprintf(stderr, "[dbus-client] Error when checking for name %s: %s\n", mpris_name, client_err.message);
            return 1;
        }
        fprintf(stderr,
                "[dbus-client] The smp daemon is not running or could not be reached. Make sure there is an instance of this program running with the daemon subcommand\n");
        return 1;
    }

    dbus_connection_read_write_dispatch(client_conn, 0); // Idk why, but this has to be here
    // Probably some dbus protocol stuff
    char *identity = NULL;
    dbus_client_get_property("org.mpris.MediaPlayer2", "Identity", DBUS_TYPE_STRING, &identity);

    if (strcmp(identity, "smp") != 0) {
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
    dbus_connection_send(client_conn, msg, &serial);

    DBusMessage *reply;
    for (int j = 0; j < REPLY_WAIT_LOOP_COUNT; ++j) {
        dbus_connection_read_write(client_conn, 0);
        reply = dbus_connection_pop_message(client_conn);

        if (reply) break;
        usleep(REPLY_WAIT_TIME_CHECK_TIME);
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
        } else if (!strcmp(name, "LoopStatus")) {
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
    dbus_connection_send(client_conn, msg, &serial);

    DBusMessage *reply;
    for (int j = 0; j < REPLY_WAIT_LOOP_COUNT; ++j) { /* Make sure to get the empty reply that is sent as a response */
        dbus_connection_read_write(client_conn, 0);
        reply = dbus_connection_pop_message(client_conn);

        if (reply) break;
        usleep(REPLY_WAIT_TIME_CHECK_TIME);
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
    dbus_connection_send(client_conn, msg, &serial);

    DBusMessage *reply;
    for (int j = 0; j < REPLY_WAIT_LOOP_COUNT; ++j) { /* Make sure to get the empty reply that is sent as a response */
        dbus_connection_read_write(client_conn, 0);
        reply = dbus_connection_pop_message(client_conn);

        if (reply) break;
        usleep(REPLY_WAIT_TIME_CHECK_TIME);
    }
    dbus_message_unref(msg);
}

void
dbus_client_set_property(const char *iface, const char *name, int type, void *value) {
    DBusMessage *msg = dbus_message_new_method_call(mpris_name, "/org/mpris/MediaPlayer2",
                                                    "org.freedesktop.DBus.Properties", "Set");
    DBusMessageIter iter, val;
    dbus_message_iter_init_append(msg, &iter);
    char type_str[] = {(char) type, '\0'};

    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, type_str, &val);
    dbus_message_iter_append_basic(&val, type, value);
    dbus_message_iter_close_container(&iter, &val);

    dbus_uint32_t serial = 0;
    dbus_connection_send(client_conn, msg, &serial);

    DBusMessage *reply;
    for (int j = 0; j < REPLY_WAIT_LOOP_COUNT; ++j) { /* Make sure to get the empty reply that is sent as a response */
        dbus_connection_read_write(client_conn, 0);
        reply = dbus_connection_pop_message(client_conn);

        if (reply) break;
        usleep(REPLY_WAIT_TIME_CHECK_TIME);
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
    dbus_connection_send(client_conn, msg, &serial);
    dbus_message_unref(msg);

    for (int j = 0; j < REPLY_WAIT_LOOP_COUNT; ++j) { /* Get reply */
        dbus_connection_read_write(client_conn, 0);
        *reply = dbus_connection_pop_message(client_conn);

        if (*reply) break;
        usleep(REPLY_WAIT_TIME_CHECK_TIME);
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
    dbus_connection_send(client_conn, msg, &serial);
    dbus_message_unref(msg);

    DBusMessage *reply;
    for (int j = 0; j < REPLY_WAIT_LOOP_COUNT; ++j) { /* Get reply */
        dbus_connection_read_write(client_conn, 0);
        reply = dbus_connection_pop_message(client_conn);

        if (reply) break;
        usleep(REPLY_WAIT_TIME_CHECK_TIME);
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
    char *s;
    switch (mode) {
        case LOOP_MODE_PLAYLIST:
            s = "Playlist";
            break;
        case LOOP_MODE_TRACK:
            s = "Track";
            break;
        default:
            s = "None";
            break;
    }
    dbus_client_set_property("org.mpris.MediaPlayer2.Player", "LoopStatus", DBUS_TYPE_STRING, &s);
}

bool
dbus_client_get_shuffle_mode() {
    dbus_bool_t s = 0;
    dbus_client_get_property("org.mpris.MediaPlayer2.Player", "Shuffle", DBUS_TYPE_BOOLEAN, &s);
    return (bool) s;
}

void
dbus_client_set_shuffle_mode(bool mode) {
    dbus_bool_t s = (dbus_bool_t) mode;
    dbus_client_set_property("org.mpris.MediaPlayer2.Player", "Shuffle", DBUS_TYPE_BOOLEAN, (void *) &s);
}

void
dbus_client_tracklist_get_tracks(char ***out, int *count) {
    DBusMessage *reply = NULL;
    dbus_client_get_property_reply("org.mpris.MediaPlayer2.TrackList", "Tracks", &reply);

    DBusMessageIter iter, var, arr;
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
    dbus_connection_send(client_conn, msg, &serial);
    dbus_message_unref(msg);

    DBusMessage *reply;
    for (int j = 0; j < REPLY_WAIT_LOOP_COUNT; ++j) { /* Get reply */
        dbus_connection_read_write(client_conn, 0);
        reply = dbus_connection_pop_message(client_conn);

        if (reply) break;
        usleep(REPLY_WAIT_TIME_CHECK_TIME);
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

int
dbus_client_get_playlists(uint32_t index, uint32_t max_count, PlaylistOrder order, bool reverse, DBusPlaylistInfo *out,
                          int *count_out) {
    DBusMessage *msg = dbus_message_new_method_call(mpris_name, "/org/mpris/MediaPlayer2",
                                                    "org.mpris.MediaPlayer2.Playlists", "GetPlaylists");
    char *order_str;
    switch (order) {
        case ORDER_ALPHABETICAL:
            order_str = "Alphabetical";
            break;
        case ORDER_CREATED:
            order_str = "Created";
            break;
        case ORDER_MODIFIED:
            order_str = "Modified";
            break;
        case ORDER_PLAYED:
            order_str = "Played";
            break;
        default:
            order_str = "User";
            break;
    }
    uint32_t v = (uint32_t) reverse;

    dbus_message_append_args(msg,
                             DBUS_TYPE_UINT32, &index,
                             DBUS_TYPE_UINT32, &max_count,
                             DBUS_TYPE_STRING, &order_str,
                             DBUS_TYPE_BOOLEAN, &v,
                             DBUS_TYPE_INVALID);

    dbus_uint32_t serial = 0;
    dbus_connection_send(client_conn, msg, &serial);

    DBusMessage *reply;
    for (int j = 0; j < REPLY_WAIT_LOOP_COUNT; ++j) { /* Make sure to get the empty reply that is sent as a response */
        dbus_connection_read_write(client_conn, 0);
        reply = dbus_connection_pop_message(client_conn);

        if (reply) break;
        usleep(REPLY_WAIT_TIME_CHECK_TIME);
    }
    dbus_message_unref(msg);
    if (!reply) {
        fprintf(stderr, "[dbus-client] Didn't get reply when trying to get playlists\n");
        return 1;
    }

    DBusMessageIter iter, arr, stru;
    dbus_message_iter_init(reply, &iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
        char *s = NULL;
        dbus_message_iter_get_basic(&iter, &s);
        fprintf(stderr, "Got error: %s\n", s);
        return 1;
    }
    *count_out = dbus_message_iter_get_element_count(&iter);
    dbus_message_iter_recurse(&iter, &arr);
    for (int i = 0; i < *count_out; ++i) {
        dbus_message_iter_recurse(&arr, &stru);
        char *val = NULL;
        dbus_message_iter_get_basic(&stru, &val);
        out[i].id = strdup(val);
        dbus_message_iter_next(&stru);
        dbus_message_iter_get_basic(&stru, &val);
        out[i].name = strdup(val);
        dbus_message_iter_next(&stru);
        dbus_message_iter_get_basic(&stru, &val);
        out[i].icon = strdup(val);
        out[i].valid = true;
        dbus_message_iter_next(&arr);
    }
    dbus_message_unref(reply);
    return 0;
}

int
dbus_client_get_playlist_count(uint32_t *count) {
    return dbus_client_get_property("org.mpris.MediaPlayer2.Playlists", "PlaylistCount", DBUS_TYPE_UINT32, count);
}

int
dbus_client_get_active_playlist(DBusPlaylistInfo *out) {
    DBusMessage *reply;
    if (dbus_client_get_property_reply("org.mpris.MediaPlayer2.Playlists", "ActivePlaylist", &reply)) {
        return 1;
    }

    DBusMessageIter iter, val, stru1, stru2;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &val);
    dbus_message_iter_recurse(&val, &stru1);
    dbus_bool_t playlist = 0;
    dbus_message_iter_get_basic(&stru1, &playlist);
    if (!playlist) {
        out->valid = false;
        return 0;
    }
    dbus_message_iter_next(&stru1);
    dbus_message_iter_recurse(&stru1, &stru2);
    dbus_message_iter_get_basic(&stru2, &out->id);
    dbus_message_iter_next(&stru2);
    dbus_message_iter_get_basic(&stru2, &out->name);
    dbus_message_iter_next(&stru2);
    dbus_message_iter_get_basic(&stru2, &out->icon);
    dbus_message_unref(reply);
    out->valid = true;
    return 0;
}

int
dbus_client_activate_playlist(const char *id /* DBus object path */) {
    DBusMessage *msg = dbus_message_new_method_call(mpris_name, "/org/mpris/MediaPlayer2",
                                                    "org.mpris.MediaPlayer2.Playlists", "ActivatePlaylist");
    dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &id, DBUS_TYPE_INVALID);

    dbus_uint32_t serial = 0;
    dbus_connection_send(client_conn, msg, &serial);

    DBusMessage *reply;
    for (int j = 0; j < REPLY_WAIT_LOOP_COUNT; ++j) { /* Make sure to get the empty reply that is sent as a response */
        dbus_connection_read_write(client_conn, 0);
        reply = dbus_connection_pop_message(client_conn);

        if (reply) break;
        usleep(REPLY_WAIT_TIME_CHECK_TIME);
    }
    dbus_message_unref(msg);
    return reply != NULL;
}

void
dbus_parse_track(Track *track, DBusMessageIter *iter) {
    DBusMessageIter sub;
    char *val = NULL;
    dbus_message_iter_recurse(iter, &sub);
    dbus_message_iter_get_basic(&sub, &val);
    memcpy(track->spotify_id, val, sizeof(track->spotify_id));
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &val);
    memcpy(track->spotify_uri, val, sizeof(track->spotify_uri));
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &val);
    track->spotify_name = strdup(val);
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &val);
    track->spotify_name_escaped = strdup(val);
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &val);
    track->spotify_album_art = strdup(val);
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &val);
    track->artist = strdup(val);
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &val);
    track->artist_escaped = strdup(val);
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &val);
    memcpy(track->spotify_artist_id, val, sizeof(track->spotify_artist_id));
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &track->duration_ms);
}

void
dbus_parse_playlist(PlaylistInfo *playlist, DBusMessageIter *iter) {
    DBusMessageIter sub;
    char *val;
    dbus_bool_t valb = 0;
    dbus_message_iter_recurse(iter, &sub);
    dbus_message_iter_get_basic(&sub, &valb);
    playlist->album = valb;
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &val);
    playlist->name = strdup(val);
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &val);
    playlist->image_url = strdup(val);
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &val);
    memcpy(playlist->spotify_id, val, sizeof(playlist->spotify_id));
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &playlist->track_count);
    playlist->not_empty = true;
}

void
dbus_parse_artist(Artist *artist, DBusMessageIter *iter) {
    DBusMessageIter sub;
    char *val;
    dbus_message_iter_recurse(iter, &sub);
    dbus_message_iter_get_basic(&sub, &val);
    memcpy(artist->spotify_id, val, sizeof(artist->spotify_id));
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &val);
    artist->name = strdup(val);
    dbus_message_iter_next(&sub);
    dbus_message_iter_get_basic(&sub, &artist->followers);
}

#define ADD_ARRAY(iter, out, outlen, func) { \
    DBusMessageIter tracks_struct;\
    dbus_message_iter_recurse(&(iter), &tracks_struct); \
    dbus_bool_t b = 0;\
    dbus_message_iter_get_basic(&tracks_struct, &b);\
    dbus_message_iter_next(&tracks_struct);\
    if (b){                                  \
        dbus_uint32_t count;\
        dbus_message_iter_get_basic(&tracks_struct, &count);\
        *(out) = calloc(count, sizeof(**(out)));\
        *(outlen) = count;\
        dbus_message_iter_next(&tracks_struct);\
        DBusMessageIter tracks_array;\
        dbus_message_iter_recurse(&tracks_struct, &tracks_array);\
        for (int i = 0; i < count; ++i) {\
            func(&(*(out))[i], &tracks_array);\
            dbus_message_iter_next(&tracks_array);\
        }\
    }                   \
}

int
dbus_client_search(bool tracks, bool albums, bool artists, bool playlists, char *query, Track **tracksOut,
                   size_t *tracks_len, PlaylistInfo **albumsOut, size_t *albums_len, Artist **artistsOut,
                   size_t *artists_len, PlaylistInfo **playlistsOut, size_t *playlists_len) {
    DBusMessage *msg = dbus_message_new_method_call(mpris_name, "/org/mpris/MediaPlayer2",
                                                    "me.quartzy.smp", "Search");
    dbus_bool_t t = tracks, a1 = artists, a = albums, p = playlists;
    dbus_message_append_args(msg, DBUS_TYPE_BOOLEAN, &t, DBUS_TYPE_BOOLEAN, &a, DBUS_TYPE_BOOLEAN, &a1,
                             DBUS_TYPE_BOOLEAN, &p, DBUS_TYPE_STRING, &query, DBUS_TYPE_INVALID);

    dbus_uint32_t serial = 0;
    dbus_connection_send(client_conn, msg, &serial);

    DBusMessage *reply;
    for (int j = 0; j < REPLY_WAIT_LOOP_COUNT; ++j) {
        dbus_connection_read_write(client_conn, 0);
        reply = dbus_connection_pop_message(client_conn);

        if (reply) break;
        usleep(REPLY_WAIT_TIME_CHECK_TIME);
    }
    if (!reply) return 1;

    DBusMessageIter iter;
    dbus_message_iter_init(reply, &iter);

    // Tracks
    ADD_ARRAY(iter, tracksOut, tracks_len, dbus_parse_track);
    dbus_message_iter_next(&iter);
    ADD_ARRAY(iter, albumsOut, albums_len, dbus_parse_playlist);
    dbus_message_iter_next(&iter);
    ADD_ARRAY(iter, playlistsOut, playlists_len, dbus_parse_playlist);
    dbus_message_iter_next(&iter);
    ADD_ARRAY(iter, artistsOut, artists_len, dbus_parse_artist);

    dbus_message_unref(reply);
    return 0;
}

void
free_dbus_playlist(DBusPlaylistInfo *in) {
    if (!in || !in->valid) return;
    free(in->name);
    free(in->icon);
    free(in->id);
    memset(in, 0, sizeof(*in));
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
    char *loop_status;
    switch (properties->loop_mode) {
        case LOOP_MODE_PLAYLIST:
            loop_status = "Playlist";
            break;
        case LOOP_MODE_TRACK:
            loop_status = "Track";
            break;
        default:
            loop_status = "No";
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