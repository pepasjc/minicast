#pragma once

// RetroAchievements for minicast (Dreamcast), milestone 1.
//
// Everything rcheevos does runs on one "RA thread" pinned to cpu0: hashing
// the disc, login, game load, rc_client_do_frame, and every rc_client
// callback / HTTP response (rc_client is driven from that thread only). The
// emulator threads only touch the few entry points below:
//
//   main thread   ra_init(game) before the emulator starts, ra_shutdown() on
//                 every exit path
//   emu init      ra_set_ram(main RAM) once the 16 MB are allocated
//   SH4 thread    ra_frame_hook(src) once per emulated frame: when the RA
//                 thread is idle it is woken (sem_post); when it is still
//                 busy with the previous frame this frame is skipped
//   audio         RaChimeMixer (below) mixes the unlock chime into the
//                 48 kHz output on the audio feeder thread
//   (milestone 2) ra_notification_pop() for an on-screen popup renderer
//
// With /media/fat/retroachievements.cfg absent (or no username), or with
// MINICAST_RA=0, ra_init() returns without starting anything and
// ra_frame_hook() is one load + compare per frame.

#include <stdint.h>
#include <atomic>

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

// Reads the environment + retroachievements.cfg and starts the RA thread.
// game_path: the image minicast was started with (argv[1]); "" = no disc.
void ra_init(const char* game_path);

// The emulated Dreamcast system RAM (physical 0x0C000000, RA 0x000000..).
void ra_set_ram(uint8_t* ram, uint32_t size);

// The emulated console was reset (soft reset back to the BIOS): the RA
// thread resets the achievement runtime (rc_client_reset).
void ra_notify_reset(void);

// Stops the RA thread. Waits (at most timeout_ms) for in-flight requests,
// so an achievement unlocked right before quitting still gets submitted.
// Safe from any thread, idempotent, no-op when RA is off.
void ra_shutdown(int timeout_ms);

// ---------------------------------------------------------------------------
// per-frame hook (SH4 thread)
// ---------------------------------------------------------------------------

enum { RA_FRAME_OFF = 0, RA_FRAME_VBLANK = 1, RA_FRAME_RENDER = 2 };

// Which hook drives rc_client_do_frame; RA_FRAME_OFF when RA is inactive.
extern std::atomic<int> ra_frame_source;

void ra_frame_signal(void);

static inline void ra_frame_hook(int src)
{
	if (ra_frame_source.load(std::memory_order_relaxed) == src)
		ra_frame_signal();
}

// ---------------------------------------------------------------------------
// unlock chime (audio)
// ---------------------------------------------------------------------------

// 48 kHz interleaved stereo s16, volume already applied. Published once by
// the RA thread before the first ra_chime_seq bump, immutable afterwards.
struct ra_chime_t {
	const int16_t* pcm;
	uint32_t frames;
};
extern ra_chime_t ra_chime;
extern std::atomic<uint32_t> ra_chime_seq;   // +1 per play request

// Owned by the thread that pushes 48 kHz output. poll() is one relaxed load
// (call it once per batch), mix() only while active().
struct RaChimeMixer {
	uint32_t seen = 0, pos = 0, len = 0;
	const int16_t* pcm = nullptr;

	inline void poll()
	{
		uint32_t s = ra_chime_seq.load(std::memory_order_relaxed);
		if (s != seen) {
			std::atomic_thread_fence(std::memory_order_acquire);
			seen = s;
			pcm = ra_chime.pcm;
			len = pcm ? ra_chime.frames : 0;
			pos = 0;   // a new unlock restarts the chime
		}
	}
	inline bool active() const { return pos < len; }

	// w = (r << 16) | l, as the polly2 AUDIO_DATA register wants it
	inline uint32_t mix(uint32_t w)
	{
		int l = (int16_t)(w & 0xFFFF) + pcm[2 * pos];
		int r = (int16_t)(w >> 16) + pcm[2 * pos + 1];
		pos++;
		l = l > 32767 ? 32767 : (l < -32768 ? -32768 : l);
		r = r > 32767 ? 32767 : (r < -32768 ? -32768 : r);
		return ((uint32_t)(uint16_t)r << 16) | (uint16_t)l;
	}
};

// ---------------------------------------------------------------------------
// notifications (for the milestone 2 on-screen popup)
// ---------------------------------------------------------------------------

enum ra_notification_kind {
	RA_NOTIF_GAME_LOADED = 1,        // title = game, text = "x/y achievements (softcore)"
	RA_NOTIF_ACHIEVEMENT,            // title/text = achievement title/description
	RA_NOTIF_GAME_COMPLETED,
	RA_NOTIF_LEADERBOARD_SUBMITTED,  // text = score
	RA_NOTIF_ERROR,                  // text = message
};

struct ra_notification_t {
	uint32_t seq;          // increasing; gaps = entries overwritten
	int kind;              // ra_notification_kind
	uint32_t id;           // achievement / leaderboard / game id
	uint32_t points;
	char badge[16];        // RA badge name ("12345"), "" if none
	char title[96];
	char text[160];
	uint64_t time_ms;      // CLOCK_MONOTONIC
};

// Pops the oldest pending notification (any single consumer thread).
// Returns 1 if *out was filled. The queue keeps the newest 16.
int ra_notification_pop(ra_notification_t* out);

// ---------------------------------------------------------------------------
// logging (stdout = the per-game log the launcher captures)
// ---------------------------------------------------------------------------

void ra_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void ra_log_debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ---------------------------------------------------------------------------
// internals exposed for the host test (tests/ra_test.cpp)
// ---------------------------------------------------------------------------

uint32_t ra_test_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes);
int ra_test_load_chime(const char* path, int volume_percent);
int ra_test_hash(const char* path, char hash_out[33]);
// Builds the fake server response a dry run hands rc_client for a
// submission; returns NULL for requests that are not submissions.
char* ra_test_dryrun_response(const char* post_data);
