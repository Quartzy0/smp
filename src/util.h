#ifndef H_CURLUTIL
#define H_CURLUTIL

#include <curl/curl.h>
#include <stdbool.h>

typedef struct Response {
    char *data;
    unsigned int size;
} Response;

typedef enum LoopMode {
    LOOP_MODE_NONE = 0,
    LOOP_MODE_PLAYLIST,
    LOOP_MODE_TRACK,
    LOOP_MODE_LAST
} LoopMode;

typedef enum ActionType {
    ACTION_NONE,
    ACTION_PLAY,
    ACTION_PAUSE,
    ACTION_PLAYPAUSE,
    ACTION_STOP,
    ACTION_QUIT,
    ACTION_PLAYLIST,
    ACTION_TRACK,
    ACTION_ALBUM,
    ACTION_POSITION_RELATIVE,
    ACTION_POSITION_ABSOLUTE,
    ACTION_TRACK_OVER,
    ACTION_SEEK,
    ACTION_SET_POSITION
} ActionType;

//6yKMo95JkibE9tGN81dgFh
typedef struct Action {
    ActionType type;
    union {
        char id[23]; // All spotify ids are 22 chars long (track, album and playlist ids) + 1 null byte
        int64_t position;
    };
} Action;

struct ArtistQuantity {
    char id[23];
    size_t appearances;
};

#define QUEUE_MAX 100

extern Action action_queue[QUEUE_MAX];
extern unsigned queue_front;
extern unsigned queue_end;

#define rear action_queue[++queue_end >= QUEUE_MAX ? (queue_end = 0) : queue_end]

#define last_rear action_queue[queue_end]

#define front action_queue[++queue_front >= QUEUE_MAX ? (queue_front = 0) : queue_front]

extern LoopMode loop_mode;
extern bool shuffle;

int read_url(const char *url, Response *response, struct curl_slist *headers);

// https://gist.github.com/jesobreira/4ba48d1699b7527a4a514bfa1d70f61a
char *urlencode(char *src);

void clean_encode_curl();

ActionType id_from_url(const char *src, char *out);

void sanitize(char **in);

int compare_alphabetical(const void *a, const void *b);

int compare_alphabetical_reverse(const void *a, const void *b);

int compare_last_played(const void *a, const void *b);

int compare_last_played_reverse(const void *a, const void *b);

int compare_artist_quantities(const void *a, const void *b);

bool str_is_empty(const char *str);

//https://stackoverflow.com/a/49028514
void rek_mkdir(const char *path);

FILE *fopen_mkdir(const char *path, char *mode);

#endif
