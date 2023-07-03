//
// Created by quartzy on 6/30/22.
//

#ifndef SMP_DBUS_CLIENT_H
#define SMP_DBUS_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "spotify.h"

typedef enum PlaybackStatus {
    PBS_PLAYING,
    PBS_PAUSED,
    PBS_STOPPED
} PlaybackStatus;

typedef enum PlaylistOrder {
    ORDER_ALPHABETICAL = 0,
    ORDER_CREATED,
    ORDER_MODIFIED,
    ORDER_PLAYED,
    ORDER_USER
} PlaylistOrder;

typedef struct DBusPlaylistInfo {
    bool valid;
    char *id; //DBus object path, not spotify id
    char *name;
    char *icon;
} DBusPlaylistInfo;

typedef struct Metadata {
    char *track_id;
    int64_t length;
    char *art_url;
    char *artist;
    char *title;
    char *url;
} Metadata;

typedef struct PlayerProperties {
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

void dbus_client_tracklist_get_tracks(char ***out, int *count);

int dbus_client_get_tracks_metadata(char **tracks, int count, Metadata *out);

void free_metadata(Metadata *metadata);

int
dbus_client_get_playlists(uint32_t index, uint32_t max_count, PlaylistOrder order, bool reverse, DBusPlaylistInfo *out,
                          int *count_out);

int dbus_client_get_playlist_count(uint32_t *count);

int dbus_client_get_active_playlist(DBusPlaylistInfo *out);

int dbus_client_activate_playlist(const char *id /* DBus object path */);

void dbus_client_call_method(const char *iface, const char *method);

void dbus_client_set_property(const char *iface, const char *name, int type, void *value);

int dbus_client_get_property(const char *iface, const char *name, int type, void *value);

int dbus_client_search(bool tracks, bool albums, bool artists, bool playlists, char *query, Track **tracksOut,
                       size_t *tracks_len, PlaylistInfo **albumsOut, size_t *albums_len, Artist **artistsOut,
                       size_t *artists_len, PlaylistInfo **playlistsOut, size_t *playlists_len);

void print_properties(FILE *stream, PlayerProperties *properties);

void free_dbus_playlist(DBusPlaylistInfo *in);

#endif //SMP_DBUS_CLIENT_H
