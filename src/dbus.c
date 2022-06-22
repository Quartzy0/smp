//
// Created by quartzy on 5/1/22.
//

#include "dbus.h"
#include <dbus/dbus.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "audio.h"
#include "util.h"
#include "spotify.h"

bool running = false;

static const char mpris_name[] = "org.mpris.MediaPlayer2.musiccontroller";

DBusConnection *conn;

sem_t state_change_lock;

DBusError err;

void get_mediaplayer(DBusMessage *pMessage, DBusConnection *pConnection, char *property);

#define CHECK_TYPE(args, type) if (dbus_message_iter_get_arg_type(args)!=(type)){ fprintf(stderr, "[dbus] Invalid argument type\n");return; }

int
init_dbus() {
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

    // request our name on the bus and make sure there is only one version of this code running
    int name_request = dbus_bus_request_name(conn, mpris_name, DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "[dbus] Name Error (%s)\n", err.message);
        dbus_error_free(&err);
        return 1;
    }
    if (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER != name_request) {
        fprintf(stderr, "[dbus] Not Primary Owner (%d)\n", name_request);
        return 1;
    }

    return 0;
}

void add_dict_entry_p(DBusMessageIter *dict, char *attribute, void *attr_value, int type) {
    // Get the string representation of this type
    char type_string[2] = {type, '\0'};

    DBusMessageIter dict_entry, dict_val;
    // Create our entry in the dictionary
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry);
    // Add the attribute string
    dbus_message_iter_append_basic(&dict_entry, DBUS_TYPE_STRING, &attribute);
    // Create the value for this entry
    dbus_message_iter_open_container(&dict_entry, DBUS_TYPE_VARIANT, type_string, &dict_val);
    dbus_message_iter_append_basic(&dict_val, type, attr_value);
    // Clean up and return
    dbus_message_iter_close_container(&dict_entry, &dict_val);
    dbus_message_iter_close_container(dict, &dict_entry);
}

// Adds one string/{type} dictionary entry to dict
void add_dict_entry(DBusMessageIter *dict, char *attribute, void *attr_value, int type) {
    add_dict_entry_p(dict, attribute, &attr_value, type);
}

// Adds one string/a{type} dictionary entry to dict
void add_dict_entry_array(DBusMessageIter *dict, char *attribute, void **attr_value, int count, int type) {
    char type_array[3] = {DBUS_TYPE_ARRAY, type, '\0'};
    char type_str[2] = {type, '\0'};
    DBusMessageIter dict_entry, dict_val, arr_val;
    // Create our entry in the dictionary
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry);
    // Add the attribute string
    dbus_message_iter_append_basic(&dict_entry, DBUS_TYPE_STRING, &attribute);
    // Create the value for this entry
    dbus_message_iter_open_container(&dict_entry, DBUS_TYPE_VARIANT, type_array, &dict_val);
    dbus_message_iter_open_container(&dict_val, DBUS_TYPE_ARRAY, type_str, &arr_val);
    for (int i = 0; i < count; ++i) {
        dbus_message_iter_append_basic(&arr_val, DBUS_TYPE_STRING, &attr_value[i]);
    }
    // Clean up and return
    dbus_message_iter_close_container(&dict_val, &arr_val);
    dbus_message_iter_close_container(&dict_entry, &dict_val);
    dbus_message_iter_close_container(dict, &dict_entry);
}

void get_track_metadata(DBusMessageIter *iter) {
    if (!started)return;
    DBusMessageIter dict;
    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

    Track *ctrack = &tracks[track_index];
    char *obj_path = malloc((strlen(ctrack->spotify_id) + 46 + 1) * sizeof(*obj_path));
    snprintf(obj_path, (strlen(ctrack->spotify_id) + 46 + 1) * sizeof(*obj_path),
             "/org/mpris/MediaPlayer2/musiccontroller/track/%s", ctrack->spotify_id);

    add_dict_entry(&dict, "mpris:trackid", obj_path, DBUS_TYPE_OBJECT_PATH);
    add_dict_entry(&dict, "mpris:length", (void *) (((int64_t) frames / audio_samplerate) * 1000000), DBUS_TYPE_INT64);
    add_dict_entry(&dict, "mpris:artUrl", ctrack->spotify_album_art, DBUS_TYPE_STRING);
    add_dict_entry_array(&dict, "xesam:artist", (void **) &ctrack->artist, 1, DBUS_TYPE_STRING);
    add_dict_entry(&dict, "xesam:url", ctrack->spotify_uri, DBUS_TYPE_STRING);

    dbus_message_iter_close_container(iter, &dict);
}

