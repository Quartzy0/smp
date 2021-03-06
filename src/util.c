#include "util.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
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

const char find_str[] = "???";
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