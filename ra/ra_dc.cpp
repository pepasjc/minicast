// ra_dc.cpp -- RetroAchievements for minicast (Dreamcast), milestone 1
//
// rc_client setup, credentials file, login -> identify -> load flow, server
// bridge, event handling and logging follow the MiSTer RetroAchievements
// fork of Main_MiSTer (achievements.cpp / ra_http.cpp, odelot's MiSTer_RA
// work), trimmed to one console and moved onto a dedicated thread:
//
//   - one RA thread (cpu0) owns rc_client: hashing, login, load, do_frame,
//     every callback and every HTTP response dispatch
//   - the SH4 thread (cpu1, the bottleneck) only signals a frame: one
//     atomic exchange + sem_post, and a frame is skipped instead of waited
//     for if the RA thread is still busy with the previous one
//   - softcore only: minicast has no hardcore safeguards (save states,
//     cheats, speed controls) yet, so hardcore=1 in the cfg is ignored
//   - dry run (MINICAST_RA_DRYRUN=1 / dreamster_dryrun=1): achievement and
//     leaderboard submissions are logged and answered locally, never sent
//
// /media/fat/retroachievements.cfg keys read (the file is never written):
//   username, password    login (no username: RA off, silently)
//   hardcore              ignored (logged): softcore only
//   debug=1               verbose rcheevos + HTTP logging
//   dreamster_dryrun=1    see above
//   dreamster_frame_source=vblank|render   what drives rc_client_do_frame
//   dreamster_chime_volume=0..100          default 70
//   dreamster_chime_test=1                 play the chime when the game loads
//   dreamster_popups=0                     no on-screen popups (default on)
//   popup_position=left|center|right       popup corner (MiSTer_RA's key)
//   dreamster_list_hotkey=0                no achievement list hotkey
// Environment (wins over the cfg): MINICAST_RA=0 (off), MINICAST_RA_DRYRUN,
// MINICAST_RA_DEBUG, MINICAST_RA_FRAME, MINICAST_RA_CHIME_VOLUME,
// MINICAST_RA_CHIME_TEST, MINICAST_RA_POPUPS, MINICAST_RA_LIST, and for
// tests MINICAST_RA_CFG / _WAV / _UNLOCK_LOG / _CURL (alternate paths).

#include "ra_dc.h"
#include "ra_http.h"
#include "ra_osd.h"

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include "rc_api_request.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#define RA_CFG_PATH        "/media/fat/retroachievements.cfg"
#define RA_WAV_PATH        "/media/fat/minicast/achievement.wav"
#define RA_UNLOCK_LOG_PATH "/media/fat/minicast/logs/ra_unlocks.log"
#define RA_CLIENT_NAME     "DreamSTer_MiSTer/0.1"

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

static int g_debug = 0;

static void ra_vlog(const char* fmt, va_list ap, bool flush)
{
	char line[1024];
	vsnprintf(line, sizeof(line), fmt, ap);
	// one locked write per line: never interleaves with other threads' output
	flockfile(stdout);
	fputs("RA: ", stdout);
	fputs(line, stdout);
	fputc('\n', stdout);
	if (flush)
		fflush(stdout);
	funlockfile(stdout);
}

void ra_log(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	ra_vlog(fmt, ap, true);
	va_end(ap);
}

void ra_log_debug(const char* fmt, ...)
{
	if (!g_debug)
		return;
	va_list ap;
	va_start(ap, fmt);
	ra_vlog(fmt, ap, false);
	va_end(ap);
}

static uint64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t mono_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static const char* env_str(const char* name)
{
	const char* v = getenv(name);
	return (v && v[0]) ? v : NULL;
}

// ---------------------------------------------------------------------------
// configuration: /media/fat/retroachievements.cfg (same file and keys as the
// MiSTer_RA fork; never written) + environment overrides
// ---------------------------------------------------------------------------

struct RaConfig {
	char user[128];
	char password[128];
	int hardcore_requested;   // cfg hardcore=1: ignored (softcore only), logged
	int dryrun;
	int frame_source;         // RA_FRAME_VBLANK / RA_FRAME_RENDER
	int chime_volume;         // percent
	int chime_test;           // play the chime once the game is identified
	int popups;               // on-screen popups
	int popup_position;       // RA_POS_*
	int list_hotkey;          // achievement list hotkey
};

static RaConfig g_cfg;

static int parse_frame_source(const char* v)
{
	if (!strcasecmp(v, "render") || !strcmp(v, "2"))
		return RA_FRAME_RENDER;
	return RA_FRAME_VBLANK;
}