void get_playlist_dbus(DBusMessageIter *iter, PlaylistInfo *info) {
    DBusMessageIter props;
    dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT, NULL, &props);

    if (info) {
        char *obj_path = malloc((strlen(info->spotify_id) + 50 + 1) * sizeof(*obj_path));
        snprintf(obj_path, (strlen(info->spotify_id) + 50 + 1) * sizeof(*obj_path),
                 "/org/mpris/MediaPlayer2/musiccontroller/playlist/%s%s", info->spotify_id, info->album ? "a" : "p");
        dbus_message_iter_append_basic(&props, DBUS_TYPE_OBJECT_PATH, &obj_path);
        dbus_message_iter_append_basic(&props, DBUS_TYPE_STRING, &info->name);
        dbus_message_iter_append_basic(&props, DBUS_TYPE_STRING, &info->image_url);
    } else {
        const char *obj_path = "/";
        const char *name = "";
        const char *url = "";
        dbus_message_iter_append_basic(&props, DBUS_TYPE_OBJECT_PATH, &obj_path);
        dbus_message_iter_append_basic(&props, DBUS_TYPE_STRING, &name);
        dbus_message_iter_append_basic(&props, DBUS_TYPE_STRING, &url);
    }
    dbus_message_iter_close_container(iter, &props);
}

void get_playlist_maybe(DBusMessageIter *iter) {
    DBusMessageIter bools;
    dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT, NULL, &bools);

    dbus_message_iter_append_basic(&bools, DBUS_TYPE_BOOLEAN, &started);

    get_playlist_dbus(&bools, (PlaylistInfo *) (started * (uint64_t) &cplaylist) /* No branching ;) */);
    dbus_message_iter_close_container(iter, &bools);
}

void get_mediaplayer(DBusMessage *msg, DBusConnection *conn, char *property) {
    dbus_uint32_t serial = 0;
    // Generate a message to return
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter reply_args, var;
    dbus_message_iter_init_append(reply, &reply_args);
    if (!strcmp(property, "CanQuit")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "b", &var);
        bool value = true;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &value);
    } else if (!strcmp(property, "Fullscreen")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "b", &var);
        bool value = false;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &value);
    } else if (!strcmp(property, "CanSetFullscreen")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "b", &var);
        bool value = false;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &value);
    } else if (!strcmp(property, "CanRaise")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "b", &var);
        bool value = false;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &value);
    } else if (!strcmp(property, "Identity")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "s", &var);
        const char *value = "SpotifyDL";
        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &value);
    } else if (!strcmp(property, "SupportedUriSchemes")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "as", &var);
        const char *value = "spotify";
        dbus_message_iter_append_fixed_array(&var, DBUS_TYPE_STRING, value, 1);
    }
    dbus_message_iter_close_container(&reply_args, &var);

    // send the reply && flush the connection
    if (!dbus_connection_send(conn, reply, &serial)) {
        fprintf(stderr, "[dbus] Out Of Memory!\n");
        exit(1);
    }

    // free the reply
    dbus_message_unref(reply);
}

