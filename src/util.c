#include "util.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include "spotify.h"

Action action_queue[QUEUE_MAX];
unsigned queue_front = 0;
unsigned queue_end = 0;
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

int read_url(const char *url, Response *response, struct curl_slist *headers) {
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
urlencode(char *src) {
    return curl_easy_escape(NULL, src, (int) strlen(src));
}

ActionType
id_from_url(const char *src, char *out) {
    char *first = strchr(src, ':');
    if (!first) return ACTION_NONE;
    char *second = strchr(first + 1, ':');
    if (!second) return ACTION_NONE;
    second++;
    ActionType type;
    if (!memcmp(first + 1, "track", 5)) {
        type = ACTION_TRACK;
    } else if (!memcmp(first + 1, "album", 5)) {
        type = ACTION_ALBUM;
    } else if (!memcmp(first + 1, "playlist", 8)) {
        type = ACTION_PLAYLIST;
    } else {
        type = ACTION_NONE;
    }
    unsigned count = strlen(src) - (second - src);
    memcpy(out, second, count);
    out[count] = 0;
    return type;
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
    return strcmp(((const PlaylistInfo*) a)->name, ((const PlaylistInfo*) b)->name);
}

int
compare_alphabetical_reverse(const void *a, const void *b) {
    return -strcmp(((const PlaylistInfo*) a)->name, ((const PlaylistInfo*) b)->name);
}

int
compare_last_played(const void *a, const void *b){
    return (int) (((const PlaylistInfo*) b)->last_played-((const PlaylistInfo*) a)->last_played);
}

int
compare_last_played_reverse(const void *a, const void *b){
    return (int) (((const PlaylistInfo*) a)->last_played-((const PlaylistInfo*) b)->last_played);
}