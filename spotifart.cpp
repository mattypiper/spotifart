#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <Windows.h>
#define strcasecmp _stricmp
//#pragma comment(lib, "lib/win32/libspotify.lib")
#include "include/api.h"
#endif

// C++ headers
#include <iostream>
#include <ios>
#include <sstream>
#include <fstream>
#include <string>
#include <vector>

// C++11 headers
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>


// forward declare getopt (included in project as a c file)
extern "C" int getopt(int nargc, char * const nargv[], const char *ostr);
extern "C" char *optarg;

extern "C" const uint8_t g_appkey[];
extern "C" const size_t g_appkey_size;

static std::mutex g_notify_mutex;
static std::condition_variable g_notify_cond;

// Track vector handling
static std::mutex g_tracklist_mutex;
static std::vector<sp_track*> g_track_vector;
static std::atomic<unsigned int> g_tracks_processing(0);
static std::atomic<bool> g_track_worker_run(true);

static int g_notify_do;
static bool g_verbose = false;
static std::atomic<bool> g_browse_success(false);
static sp_session *g_session = NULL;
static const char *g_strlistname = NULL;
static sp_playlist *g_playlist = NULL;
static sp_track *g_currenttrack = NULL;
static std::atomic<unsigned int> g_todo_items(1);

static sp_playlistcontainer_callbacks pc_callbacks = {};
static sp_playlist_callbacks pl_skim_callbacks = {};
static sp_playlist_callbacks pl_scan_callbacks = {};
static sp_session_callbacks session_callbacks = {};
static sp_session_config spconfig = {};

struct userdata
{
	const char *artist;
	const char *album;
};

static void sig_handler(int signo)
{
	if (signo == SIGINT)
		g_todo_items = 0;
}

// TODO file name handling needs to be done in unicode
// TODO certain filenames don't get created in windows (colon in name)
static void SP_CALLCONV image_cb(sp_image *image, void *userdata)
{
	struct userdata *cb_data = (struct userdata*)userdata;
	const char *str_artist = cb_data->artist;
	const char *str_album = cb_data->album;

	size_t len;
	const void * data = sp_image_data(image, &len);
	sp_imageformat format = sp_image_format(image);
	if (format != SP_IMAGE_FORMAT_JPEG)
	{
		fprintf(stderr, "[!] Unsupported image format for %s - %s: %d\n",
			str_artist, str_album, format);
	}

	std::stringstream ss;
	ss << "img/" << str_artist << " - " << str_album << ".jpg";
	std::string filename = ss.str();
	std::cout << "[+] Writing " << filename << " --- " << len << " bytes" << std::endl;
	std::ofstream file;
	file.open(filename, std::ios::binary);
	file.write(static_cast<const char*>(data), len);

	free(userdata);

	g_todo_items--;
}

static int get_album_image(sp_album* album)
{
	const char *str_album = sp_album_name(album);
	const char *str_artist = sp_artist_name(sp_album_artist(album));
	
	// can be SP_IMAGE_SMALL, _NORMAL, or _LARGE
	const byte * image_id = sp_album_cover(album, SP_IMAGE_SIZE_NORMAL);

	sp_image *image = sp_image_create(g_session, image_id);
	if (image == NULL)
	{
		fprintf(stderr, "[!] Album cover not available for %s - %s\n",
			str_artist, str_album);
		return -1;
	}
	struct userdata *cb_data = (struct userdata *)malloc(sizeof(struct userdata));
	if (!cb_data)
	{
		fprintf(stderr, "[!] Memory allocation error\n");
		return -1;
	}

	cb_data->artist = str_artist;
	cb_data->album = str_album;

	sp_image_add_load_callback(image, image_cb, (void*)cb_data);
	return 0;
}

// this will service on the main thread	
static void SP_CALLCONV album_cb(sp_albumbrowse *result, void *userdata)
{
	sp_album *album = sp_albumbrowse_album(result);
	if (!album) {
		fprintf(stderr, "[!] WTF - null album pointer in the album browse callback\n");
		sp_albumbrowse_release(result);
		g_todo_items--;
		return;
	}
	sp_artist *artist = sp_album_artist(album);
	const char *str_album = sp_album_name(album);
	const char *str_artist = sp_artist_name(artist);

	// TODO I had retries here to wait for the album to become available
	// but that was pointless... need to move this to another thread to allow
	// main thread to work (I think)
	if (!sp_album_is_available(album)) {
		fprintf(stderr, "[!] Album not available: %s - %s\n",
				str_artist, str_album);
		g_todo_items--;
		return;
	}

	// TODO offload to a background worker?
	// seems unnecessary as the track worker will throttle the overall flow
	get_album_image(album);
	sp_albumbrowse_release(result);
}