// Returns 0 (RA stays off, silently) when the file is missing or has no
// username; the MiSTer_RA fork's parser, reduced to the keys used here.
static int load_config(void)
{
	memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.frame_source = RA_FRAME_VBLANK;
	g_cfg.chime_volume = 70;
	g_cfg.popups = 1;
	g_cfg.popup_position = RA_POS_LEFT;
	g_cfg.list_hotkey = 1;
	int bad_position = 0;

	const char* path = env_str("MINICAST_RA_CFG");
	FILE* f = fopen(path ? path : RA_CFG_PATH, "r");
	if (!f)
		return 0;

	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char* nl = strchr(line, '\n');
		if (nl) *nl = '\0';
		nl = strchr(line, '\r');
		if (nl) *nl = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;
		char* eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		const char* key = line;
		const char* val = eq + 1;
		while (*val == ' ' || *val == '\t')
			val++;

		if (!strcasecmp(key, "username"))
			snprintf(g_cfg.user, sizeof(g_cfg.user), "%s", val);
		else if (!strcasecmp(key, "password"))
			snprintf(g_cfg.password, sizeof(g_cfg.password), "%s", val);
		else if (!strcasecmp(key, "hardcore"))
			g_cfg.hardcore_requested = atoi(val);
		else if (!strcasecmp(key, "debug"))
			g_debug = atoi(val);
		else if (!strcasecmp(key, "dreamster_dryrun"))
			g_cfg.dryrun = atoi(val);
		else if (!strcasecmp(key, "dreamster_frame_source"))
			g_cfg.frame_source = parse_frame_source(val);
		else if (!strcasecmp(key, "dreamster_chime_volume"))
			g_cfg.chime_volume = atoi(val);
		else if (!strcasecmp(key, "dreamster_chime_test"))
			g_cfg.chime_test = atoi(val);
		else if (!strcasecmp(key, "dreamster_popups"))
			g_cfg.popups = atoi(val);
		else if (!strcasecmp(key, "dreamster_list_hotkey"))
			g_cfg.list_hotkey = atoi(val);
		else if (!strcasecmp(key, "popup_position")) {
			int p = ra_parse_position(val);
			if (p < 0)
				bad_position = 1;
			else
				g_cfg.popup_position = p;
		}
	}
	fclose(f);

	// environment wins over the cfg
	const char* v;
	if ((v = env_str("MINICAST_RA_DRYRUN")))
		g_cfg.dryrun = atoi(v);
	if ((v = env_str("MINICAST_RA_DEBUG")))
		g_debug = atoi(v);
	if ((v = env_str("MINICAST_RA_FRAME")))
		g_cfg.frame_source = parse_frame_source(v);
	if ((v = env_str("MINICAST_RA_CHIME_VOLUME")))
		g_cfg.chime_volume = atoi(v);
	if ((v = env_str("MINICAST_RA_CHIME_TEST")))
		g_cfg.chime_test = atoi(v);
	if ((v = env_str("MINICAST_RA_POPUPS")))
		g_cfg.popups = atoi(v);
	if ((v = env_str("MINICAST_RA_LIST")))
		g_cfg.list_hotkey = atoi(v);
	if (bad_position && g_cfg.user[0])
		ra_log("popup_position not left/center/right, using left");
	if (g_cfg.chime_volume < 0)
		g_cfg.chime_volume = 0;
	if (g_cfg.chime_volume > 100)
		g_cfg.chime_volume = 100;

	return g_cfg.user[0] != 0;
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

std::atomic<int> ra_frame_source{RA_FRAME_OFF};

static std::atomic<uint8_t*> g_ram{nullptr};
static std::atomic<uint32_t> g_ram_size{0};

static rc_client_t* g_client = NULL;         // RA thread only
static char g_game_path[1024];
static char g_hash[33];
static int g_logged_in = 0;
static int g_load_started = 0;
static int g_game_loaded = 0;
static int g_ra_dead = 0;                    // login/hash/load failed: idle until exit

static pthread_t g_thread;
static sem_t g_frame_sem;
static std::atomic<int> g_thread_running{0};
static std::atomic<int> g_thread_done{0};
static std::atomic<int> g_quit{0};
static std::atomic<int> g_quit_timeout_ms{3000};
static std::atomic<int> g_reset_pending{0};
static std::atomic<int> g_game_ready{0};             // load done, until shutdown
static std::atomic<int> g_list_req{0};               // overlay wants a list snapshot
static std::atomic<ra_list_data*> g_list_result{nullptr};

// frame handshake: set by the SH4 thread when it wakes the RA thread,
// cleared by the RA thread once that frame is processed
static std::atomic<int> g_frame_busy{0};

// statistics; the frame counters have a single writer each
static std::atomic<uint32_t> g_frames_signaled{0};   // SH4 thread
static std::atomic<uint32_t> g_frames_skipped{0};    // SH4 thread
static uint32_t g_frames_processed = 0;              // RA thread
static uint64_t g_frame_us_total = 0, g_frame_us_max = 0;

// ---------------------------------------------------------------------------
// per-frame signal (SH4 thread)
// ---------------------------------------------------------------------------

void ra_frame_signal(void)
{
	g_frames_signaled.store(g_frames_signaled.load(std::memory_order_relaxed) + 1,
		std::memory_order_relaxed);
	if (g_frame_busy.exchange(1, std::memory_order_acq_rel)) {
		// still evaluating the previous frame: never block the emulator
		g_frames_skipped.store(g_frames_skipped.load(std::memory_order_relaxed) + 1,
			std::memory_order_relaxed);
		return;
	}
	sem_post(&g_frame_sem);
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

// RA's Dreamcast map (rcheevos consoleinfo.c) is a single region: RA
// 0x00000000-0x00FFFFFF = 16 MB system RAM at physical 0x0C000000. minicast
// keeps that RAM byte-for-byte in SH4 (little-endian) order, so a read is a
// plain copy. It races with the SH4 thread, like the MiSTer cores' mirrors.
static uint32_t read_ram(uint32_t address, uint8_t* buffer, uint32_t num_bytes)
{
	uint8_t* ram = g_ram.load(std::memory_order_acquire);
	uint32_t size = g_ram_size.load(std::memory_order_relaxed);
	if (!ram || address >= size)
		return 0;
	uint32_t n = num_bytes;
	if (n > size - address)
		n = size - address;
	switch (n) {   // the common sizes, without a memcpy call
	case 1: buffer[0] = ram[address]; break;
	case 2: memcpy(buffer, ram + address, 2); break;
	case 4: memcpy(buffer, ram + address, 4); break;
	default: memcpy(buffer, ram + address, n); break;
	}
	return n;
}

static uint32_t RC_CCONV ra_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
	(void)client;
	return read_ram(address, buffer, num_bytes);
}