void get_mediaplayer_player(DBusMessage *msg, DBusConnection *conn, char *property) {
    dbus_uint32_t serial = 0;
    // Generate a message to return
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter reply_args, var;
    dbus_message_iter_init_append(reply, &reply_args);
    if (!strcmp(property, "CanGoNext")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "b", &var);
        bool value = true;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &value);
    } else if (!strcmp(property, "CanGoPrevious")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "b", &var);
        bool value = true;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &value);
    } else if (!strcmp(property, "CanPlay")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "b", &var);
        bool value = true;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &value);
    } else if (!strcmp(property, "CanPause")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "b", &var);
        bool value = true;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &value);
    } else if (!strcmp(property, "CanControl")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "b", &var);
        bool value = true;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &value);
    } else if (!strcmp(property, "CanSeek")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "b", &var);
        bool value = true;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &value);
    } else if (!strcmp(property, "PlaybackStatus")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "s", &var);
        const char *value = (status ? "Playing" : "Paused");
        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &value);
    } else if (!strcmp(property, "MinimumRate")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "d", &var);
        const double value = 1.0;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_DOUBLE, &value);
    } else if (!strcmp(property, "MaximumRate")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "d", &var);
        const double value = 1.0;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_DOUBLE, &value);
    } else if (!strcmp(property, "Rate")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "d", &var);
        const double value = 1.0;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_DOUBLE, &value);
    } else if (!strcmp(property, "LoopStatus")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "s", &var);
        char *value;
        switch (loop_mode) {
            case LOOP_MODE_NONE:
                value = "None";
                break;
            case LOOP_MODE_TRACK:
                value = "Track";
                break;
            case LOOP_MODE_PLAYLIST:
                value = "Playlist";
                break;
            default:
                value = "None";
                break;
        }
        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &value);
    } else if (!strcmp(property, "Shuffle")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "b", &var);
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &shuffle);
    } else if (!strcmp(property, "Volume")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "d", &var);
        dbus_message_iter_append_basic(&var, DBUS_TYPE_DOUBLE, &volume);
    } else if (!strcmp(property, "Position") && started) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "x", &var);
        int64_t position = ((int64_t) offset / audio_samplerate) * 1000000; //Convert to micoseconds
        dbus_message_iter_append_basic(&var, DBUS_TYPE_INT64, &position);
    } else if (!strcmp(property, "Metadata") && started) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "a{sv}", &var);
        get_track_metadata(&var);
    } else {
        dbus_message_unref(reply);
        //Send empty reply
        reply = dbus_message_new_method_return(msg);

        // send the reply && flush the connection
        if (!dbus_connection_send(conn, reply, &serial)) {
            fprintf(stderr, "[dbus] Out Of Memory!\n");
        }

        // free the reply
        dbus_message_unref(reply);
        return;
    }
    dbus_message_iter_close_container(&reply_args, &var);

    // send the reply && flush the connection
    if (!dbus_connection_send(conn, reply, &serial)) {
        fprintf(stderr, "[dbus] Out Of Memory!\n");
        exit(1);
    }

    // free the reply
    dbus_message_unref(reply);
}

void get_mediaplayer_playlists(DBusMessage *msg, DBusConnection *conn, char *property) {
    dbus_uint32_t serial = 0;
    // Generate a message to return
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter reply_args, var;
    dbus_message_iter_init_append(reply, &reply_args);

    if (!strcmp(property, "PlaylistCount")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "u", &var);
        uint32_t playlistCount = get_saved_playlist_count();
        dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &playlistCount);
    } else if (!strcmp(property, "Orderings")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "as", &var);
        DBusMessageIter arr_val;
        dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &arr_val);
        char *orderings[] = {"Alphabetical", "Played"};
        dbus_message_iter_append_basic(&arr_val, DBUS_TYPE_STRING, &orderings[0]);
        dbus_message_iter_append_basic(&arr_val, DBUS_TYPE_STRING, &orderings[1]);
        dbus_message_iter_close_container(&var, &arr_val);
    } else if (!strcmp(property, "ActivePlaylist")) {
        dbus_message_iter_open_container(&reply_args, DBUS_TYPE_VARIANT, "(b(oss))", &var);
        get_playlist_maybe(&var);
    } else {
        dbus_message_unref(reply);
        //Send empty reply
        reply = dbus_message_new_method_return(msg);

        // send the reply && flush the connection
        if (!dbus_connection_send(conn, reply, &serial)) {
            fprintf(stderr, "[dbus] Out Of Memory!\n");
        }

        // free the reply
        dbus_message_unref(reply);
        return;
    }

    dbus_message_iter_close_container(&reply_args, &var);

    // send the reply && flush the connection
    if (!dbus_connection_send(conn, reply, &serial)) {
        fprintf(stderr, "[dbus] Out Of Memory!\n");
        exit(1);
    }

    // free the reply
    dbus_message_unref(reply);
}