/**
 * Service the track vector on a background thread.
 *
 * The whole point of this worker thread is to stare down the track vector (lame)
 * and issue album browse requests periodically. Performance was awful when issuing
 * several hundred album browse requests, and they would start failing as well. So
 * throttling back like this seems to work well. I guess I could do it in the main thread
 * but I wrote it when I wasn't sure what was going on with main. It _is_ the callback
 * thread, apparently. The docs at developer.spotify.com had me thinking it was a
 * library thread.
 */
static void track_work()
{
	while (g_track_worker_run.load()) {
		{
			std::lock_guard<std::mutex> lock(g_tracklist_mutex);
			if (g_tracks_processing.load() < 5 && g_track_vector.size() > 0) {
				sp_track *track = g_track_vector.back();
				g_track_vector.pop_back();
				sp_album *album = sp_track_album(track);
				sp_albumbrowse *albumbrowse = sp_albumbrowse_create(
					g_session, album, &album_cb, NULL);
				sp_albumbrowse_add_ref(albumbrowse);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

static void playlist_browse_try()
{
	sp_playlist_add_ref(g_playlist);
	sp_playlist *pl = g_playlist;
	
	// printf("[*] Browsing playlist %s\n", sp_playlist_name(pl));

	if (!sp_playlist_is_loaded(pl)) {
		fprintf(stderr, "[!] Playlist %s not loaded yet\n", sp_playlist_name(pl));
		sp_playlist_release(pl);
		return;
	}

	int tracks = sp_playlist_num_tracks(pl);

	for (int i = 0; i < tracks; ++i) {
		sp_track *t = sp_playlist_track(pl, i);
		if (!t || !sp_track_is_loaded(t)) {
			// fprintf(stderr, "[!] Track %d is not loaded\n", i);
			// fprintf(stderr, "[!] Playlist %s not loaded yet\n", sp_playlist_name(pl));
			sp_playlist_release(pl);
			return;
		}
	}

	g_browse_success = true;
	printf("[*] Playlist loaded: %s\n", sp_playlist_name(pl));

	for (int j = 0; j < tracks; j++)
	{
		sp_track *t = sp_playlist_track(pl, j);
		
		if (sp_track_get_availability(g_session, t) != 
							SP_TRACK_AVAILABILITY_AVAILABLE) {
			fprintf(stderr, "[!] Track %d: %s is not available\n",
				j+1, sp_track_name(t));
			g_todo_items--;
		} else {
			// add reference to track and add it the track vector
			sp_track_add_ref(t);
			std::lock_guard<std::mutex> lock(g_tracklist_mutex);
			g_track_vector.push_back(t);
#if 0
			printf("[+] Track %d: %s - %s\n", j+1,
				sp_artist_name(sp_track_artist(t, 0)),
				sp_track_name(t));
#endif
			
		}
	}
	sp_playlist_release(pl);
}

static void SP_CALLCONV tracks_added(sp_playlist *pl, sp_track *const *tracks, int num_tracks,
	int position, void *userdata)
{
	if (g_strlistname && !strcasecmp(sp_playlist_name(pl), g_strlistname))
		printf("[*] %d tracks added to %s\n", num_tracks, sp_playlist_name(pl));
}

static void SP_CALLCONV playlist_state_changed(sp_playlist *pl, void *userdata)
{
}

static void SP_CALLCONV playlist_metadata_updated(sp_playlist *pl, void *userdata)
{
	const char* playlist_name = sp_playlist_name(pl);

	// argument check
	if (!g_strlistname) {
		return;
	}

	// skip this playlist if it is not the playlist name of interest
	if (strcasecmp(playlist_name, g_strlistname)) {
		return;
	}

	// don't try to browse the playlist again if we've already successfully
	// browsed once... (this callback will keep firing after we've moved on to
	// other things)
	if (g_browse_success.load())
		return;

	// printf("[*] Found playlist %s\n", playlist_name);

	g_todo_items = sp_playlist_num_tracks(pl);
	g_playlist = pl;
	playlist_browse_try();
}

static void SP_CALLCONV container_loaded(sp_playlistcontainer *pc, void *userdata)
{
	int num_playlists = sp_playlistcontainer_num_playlists(pc);
	printf("[*] %d root playlists loaded\n", num_playlists);
	
	if (num_playlists == 0) {
		fprintf(stderr, "[!] No playlists in root container\n");
		exit(0);
	}

	// it would be neat if we could just read the playlist name here
	// but I tried that and they were all blank (as of v12.1.51)
	// so instead register the playlist_metadata_changed callback for
	// all playlists (lame) and check the name there
	for (int i = 0; i < num_playlists; ++i) {
		sp_playlist *pl = sp_playlistcontainer_playlist(pc, i);
		// TODO remove the callbacks somewhere
		sp_error err = sp_playlist_add_callbacks(pl, &pl_skim_callbacks, NULL);
		if (err != SP_ERROR_OK) {
			fprintf(stderr, "[!] %s\n", sp_error_message(err));
		}
	}
}

static void SP_CALLCONV logged_in(sp_session *sess, sp_error error)
{
	sp_playlistcontainer *pc = sp_session_playlistcontainer(sess);

	if (SP_ERROR_OK != error) {
		fprintf(stderr, "[!] Login failed: %s\n", sp_error_message(error));
		exit(1);
	}

	printf("[*] Login successful\n");

	// TODO remove this callback somewhere
	sp_playlistcontainer_add_callbacks(pc, &pc_callbacks, NULL);
}

static void SP_CALLCONV logged_out(sp_session *session)
{
	//g_logged_out = 1;
}

static void SP_CALLCONV connection_error(sp_session *session, sp_error error)
{
	fprintf(stderr, "[!] Spotify Connection Error: %s\n",
		sp_error_message(error));
}

static void SP_CALLCONV log_message(sp_session *session, const char *data)
{
	if (g_verbose)
		fprintf(stderr, "[~] %s", data);
}

static void SP_CALLCONV notify_main_thread(sp_session *sess)
{
	std::unique_lock<std::mutex> lock(g_notify_mutex);
	g_notify_do = 1;
	g_notify_cond.notify_all();
}

static void init_callbacks()
{
	pc_callbacks.container_loaded = container_loaded;

	pl_skim_callbacks.playlist_state_changed = playlist_state_changed;
	pl_skim_callbacks.playlist_metadata_updated = playlist_metadata_updated;

	pl_scan_callbacks.playlist_state_changed = playlist_state_changed;
	pl_scan_callbacks.tracks_added = tracks_added;
	pl_scan_callbacks.playlist_metadata_updated = playlist_metadata_updated;

	session_callbacks.logged_in = logged_in;
	session_callbacks.logged_out = logged_out;
	session_callbacks.connection_error = connection_error;
	session_callbacks.notify_main_thread = notify_main_thread;
	session_callbacks.log_message = log_message;

	spconfig.api_version = SPOTIFY_API_VERSION;
	spconfig.cache_location = "sp_tmp";
	spconfig.settings_location = "sp_tmp";
	spconfig.application_key = g_appkey;
	spconfig.application_key_size = g_appkey_size;
	spconfig.user_agent = "spotifart";
	spconfig.callbacks = &session_callbacks;
}

static void usage(const char *progname)
{
	fprintf(stderr, "Usage: %s -u <username> -p <password> -l <listname>\n", progname);
}

static bool predicate()
{
	return g_notify_do ? true : false;
}

static bool create_dir(const char *path)
{
	int ret = 0;
	struct stat st = {};
	stat(path, &st);
	if (st.st_mode & S_IFDIR)
		return true;

#ifdef _WIN32
	if (!CreateDirectoryA(path, NULL))
		ret = -1;
#else
	ret = mkdir(path, 0777);
#endif

	if (-1 == ret) {
		fprintf(stderr, "Error creating directory %s\n", path);
		return false;
	}

	return true;
}

int main(int argc, char **argv)
{
	sp_session *sp;
	sp_error err;
	int next_timeout = 0;
	const char *username = NULL;
	const char *password = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "u:p:l:v")) != EOF) {
		switch (opt) {
		case 'u':
			username = optarg;
			break;

		case 'p':
			password = optarg;
			break;

		case 'l':
			g_strlistname = optarg;
			break;

		case 'v':
			g_verbose = true;
			break;

		default:
			exit(1);
		}
	}

		// Create img dir if necessary
	if (!create_dir("img"))
		exit(1);

	if (!username || !password) {
		usage(argv[0]);
		exit(1);
	}

	// initialize sigint handler
	signal(SIGINT, sig_handler);

	// initialize libspotify callbacks and spconfig
	init_callbacks();

	// Create session
	err = sp_session_create(&spconfig, &sp);

	if (SP_ERROR_OK != err) {
		fprintf(stderr, "[!] Unable to create session: %s\n",
			sp_error_message(err));
		exit(1);
	}

	g_session = sp;

	sp_session_login(sp, username, password, 0, NULL);



	// Create track worker
	std::thread track_worker(track_work);

	std::unique_lock<std::mutex> lock(g_notify_mutex);

	while (g_todo_items) {
		g_notify_cond.wait_for(lock,
			std::chrono::milliseconds(next_timeout), predicate);

		g_notify_do = 0;
		g_notify_mutex.unlock();

		do {
			sp_session_process_events(sp, &next_timeout);
		} while (next_timeout == 0);

		// if the playlist of interest has been found, scan the tracks.
		// important not to do the callback registration changes here in main,
		// not in a callback
		if (g_playlist) {
			sp_playlist_add_callbacks(g_playlist, &pl_scan_callbacks, NULL);
			sp_playlist_remove_callbacks(g_playlist, &pl_skim_callbacks, NULL);
		}

		g_notify_mutex.lock();
	}
	
	g_track_worker_run = false;
	track_worker.join();

	sp_session_logout(g_session);

	return 0;
}

