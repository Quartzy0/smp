#ifndef H_CURLUTIL
#define H_CURLUTIL

#include <curl/curl.h>
#include <stdbool.h>
#include <vorbis/codec.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <time.h>

#define TIMER_START(name) clock_t __gen_timer_ ##name = clock()
#define TIMER_END(name) printf("Timer '" #name "' took %2.f ms\n", (double) (clock()-__gen_timer_##name) / (double) CLOCKS_PER_SEC * 1000.0)

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

struct smp_context{
    struct event_base *base;
    int action_fd[2];
    struct evbuffer *audio_buf;
    struct spotify_state *spotify;
    struct audio_info{
        double volume;
        size_t sample_rate;
        size_t bitrate;
        size_t offset;
        size_t total_frames;
        int channels;
        struct audio_info *previous;
    } audio_info;
    struct audio_info previous;
};

struct decode_context{
    enum VorbisDecodeState {
        START,
        HEADERS,
        DECODE,
        EOS
    } state;

    ogg_sync_state oy; /* sync and verify incoming physical bitstream */
    ogg_stream_state os; /* take physical pages, weld into a logical
                          stream of packets */
    ogg_page og; /* one Ogg bitstream page. Vorbis packets are inside */
    ogg_packet op; /* one raw packet of data for decode */

    vorbis_info vi; /* struct that stores all the static vorbis bitstream
                          settings */
    vorbis_comment vc; /* struct that stores all the bitstream user comments */
    vorbis_dsp_state vd; /* central working state for the packet->PCM decoder */
    vorbis_block vb; /* local working space for packet->PCM decode */
    int p;
    int zero_count;
};

typedef void(*audio_info_cb)(struct audio_info *info);

extern LoopMode loop_mode;
extern bool shuffle;

int read_url(const char *url, Response *response, struct curl_slist *headers);

// https://gist.github.com/jesobreira/4ba48d1699b7527a4a514bfa1d70f61a
char *urlencode(const char *src);

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

int
decode_vorbis(struct evbuffer *in, struct evbuffer *buf_out, struct decode_context *ctx, size_t *progress,
              struct audio_info *info, audio_info_cb cb);

#endif
