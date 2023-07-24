#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include "spotify.h"
#include "audio.h"

LoopMode loop_mode = LOOP_MODE_NONE;
bool shuffle = false;

enum UriType
id_from_url(const char *src, char *out) {
    char s;
    if (*src == 'h') { // e.g. https://open.spotify.com/track/4pQRZ0Pt9VPWtqpYsMvomM ...
        s = '/';
    } else if (*src == 's') { // e.g. spotify:track:4pQRZ0Pt9VPWtqpYsMvomM
        s = ':';
    } else {
        return URI_INVALID;
    }
    char *last = strrchr(src, s);
    if (!last) return URI_INVALID;
    if (strlen(last + 1) < SPOTIFY_ID_LEN) return URI_INVALID;
    for (int i = 0; i < SPOTIFY_ID_LEN; ++i) {
        if (!isalnum(last[1+i])) return URI_INVALID;
    }
    memcpy(out, last + 1, SPOTIFY_ID_LEN);
    out[SPOTIFY_ID_LEN] = 0;
    switch (*(last - 1)) {
        case 'k': // tracK
            return URI_TRACK;
        case 'm': // albuM
            return URI_ALBUM;
        case 't': // playlisT
            return URI_PLAYLIST;
    }
    return URI_INVALID;
}

const char find_str[] = "’";
const size_t find_len = sizeof(find_str);

void
sanitize(char **in) {
    char *pp = *in;
    char *p = *in;
    while ((p = strstr(p, find_str))) {
        if (pp != *in) {
            memmove(pp - find_len + 1, pp, p - pp);
        }
        *p = '\'';
        p += find_len;
        pp = p;
    }
    if (pp != *in) {
        memmove(pp - find_len + 1, pp, strlen(*in) - (pp - *in));
    }
}

bool str_is_empty(const char *str) {
    size_t len = strlen(str);
    for (int i = 0; i < len; ++i) {
        if (!isspace(str[i]))return false;
    }
    return true;
}