void ra_set_ram(uint8_t* ram, uint32_t size)
{
	g_ram_size.store(size, std::memory_order_relaxed);
	g_ram.store(ram, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// unlock chime: /media/fat/minicast/achievement.wav -> 48 kHz stereo s16
// ---------------------------------------------------------------------------

ra_chime_t ra_chime = { nullptr, 0 };
std::atomic<uint32_t> ra_chime_seq{0};

static uint32_t rd_le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd_le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// PCM 16-bit mono/stereo WAV at any rate, linearly resampled to 48 kHz.
static int load_chime(const char* path, int volume_percent)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsize < 44 || fsize > 8 * 1024 * 1024) {
		fclose(f);
		return 0;
	}
	uint8_t* file = (uint8_t*)malloc((size_t)fsize);
	if (!file || fread(file, 1, (size_t)fsize, f) != (size_t)fsize) {
		free(file);
		fclose(f);
		return 0;
	}
	fclose(f);

	int ok = 0;
	if (!memcmp(file, "RIFF", 4) && !memcmp(file + 8, "WAVE", 4)) {
		uint32_t channels = 0, rate = 0, bits = 0, fmt = 0;
		const uint8_t* data = NULL;
		uint32_t data_len = 0;
		long off = 12;
		while (off + 8 <= fsize) {
			uint32_t id_len = rd_le32(file + off + 4);
			const uint8_t* body = file + off + 8;
			if (off + 8 + (long)id_len > fsize)
				id_len = (uint32_t)(fsize - off - 8);
			if (!memcmp(file + off, "fmt ", 4) && id_len >= 16) {
				fmt = rd_le16(body);
				channels = rd_le16(body + 2);
				rate = rd_le32(body + 4);
				bits = rd_le16(body + 14);
			} else if (!memcmp(file + off, "data", 4)) {
				data = body;
				data_len = id_len;
			}
			off += 8 + id_len + (id_len & 1);
		}
		if (fmt == 1 && bits == 16 && (channels == 1 || channels == 2) &&
			rate >= 8000 && rate <= 192000 && data && data_len >= 2 * channels) {
			uint32_t in_frames = data_len / (2 * channels);
			uint32_t out_frames = (uint32_t)((uint64_t)in_frames * 48000 / rate);
			int16_t* out = (int16_t*)malloc((size_t)out_frames * 2 * sizeof(int16_t));
			if (out && out_frames) {
				const float vol = volume_percent / 100.0f;
				const double step = (double)rate / 48000.0;
				for (uint32_t i = 0; i < out_frames; i++) {
					double pos = i * step;
					uint32_t i0 = (uint32_t)pos;
					uint32_t i1 = i0 + 1 < in_frames ? i0 + 1 : i0;
					float t = (float)(pos - i0);
					for (uint32_t c = 0; c < 2; c++) {
						uint32_t ch = channels == 2 ? c : 0;
						float s0 = (int16_t)rd_le16(data + (i0 * channels + ch) * 2);
						float s1 = (int16_t)rd_le16(data + (i1 * channels + ch) * 2);
						float s = (s0 + (s1 - s0) * t) * vol;
						out[2 * i + c] = (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
					}
				}
				// published once, before any ra_chime_seq bump (release)
				ra_chime.pcm = out;
				ra_chime.frames = out_frames;
				ok = 1;
				ra_log_debug("chime %s: %u Hz %u ch -> %u frames @48 kHz, volume %d%%",
					path, rate, channels, out_frames, volume_percent);
			} else {
				free(out);
			}
		}
	}
	free(file);
	return ok;
}

static void play_chime(void)
{
	if (ra_chime.pcm)
		ra_chime_seq.fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// notification queue (milestone 2: on-screen popup)
// ---------------------------------------------------------------------------

static const int NOTIF_CAP = 16;
static ra_notification_t g_notif[NOTIF_CAP];
static uint32_t g_notif_head = 0, g_notif_tail = 0, g_notif_seq = 0;
static pthread_mutex_t g_notif_mutex = PTHREAD_MUTEX_INITIALIZER;

static void notify(int kind, uint32_t id, uint32_t points, const char* badge,
	const char* title, const char* text)
{
	pthread_mutex_lock(&g_notif_mutex);
	if (g_notif_head - g_notif_tail >= (uint32_t)NOTIF_CAP)
		g_notif_tail++;   // drop the oldest
	ra_notification_t* n = &g_notif[g_notif_head % NOTIF_CAP];
	memset(n, 0, sizeof(*n));
	n->seq = ++g_notif_seq;
	n->kind = kind;
	n->id = id;
	n->points = points;
	snprintf(n->badge, sizeof(n->badge), "%s", badge ? badge : "");
	snprintf(n->title, sizeof(n->title), "%s", title ? title : "");
	snprintf(n->text, sizeof(n->text), "%s", text ? text : "");
	n->time_ms = mono_ms();
	g_notif_head++;
	pthread_mutex_unlock(&g_notif_mutex);
}

void ra_notify_local(int kind, const char* title, const char* text)
{
	notify(kind, 0, 0, "", title, text);
}

int ra_notification_pop(ra_notification_t* out)
{
	int got = 0;
	pthread_mutex_lock(&g_notif_mutex);
	if (g_notif_head != g_notif_tail) {
		*out = g_notif[g_notif_tail % NOTIF_CAP];
		g_notif_tail++;
		got = 1;
	}
	pthread_mutex_unlock(&g_notif_mutex);
	return got;
}

// ---------------------------------------------------------------------------
// unlock log: /media/fat/minicast/logs/ra_unlocks.log
// ---------------------------------------------------------------------------

static void mkdir_parents(const char* path)
{
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char* p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
}

static void append_unlock_log(const char* what, uint32_t id, const char* title,
	uint32_t points, const char* description)
{
	const char* path = env_str("MINICAST_RA_UNLOCK_LOG");
	if (!path)
		path = RA_UNLOCK_LOG_PATH;
	mkdir_parents(path);
	FILE* f = fopen(path, "a");
	if (!f) {
		ra_log("could not append to %s: %s", path, strerror(errno));
		return;
	}
	char ts[32];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

	const rc_client_game_t* game = g_client ? rc_client_get_game_info(g_client) : NULL;
	fprintf(f, "%s  %s  user=%s  game=%u \"%s\"  %s %u \"%s\" (%u pts) - %s  [softcore%s]\n",
		ts, what, g_cfg.user,
		game ? game->id : 0, game && game->title ? game->title : "?",
		what, id, title ? title : "", points, description ? description : "",
		g_cfg.dryrun ? ", DRY RUN: not submitted" : "");
	fclose(f);
}

// ---------------------------------------------------------------------------
// server bridge (fork's ra_server_call) + dry run
// ---------------------------------------------------------------------------

// Value of "&key=" (or leading "key=") in a form body, copied into out.
static int form_value(const char* body, const char* key, char* out, size_t out_size)
{
	size_t klen = strlen(key);
	const char* p = body;
	while (p && *p) {
		if (!strncmp(p, key, klen) && p[klen] == '=') {
			p += klen + 1;
			size_t n = 0;
			while (p[n] && p[n] != '&')
				n++;
			if (n >= out_size)
				n = out_size - 1;
			memcpy(out, p, n);
			out[n] = '\0';
			return 1;
		}
		p = strchr(p, '&');
		if (p)
			p++;
	}
	return 0;
}

// Fake success responses for the two submission requests. Everything else
// (login, game data, start session, pings) still goes to the server.
static char* dryrun_response(const char* post_data)
{
	if (!post_data)
		return NULL;
	char r[32], id[16];
	if (!form_value(post_data, "r", r, sizeof(r)))
		return NULL;
	char* out = (char*)malloc(512);
	if (!out)
		return NULL;
	if (!strcmp(r, "awardachievement")) {
		if (!form_value(post_data, "a", id, sizeof(id)))
			strcpy(id, "0");
		const rc_client_user_t* u = g_client ? rc_client_get_user_info(g_client) : NULL;
		// no AchievementsRemaining: rc_client then skips its mastery logic
		snprintf(out, 512, "{\"Success\":true,\"Score\":%u,\"SoftcoreScore\":%u,\"AchievementID\":%s}",
			u ? u->score : 0, u ? u->score_softcore : 0, id);
		return out;
	}
	if (!strcmp(r, "submitlbentry")) {
		char score[24];
		if (!form_value(post_data, "s", score, sizeof(score)))
			strcpy(score, "0");
		snprintf(out, 512, "{\"Success\":true,\"Response\":{\"Score\":%s,\"BestScore\":%s,"
			"\"RankInfo\":{\"Rank\":1,\"NumEntries\":\"1\"},\"TopEntries\":[]}}", score, score);
		return out;
	}
	free(out);
	return NULL;
}

struct ServerBridge {
	rc_client_server_callback_t callback;
	void* callback_data;
};

static void http_done(const ra_http_resp* hr, void* userdata)
{
	ServerBridge* br = (ServerBridge*)userdata;
	rc_api_server_response_t resp;
	memset(&resp, 0, sizeof(resp));
	resp.http_status_code = hr->http_status;
	resp.body = hr->body ? hr->body : "";
	resp.body_length = hr->body_len;

	if (g_debug) {
		char preview[220];
		snprintf(preview, sizeof(preview), "%.200s", resp.body);
		char* tp = strstr(preview, "\"Token\":\"");   // mask the token
		if (tp) {
			char* v = tp + 9;
			char* e = strchr(v, '"');
			if (e) {
				memmove(v + 3, e, strlen(e) + 1);
				memcpy(v, "***", 3);
			}
		}
		ra_log_debug("HTTP response: status=%d len=%zu body=%s", hr->http_status, hr->body_len, preview);
	}

	if (hr->http_status == 0)   // curl failed entirely: retryable
		resp.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;

	br->callback(&resp, br->callback_data);
	free(br);
}

static void RC_CCONV ra_server_call(const rc_api_request_t* request,
	rc_client_server_callback_t callback, void* callback_data, rc_client_t* client)
{
	(void)client;
	ServerBridge* br = (ServerBridge*)malloc(sizeof(ServerBridge));
	if (!br) {
		rc_api_server_response_t resp;
		memset(&resp, 0, sizeof(resp));
		resp.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
		resp.body = "malloc failed";
		resp.body_length = strlen(resp.body);
		callback(&resp, callback_data);
		return;
	}
	br->callback = callback;
	br->callback_data = callback_data;

	if (g_cfg.dryrun) {
		char* fake = dryrun_response(request->post_data);
		if (fake) {
			char r[32] = "", id[16] = "";
			form_value(request->post_data, "r", r, sizeof(r));
			form_value(request->post_data, !strcmp(r, "submitlbentry") ? "i" : "a", id, sizeof(id));
			ra_log("DRY RUN: %s %s NOT submitted (answered locally)", r, id);
			// asynchronous like a real response: dispatched by the next poll
			ra_http_inject_response(200, fake, http_done, br);
			return;
		}
	}
	ra_http_request(request->url, request->post_data, request->content_type, http_done, br);
}

// ---------------------------------------------------------------------------
// rc_client callbacks (RA thread)
// ---------------------------------------------------------------------------

static void RC_CCONV ra_client_log(const char* message, const rc_client_t* client)
{
	(void)client;
	ra_log("rcheevos: %s", message);
}

static void RC_CCONV ra_event_handler(const rc_client_event_t* event, rc_client_t* client)
{
	switch (event->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
		const rc_client_achievement_t* a = event->achievement;
		ra_log("*** ACHIEVEMENT UNLOCKED [%u] \"%s\" (%u pts) - %s [softcore%s]",
			a->id, a->title, a->points, a->description,
			g_cfg.dryrun ? ", DRY RUN" : "");
		append_unlock_log("achievement", a->id, a->title, a->points, a->description);
		notify(RA_NOTIF_ACHIEVEMENT, a->id, a->points, a->badge_name, a->title, a->description);
		play_chime();
		break;
	}
	case RC_CLIENT_EVENT_GAME_COMPLETED: {
		const rc_client_game_t* g = rc_client_get_game_info(client);
		ra_log("*** GAME COMPLETED (softcore): %s", g && g->title ? g->title : "?");
		append_unlock_log("completed", g ? g->id : 0, g ? g->title : "", 0, "all achievements earned");
		notify(RA_NOTIF_GAME_COMPLETED, g ? g->id : 0, 0, g ? g->badge_name : "", g ? g->title : "", "Completed");
		play_chime();
		break;
	}
	case RC_CLIENT_EVENT_SUBSET_COMPLETED:
		ra_log("*** SUBSET COMPLETED: %s", event->subset && event->subset->title ? event->subset->title : "?");
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
		ra_log_debug("leaderboard started [%u] %s", event->leaderboard->id, event->leaderboard->title);
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
		ra_log_debug("leaderboard failed [%u] %s", event->leaderboard->id, event->leaderboard->title);
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED: {
		const char* value = event->leaderboard->tracker_value ? event->leaderboard->tracker_value : "-";
		ra_log("leaderboard submitted [%u] %s: %s%s", event->leaderboard->id, event->leaderboard->title,
			value, g_cfg.dryrun ? " (DRY RUN)" : "");
		notify(RA_NOTIF_LEADERBOARD_SUBMITTED, event->leaderboard->id, 0, "",
			event->leaderboard->title, value);
		break;
	}
	case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
		ra_log("leaderboard %u: rank %u/%u, submitted %s, best %s",
			event->leaderboard_scoreboard->leaderboard_id,
			event->leaderboard_scoreboard->new_rank, event->leaderboard_scoreboard->num_entries,
			event->leaderboard_scoreboard->submitted_score, event->leaderboard_scoreboard->best_score);
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
		ra_log_debug("challenge show [%u] %s", event->achievement->id, event->achievement->title);
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
		ra_log_debug("challenge hide [%u] %s", event->achievement->id, event->achievement->title);
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
		ra_log_debug("progress [%u] %s: %s", event->achievement->id, event->achievement->title,
			event->achievement->measured_progress);
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
	case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
	case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
	case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
		break;
	case RC_CLIENT_EVENT_SERVER_ERROR:
		ra_log("server error (%s): %s", event->server_error->api ? event->server_error->api : "?",
			event->server_error->error_message ? event->server_error->error_message : "?");
		notify(RA_NOTIF_ERROR, event->server_error->related_id, 0, "", "RetroAchievements",
			event->server_error->error_message);
		break;
	case RC_CLIENT_EVENT_DISCONNECTED:
		ra_log("unlock request pending (server unreachable), will retry");
		break;
	case RC_CLIENT_EVENT_RECONNECTED:
		ra_log("pending unlocks submitted");
		break;
	case RC_CLIENT_EVENT_RESET:
		ra_log("runtime requested a reset (hardcore only; ignored)");
		break;
	default:
		ra_log_debug("event %u", event->type);
		break;
	}
}

static void stop_frames(void)
{
	ra_frame_source.store(RA_FRAME_OFF, std::memory_order_relaxed);
}

static void RC_CCONV load_game_callback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
	(void)userdata;
	if (result != RC_OK) {
		if (result == RC_NO_GAME_LOADED) {
			ra_log("game not recognized by RetroAchievements (hash %s), RA idle for this session", g_hash);
			notify(RA_NOTIF_INFO, 0, 0, "", "RetroAchievements", "Game not recognized - no achievements");
		} else {
			ra_log("game load failed: %s (%d)", error_message ? error_message : "?", result);
			notify(RA_NOTIF_ERROR, 0, 0, "", "RetroAchievements: game load failed",
				error_message ? error_message : "unknown error");
		}
		g_ra_dead = 1;
		stop_frames();
		return;
	}

	const rc_client_game_t* game = rc_client_get_game_info(client);
	rc_client_user_game_summary_t sum;
	memset(&sum, 0, sizeof(sum));
	rc_client_get_user_game_summary(client, &sum);
	ra_log("=== GAME IDENTIFIED: [%u] %s ===", game->id, game->title);
	ra_log("hash %s, %u achievements (%u pts), %u unlocked (%u pts), %u unsupported",
		g_hash, sum.num_core_achievements, sum.points_core,
		sum.num_unlocked_achievements, sum.points_unlocked, sum.num_unsupported_achievements);
	ra_log("mode: SOFTCORE%s; frames from %s", g_cfg.dryrun ? ", DRY RUN (no submissions)" : "",
		g_cfg.frame_source == RA_FRAME_RENDER ? "render end" : "vblank");

	char text[96];
	snprintf(text, sizeof(text), "%u of %u achievements unlocked",
		sum.num_unlocked_achievements, sum.num_core_achievements);
	notify(RA_NOTIF_GAME_LOADED, game->id, sum.points_core, game->badge_name, game->title, text);

	g_game_loaded = 1;
	g_game_ready.store(1, std::memory_order_release);
	if (g_cfg.chime_test) {
		ra_log("chime test (dreamster_chime_test / MINICAST_RA_CHIME_TEST)");
		play_chime();
	}
	// start evaluating: from now on the SH4 thread signals every frame
	ra_frame_source.store(g_cfg.frame_source, std::memory_order_relaxed);
}

