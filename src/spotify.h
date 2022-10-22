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
#define PLAYLIST_NAME_LEN (SPOTIFY_ID_LEN+1)
#define PLAYLIST_NAME_LEN_NULL (PLAYLIST_NAME_LEN+1)
#define CONNECTION_POOL_MAX 10
#define SPOTIFY_MAX_INSTANCES 10
#define SPOTIFY_PORT 5394

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
    char *spotify_name_escaped;
    char *spotify_album_art;
    char *artist;
    char *artist_escaped;
    char spotify_artist_id[SPOTIFY_ID_LEN_NULL];
    uint32_t duration_ms;
    uint32_t download_state; //enum DownloadState
    PlaylistInfo *playlist;
} Track;

enum spotify_packet_type {
    MUSIC_DATA = 0,
    MUSIC_INFO = 1,
    PLAYLIST_INFO = 2,
    ALBUM_INFO = 3,
    RECOMMENDATIONS = 4
};

enum error_type {
    ET_NO_ERROR,
    ET_SPOTIFY,
    ET_HTTP,
    ET_FULL
};
struct connection;

typedef void (*spotify_conn_cb)(struct bufferevent *bev, struct connection *conn, void *arg);
typedef int(*json_parse_func)(char *data, size_t len, Track **tracks, size_t *track_size, size_t *track_len);
typedef void(*info_received_cb)(struct spotify_state *spotify, Track *tracks);

struct parse_func_params{
    Track **tracks;
    size_t *track_size;
    size_t *track_len;
    char *path;
    json_parse_func func;
    info_received_cb func1;
};

struct spotify_state {
    struct connection {
        struct bufferevent *bev;
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
    } connections[CONNECTION_POOL_MAX];
    size_t connections_len;
    struct event_base *base;
    char **instances;
    size_t instances_len;
    struct decode_context decode_ctx;
    struct smp_context *smp_ctx;
};

void get_token();

void ensure_token();

void *init_spotify(void *state);

void clear_tracks(Track *tracks, size_t *track_len, size_t *track_size);

int play_track(struct spotify_state *spotify, const char id[SPOTIFY_ID_LEN], struct evbuffer *buf);

int ensure_track(struct spotify_state *spotify, const char id[SPOTIFY_ID_LEN]);

int add_track_info(struct spotify_state *spotify, const char id[SPOTIFY_ID_LEN], Track **tracks, size_t *track_size, size_t *track_len,
                   info_received_cb func);

int add_playlist(struct spotify_state *spotify, const char id[SPOTIFY_ID_LEN], Track **tracks, size_t *track_size, size_t *track_len,
                 bool album, info_received_cb func);

int search(const char *query, Track **tracks, size_t *tracks_count, PlaylistInfo **playlists, size_t *playlist_count,
           PlaylistInfo **albums, size_t *album_count);

int track_by_id(const char *id, Track **track);

int get_playlist(char *playlistId, PlaylistInfo *playlistOut, Track **tracksOut);

int get_album(char *albumId, PlaylistInfo *playlistOut, Track **tracksOut);

void free_track(Track *track);

void free_tracks(Track *track, size_t count);

void free_playlist(PlaylistInfo *playlist);

void download_track(Track *track, bool block, FILE **pcm);

void track_filepath(Track *track, char **out);

void playlist_filepath(char *id, char **out, bool album);

int get_recommendations(char **seed_tracks, size_t seed_track_count, char **seed_artists, size_t seed_artist_count,
                        char **seed_genres, size_t genre_count, size_t limit, Track **tracksOut, size_t *track_count);

int get_recommendations_from_tracks(Track *tracks, size_t track_count, size_t limit, Track **tracksOut,
                                    size_t *track_count_out);

void save_playlist_to_file(const char *path, PlaylistInfo *playlist, Track *tracks);

void save_playlist_last_played_to_file(const char *path, PlaylistInfo *playlist);

void read_playlist_from_file(const char *path, PlaylistInfo *playlistOut, Track **tracksOut);

void read_playlist_info_from_file(const char *path, PlaylistInfo *playlistOut);

size_t get_saved_playlist_count();

void get_all_playlist_info(PlaylistInfo **playlistInfo, size_t *count);

void cleanup();

#endif