void getall_mediaplayer(DBusMessage *msg, DBusConnection *conn) {
    dbus_uint32_t serial = 0;
    // Generate a message to return
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter reply_args, dict;
    dbus_message_iter_init_append(reply, &reply_args);

    // Create our array to hold dictionary entries
    dbus_message_iter_open_container(&reply_args, DBUS_TYPE_ARRAY, "{sv}", &dict);

    // Add dictionary entries claiming we don't have these capabilities
    add_dict_entry(&dict, "CanQuit", (void *) 1, DBUS_TYPE_BOOLEAN);
    add_dict_entry(&dict, "CanRaise", (void *) 0, DBUS_TYPE_BOOLEAN);
    add_dict_entry(&dict, "HasTrackList", (void *) 0, DBUS_TYPE_BOOLEAN);
    char *scheme = "spotify";
    add_dict_entry_array(&dict, "SupportedUriSchemes", (void **) &scheme, 1, DBUS_TYPE_STRING);

    // Clean up our array
    dbus_message_iter_close_container(&reply_args, &dict);


    // send the reply && flush the connection
    if (!dbus_connection_send(conn, reply, &serial)) {
        fprintf(stderr, "[dbus] Out Of Memory!\n");
        exit(1);
    }

    // free the reply
    dbus_message_unref(reply);
}

// Tell about our capabilities, few as they are
void getall_mediaplayer_player(DBusMessage *msg, DBusConnection *conn) {
    dbus_uint32_t serial = 0;
    // Generate a message to return
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter reply_args, dict;
    dbus_message_iter_init_append(reply, &reply_args);

    // Create our array to hold dictionary entries
    dbus_message_iter_open_container(&reply_args, DBUS_TYPE_ARRAY, "{sv}", &dict);

    // Add dictionary entries telling we have these capabilities
    add_dict_entry(&dict, "CanGoNext", (void *) 1, DBUS_TYPE_BOOLEAN);
    add_dict_entry(&dict, "CanGoPrevious", (void *) 1, DBUS_TYPE_BOOLEAN);
    add_dict_entry(&dict, "CanPlay", (void *) 1, DBUS_TYPE_BOOLEAN);
    add_dict_entry(&dict, "CanPause", (void *) 1, DBUS_TYPE_BOOLEAN);
    add_dict_entry(&dict, "CanControl", (void *) 1, DBUS_TYPE_BOOLEAN);
    add_dict_entry(&dict, "CanSeek", (void *) 1, DBUS_TYPE_BOOLEAN);
    double value = 1.0;
    add_dict_entry_p(&dict, "MinimumRate", &value, DBUS_TYPE_DOUBLE);
    add_dict_entry_p(&dict, "MaximumRate", &value, DBUS_TYPE_DOUBLE);
    add_dict_entry_p(&dict, "Rate", &value, DBUS_TYPE_DOUBLE);
    // Add dictionary entry telling current playback status
    add_dict_entry(&dict, "PlaybackStatus", (status ? "Playing" : "Paused"), DBUS_TYPE_STRING);
    switch (loop_mode) {
        case LOOP_MODE_NONE:
            add_dict_entry(&dict, "LoopStatus", "None", DBUS_TYPE_STRING);
            break;
        case LOOP_MODE_TRACK:
            add_dict_entry(&dict, "LoopStatus", "Track", DBUS_TYPE_STRING);
            break;
        case LOOP_MODE_PLAYLIST:
            add_dict_entry(&dict, "LoopStatus", "Playlist", DBUS_TYPE_STRING);
            break;
        default:
            add_dict_entry(&dict, "LoopStatus", "None", DBUS_TYPE_STRING);
            break;
    }
    add_dict_entry(&dict, "Shuffle", (void *) shuffle, DBUS_TYPE_BOOLEAN);
    add_dict_entry_p(&dict, "Volume", &volume, DBUS_TYPE_DOUBLE);

    //Metadata
    if (started) {
        int64_t position = ((int64_t) offset / audio_samplerate) * 1000000; //Convert to micoseconds
        add_dict_entry_p(&dict, "Position", &position, DBUS_TYPE_INT64);
        DBusMessageIter dict_entry, dict_val;
        // Create our entry in the dictionary
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry);
        // Add the attribute string
        const char *attribute = "Metadata";
        dbus_message_iter_append_basic(&dict_entry, DBUS_TYPE_STRING, &attribute);
        // Create the value for this entry
        dbus_message_iter_open_container(&dict_entry, DBUS_TYPE_VARIANT, "a{sv}", &dict_val);
        get_track_metadata(&dict_val);
        // Clean up and return
        dbus_message_iter_close_container(&dict_entry, &dict_val);
        dbus_message_iter_close_container(&dict, &dict_entry);
    }


    // Clean up our array
    dbus_message_iter_close_container(&reply_args, &dict);

    // send the reply && flush the connection
    if (!dbus_connection_send(conn, reply, &serial)) {
        fprintf(stderr, "[dbus] Out Of Memory!\n");
        exit(1);
    }

    // free the reply
    dbus_message_unref(reply);
}