static void RC_CCONV login_callback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
	(void)userdata;
	if (result != RC_OK) {
		ra_log("LOGIN FAILED for '%s': %s (%d)", g_cfg.user, error_message ? error_message : "?", result);
		notify(RA_NOTIF_ERROR, 0, 0, "", "RetroAchievements login failed",
			error_message ? error_message : "check retroachievements.cfg");
		g_ra_dead = 1;
		return;
	}
	const rc_client_user_t* user = rc_client_get_user_info(client);
	ra_log("logged in as %s (softcore score %u, hardcore %u)",
		user->display_name, user->score_softcore, user->score);
	g_logged_in = 1;
}

// login done + RAM allocated -> load the game (the load validates the
// achievement addresses by reading them, so RAM must be there first)
static void maybe_load_game(void)
{
	if (g_load_started || !g_logged_in || g_ra_dead || !g_ram.load(std::memory_order_acquire))
		return;
	g_load_started = 1;
	ra_log("loading game data for hash %s", g_hash);
	rc_client_begin_load_game(g_client, g_hash, load_game_callback, NULL);
}

// ---------------------------------------------------------------------------
// hashing (rcheevos' own Dreamcast hash, .gdi/.cue via the default cdreader)
// ---------------------------------------------------------------------------

static void RC_CCONV hash_error(const char* msg) { ra_log("hash: %s", msg); }
static void RC_CCONV hash_verbose(const char* msg) { ra_log_debug("hash: %s", msg); }

