#ifndef SMP_SPOTIFY_H
#define SMP_SPOTIFY_H

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <event2/bufferevent.h>
#include "util.h"

#define SPOTIFY_ID_LEN 22
#define SPOTIFY_ID_LEN_NULL (SPOTIFY_ID_LEN+1)
#define SPOTIFY_URI_LEN 36
#define SPOTIFY_URI_LEN_NULL (SPOTIFY_URI_LEN+1)
#define PLAYLIST_NAME_LEN SPOTIFY_ID_LEN
#define PLAYLIST_NAME_LEN_NULL (PLAYLIST_NAME_LEN+1)
#define CONNECTION_POOL_MAX 10
#define SPOTIFY_PORT 5394

struct spotify_state;

typedef enum DownloadState {
    DS_NOT_DOWNLOADED,
    DS_DOWNLOADING,
    DS_DOWNLOADED,
    DS_DOWNLOAD_FAILED
} DownloadState;

typedef struct PlaylistInfo {
    bool not_empty; //Used when no playlist is playing
    bool album; //This struct represents both playlists and albums.
    char *name;
    char *image_url;
    char spotify_id[SPOTIFY_ID_LEN_NULL];
    time_t last_played;
    uint32_t track_count;

    size_t reference_count; // Used when songs from multiple playlists are in the queue allowing each track to point to the correct playlist
} PlaylistInfo;

typedef struct Track {
    char spotify_id[SPOTIFY_ID_LEN_NULL];
    char spotify_uri[SPOTIFY_URI_LEN_NULL];
    char *spotify_name;
    char *spotify_album_art;
    char *artist;
    char spotify_artist_id[SPOTIFY_ID_LEN_NULL];
    char *regions;
    size_t region_count;
    uint32_t duration_ms;
    uint32_t download_state; //enum DownloadState
    PlaylistInfo *playlist;
} Track;

typedef struct Artist {
    char spotify_id[SPOTIFY_ID_LEN_NULL];
    char *name;
    uint32_t followers;
} Artist;

enum spotify_packet_type {
    MUSIC_DATA = 0,
    MUSIC_INFO = 1,
    PLAYLIST_INFO = 2,
    ALBUM_INFO = 3,
    RECOMMENDATIONS = 4,
    ARTIST_INFO = 5,
    SEARCH = 6,
    AVAILABLE_REGIONS = 7,
};

enum error_type {
    ET_NO_ERROR = 0,
    ET_SPOTIFY = 1,
    ET_SPOTIFY_INTERNAL = 2,
    ET_HTTP = 3,
    ET_FULL = 4
};
struct connection;

struct spotify_search_results {
    bool qtracks, qalbums, qplaylists, qartists;
    Track *tracks;
    size_t track_len;
    PlaylistInfo *albums;
    size_t album_len;
    PlaylistInfo *playlists;
    size_t playlist_len;
    Artist *artists;
    size_t artist_len;
    void *userp;
};

typedef void (*spotify_conn_cb)(struct bufferevent *bev, struct connection *conn, void *arg);

typedef int(*json_parse_func)(const char *data, size_t len, void *userp);

typedef void(*info_received_cb)(struct spotify_state *spotify, void *userp);

typedef void(*connection_error_cb)(struct connection *conn, void *userp);

struct parse_func_params {
    char *path;
    json_parse_func func;
    void *func_userp;
    info_received_cb func1;
    void *func1_userp;
};

struct spotify_state {
    struct connection {
        struct bufferevent *bev;
        struct backend_instance *inst;
        bool busy;

        size_t expecting;
        size_t progress;
        struct spotify_state *spotify;
        spotify_conn_cb cb;
        void *cb_arg;
        FILE *cache_fp;
        char *cache_path;
        struct parse_func_params params;

        char *error_buffer;
        enum error_type error_type;

        char *payload;
        size_t payload_len;
        int retries;
    } connections[CONNECTION_POOL_MAX];
    size_t connections_len;
    struct event_base *base;
    struct decode_context decode_ctx;
    struct smp_context *smp_ctx;

    Track *tracks;
    size_t track_count;
    size_t track_size;

    connection_error_cb err_cb;
    void *err_userp;
};

void clear_tracks(Track *tracks, size_t *track_len, size_t *track_size);

int play_track(struct spotify_state *spotify, const Track *track, struct buffer *buf, struct connection **conn_out);

int ensure_track(struct spotify_state *spotify, const Track *track, char *region, struct connection **conn_out);

int refresh_available_regions(struct spotify_state *spotify);

int
add_track_info(struct spotify_state *spotify, const char id[22], Track **tracks, size_t *track_size, size_t *track_len,
               info_received_cb func, void *userp);

int
add_playlist(struct spotify_state *spotify, const char id[22], Track **tracks, size_t *track_size, size_t *track_len,
             bool album, info_received_cb func, void *userp, info_received_cb read_local_cb);

int
add_recommendations(struct spotify_state *spotify, const char *track_ids, const char *artist_ids, size_t track_count,
                    size_t artist_count, Track **tracks, size_t *track_size, size_t *track_len, info_received_cb func,
                    void *userp);

int
add_recommendations_from_tracks(struct spotify_state *spotify, Track **tracks, size_t *track_size, size_t *track_len,
                                info_received_cb func, void *userp);

int
search(struct spotify_state *spotify, const char *query, info_received_cb cb, bool tracks, bool artists, bool albums,
       bool playlists, void *userp);

void free_track(Track *track);

void free_tracks(Track *track, size_t count);

void deref_playlist(PlaylistInfo *playlist);

void free_playlist(PlaylistInfo *playlist);

void free_artist(Artist *artist);

size_t get_saved_playlist_count();

int get_all_playlist_info(PlaylistInfo **playlistInfo, size_t *countOut);

void track_info_filepath_id(const char id[SPOTIFY_ID_LEN], char **out);

void track_filepath_id(const char id[SPOTIFY_ID_LEN], char **out);

void cancel_track_transfer(struct connection *conn);

#endif