void getall_mediaplayer_playlists(DBusMessage *msg, DBusConnection *conn) {
    dbus_uint32_t serial = 0;
    // Generate a message to return
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter reply_args, dict;
    dbus_message_iter_init_append(reply, &reply_args);

    // Create our array to hold dictionary entries
    dbus_message_iter_open_container(&reply_args, DBUS_TYPE_ARRAY, "{sv}", &dict);

    add_dict_entry(&dict, "PlaylistCount", (void *) get_saved_playlist_count(), DBUS_TYPE_UINT32);
    char *orderings[] = {"Alphabetical", "Played"};
    add_dict_entry_array(&dict, "Orderings", (void **) orderings, 2, DBUS_TYPE_STRING);
    DBusMessageIter dict_entry, dict_val;
    // Create our entry in the dictionary
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry);
    // Add the attribute string
    char *attribute = "ActivePlaylist";
    dbus_message_iter_append_basic(&dict_entry, DBUS_TYPE_STRING, &attribute);
    // Create the value for this entry
    dbus_message_iter_open_container(&dict_entry, DBUS_TYPE_VARIANT, "(b(oss))", &dict_val);
    get_playlist_maybe(&dict_val);
    // Clean up and return
    dbus_message_iter_close_container(&dict_entry, &dict_val);
    dbus_message_iter_close_container(&dict, &dict_entry);

    // Clean up our array
    dbus_message_iter_close_container(&reply_args, &dict);

    // send the reply && flush the connection
    if (!dbus_connection_send(conn, reply, &serial)) {
        fprintf(stderr, "[dbus] Out Of Memory!\n");
        exit(1);
    }

    // free the reply
    dbus_message_unref(reply);
}