static int compute_hash(const char* path, char out[33])
{
	rc_hash_init_error_message_callback(hash_error);
	rc_hash_init_verbose_message_callback(hash_verbose);
	rc_hash_init_default_cdreader();
	out[0] = '\0';
	return rc_hash_generate_from_file(out, RC_CONSOLE_DREAMCAST, path) && out[0];
}

// Hashing runs on a short-lived helper at nice 10 (inherits the RA thread's
// cpu0 affinity and blocked signals): it reads the boot executable (a few
// MB) while the BIOS animation plays, and cpu0 also hosts the TA consumer.
static void* hash_thread_main(void* arg)
{
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
	*(int*)arg = compute_hash(g_game_path, g_hash);
	return NULL;
}

// ---------------------------------------------------------------------------
// RA thread
// ---------------------------------------------------------------------------

static void log_stats(const char* when)
{
	uint32_t sig = g_frames_signaled.load(std::memory_order_relaxed);
	uint32_t skip = g_frames_skipped.load(std::memory_order_relaxed);
	ra_log("%s: frames signaled %u, evaluated %u, skipped %u (busy), do_frame avg %llu us, max %llu us",
		when, sig, g_frames_processed, skip,
		(unsigned long long)(g_frames_processed ? g_frame_us_total / g_frames_processed : 0),
		(unsigned long long)g_frame_us_max);
}

