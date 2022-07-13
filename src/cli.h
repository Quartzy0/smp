//
// Created by quartzy on 7/4/22.
//

#ifndef SMP_CLI_H
#define SMP_CLI_H

#define HELP_TXT_GENERAL    "Usage: smp SUBCOMMAND [OPTIONS...]\n\n"\
                            "The spotify music player a program which downloads and plays songs from spotify. "\
                            "It can also play albums and playlists. After a track, album or playlist has ended, "\
                            "it will use the spotify recommendations API to automatically continue playing similar"\
                            " tracks.\n\nSmp is split into two parts: the daemon and the controller. The daemon can"\
                            " be started using the command smp daemon. This part of the program downloads and plays tracks. "\
                            "On the other hand, the controller is the one who tells the daemon what tracks, playlists or albums"\
                            " to play. It can be used with the commands smp playback, smp tracks, smp search, smp open and smp quit.\n\n"\
                            "SUBCOMMANDS\n\n"\
                            "\td, daemon\n"\
                            "\t\tStarts the daemon in the current process and does not detach.\n\n"\
                            "\tpb, playback\n"\
                            "\t\tControl properties related to the playback mode.\n\n"\
                            "\tt, track\n"\
                            "\t\tControl properties related to the currently loaded tracks.\n\n"\
                            "\ts, search\n"\
                            "\t\tSearch for track, albums or playlists.\n\n"\
                            "\tq, quit\n"\
                            "\t\tQuit the daemon.\n\n"\
                            "\to, open\n"\
                            "\t\tOpen a URL to a track, album or playlist.\n\n"\
                            "\tp, playlist\n"\
                            "\t\tShow saved or currently playing playlists\n\n"\
                            "\tFor more information regarding any of the subcommands consult their individual help pages by "\
                            "running smp SUBCOMMAND --help or smp SUBCOMMAND -?\n\n"\

#define HELP_TXT_PLAYBACK   "Usage: smp playback [OPTIONS...]\n\n"\
                            "Without any options, this command will print the current status of the player. "\
                            "This includes information about the track's title, artist, uri and"\
                            " album art. Information about the player's mode is also show, like the volume, loop mode and shuffle status\n\n"\
                            "OPTIONS\n"\
                            "\t-?, --help\n"\
                            "\t\tShow this menu\n\n"\
                            "\t-t, --toggle\n"\
                            "\t\tToggle playback. If playback is paused, it will start playing and if is paused, it will start playing\n\n"\
                            "\t-p, --play\n"\
                            "\t\tContinue playing the current track. Does nothing if no track is playing\n\n"\
                            "\t-P, --pause\n"\
                            "\t\tPause the current track. Does nothing if no track is playing\n\n"\
                            "\t-n, --next\n"\
                            "\t\tWill play the next track in the playlist. The behaviour of this command varies based on the "\
                            "loop/shuffle modes. If shuffle is enabled, it will not play the next track in the playlist but will"\
                            " play a random one. It will also not play recommendations from spotify but will endlessly play songs from"\
                            " the current playlist. If loop mode is set to track, it will still play the next track. If loop mode"\
                            " is set to playlist, it will start playing the first track once the end is reached.\n\n"\
                            "\t-b, --previous\n"\
                            "\t\tThis option behaves the same way as the --next option, except it plays the previous track.\n\n"\
                            "\t-v, --volume\n"\
                            "\t\tSet the volume. The volume goes from 1 to 0.\n\n"\
                            "\t-s, --shuffle\n"\
                            "\t\tToggles whether shuffle is enabled or disabled\n\n"\
                            "\t-l, --loop\n"\
                            "\t\tCycles through the available loop modes. These are: None, Playlist or Track\n\n"\
                            "\t-S, --stop\n"\
                            "\t\tStops the player. The current playing playlist/album is freed and the audio steam is closed."\
                            " The play/pause/next/previous commands will do nothing. The player can be started again by opening another playlist/album/track.\n\n"
#define HELP_TXT_TRACK      "Usage: smp track [OPTIONS...]\n\n"\
                            "Without any options, this command will print the current status of the player. "\
                            "This includes information about the track's title, artist, uri and"\
                            " album art. Information about the player's mode is also show, like the volume, loop mode and shuffle status\n\n" \
                            "OPTIONS\n"\
                            "\t-?, --help\n"\
                            "\t\tShow this menu\n\n"\
                            "\t-l, --list\n"\
                            "\t\tList the tracks in the track list. This can mean the tracks in the current playlist/album, "\
                            "the recommendations from a track/playlist/album or just the a single track that is playing.\n\n"
#define HELP_TXT_SEARCH     "Usage: smp search [FLAGS] QUERY\n\n"\
                            "Without any of the of the flags enabled, it is assumed that all of the flags are enabled. "\
                            "If any of them are enabled, however, the rest are considered to be disabled.\n\n"\
                            "Once the search results are retrieved, they are printed and the user is prompted "\
                            "for if they wish to play any of them.\n\n"\
                            "FLAGS\n"\
                            "\t-t, --tracks\n"\
                            "\t\tSearch for tracks\n\n"\
                            "\t-a, --albums\n"\
                            "\t\tSearch for albums\n\n"\
                            "\t-p, --playlists\n"\
                            "\t\tSearch for playlists\n\n"
#define HELP_TXT_QUIT       "Usage: smp quit\n\n"\
                            "Causes the daemon to quit\n\n"
#define HELP_TXT_OPEN       "Usage: smp open [URI]\n\n" \
                            "Start playing the tracks from the specified URI. If the URI points to a playlist/album, "\
                            "the first track in the playlist/album will start to be played and the rest will be added to the "\
                            "track queue. If a there are already any tracks playing it will replace them.\n\n"\
                            "The supported URI formats are:\n\tspotify:[track/album/playlist]:[22 character id]\n\tOR\n\t"\
                            "http[s]://open.spotify.com/[track/album/playlist]/[22 character id][any other parameters - ignored]\n\n"
#define HELP_TXT_DAEMON     "Usage: smp daemon\n\n"\
                            "Starts the smp daemon which plays and downloads tracks. The daemon can be controlled using"\
                            " this program with other options, like smp open, smp playback etc., or programs like playerctl"\
                            " which can control it using the DBus MPRIS standards.\n\n"
#define HELP_TXT_PLAYLIST   "Usage: smp playlist [OPTIONS...]\n\n"\
                            "Without any options, this command will show the name of the currently playing playlist\n\n"\
                            "OPTIONS\n"\
                            "\t-?, --help\n"\
                            "\t\tShow this menu\n\n"\
                            "\t-l, --list\n"\
                            "\t\tList the saved playlists in an interactive menu. If a playlists is ever played it is automatically "\
                            "saved and will appear here. This list is paginated. The page number can be controlled using the --page "\
                            "option or in the interactive menu.\n\n"\
                            "\t-p, --page\n"\
                            "\t\tControl what page should be show on the playlist list. Does nothing by itself.\n\n"

#define PLAYLISTS_PER_PAGE 10

int handle_cli(int argc, char **argv);

#endif //SMP_CLI_H