void
set_values_player(const char *property, DBusMessageIter *args) {
    if (!strcmp(property, "Shuffle")) {
        CHECK_TYPE(args, DBUS_TYPE_BOOLEAN);
        dbus_message_iter_get_basic(args, &shuffle);
        printf("[dbus] Shuffle status: %s\n", shuffle ? "on" : "off");
    } else if (!strcmp(property, "LoopStatus")) {
        CHECK_TYPE(args, DBUS_TYPE_STRING);
        char *loopm_str = NULL;
        dbus_message_iter_get_basic(args, &loopm_str);
        if (!strcmp(loopm_str, "None")) {
            loop_mode = LOOP_MODE_NONE;
            printf("[spotify] Loop mode set to None\n");
        } else if (!strcmp(loopm_str, "Track")) {
            loop_mode = LOOP_MODE_TRACK;
            printf("[spotify] Loop mode set to Track\n");
        } else if (!strcmp(loopm_str, "Playlist")) {
            loop_mode = LOOP_MODE_PLAYLIST;
            printf("[spotify] Loop mode set to Playlist\n");
        } else {
            fprintf(stderr, "[spotify] Unrecognized loop mode: %s (supported: None, Track, Playlist)\n", loopm_str);
        }
    } else if (!strcmp(property, "Volume")) {
        CHECK_TYPE(args, DBUS_TYPE_DOUBLE);
        double volume_new = 0;
        dbus_message_iter_get_basic(args, &volume_new);
        if (volume_new < 0) volume_new = 0;
        volume = volume_new;
        printf("[dbus] Volume set to: %f\n", volume_new);
    }
}

// If we recieve a player command, output the magic word to make our extension do it
int check_player_command(DBusMessage *msg) {
    if (dbus_message_is_method_call(msg, "org.mpris.MediaPlayer2.Player", "Pause")) {
        rear.type = ACTION_PAUSE;
        sem_post(&state_change_lock);
    } else if (dbus_message_is_method_call(msg, "org.mpris.MediaPlayer2.Player", "Play")) {
        rear.type = ACTION_PLAY;
        sem_post(&state_change_lock);
    } else if (dbus_message_is_method_call(msg, "org.mpris.MediaPlayer2.Player", "PlayPause")) {
        rear.type = ACTION_PLAYPAUSE;
        sem_post(&state_change_lock);
    } else if (dbus_message_is_method_call(msg, "org.mpris.MediaPlayer2.Player", "Next")) {
        rear.type = ACTION_POSITION_RELATIVE;
        last_rear.position = 1;
        sem_post(&state_change_lock);
    } else if (dbus_message_is_method_call(msg, "org.mpris.MediaPlayer2.Player", "Previous")) {
        rear.type = ACTION_POSITION_RELATIVE;
        last_rear.position = -1;
        sem_post(&state_change_lock);
    } else if (dbus_message_is_method_call(msg, "org.mpris.MediaPlayer2.Player", "OpenUri")) {
        const char *uri = NULL;
        if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &uri, DBUS_TYPE_INVALID)) {
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
                fprintf(stderr, "[dbus] Error while decoding arguments: %s\n", err.message);
            }
            return 0;
        }

        Action *a = &rear;
        a->type = id_from_url(uri, a->id);

        sem_post(&state_change_lock);
    } else if (dbus_message_is_method_call(msg, "org.mpris.MediaPlayer2", "Quit")) {
        running = 0;
        rear.type = ACTION_QUIT;
        sem_post(&state_change_lock);
    } else if (dbus_message_is_method_call(msg, "org.mpris.MediaPlayer2.Player", "Stop")) {
        running = 0;
        rear.type = ACTION_QUIT;
        sem_post(&state_change_lock);
    } else if (dbus_message_is_method_call(msg, "org.mpris.MediaPlayer2.Player", "Seek")) {
        rear.type = ACTION_SEEK;
        if (!dbus_message_get_args(msg, &err, DBUS_TYPE_INT64, &last_rear.position, DBUS_TYPE_INVALID)) {
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
                fprintf(stderr, "[dbus] Error while decoding arguments: %s\n", err.message);
            }
            return 0;
        }
        sem_post(&state_change_lock);
    } else if (dbus_message_is_method_call(msg, "org.mpris.MediaPlayer2.Player", "SetPosition")) {
        rear.type = ACTION_SET_POSITION;
        char *obj;
        if (!dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &obj, DBUS_TYPE_INT64, &last_rear.position,
                                   DBUS_TYPE_INVALID)) {
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
                fprintf(stderr, "[dbus] Error while decoding arguments: %s\n", err.message);
            }
            return 0;
        }
        sem_post(&state_change_lock);
    } else {
        return 0;
    }
    return 1;
}

