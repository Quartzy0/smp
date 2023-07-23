#ifndef H_CURLUTIL
#define H_CURLUTIL

#include <curl/curl.h>
#include <stdbool.h>
#include <vorbis/codec.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <time.h>
#include <dbus/dbus.h>
#include "dbus-util.h"

#define TIMER_START(name) clock_t __gen_timer_ ##name = clock()
#define TIMER_END(name) printf("Timer '" #name "' took %2.f ms\n", (double) (clock()-__gen_timer_##name) / (double) CLOCKS_PER_SEC * 1000.0)

struct buffer {
    float *buf;
    size_t size;
    size_t len;
    size_t offset;
};

typedef enum LoopMode {
    LOOP_MODE_NONE = 0,
    LOOP_MODE_PLAYLIST,
    LOOP_MODE_TRACK,
    LOOP_MODE_LAST
} LoopMode;

enum UriType{
    URI_INVALID = 0,
    URI_TRACK = 1,
    URI_ALBUM = 2,
    URI_PLAYLIST = 3,
};

struct decode_context {
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
    bool cb_called;
};
struct audio_info;

typedef void(*audio_info_cb)(void *userp, struct audio_info *info, struct audio_info *previous);

extern LoopMode loop_mode;
extern bool shuffle;

#ifndef VEC_STEP
#define VEC_STEP 10
#endif

// https://gist.github.com/jesobreira/4ba48d1699b7527a4a514bfa1d70f61a
char *urlencode(const char *src);

enum UriType id_from_url(const char *src, char *out);

void sanitize(char **in);

bool str_is_empty(const char *str);

//https://stackoverflow.com/a/49028514
int rek_mkdir(const char *path);

FILE *fopen_mkdir(const char *path, char *mode);

int
decode_vorbis(struct evbuffer *in, struct buffer *buf_out, struct decode_context *ctx, size_t *progress,
              struct audio_info *info, struct audio_info *previous, audio_info_cb cb, void *userp);

void
clean_vorbis_decode(struct decode_context *ctx);

#endif