// Achievement list snapshot for the overlay: every core achievement with its
// strings copied (ASCII), unlocked first. rc_client's lock-state grouping
// already puts "recently unlocked" ahead of older unlocks.
static void build_list(void)
{
	uint64_t t0 = mono_us();
	rc_client_achievement_list_t* list = rc_client_create_achievement_list(g_client,
		RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	uint32_t total = 0;
	if (list)
		for (uint32_t b = 0; b < list->num_buckets; b++)
			total += list->buckets[b].num_achievements;

	ra_list_data* d = (ra_list_data*)calloc(1, sizeof(ra_list_data));
	if (d && total)
		d->items = (ra_list_item*)calloc(total, sizeof(ra_list_item));
	if (!d || (total && !d->items)) {
		ra_log("list: out of memory");
		ra_list_free(d);
		if (list)
			rc_client_destroy_achievement_list(list);
		return;
	}
	const rc_client_game_t* game = rc_client_get_game_info(g_client);
	ra_to_ascii(game && game->title ? game->title : "", d->game, sizeof(d->game));
	for (uint32_t b = 0; list && b < list->num_buckets; b++) {
		const rc_client_achievement_bucket_t& bk = list->buckets[b];
		for (uint32_t i = 0; i < bk.num_achievements; i++) {
			const rc_client_achievement_t* a = bk.achievements[i];
			ra_list_item& it = d->items[d->count++];
			it.id = a->id;
			it.points = a->points;
			it.unlocked = a->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED ||
				(a->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE);
			it.unsupported = a->state == RC_CLIENT_ACHIEVEMENT_STATE_DISABLED ||
				bk.bucket_type == RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED;
			it.unlock_time = it.unlocked ? (int64_t)a->unlock_time : 0;
			ra_to_ascii(a->title, it.title, sizeof(it.title));
			ra_to_ascii(a->description, it.desc, sizeof(it.desc));
			ra_to_ascii(a->measured_progress, it.progress, sizeof(it.progress));
		}
	}
	if (list)
		rc_client_destroy_achievement_list(list);
	ra_list_sort(d);
	ra_list_totals(d);
	ra_log("list: %u achievements, %u unlocked (%u/%u pts), built in %llu us",
		d->count, d->num_unlocked, d->points_unlocked, d->points_total,
		(unsigned long long)(mono_us() - t0));
	ra_list_free(g_list_result.exchange(d, std::memory_order_acq_rel));
}

// 1 = frame signaled, 0 = timeout / quit
static int wait_frame(int timeout_ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long)timeout_ms * 1000000L;
	while (ts.tv_nsec >= 1000000000L) {
		ts.tv_nsec -= 1000000000L;
		ts.tv_sec++;
	}
	for (;;) {
		if (sem_timedwait(&g_frame_sem, &ts) == 0)
			return 1;
		if (errno != EINTR)
			return 0;
	}
}