int check_playlist_command(DBusMessage *msg) {
    if (dbus_message_is_method_call(msg, "org.mprs.MediaPlayer2.Playlists", "ActivatePlaylist")) {
        char *obj;
        if (!dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &obj, DBUS_TYPE_INVALID)) {
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
                fprintf(stderr, "[dbus] Error while decoding arguments: %s\n", err.message);
            }
            return 0;
        }
        size_t objLen = strlen(obj);
        rear.type = obj[objLen - 1] == 'a' ? ACTION_ALBUM : ACTION_PLAYLIST;
        memcpy(last_rear.id, &obj[objLen - 23], 22 * sizeof(char));
        last_rear.id[22] = 0;
        sem_post(&state_change_lock);
    } else if (dbus_message_is_method_call(msg, "org.mprs.MediaPlayer2.Playlists", "GetPlaylists")) {
        uint32_t index, max_count;
        char *order;
        bool reverse_order;
        if (!dbus_message_get_args(msg, &err, DBUS_TYPE_UINT32, &index, DBUS_TYPE_UINT32, &max_count, DBUS_TYPE_STRING,
                                   &order, DBUS_TYPE_BOOLEAN, &reverse_order, DBUS_TYPE_INVALID)) {
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
                fprintf(stderr, "[dbus] Error while decoding arguments: %s\n", err.message);
            }
            return 0;
        }
        PlaylistInfo *playlists;
        size_t count;
        get_all_playlist_info(&playlists, &count);

        int (*compar)(const void *, const void *) = compare_alphabetical;
        if (reverse_order){
            if (order[0]=='P' || order[0]=='p') compar = compare_last_played_reverse;
            else compar = compare_alphabetical_reverse;
        }else{
            if (order[0]=='P' || order[0]=='p') compar = compare_last_played;
        }
        qsort(playlists, count, sizeof(*playlists), compar);

        count = count > max_count ? max_count : count;

        dbus_uint32_t serial = 0;
        DBusMessage *ret = dbus_message_new_method_return(msg);
        DBusMessageIter reply, arr;
        dbus_message_iter_init_append(ret, &reply);
        dbus_message_iter_open_container(&reply, DBUS_TYPE_ARRAY, "(oss)", &arr);
        for (uint32_t i = index; i < count; ++i) {
            get_playlist_dbus(&arr, &playlists[i]);
        }
        dbus_message_iter_close_container(&reply, &arr);

        // send the reply && flush the connection
        if (!dbus_connection_send(conn, ret, &serial)) {
            fprintf(stderr, "[dbus] Out Of Memory!\n");
            exit(1);
        }

        // free the reply
        dbus_message_unref(ret);
        return 2;
    } else {
        return 0;
    }
    return 1;
}

