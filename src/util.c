#include "util.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include "spotify.h"

LoopMode loop_mode = LOOP_MODE_NONE;
bool shuffle = false;

static size_t
curl_easy_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    Response **mem = (Response **) userp;

    char *ptr = realloc((*mem)->data, (*mem)->size + realsize + 1);
    if (!ptr) {
        /* out of memory! */
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    (*mem)->data = ptr;
    memcpy(&((*mem)->data[(*mem)->size]), contents, realsize);
    (*mem)->size += realsize;
    (*mem)->data[(*mem)->size] = 0;

    return realsize;
}

int
read_url(const char *url, Response *response, struct curl_slist *headers) {
    response->size = 0;
    response->data = calloc(1, 1);

    CURL *curl = curl_easy_init();
    if (curl) {
        CURLcode res;
        curl_easy_setopt(curl, CURLOPT_URL, url);
        /* send all data to this function  */
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_easy_write_callback);

        /* we pass our 'chunk' struct to the callback function */
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1l); // Follow one redirect (fixes some piped instances)
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 1);
        /* some servers do not like requests that are made without a user-agent
            field, so we provide one */
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:91.0) Gecko/20100101 Firefox/91.0");
        res = curl_easy_perform(curl);
        long response_code = 0;
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        }
        curl_easy_cleanup(curl);
        return (int) response_code;
    }
    curl_easy_cleanup(curl);
    return 0;
}

//https://gist.github.com/jesobreira/4ba48d1699b7527a4a514bfa1d70f61a
char *
urlencode(const char *src) {
    return curl_easy_escape(NULL, src, (int) strlen(src));
}

ActionType
id_from_url(const char *src, char *out) {
    char s;
    if (*src == 'h'){ // e.g. https://open.spotify.com/track/4pQRZ0Pt9VPWtqpYsMvomM ...
        s = '/';
    }else if (*src == 's'){ // e.g. spotify:track:4pQRZ0Pt9VPWtqpYsMvomM
        s = ':';
    }else{
        return ACTION_NONE;
    }
    char *last = strrchr(src, s);
    memcpy(out, last + 1, SPOTIFY_ID_LEN);
    out[SPOTIFY_ID_LEN] = 0;
    switch (*(last - 1)) {
        case 'k': // tracK
            return ACTION_TRACK;
        case 'm': // albuM
            return ACTION_ALBUM;
        case 't': // playlisT
            return ACTION_PLAYLIST;
    }
    return ACTION_NONE;
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

int
compare_alphabetical(const void *a, const void *b) {
    return strcmp(((const PlaylistInfo *) a)->name, ((const PlaylistInfo *) b)->name);
}

int
compare_alphabetical_reverse(const void *a, const void *b) {
    return -strcmp(((const PlaylistInfo *) a)->name, ((const PlaylistInfo *) b)->name);
}

int
compare_last_played(const void *a, const void *b) {
    return (int) (((const PlaylistInfo *) b)->last_played - ((const PlaylistInfo *) a)->last_played);
}

int
compare_last_played_reverse(const void *a, const void *b) {
    return (int) (((const PlaylistInfo *) a)->last_played - ((const PlaylistInfo *) b)->last_played);
}

int compare_artist_quantities(const void *a, const void *b) {
    return (int) (((struct ArtistQuantity *) b)->appearances - ((struct ArtistQuantity *) a)->appearances);
}

bool str_is_empty(const char *str){
    size_t len = strlen(str);
    for (int i = 0; i < len; ++i) {
        if (!isspace(str[i]))return false;
    }
    return true;
}

void
rek_mkdir(const char *path) {
    char *sep = strrchr(path, '/');
    if (sep != NULL) {
        *sep = 0;
        rek_mkdir(path);
        *sep = '/';
    }
    if (strlen(path) == 0)return;
    if (mkdir(path, 0777) && errno != EEXIST)
        printf("[util] Error while trying to create '%s': %s\n", path, strerror(errno));
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
              struct audio_info *info, struct audio_info *previous, audio_info_cb cb) {
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
                    info->sample_rate = ctx->vi.rate;
                    info->bitrate = ctx->vi.bitrate_nominal;
                    info->channels = ctx->vi.channels;
                    info->finished_reading = false;
                    info->total_frames = 0;
                    if (cb){
                        cb(info, previous);
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
        case DECODE:
        {
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
                            int samples;

                            if (vorbis_synthesis(&ctx->vb, &ctx->op) == 0) /* test for success! */
                                vorbis_synthesis_blockin(&ctx->vd, &ctx->vb);
                            /*

                            **pcm is a multichannel float vector.  In stereo, for
                            example, pcm[0] is left, and pcm[1] is right.  samples is
                            the size of each channel.  Convert the float values
                            (-1.<=range<=1.) to whatever PCM format and write it out */

                            while ((samples = vorbis_synthesis_pcmout(&ctx->vd, &pcm)) > 0) {
                                if (buf_out->size < buf_out->len+samples*ctx->vi.channels){
                                    float *tmp = realloc(buf_out->buf, (buf_out->size*2)*sizeof(*tmp));
                                    if (!tmp) perror("error when reallocating audio buffer");
                                    buf_out->buf = tmp;
                                    buf_out->size = buf_out->size*2;
                                }
                                for (int i = 0; i < samples; ++i) {
                                    for (int j = 0; j < ctx->vi.channels; ++j) {
                                        buf_out->buf[buf_out->len + i*ctx->vi.channels + j] = pcm[j][i];
                                    }
                                }

                                buf_out->len += samples * ctx->vi.channels;
                                info->total_frames += samples;

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
            info->finished_reading = true;
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
clean_vorbis_decode(struct decode_context *ctx){
    ctx->state = EOS;
    vorbis_block_clear(&ctx->vb);
    vorbis_dsp_clear(&ctx->vd);
    ogg_stream_clear(&ctx->os);
    vorbis_comment_clear(&ctx->vc);
    vorbis_info_clear(&ctx->vi);
    ogg_sync_clear(&ctx->oy);
}