static void flush_and_close(void)
{
	stop_frames();
	g_game_ready.store(0, std::memory_order_release);
	int timeout = g_quit_timeout_ms.load();
	uint64_t start = mono_ms(), deadline = start + (uint64_t)(timeout > 0 ? timeout : 0);
	int pending = ra_http_pending();
	if (pending)
		ra_log("shutdown: waiting for %d pending request(s), up to %d ms", pending, timeout);
	while ((pending = ra_http_pending()) > 0 && mono_ms() < deadline) {
		ra_http_poll();
		usleep(10000);
	}
	ra_http_poll();
	if (pending)
		ra_log("shutdown: %d request(s) still pending after %llu ms, giving up",
			pending, (unsigned long long)(mono_ms() - start));
	else if (mono_ms() - start > 20)
		ra_log("shutdown: requests flushed in %llu ms", (unsigned long long)(mono_ms() - start));
	if (g_game_loaded)
		log_stats("session");
	if (g_client) {
		rc_client_unload_game(g_client);
		rc_client_destroy(g_client);
		g_client = NULL;
	}
	g_game_loaded = 0;
}

static void* ra_thread_main(void*)
{
	// no process-directed signal may land here (polly2 SIGUSR1, SIGTERM is
	// sigwait()ed by main.cpp); cpu0 -- cpu1 is the SH4 thread's
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);
	cpu_set_t cpus;
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0)
		ra_log("could not pin the RA thread to cpu0");
	pthread_setname_np(pthread_self(), "ra");

	const char* wav = env_str("MINICAST_RA_WAV");
	if (!load_chime(wav ? wav : RA_WAV_PATH, g_cfg.chime_volume))
		ra_log("no unlock chime (%s missing or not 16-bit PCM)", wav ? wav : RA_WAV_PATH);

	uint64_t t0 = mono_ms();
	int hashed = 0;
	pthread_t hash_thread;
	if (pthread_create(&hash_thread, NULL, hash_thread_main, &hashed) == 0)
		pthread_join(hash_thread, NULL);
	else
		hashed = compute_hash(g_game_path, g_hash);
	if (!hashed) {
		ra_log("could not compute the RetroAchievements hash of %s, RA off for this session", g_game_path);
		g_ra_dead = 1;
	} else {
		ra_log("hash %s for %s (%llu ms)", g_hash, g_game_path, (unsigned long long)(mono_ms() - t0));
	}

	if (!g_ra_dead) {
		ra_http_init();
		g_client = rc_client_create(ra_read_memory, ra_server_call);
		if (!g_client) {
			ra_log("rc_client_create failed");
			g_ra_dead = 1;
		}
	}
	if (!g_ra_dead) {
		rc_client_enable_logging(g_client, g_debug ? RC_CLIENT_LOG_LEVEL_VERBOSE : RC_CLIENT_LOG_LEVEL_WARN, ra_client_log);
		rc_client_set_event_handler(g_client, ra_event_handler);
		rc_client_set_hardcore_enabled(g_client, 0);   // softcore only, see top

		char clause[64] = "", ua[128];
		rc_client_get_user_agent_clause(g_client, clause, sizeof(clause));
		snprintf(ua, sizeof(ua), "%s %s", RA_CLIENT_NAME, clause);
		ra_http_set_user_agent(ua);
		ra_log("User-Agent: %s", ua);

		ra_log("logging in as '%s'", g_cfg.user);
		rc_client_begin_login_with_password(g_client, g_cfg.user, g_cfg.password, login_callback, NULL);
	}

	uint64_t last_idle = mono_ms(), last_stats = mono_ms();
	const uint64_t stats_every = g_debug ? 60000 : 600000;
	while (!g_quit.load(std::memory_order_acquire)) {
		int frame = wait_frame(50);
		if (g_quit.load(std::memory_order_acquire))
			break;
		if (g_ra_dead) {
			// nothing to do until exit; frames are no longer signaled
			if (frame)
				g_frame_busy.store(0, std::memory_order_release);
			ra_http_poll();
			continue;
		}

		ra_http_poll();
		maybe_load_game();
		if (g_list_req.exchange(0) && g_game_loaded)
			build_list();
		if (g_reset_pending.exchange(0) && g_game_loaded) {
			ra_log("console reset: achievement runtime reset");
			rc_client_reset(g_client);
		}

		if (frame) {
			if (g_game_loaded) {
				uint64_t a = mono_us();
				rc_client_do_frame(g_client);
				uint64_t d = mono_us() - a;
				g_frames_processed++;
				g_frame_us_total += d;
				if (d > g_frame_us_max)
					g_frame_us_max = d;
				last_idle = mono_ms();
			}
			g_frame_busy.store(0, std::memory_order_release);
		}
		uint64_t now = mono_ms();
		if (now - last_idle >= 100) {   // no frames (loading, paused): keep pings/retries going
			rc_client_idle(g_client);
			last_idle = now;
		}
		if (g_game_loaded && now - last_stats >= stats_every) {
			log_stats("stats");
			last_stats = now;
		}
	}

	flush_and_close();
	g_thread_done.store(1, std::memory_order_release);
	return NULL;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void ra_init(const char* game_path)
{
	const char* e = env_str("MINICAST_RA");
	if (e && atoi(e) == 0) {
		printf("RA: disabled (MINICAST_RA=%s)\n", e);
		return;
	}
	if (!load_config())
		return;   // no cfg / no username: RetroAchievements stay off, silently
	if (!g_cfg.password[0]) {
		ra_log("retroachievements.cfg has a username but no password, RA off");
		return;
	}
	if (!game_path || !game_path[0] || !strcmp(game_path, "nodisk")) {
		ra_log("no disc, RA off");
		return;
	}
	snprintf(g_game_path, sizeof(g_game_path), "%s", game_path);

	ra_log("RetroAchievements for %s", g_game_path);
	if (g_cfg.hardcore_requested)
		ra_log("hardcore=1 in retroachievements.cfg IGNORED: minicast runs SOFTCORE only");
	ra_log("mode: SOFTCORE%s", g_cfg.dryrun ? ", DRY RUN: achievements/leaderboards are NOT submitted" : "");

	sem_init(&g_frame_sem, 0, 0);

	// plain SCHED_OTHER even if the creating thread runs RT
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
	struct sched_param sp;
	memset(&sp, 0, sizeof(sp));
	pthread_attr_setschedparam(&attr, &sp);
	int err = pthread_create(&g_thread, &attr, ra_thread_main, NULL);
	pthread_attr_destroy(&attr);
	if (err) {
		ra_log("RA thread failed to start (%d), RA off", err);
		return;
	}
	g_thread_running.store(1, std::memory_order_release);
}

