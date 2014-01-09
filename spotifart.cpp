#include <errno.h>
#include <libgen.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include <libspotify/api.h>

extern "C" const uint8_t g_appkey[];
extern "C" const size_t g_appkey_size;

static pthread_mutex_t g_notify_mutex;
static pthread_cond_t g_notify_cond;
static int g_notify_do;
static sp_session *g_sess = NULL;
static sp_playlist *g_playlist = NULL;
const char *g_strlistname = NULL;
static int g_remove_tracks = 0;
static sp_track *g_currenttrack = NULL;
static int g_continue = 1;

static sp_playlistcontainer_callbacks pc_callbacks = {};
static sp_session_callbacks session_callbacks = {};
static sp_session_config spconfig = {};

static void container_loaded(sp_playlistcontainer *pc, void *userdata)
{
	int num_playlists = sp_playlistcontainer_num_playlists(pc);

	fprintf(stderr, "%d playlists loaded\n", num_playlists);
	
	if (num_playlists == 0) {
		fprintf(stderr, "No tracks in playlist.\n");
		exit(0);
	}
	
	printf("Playlists:\n");
	for (int i = 0; i < num_playlists; ++i) {

		sp_playlist *pl = sp_playlistcontainer_playlist(pc, i);

		const char* playlist_name = sp_playlist_name(pl);
		printf("%d: %s\n", i, playlist_name);

		if (!strcasecmp(playlist_name, g_strlistname)) {
			printf("Match!\n\nTrack List:\n\n");
			int tracks = sp_playlist_num_tracks(pl);
			for (int j = 0; j < tracks; j++)
			{
				sp_track *t = sp_playlist_track(pl, j);
				if (!t) {
					fprintf(stderr, "cannot get track %d\n", j);
					g_continue = 0;
					return;
				}

				printf("Track %d: %s - %s\n", j,
						sp_artist_name(sp_track_artist(t, 0)),
						sp_track_name(t));
			}

			g_continue = 0;
			return;
		}
	}

	printf("Playlist %s not found.\n", g_strlistname);
	g_continue = 0;
}

static void logged_in(sp_session *sess, sp_error error)
{
	sp_playlistcontainer *pc = sp_session_playlistcontainer(sess);

	if (SP_ERROR_OK != error) {
		fprintf(stderr, "Login failed: %s\n",
			sp_error_message(error));
		exit(2);
	}

	printf("Login successful.\n");

	sp_playlistcontainer_add_callbacks(
		pc,
		&pc_callbacks,
		NULL);
}

static void notify_main_thread(sp_session *sess)
{
	pthread_mutex_lock(&g_notify_mutex);
	g_notify_do = 1;
	pthread_cond_signal(&g_notify_cond);
	pthread_mutex_unlock(&g_notify_mutex);
}

static void init_callbacks()
{
	pc_callbacks.container_loaded = &container_loaded;
	session_callbacks.logged_in = &logged_in;
	session_callbacks.notify_main_thread = &notify_main_thread;
	spconfig.api_version = SPOTIFY_API_VERSION;
	spconfig.cache_location = "tmp";
	spconfig.settings_location = "tmp";
	spconfig.application_key = g_appkey;
	spconfig.application_key_size = g_appkey_size;
	spconfig.user_agent = "spotifart";
	spconfig.callbacks = &session_callbacks;
}

static void usage(const char *progname)
{
	fprintf(stderr, "usage: %s -u <username> -p <password> -l <listname>\n", progname);
}

int main(int argc, char **argv)
{
	sp_session *sp;
	sp_error err;
	int next_timeout = 0;
	const char *username = NULL;
	const char *password = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "u:p:l:d")) != EOF) {
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

		default:
			exit(1);
		}
	}

	if (!username || !password || !g_strlistname) {
		usage(basename(argv[0]));
		exit(1);
	}

	/* initialize libspotify callbacks and spconfig */
	init_callbacks();

	/* Create session */
	err = sp_session_create(&spconfig, &sp);

	if (SP_ERROR_OK != err) {
		fprintf(stderr, "Unable to create session: %s\n",
			sp_error_message(err));
		exit(1);
	}

	g_sess = sp;

	pthread_mutex_init(&g_notify_mutex, NULL);
	pthread_cond_init(&g_notify_cond, NULL);

	sp_session_login(sp, username, password, 0, NULL);
	pthread_mutex_lock(&g_notify_mutex);

	while (g_continue) {
		if (next_timeout == 0) {
			while(!g_notify_do)
				pthread_cond_wait(&g_notify_cond, &g_notify_mutex);
		} else {
			struct timespec ts;

#if _POSIX_TIMERS > 0
			clock_gettime(CLOCK_REALTIME, &ts);
#else
			struct timeval tv;
			gettimeofday(&tv, NULL);
			TIMEVAL_TO_TIMESPEC(&tv, &ts);
#endif
			ts.tv_sec += next_timeout / 1000;
			ts.tv_nsec += (next_timeout % 1000) * 1000000;

			pthread_cond_timedwait(&g_notify_cond, &g_notify_mutex, &ts);
		}

		g_notify_do = 0;
		pthread_mutex_unlock(&g_notify_mutex);

		do {
			sp_session_process_events(sp, &next_timeout);
		} while (next_timeout == 0);

		pthread_mutex_lock(&g_notify_mutex);
	}

	return 0;
}

