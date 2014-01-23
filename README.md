# Command Line Downloader
The command line tool (CLI) will create a directory "img", then download all the album art from a playlist to that directory. It will use a filename format of "Artist - Album.jpg".

Specify your username on the command line with -u.

Specify -l as the text string of the playlist you want to fetch album art for.

Example Usage:
```./spotifart -u user -p password -l "My Rock Playlist"```

## Linux Build Instructions
1. Download and install [libspotify](https://developer.spotify.com/technologies/libspotify/#download)
1. Add your appkey.c file (rename to cpp)
1. ```make```

## Windows Build Instructions
1. The win32 lib/dll has already been added to the lib folder. No need to download.
1. Add your appkey.c file
1. Open Visual Studio 2012
1. Open the SLN file
1. Build
1. Run with 

# node.js Web App
Coming soon...
