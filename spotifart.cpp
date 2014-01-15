#include <errno.h>
#include <libgen.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef _WIN32
#include <direct.h> // for _mkdir
#endif

// C++ headers
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <vector>

// C++11 headers
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>

// spotify headers
#include <libspotify/api.h>

extern "C" const uint8_t g_appkey[];
extern "C" const size_t g_appkey_size;

std::mutex g_notify_mutex;
std::condition_variable g_notify_cond;

// Track vector handling
std::mutex g_tracklist_mutex;
std::vector<sp_track*> g_track_vector;
std::atomic_uint g_tracks_processing(0);
std::atomic_bool g_track_worker_run(true);

static int g_notify_do;
static bool g_verbose = false;
static std::atomic<bool> g_browse_success(false);
static sp_session *g_session = NULL;
const char *g_strlistname = NULL;
static sp_playlist *g_playlist = NULL;
static sp_track *g_currenttrack = NULL;
static std::atomic_uint g_todo_items(1);

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

static void image_cb(sp_image *image, void *userdata)
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

	std::ostringstream oss;
	oss << "img/" << str_artist << " - " << str_album << ".jpg";
	std::string filename = oss.str();
	std::cout << "[+] Writing " << filename << " --- " << len << " bytes" << std::endl;
	std::ofstream ofs(filename);
	ofs.write(static_cast<const char*>(data), len);

	free(userdata);

	g_todo_items--;
}

static int get_album_image(sp_album* album)
{
	int ret = 0;
	const char *str_album = sp_album_name(album);
	const char *str_artist = sp_artist_name(sp_album_artist(album));
	
	// can be SP_IMAGE_SMALL, _NORMAL, or _LARGE
	const byte * image_id = sp_album_cover(album, SP_IMAGE_SIZE_LARGE);

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
}

// this will service on the main thread	
static void album_cb(sp_albumbrowse *result, void *userdata)
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

// service the track vector on a worker thread
static void track_work()
{
	while (g_track_worker_run.load()) {
		{
			std::lock_guard<std::mutex> lock(g_tracklist_mutex);
			if (g_tracks_processing.load() < 5 && g_track_vector.size() > 0) {
				sp_track *track = g_track_vector.back();
				g_track_vector.pop_back();
				sp_album *album = sp_track_album(track);
				// create an album browse request for this track/album 
				// the returned albumbrowse object is freed in the callback
				sp_albumbrowse *albumbrowse = sp_albumbrowse_create(
					g_session, album, &album_cb, NULL);
				sp_albumbrowse_add_ref(albumbrowse);
				//printf("browse object: %p\n", albumbrowse);
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

static void tracks_added(sp_playlist *pl, sp_track *const *tracks, int num_tracks,
	int position, void *userdata)
{
	if (g_strlistname && !strcasecmp(sp_playlist_name(pl), g_strlistname))
		printf("[*] %d tracks added to %s\n", num_tracks, sp_playlist_name(pl));
}

static void playlist_state_changed(sp_playlist *pl, void *userdata)
{
#if 0
	const char* playlist_name = sp_playlist_name(pl);

	// argument check
	if (!g_strlistname) {
		return;
	}

	// skip this playlist if it is not the playlist name of interest
	if (strcasecmp(playlist_name, g_strlistname)) {
		return;
	}

	if (g_browse_success.load())
		return;

	int tracks = sp_playlist_num_tracks(pl);
	g_todo_items = tracks;

	printf("[*] Found playlist %s\n", playlist_name);
	g_playlist = pl;
	playlist_browse_try();
#endif
}

static void playlist_metadata_updated(sp_playlist *pl, void *userdata)
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

	if (g_browse_success.load())
		return;

	int tracks = sp_playlist_num_tracks(pl);
	g_todo_items = tracks;

	// printf("[*] Found playlist %s\n", playlist_name);
	g_playlist = pl;
	playlist_browse_try();
}

static void container_loaded(sp_playlistcontainer *pc, void *userdata)
{
	int num_playlists = sp_playlistcontainer_num_playlists(pc);
	printf("[*] %d root playlists loaded\n", num_playlists);
	
	if (num_playlists == 0) {
		fprintf(stderr, "[!] No playlists in root container\n");
		exit(0);
	}

	for (int i = 0; i < num_playlists; ++i) {
		sp_playlist *pl = sp_playlistcontainer_playlist(pc, i);
		sp_error err = sp_playlist_add_callbacks(pl, &pl_skim_callbacks, NULL);
		if (err != SP_ERROR_OK) {
			fprintf(stderr, "[!] %s\n", sp_error_message(err));
		}
	}
}

static void logged_in(sp_session *sess, sp_error error)
{
	sp_playlistcontainer *pc = sp_session_playlistcontainer(sess);

	if (SP_ERROR_OK != error) {
		fprintf(stderr, "[!] Login failed: %s\n", sp_error_message(error));
		exit(1);
	}

	printf("[*] Login successful\n");

	sp_playlistcontainer_add_callbacks(pc, &pc_callbacks, NULL);
}

static void logged_out(sp_session *session)
{
	//g_logged_out = 1;
}

static void connection_error(sp_session *session, sp_error error)
{
	fprintf(stderr, "[!] Spotify Connection Error: %s\n",
		sp_error_message(error));
}

static void log_message(sp_session *session, const char *data)
{
	if (g_verbose)
		fprintf(stderr, "[~] %s", data);
}

static void notify_main_thread(sp_session *sess)
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

static void create_dir(const char *path)
{
	struct stat st;
	stat(path, &st);
	if (!(st.st_mode & S_IFDIR))
#ifdef _WIN32
		_mkdir(path);
#else
		mkdir(path, 0777);
#endif
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

	if (!username || !password) {
		usage(basename(argv[0]));
		exit(1);
	}

	/* initialize sigint handler */
	signal(SIGINT, sig_handler);

	/* initialize libspotify callbacks and spconfig */
	init_callbacks();

	/* Create session */
	err = sp_session_create(&spconfig, &sp);

	if (SP_ERROR_OK != err) {
		fprintf(stderr, "[!] Unable to create session: %s\n",
			sp_error_message(err));
		exit(1);
	}

	g_session = sp;

	sp_session_login(sp, username, password, 0, NULL);

	/* Create img dir if necessary */
	create_dir("img");

	/* Create track worker */
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

		/* if playlist of interest has been found, scan the tracks */
		if (g_playlist) {
			sp_playlist_add_callbacks(g_playlist, &pl_scan_callbacks, NULL);
			sp_playlist_remove_callbacks(g_playlist, &pl_skim_callbacks, NULL);
		}

		if (g_tracks_processing.load())
			printf("Current processing %d tracks\n", g_tracks_processing.load());
		g_notify_mutex.lock();
	}
	
	g_track_worker_run = false;
	track_worker.join();

	sp_session_logout(g_session);

	return 0;
}

