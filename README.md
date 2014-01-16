The command line spotifart tool will create a directory "img" and download all the album art in a playlist to that directory, with "Artist - Album.jpg" filename format.

Specify your -u username and -p password on the command line. (TODO: change password to silent entry). Specify -l as the text string of the playlist you want to fetch album art for.

For example:
```./spotifart -u user -p password -l "My Rock Playlist"```

# Linux Build Instructions
1. Download and install [libspotify](https://developer.spotify.com/technologies/libspotify/#download)
1. Add your appkey.c file (rename to cpp)
1. ```make```

#Windows Build Instructions
1. The win32 lib/dll has already been added to the lib folder. No need to download.
1. Add your appkey.c file
1. Open Visual Studio 2012
1. Open the SLN file
1. Build
1. Run with 

