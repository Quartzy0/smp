# Simple Music Player
### About
The simple music player is a music player which automatically downloads music from spotify playlists/albums and plays it.
It was created as an alternative to [spotifyd](https://github.com/Spotifyd/spotifyd). Unlike spotifyd, this music player
does not register itself as a spotify device and does not play the tracks directly from spotify. Instead, it looks up the
tracks in a playlist/album and downloads them to your computer from [invidious](https://invidious.io/) (an open source alternative front-end to YouTube).
It is also fully implements the MPRIS DBUS standards, meaning it can be controlled using utilities like [playerctl](https://github.com/altdesktop/playerctl) or [mpris-control](https://github.com/BlackDex/mpris-control).

### Features
 - Open spotify playlist/album URIs
 - Continue playing after the playlist/album has ended using spotify's recommendations API
 - Full MPRIS implementation