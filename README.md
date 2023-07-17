# Simple Music Player

### About
The simple music player is a music player which automatically downloads music from spotify playlists/albums and plays it.
It was created as an alternative to [spotifyd](https://github.com/Spotifyd/spotifyd).

In order to get tracks from spotify it makes use of [smp-backend](https://github.com/Quartzy0/smp-backend). The backend
is what actually fetches data about the tracks, albums and playlists from spotify. It acts as a kind of proxy between
the client and spotify, which results in the client never having direct contact with spotify.

It is also fully implements the MPRIS DBUS standards, meaning it can be controlled using utilities like [playerctl](https://github.com/altdesktop/playerctl) or [mpris-control](https://github.com/BlackDex/mpris-control).

* [Compiling](#compiling)
* [Using the CLI](#using-the-cli)
  * [Starting the daemon](#starting-the-daemon)
  * [Getting playback information](#getting-playback-information)
  * [Controlling playback](#controlling-playback)
  * [List tracks in current context](#list-tracks-in-current-context)
  * [Search](#search)
  * [Playing from a URL](#playing-from-a-url)
  * [Saved playlists](#saved-playlists)
  * [Killing the daemon](#killing-the-daemon)
* [Configuration file](#configuration-file)
  * [Contents](#contents)

### Features
 - Open spotify playlist/album URIs
 - Continue playing after the playlist/album has ended using spotify's recommendations API
 - Full MPRIS implementation
 - Control daemon through a CLI

### Compiling
First, clone the git repository to obtain the source code and cd into the cloned repository:
```shell
git clone --recursive https://github.com/Quartzy0/smp && cd smp
```
Then create a directory for the build files and cd into it:
```shell
mkdir build && cd build
```
In this directory execute the following commands to build the project:
```shell
cmake -DCMAKE_BUILD_TYPE=Release ..
```
Building smp using this command will cause it to automatically check if PipeWire is installed
and use it as opposed to the default PortAudio backend. If you want to you the PortAudio backend regardless,
you can use the following command:
```shell
cmake -DCMAKE_BUILD_TYPE=Release -DNO_PIPEWIRE=ON ..
```
Lastly, compile the program:
```shell
make
```
Once the compilation is finished, the executable, named `smp` will be in the build directory.

### Using the CLI
#### Starting the daemon
```shell
smp daemon
```
This will start the daemon and will not detach. This could be used in a systemd service.
The daemon has to be running for any of the other CLI commands to work.

#### Getting playback information
```shell
smp playback
```
This will print information about the current state of the player in the following format:
```
Track info:
        ▶ <Track title>
                by <Artist>
        (<Track URI>)
        (art:<Track cover art URL>)
Player info:
        Looping: <Loop status>
        Shuffle: <Shuffle status>
        Volume: <Volume %>
```
#### Controlling playback
```shell
smp playback --toggle
```
Toggles playback. If playback is paused, it will start playing and if is paused, it will start playing.
```shell
smp playback --play
```
Continue playing the current track. Does nothing if no track is playing
```shell
smp playback --pause
```
Pause the current track. Does nothing if no track is playing
```shell
smp playback --next
```
Will play the next track in the playlist. The behaviour of this command varies based on the
loop/shuffle modes. If shuffle is enabled, it will not play the next track in the playlist but will
play a random one. It will also not play recommendations from spotify but will endlessly play songs from
the current playlist. If loop mode is set to track, it will still play the next track. If loop mode
is set to playlist, it will start playing the first track once the end is reached.
```shell
smp playback --previous
```
This option behaves the same way as the --next option, except it plays the previous track.
```shell
smp playback --volume 0.5
```
Set the volume. The volume level goes from 1 to 0. In this example it is set to 0.5, meaning 50% of the original volume.
```shell
smp playback --shuffle
```
Toggles whether shuffle is enabled or disabled.
```shell
smp playback --loop
```
Cycles through the available loop modes. These are: None, Playlist or Track.
```shell
smp playback --stop
```
Stops the player. The current playing playlist/album is freed and the audio steam is closed.
The play/pause/next/previous commands will do nothing. The player can be started again by opening another playlist/album/track.

#### List tracks in current context
```shell
smp track --list
```
List the tracks in the track list. This can mean the tracks in the current playlist/album,
the recommendations from a track/playlist/album or just a single track that is playing.

If the `track` subcommand is launched without any options, the output will be the same as the
`playback` subcommand's.

#### Search
```shell
smp search -t Never Gonna Give You Up
```
This example would search for the track "Never Gonna Give You Up" using spotify's api. It is
also possible to search for playlists or albums using the -p or -a flags respectively. If no
flag is enabled, it is assumed that you wish to search for tracks, albums and playlists. The
results of the search are presented in an interactive manner, allowing you to play any of the
results.

#### Playing from a URL
```shell
smp open https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWqT
```
This command will start playing the track, album or playlist pointed to by the URL.

#### Saved playlists
```shell
smp playlist --list
```
List the saved playlists in an interactive menu. If a playlists is ever played it is automatically
saved and will appear here. This list is paginated. The page number can be controlled using the --page
option or in the interactive menu.

#### Killing the daemon
```shell
smp quit
```
This command will cause the daemon to stop.

### Configuration file
The configuration file should be located at `XDG_CONFIG_HOME/smp/smp.json`. If the
`XDG_CONFIG_HOME` environment variable is not set, smp will look for the configuration
file at `$HOME/.config/smp/smp.json`
#### Contents
```json5
{
    //Number of tracks to download ahead of the current track
    //This option is useful since each track has to be downloaded,
    //which would result in long wait times between tracks
    "preload_amount": 3,
    
    //The volume the player should start with. Even if this option
    //is set, the volume can still be changed using the CLI
    "initial_volume": 1.0,
    
    //The location where the downloaded tracks should be stored.
    //Defaults to $XDG_CACHE_HOME/smp/tracks or $HOME/.cache/smp/tracks
    "track_save_path": "/home/user/.cache/smp/tracks",
    
    //The location where the saved playlists should be stored.
    //Defaults to $XDG_DATA_HOME/smp/playlists or $HOME/.local/share/smp/playlists
    "playlist_save_path": "/home/user/.local/share/smp/playlists",
    
    //A string array of piped instances to use. These are used to search
    //for tracks on YouTube and YouTube Music but not to download the tracks.
    //By default, it is populated by piped instances from https://piped-instances.kavin.rocks/
    "piped_api_instances": [],
    
    //A string array of invidious/piped instances to use for downloading tracks.
    //By default, it is populated by invidious instances from https://api.invidious.io/instances.json
    "download_instances": []
}
```