int
rek_mkdir(const char *path) {
    char *sep = strrchr(path, '/');
    if (sep != NULL) {
        *sep = 0;
        if(rek_mkdir(path)) return 1;
        *sep = '/';
    }
    if (strlen(path) == 0)return 0;
    if (mkdir(path, 0777) && errno != EEXIST){
        printf("[util] Error while trying to create '%s': %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

FILE
*fopen_mkdir(const char *path, char *mode) {
    char *sep = strrchr(path, '/');
    if (sep) {
        char *path0 = strdup(path);
        path0[sep - path] = 0;
        rek_mkdir(path0);
        free(path0);
    }
    return fopen(path, mode);
}

int
decode_vorbis(struct evbuffer *in, struct buffer *buf_out, struct decode_context *ctx, size_t *progress,
              struct audio_info *info, struct audio_info *previous, audio_info_cb cb, void *userp) {
    switch (ctx->state) {
        case START: {
            ogg_sync_init(&ctx->oy);
            ctx->state = HEADERS;
            ctx->p = 0;
            ctx->cb_called = false;
        }
        case HEADERS: {
            size_t buf_len = evbuffer_get_length(in);
            char *buf = ogg_sync_buffer(&ctx->oy, buf_len);
            size_t bytes = evbuffer_remove(in, buf, buf_len);
            *progress += bytes;
            ogg_sync_wrote(&ctx->oy, bytes);

            int result = ogg_sync_pageout(&ctx->oy, &ctx->og);
            if (result == 0)break; /* Need more data */

            if (result == 1) {
                if (ctx->p == 0) {
                    ogg_stream_init(&ctx->os, ogg_page_serialno(&ctx->og));
                    vorbis_info_init(&ctx->vi);
                    vorbis_comment_init(&ctx->vc);
                }

                ogg_stream_pagein(&ctx->os, &ctx->og); /* we can ignore any errors here
                                         as they'll also become apparent
                                         at packetout */
                while (ctx->p < 3) {
                    result = ogg_stream_packetout(&ctx->os, &ctx->op);
                    if (result == 0)break;
                    if (result < 0) {
                        /* Uh oh; data at some point was corrupted or missing!
                           We can't tolerate that in a header.  Die. */
                        fprintf(stderr, "Corrupt secondary header.  Exiting.\n");
                        return -1;
                    }
                    result = vorbis_synthesis_headerin(&ctx->vi, &ctx->vc, &ctx->op);
                    if (result < 0) {
                        fprintf(stderr, "Corrupt secondary header.  Exiting.\n");
                        return -1;
                    }
                    ctx->p++;
                }

                if (ctx->p >= 3) {
                    audio_info_set(info, ctx->vi.rate, ctx->vi.bitrate_nominal, ctx->vi.channels);
                    if (cb) {
                        cb(userp, info, previous);
                        ctx->cb_called = true;
                    }
                    if (vorbis_synthesis_init(&ctx->vd, &ctx->vi)) {
                        fprintf(stderr, "Error: Corrupt header during playback initialization.\n");
                        return -1;
                    }
                    vorbis_block_init(&ctx->vd, &ctx->vb);
                    ctx->state = DECODE;
                    ctx->p = 0;
                    goto decode_no_read;
                }
            }

            break;
        }
        case DECODE: {
            ctx->p = 0;
            size_t buf_len = evbuffer_get_length(in);
            if (!buf_len) return 1;
            char *buf = ogg_sync_buffer(&ctx->oy, buf_len);
            size_t bytes = evbuffer_remove(in, buf, buf_len);
            *progress += bytes;
            ogg_sync_wrote(&ctx->oy, bytes);

            decode_no_read:;
            int fails = 0;
            while (1) {
                int result = ogg_sync_pageout(&ctx->oy, &ctx->og);
                if (result == 0)break; /* need more data */
                if (result < 0) { /* missing or corrupt data at this page position */
                    fprintf(stderr, "Corrupt or missing data in bitstream; "
                                    "continuing...\n");
                    fails++;
                } else {
                    ogg_stream_pagein(&ctx->os, &ctx->og); /* can safely ignore errors at
                                           this point */
                    while (1) {
                        result = ogg_stream_packetout(&ctx->os, &ctx->op);

                        if (result == 0)break; /* need more data */
                        if (result < 0) { /* missing or corrupt data at this page position */
                            /* no reason to complain; already complained above */
                        } else {
                            /* we have a packet.  Decode it */
                            float **pcm;
                            unsigned int samples;

                            if (vorbis_synthesis(&ctx->vb, &ctx->op) == 0) /* test for success! */
                                vorbis_synthesis_blockin(&ctx->vd, &ctx->vb);
                            /*

                            **pcm is a multichannel float vector.  In stereo, for
                            example, pcm[0] is left, and pcm[1] is right.  samples is
                            the size of each channel.  Convert the float values
                            (-1.<=range<=1.) to whatever PCM format and write it out */

                            while ((samples = vorbis_synthesis_pcmout(&ctx->vd, &pcm)) > 0) {
                                if (buf_out->size < buf_out->len + samples * ctx->vi.channels) {
                                    float *tmp = realloc(buf_out->buf, (buf_out->size * 2) * sizeof(*tmp));
                                    if (!tmp) perror("error when reallocating audio buffer");
                                    buf_out->buf = tmp;
                                    buf_out->size = buf_out->size * 2;
                                }
                                for (int i = 0; i < samples; ++i) {
                                    for (int j = 0; j < ctx->vi.channels; ++j) {
                                        buf_out->buf[buf_out->len + i * ctx->vi.channels + j] = pcm[j][i];
                                    }
                                }

                                buf_out->len += samples * ctx->vi.channels;
                                audio_info_add_frames(info, samples);

                                ctx->p += samples * ctx->vi.channels * sizeof(*buf_out->buf);

                                vorbis_synthesis_read(&ctx->vd, samples); /* tell libvorbis how
                                                      many samples we
                                                      actually consumed */
                            }
                        }
                    }
                    if (ogg_page_eos(&ctx->og))goto eos;
                }
            }
            if (ctx->p == 0) ctx->zero_count++;
            else ctx->zero_count = 0;
            if (ctx->zero_count >= 5) {
                goto eos; // Assume stream has ended if received 0 bytes more than 5 times in a row.
            }
            return 1 + fails;
            eos:
            ctx->state = EOS;
            audio_info_set_finished(info);
            vorbis_block_clear(&ctx->vb);
            vorbis_dsp_clear(&ctx->vd);
            ogg_stream_clear(&ctx->os);
            vorbis_comment_clear(&ctx->vc);
            vorbis_info_clear(&ctx->vi);
            ogg_sync_clear(&ctx->oy);
            return 0;
        }
        case EOS: {
            // Do nothing
            return 0;
        }
    }
    return 1;
}

void
clean_vorbis_decode(struct decode_context *ctx) {
    if (!ctx) return;
    ctx->state = EOS;
    vorbis_block_clear(&ctx->vb);
    vorbis_dsp_clear(&ctx->vd);
    ogg_stream_clear(&ctx->os);
    vorbis_comment_clear(&ctx->vc);
    vorbis_info_clear(&ctx->vi);
    ogg_sync_clear(&ctx->oy);
    memset(ctx, 0, sizeof(*ctx));
}