#include <pthread.h>
#include <stdlib.h>
#include "spotify.h"
#include "dbus.h"
#include "config.h"
#include "cli.h"
#include "ctrl.h"
#include <event2/event.h>
#include <unistd.h>


void
dbus_poll(int fd, short what, void *arg){
    struct dbus_state *dbus_state = (struct dbus_state*) arg;
    handle_message(dbus_state->bus);
}

static int check_for_folder(const char *path){
    if (access(track_info_path, F_OK)){
        printf("'%s' doesn't exist, creating\n", path);
        return rek_mkdir(path);
    } else if (access(track_info_path, R_OK | W_OK)){
        fprintf(stderr, "'%s' exists but can't be written/read to\n", path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    srand(clock());
    if (handle_cli(argc, argv)) {
        return EXIT_SUCCESS;
    }

    if (load_config()) {
        return EXIT_FAILURE;
    }
    if (check_for_folder(track_save_path)) return 1;
    if (check_for_folder(track_info_path)) return 1;
    if (check_for_folder(album_info_path)) return 1;
    if (check_for_folder(playlist_info_path)) return 1;

    struct event_base *base = event_base_new();
    struct smp_context *ctx = ctrl_create_context(base);

    struct dbus_state *dbus_state = init_dbus(ctx);
    struct event *dbus_polling = event_new(base, -1, EV_PERSIST, dbus_poll, dbus_state);
    struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = 50000,
    };
    event_add(dbus_polling, &tv);

    ctrl_init_audio(ctx, dbus_state->mplayer_iface, initial_volume);

    refresh_available_regions(ctrl_get_spotify_state(ctx));

    event_base_dispatch(base);

    event_free(dbus_polling);
    ctrl_free(ctx);
    dbus_util_free_bus(dbus_state->bus);
    free(dbus_state);
    clean_config();
    event_base_free(base);

    return EXIT_SUCCESS;
}