void
handle_message() {
    running = true;
    while (running) {
        // non blocking read of the next available message
        dbus_connection_read_write(conn, 0);
        DBusMessage *msg = dbus_connection_pop_message(conn);

        // loop again if we haven't got a message
        if (NULL == msg) {
            usleep(10000);
            continue;
        }

        int ret = 0;
        dbus_uint32_t serial = 0;
        // check this is a method call for the right interface & method
        if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "GetAll")) {
            // Read arguments to determine which response to give
            char *param;
            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args)) {
                fprintf(stderr, "[dbus] Message has no arguments!\n");
            } else if (DBUS_TYPE_STRING != dbus_message_iter_get_arg_type(&args)) {
                fprintf(stderr, "[dbus] Argument is not string!\n");
            } else {
                // Default to giving information about our capabilities as a player,
                // but if asked give information about our capabilities through MPRIS
                dbus_message_iter_get_basic(&args, &param);
                printf("[dbus] Status\n");
                if (strcmp(param, "org.mpris.MediaPlayer2") == 0) {
                    getall_mediaplayer(msg, conn);
                } else if (strcmp(param, "org.mpris.MediaPlayer2.Player") == 0) {
                    getall_mediaplayer_player(msg, conn);
                } else if (strcmp(param, "org.mpris.MediaPlayer2.Playlists") == 0) {
                    getall_mediaplayer_playlists(msg, conn);
                } else {
                    goto illegal_call;
                }
            }
        } else if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "Get")) {
            // Read arguments to determine which response to give
            char *param;
            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args)) {
                fprintf(stderr, "[dbus] Message has no arguments!\n");
            } else if (DBUS_TYPE_STRING != dbus_message_iter_get_arg_type(&args)) {
                fprintf(stderr, "[dbus] Argument is not string!\n");
            } else {
                // Default to giving information about our capabilities as a player,
                // but if asked give information about our capabilities through MPRIS
                dbus_message_iter_get_basic(&args, &param);
                dbus_message_iter_next(&args);
                if (DBUS_TYPE_STRING != dbus_message_iter_get_arg_type(&args)) {
                    fprintf(stderr, "[dbus] Argument is not string!\n");
                    goto illegal_call;
                }
                char *property;
                dbus_message_iter_get_basic(&args, &property);
                printf("[dbus] Status\n");
                if (strcmp(param, "org.mpris.MediaPlayer2") == 0) {
                    get_mediaplayer(msg, conn, property);
                } else if (strcmp(param, "org.mpris.MediaPlayer2.Player") == 0) {
                    get_mediaplayer_player(msg, conn, property);
                } else if (strcmp(param, "org.mpris.MediaPlayer2.Playlists") == 0) {
                    get_mediaplayer_playlists(msg, conn, property);
                } else {
                    goto illegal_call;
                }
            }
        } else if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "Set")) {
            char *param;
            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args)) {
                fprintf(stderr, "[dbus] Message has no arguments!\n");
            } else if (DBUS_TYPE_STRING != dbus_message_iter_get_arg_type(&args)) {
                fprintf(stderr, "[dbus] Argument is not string!\n");
            } else {
                dbus_message_iter_get_basic(&args, &param);

                dbus_message_iter_next(&args);
                if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING) {
                    fprintf(stderr, "[dbus] Invalid argument type\n");
                    goto illegal_call;
                }
                char *property = NULL;
                dbus_message_iter_get_basic(&args, &property);
                dbus_message_iter_next(&args);
                if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_VARIANT) {
                    fprintf(stderr, "[dbus] Invalid argument type\n");
                    goto illegal_call;
                }
                DBusMessageIter sub;
                dbus_message_iter_recurse(&args, &sub);

                if (!strcmp(param, "org.mpris.MediaPlayer2.Player")) {
                    set_values_player(property, &sub);
                }
            }
            // If this is a player command, run it and return nothing
        } else if (check_player_command(msg)) {
            DBusMessage *reply = dbus_message_new_method_return(msg);

            // send the reply && flush the connection
            if (!dbus_connection_send(conn, reply, &serial)) {
                fprintf(stderr, "[dbus] Out Of Memory!\n");
            }

            // free the reply
            dbus_message_unref(reply);
        } else if ((ret = check_playlist_command(msg))) {
            if (ret==1){
                DBusMessage *reply = dbus_message_new_method_return(msg);

                // send the reply && flush the connection
                if (!dbus_connection_send(conn, reply, &serial)) {
                    fprintf(stderr, "[dbus] Out Of Memory!\n");
                }

                // free the reply
                dbus_message_unref(reply);
            }
        }
            // If we don't recognize the call, say so
        else
            illegal_call:
            {
                char error_string[127];
                snprintf(error_string, 127, "Illegal Call: %s.%s on %s", dbus_message_get_interface(msg),
                         dbus_message_get_member(msg), dbus_message_get_path(msg));

                DBusMessage *error_reply = dbus_message_new_error(msg, DBUS_ERROR_FAILED, error_string);

                if (!dbus_connection_send(conn, error_reply, &serial)) {
                    fprintf(stderr, "[dbus] Out Of Memory!\n");
                    exit(1);
                }

                // free the reply
                dbus_message_unref(error_reply);
            }

        // free the message
        dbus_message_unref(msg);
    }
}


