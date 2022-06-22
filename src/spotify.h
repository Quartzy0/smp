#ifndef SMP_SPOTIFY_H
#define SMP_SPOTIFY_H

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>

#define SPOTIFY_ID_LEN 22
#define SPOTIFY_ID_LEN_NULL (SPOTIFY_ID_LEN+1)
#define SPOTIFY_URI_LEN 36
#define SPOTIFY_URI_LEN_NULL (SPOTIFY_URI_LEN+1)
#define PLAYLIST_NAME_LEN (SPOTIFY_ID_LEN+1)
#define PLAYLIST_NAME_LEN_NULL (PLAYLIST_NAME_LEN+1)

typedef enum DownloadState {
    DS_NOT_DOWNLOADED,
    DS_DOWNLOADING,
    DS_DOWNLOADED
} DownloadState;

typedef struct Track {
    char spotify_id[SPOTIFY_ID_LEN_NULL];
    char spotify_uri[SPOTIFY_URI_LEN_NULL];
    char *spotify_name;
    char *spotify_name_escaped;
    char *spotify_album_art;
    char *artist;
    char *artist_escaped;
    uint32_t download_state; //enum DownloadState
} Track;

typedef struct PlaylistInfo { //Same as the playlist struct but without the tracks. Used when sending info about non-loaded playlists over dbus
    bool album; //This struct represents both playlists and albums.
    char *name;
    char *image_url;
    char spotify_id[SPOTIFY_ID_LEN_NULL];
    time_t last_played;
    uint32_t track_count;
} PlaylistInfo;

extern char *authHeader;

void get_token();

void ensure_token();

void search(const char *query, Track **tracks, size_t *count);

int track_by_id(const char *id, Track **track);

int get_playlist(char *playlistId, PlaylistInfo *playlistOut, Track **tracksOut);

int get_album(char *albumId, PlaylistInfo *playlistOut, Track **tracksOut);

void free_track(Track *track);

void free_tracks(Track *track, size_t count);

void free_playlist(PlaylistInfo *playlist);

void download_track(Track *track, bool block);

void track_filepath(Track *track, char **out);

void playlist_filepath(char *id, char **out, bool album);

int get_recommendations(char seed_tracks[23][5], char seed_artists[23][5], char **seed_genres, size_t genre_count,
                        size_t limit, Track **tracksOut, size_t *track_count);

void save_playlist_to_file(const char *path, PlaylistInfo *playlist, Track *tracks);

void save_playlist_last_played_to_file(const char *path, PlaylistInfo *playlist);

void read_playlist_from_file(const char *path, PlaylistInfo *playlistOut, Track **tracksOut);

void read_playlist_info_from_file(const char *path, PlaylistInfo *playlistOut);

size_t get_saved_playlist_count();

void get_all_playlist_info(PlaylistInfo **playlistInfo, size_t *count);

void cleanup();

#endif