int ra_active(void)
{
	return g_thread_running.load(std::memory_order_acquire) && !g_quit.load(std::memory_order_acquire);
}

int ra_game_ready(void)
{
	return g_game_ready.load(std::memory_order_acquire);
}

int ra_cfg_popups(void) { return g_cfg.popups; }
int ra_cfg_popup_position(void) { return g_cfg.popup_position; }
int ra_cfg_list_hotkey(void) { return g_cfg.list_hotkey; }

int ra_list_request(void)
{
	if (!ra_game_ready())
		return 0;
	g_list_req.store(1, std::memory_order_release);
	return 1;
}

ra_list_data* ra_list_take(void)
{
	return g_list_result.exchange(nullptr, std::memory_order_acq_rel);
}

void ra_notify_reset(void)
{
	if (g_thread_running.load(std::memory_order_acquire))
		g_reset_pending.store(1, std::memory_order_release);
}

void ra_shutdown(int timeout_ms)
{
	if (!g_thread_running.load(std::memory_order_acquire))
		return;
	stop_frames();
	if (!g_quit.exchange(1)) {
		g_quit_timeout_ms.store(timeout_ms);
		sem_post(&g_frame_sem);
	}
	// the thread flushes for at most timeout_ms; allow for a frame in progress
	uint64_t deadline = mono_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) + 500;
	while (!g_thread_done.load(std::memory_order_acquire) && mono_ms() < deadline)
		usleep(5000);
	if (!g_thread_done.load(std::memory_order_acquire))
		printf("RA: shutdown timed out\n");
	fflush(stdout);
}

// ---------------------------------------------------------------------------
// host test hooks
// ---------------------------------------------------------------------------

uint32_t ra_test_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes)
{
	return read_ram(address, buffer, num_bytes);
}

int ra_test_load_chime(const char* path, int volume_percent)
{
	return load_chime(path, volume_percent) ? (int)ra_chime.frames : 0;
}

int ra_test_hash(const char* path, char hash_out[33])
{
	return compute_hash(path, hash_out);
}

char* ra_test_dryrun_response(const char* post_data)
{
	return dryrun_response(post_data);
}

void ra_test_set_game_ready(int ready)
{
	g_game_ready.store(ready, std::memory_order_release);
}

void ra_test_publish_list(ra_list_data* d)
{
	ra_list_free(g_list_result.exchange(d, std::memory_order_acq_rel));
}
