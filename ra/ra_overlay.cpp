// ra_overlay.cpp -- RetroAchievements popups and achievement list on the
// displayed framebuffer (see ra_overlay.h for the hooks, ra_osd.h for how
// pixels are saved and restored).
//
// Threads:
//   SH4 (cpu1)    vblank: seqlock snapshot of FB_R_SOF1 / FB_R_CTRL /
//                 VO_CONTROL / SPG_CONTROL + sem_post, only while a popup is
//                 up; render start/end: a ring of render target ranges and
//                 an "in flight" flag (a handful of stores per render). While
//                 the list is open it parks in ra_overlay_vblank(): reads
//                 input for the list at ~66 Hz, audio stays silent.
//   ra_ovl (cpu0) everything else: popup queue, drawing, restoring, the menu
//                 and the list (the menu's file I/O and register writes too).
//
// Menu exit (Load Game / Exit Emulator): the overlay thread writes
// request.json, draws the "Exiting..." page and raises g_exit_req; the parked
// SH4 thread picks it up in pause_loop and calls ra_frontend_exit() (the same
// save + RetroAchievements flush as the ESC / SIGTERM exits, then _exit(0)).
//
// Render/overlay handshake (the only place the SH4 thread may wait): the
// overlay announces the range it is about to read/write (g_ovl_busy, seq_cst
// store) and then checks the render flag (seq_cst load); a render start
// publishes its ranges, sets the flag (seq_cst store) and then checks the
// overlay's (seq_cst load). At least one side sees the other: either the
// overlay backs off (tries again at the next vblank / render end) or the
// render start waits until the overlay is done with that range (normally
// well under a millisecond; bounded at 50 ms).

#include "ra_overlay.h"
#include "ra_osd.h"
#include "ra_dc.h"
#include "ra_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <signal.h>

std::atomic<uint32_t> ra_ovl_flags{0};

static std::atomic<bool> g_running{false};
static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_thread_alive{false};
static pthread_t g_thread;
static sem_t g_sem;
static RaVram g_vm;
static bool (*g_gpu_idle)(void) = nullptr;
static int g_popup_ms = 4000;
static int g_position = RA_POS_LEFT, g_list_enabled = 1;
static std::atomic<int> g_popups{1};      // the menu's Options can change it
static bool g_menu_on = false;            // hotkey opens the menu (else the list)
static RaMenu g_menu;                     // overlay thread (after init)
static char g_game_path[1024];
static std::atomic<int> g_exit_req{0};    // overlay -> SH4: request.json written, exit
static std::atomic<int> g_exit_ack{0};    // SH4 -> overlay: ra_frontend_exit() returned (tests)

static uint64_t mono_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static uint64_t mono_ms(void) { return mono_us() / 1000; }

static const char* const depth_name[4] = { "0555", "565", "888", "0888" };

// ---------------------------------------------------------------------------
// SH4 -> overlay: display registers at vblank (seqlock, single writer)
// ---------------------------------------------------------------------------

static std::atomic<uint32_t> g_snap_seq{0};   // odd while written, 0 = none yet
static std::atomic<uint32_t> g_snap_reg[4];   // FB_R_SOF1, FB_R_CTRL, VO_CONTROL, SPG_CONTROL
static std::atomic<uint32_t> g_vbl{0};        // vblanks published

static bool read_snap(RaGeom* g)
{
	for (int tries = 0; tries < 1000; tries++) {
		uint32_t s0 = g_snap_seq.load(std::memory_order_acquire);
		if (!s0)
			return false;
		if (s0 & 1)
			continue;
		uint32_t r[4];
		for (int i = 0; i < 4; i++)
			r[i] = g_snap_reg[i].load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acquire);
		if (g_snap_seq.load(std::memory_order_relaxed) == s0) {
			*g = ra_geom_from_regs(r[0], r[1], r[2], r[3]);
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// render tracking (SH4 writes, overlay reads)
// ---------------------------------------------------------------------------

static const uint32_t REV_N = 32;
struct RevSlot {
	std::atomic<uint32_t> lo0, hi0, lo1, hi1;
};
static RevSlot g_rev[REV_N];
static std::atomic<uint32_t> g_rev_seq{0};      // render starts published
static std::atomic<uint32_t> g_rend_active{0};  // a render is in flight
static std::atomic<uint32_t> g_rend_done{0};    // renders finished

// overlay -> SH4: the range the overlay is working on
static std::atomic<uint32_t> g_ovl_busy{0};
static std::atomic<uint32_t> g_ovl_lo{0}, g_ovl_hi{0};

// SH4-side statistics (single writer)
static std::atomic<uint32_t> g_rs_waits{0}, g_rs_wait_max_us{0}, g_rs_timeouts{0};

static void rev_read(uint32_t idx, RaRange* a, RaRange* b)
{
	RevSlot& e = g_rev[idx % REV_N];
	a->lo = e.lo0.load(std::memory_order_relaxed);
	a->hi = e.hi0.load(std::memory_order_relaxed);
	b->lo = e.lo1.load(std::memory_order_relaxed);
	b->hi = e.hi1.load(std::memory_order_relaxed);
}

void ra_overlay_render_start(uint32_t w1, uint32_t w2, bool two_fields, uint32_t line_bytes, uint32_t lines)
{
	if (!g_running.load(std::memory_order_relaxed))
		return;
	if (!line_bytes)
		line_bytes = 2560;
	if (!lines)
		lines = 480;
	uint32_t len = line_bytes * lines;
	RaRange a = { w1 & 0x7FFFFCu, (w1 & 0x7FFFFCu) + len };
	RaRange b = { 0, 0 };
	if (two_fields) {
		b.lo = w2 & 0x7FFFFCu;
		b.hi = b.lo + len;
	}
	uint32_t s = g_rev_seq.load(std::memory_order_relaxed);
	RevSlot& e = g_rev[s % REV_N];
	e.lo0.store(a.lo, std::memory_order_relaxed);
	e.hi0.store(a.hi, std::memory_order_relaxed);
	e.lo1.store(b.lo, std::memory_order_relaxed);
	e.hi1.store(b.hi, std::memory_order_relaxed);
	g_rev_seq.store(s + 1, std::memory_order_release);
	g_rend_active.store(1, std::memory_order_seq_cst);

	if (!g_ovl_busy.load(std::memory_order_seq_cst))
		return;
	// the overlay is working on some range right now: wait if it is ours
	uint64_t t0 = 0;
	for (;;) {
		if (!g_ovl_busy.load(std::memory_order_seq_cst))
			break;
		RaRange o = { g_ovl_lo.load(std::memory_order_seq_cst), g_ovl_hi.load(std::memory_order_seq_cst) };
		if (!ra_range_overlap(o, a) && !ra_range_overlap(o, b))
			break;
		uint64_t now = mono_us();
		if (!t0) {
			t0 = now;
		} else if (now - t0 > 50000) {
			g_rs_timeouts.store(g_rs_timeouts.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
			break;
		}
	}
	if (t0) {
		uint32_t us = (uint32_t)(mono_us() - t0);
		g_rs_waits.store(g_rs_waits.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		if (us > g_rs_wait_max_us.load(std::memory_order_relaxed))
			g_rs_wait_max_us.store(us, std::memory_order_relaxed);
	}
}

void ra_overlay_render_end(void)
{
	if (!g_running.load(std::memory_order_relaxed))
		return;
	g_rend_active.store(0, std::memory_order_seq_cst);
	g_rend_done.fetch_add(1, std::memory_order_release);
	if (ra_ovl_flags.load(std::memory_order_relaxed) & RA_OVL_ACTIVE)
		sem_post(&g_sem);   // redraw the fresh buffer before the game flips to it
}

struct OvlGuard : RaGuard {
	bool paused = false;   // SH4 parked and the PowerVR idle: nothing can start
	bool begin(const RaRange& r) override
	{
		if (paused)
			return true;
		g_ovl_lo.store(r.lo, std::memory_order_seq_cst);
		g_ovl_hi.store(r.hi, std::memory_order_seq_cst);
		g_ovl_busy.store(1, std::memory_order_seq_cst);
		if (g_rend_active.load(std::memory_order_seq_cst)) {
			// the render in flight is the last one published
			uint32_t s = g_rev_seq.load(std::memory_order_acquire);
			RaRange a, b;
			rev_read(s - 1, &a, &b);
			if (ra_range_overlap(a, r) || ra_range_overlap(b, r)) {
				g_ovl_busy.store(0, std::memory_order_seq_cst);
				return false;
			}
		}
		return true;
	}
	void end() override
	{
		g_ovl_busy.store(0, std::memory_order_seq_cst);
	}
};

// ---------------------------------------------------------------------------
// pause (in-game menu / achievement list)
// ---------------------------------------------------------------------------

enum { PAUSE_NONE = 0, PAUSE_REQUESTED, PAUSE_PARKED, PAUSE_DONE };
static std::atomic<int> g_pause{PAUSE_NONE};
static std::atomic<uint32_t> g_menu_btn{0};
static std::atomic<uint32_t> g_hotkey_seq{0};
static std::atomic<uint64_t> g_list_entered_ms{0};   // overlay picked the pause up

void ra_overlay_hotkey(void)
{
	if (!g_running.load(std::memory_order_relaxed))
		return;
	int p = g_pause.load(std::memory_order_acquire);
	if (p == PAUSE_PARKED) {
		g_hotkey_seq.fetch_add(1, std::memory_order_release);   // closes the menu / list
		return;
	}
	if (p != PAUSE_NONE)
		return;
	if (g_menu_on) {
		ra_menu_log("hotkey, pausing at the next vblank");
	} else {
		if (!g_list_enabled)
			return;
		if (!ra_game_ready()) {
			ra_log("list: hotkey, but no game is loaded");
			ra_notify_local(RA_NOTIF_INFO, "RetroAchievements", "No achievement list for this game");
			return;
		}
		ra_log("list: hotkey, pausing at the next vblank");
	}
	g_pause.store(PAUSE_REQUESTED, std::memory_order_release);
	ra_ovl_flags.fetch_or(RA_OVL_PAUSE, std::memory_order_relaxed);
}

bool ra_overlay_hotkey_enabled(void)
{
	return g_running.load(std::memory_order_relaxed) && (g_menu_on || g_list_enabled);
}

bool ra_overlay_menu_enabled(void)
{
	return g_running.load(std::memory_order_relaxed) && g_menu_on;
}

static void pause_loop(void)
{
	uint64_t t0 = mono_ms();
	const char* what = g_menu_on ? "menu" : "list";
	ra_log("%s: emulation paused", what);
	AudioSetPaused(true);
	g_list_entered_ms.store(0, std::memory_order_relaxed);
	g_pause.store(PAUSE_PARKED, std::memory_order_release);
	sem_post(&g_sem);
	while (g_pause.load(std::memory_order_acquire) != PAUSE_DONE) {
		if (g_exit_req.exchange(0, std::memory_order_acq_rel)) {
			// the menu wrote request.json: leave from this (the emulation)
			// thread, like the ESC exit does
			ra_menu_log("graceful exit (%s)", g_menu.request_action().c_str());
			ra_frontend_exit();
			g_exit_ack.store(1, std::memory_order_release);   // host tests only
		}
		// reads every device: the hotkey (ra_overlay_hotkey), ESC exit and
		// the buttons for the menu / list
		uint32_t b = ra_frontend_menu_input();
		g_menu_btn.store(b, std::memory_order_release);
		sem_post(&g_sem);
		AudioPausedTick();
		usleep(15000);
		// the overlay thread never took the pause up: don't hang the game
		if (!g_thread_alive.load(std::memory_order_acquire) ||
			(!g_list_entered_ms.load(std::memory_order_relaxed) && mono_ms() - t0 > 5000)) {
			ra_log("%s: overlay thread not responding, resuming", what);
			break;
		}
	}
	g_pause.store(PAUSE_NONE, std::memory_order_release);
	ra_ovl_flags.fetch_and(~RA_OVL_PAUSE, std::memory_order_relaxed);
	AudioSetPaused(false);
	ra_frontend_input_suppress();
	ra_log("%s: emulation resumed after %llu ms", what, (unsigned long long)(mono_ms() - t0));
}

void ra_overlay_vblank(uint32_t sof1, uint32_t ctrl, uint32_t vo, uint32_t spg)
{
	uint32_t s = g_snap_seq.load(std::memory_order_relaxed);
	g_snap_seq.store(s + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	g_snap_reg[0].store(sof1, std::memory_order_relaxed);
	g_snap_reg[1].store(ctrl, std::memory_order_relaxed);
	g_snap_reg[2].store(vo, std::memory_order_relaxed);
	g_snap_reg[3].store(spg, std::memory_order_relaxed);
	g_snap_seq.store(s + 2, std::memory_order_release);
	g_vbl.fetch_add(1, std::memory_order_release);
	sem_post(&g_sem);
	if (g_pause.load(std::memory_order_acquire) == PAUSE_REQUESTED)
		pause_loop();
}

// ---------------------------------------------------------------------------
// popups
// ---------------------------------------------------------------------------

static bool popup_text(const ra_notification_t& n, RaPopupText* t)
{
	char title[128], text[192];
	ra_to_ascii(n.title, title, sizeof(title));
	ra_to_ascii(n.text, text, sizeof(text));
	memset(t, 0, sizeof(*t));
	switch (n.kind) {
	case RA_NOTIF_ACHIEVEMENT: {
		char pts[16], fitted[128];
		snprintf(pts, sizeof(pts), " (%u)", n.points);
		ra_fit(title, 36 - (int)strlen(pts), fitted, sizeof(fitted));   // keep the points visible
		snprintf(t->line1, sizeof(t->line1), "Achievement unlocked");
		snprintf(t->line2, sizeof(t->line2), "%s%s", fitted, pts);
		t->accent = RA_C_BORDER;
		return true;
	}
	case RA_NOTIF_GAME_LOADED:
		snprintf(t->line1, sizeof(t->line1), "%s", title);
		snprintf(t->line2, sizeof(t->line2), "%s", text);
		t->accent = RA_C_BLUE;
		return true;
	case RA_NOTIF_GAME_COMPLETED:
		snprintf(t->line1, sizeof(t->line1), "All achievements unlocked!");
		snprintf(t->line2, sizeof(t->line2), "%s", title);
		t->accent = RA_C_BORDER;
		return true;
	case RA_NOTIF_ERROR:
		snprintf(t->line1, sizeof(t->line1), "%s", title);
		snprintf(t->line2, sizeof(t->line2), "%s", text);
		t->accent = RA_C_RED;
		return true;
	case RA_NOTIF_INFO:
		snprintf(t->line1, sizeof(t->line1), "%s", title);
		snprintf(t->line2, sizeof(t->line2), "%s", text);
		t->accent = RA_C_BLUE;
		return true;
	default:   // leaderboards: logged only (for now)
		return false;
	}
}

static void log_geom(const char* what, const RaGeom& g)
{
	ra_log("%s: fb 0x%06x %ux%u %s%s%s", what, g.sof, g.src_w, g.src_h, depth_name[g.depth],
		g.pixdbl ? " pixel-double" : "", g.enable ? "" : " (display off)");
}

// consume the render starts published since *last
static void consume_renders(RaTracker& tr, uint32_t* last)
{
	uint32_t s = g_rev_seq.load(std::memory_order_acquire);
	if (s - *last > REV_N) {
		tr.mark_all_dirty();
	} else {
		for (uint32_t i = *last; i != s; i++) {
			RaRange a, b;
			rev_read(i, &a, &b);
			tr.render_event(a, b);
		}
		// overwritten while reading: can't trust what was read
		if (g_rev_seq.load(std::memory_order_acquire) - *last > REV_N)
			tr.mark_all_dirty();
	}
	*last = s;
}

// ---------------------------------------------------------------------------
// in-game menu / achievement list (on the overlay thread while the SH4
// thread is parked)
// ---------------------------------------------------------------------------

// the achievement list snapshot from the RA thread (within ~3 s)
static ra_list_data* fetch_list(void)
{
	ra_list_data* data = NULL;
	if (ra_list_request()) {
		for (int i = 0; i < 300 && !data && !g_quit.load(); i++) {
			usleep(10000);
			data = ra_list_take();
		}
	}
	if (!data)
		ra_log("list: no list from the RA thread");
	return data;
}

enum { UI_MENU, UI_LIST };

static void run_ui(RaTracker& tr, OvlGuard& guard)
{
	uint64_t t0 = mono_ms();
	g_list_entered_ms.store(t0 ? t0 : 1, std::memory_order_relaxed);
	const bool menu = g_menu_on;
	const char* what = menu ? "menu" : "list";

	// a render the SH4 started before parking may still be running
	usleep(60000);
	for (int i = 0; i < 44 && g_gpu_idle && !g_gpu_idle(); i++)
		usleep(10000);
	guard.paused = true;

	int left = tr.restore_all();   // popups out of the way (they come back after)
	if (left)
		ra_log("%s: %d popup buffer(s) not restored", what, left);

	RaGeom g;
	bool have_geom = read_snap(&g);
	int mode = menu ? UI_MENU : UI_LIST;
	ra_list_data* data = NULL;
	if (menu)
		g_menu.open(ra_state());
	else
		data = fetch_list();

	RaCanvas c = {};
	RaPopWords pw = {};
	RaTouched t = {};
	RaRegion r = { 0, 0, have_geom ? g.src_w : 640, 240 };
	RaListView v = {};
	v.per_page = ra_list_per_page(r.w);
	v.in_menu = menu;
	bool ok = have_geom && g.enable && (menu || data) && ra_canvas_init(&c, r.w, r.h) && ra_touched_init(&t, g, r);
	bool first = true;
	// renders the current page and puts it on screen; the first draw reads
	// what VRAM shows (the game), later ones trust drawn[] (paused: nothing
	// else writes this buffer)
	auto draw = [&]() {
		if (mode == UI_MENU)
			g_menu.render(&c);
		else
			ra_list_render(&c, data, v, true);
		if (!ra_pop_build(&pw, g, r, c))
			return false;
		ra_touched_refresh(&t, g_vm, pw, first, NULL);
		ra_vram_flush();
		first = false;
		return true;
	};
	if (ok)
		ok = draw();
	if (!ok) {
		if (have_geom)
			log_geom(menu ? "menu: cannot show the menu" : "list: cannot show the list", g);
		else
			ra_log("%s: cannot show it (no display snapshot)", what);
	} else {
		log_geom(menu ? "menu: open" : "list: open", g);
	}

	uint32_t hk = g_hotkey_seq.load(std::memory_order_acquire);
	uint32_t prev = g_menu_btn.load(std::memory_order_acquire);   // held at open: not a press
	uint64_t held_since = 0, next_repeat = 0;
	uint32_t repeat_btn = 0;
	const uint32_t REPEATABLE = RA_MENU_UP | RA_MENU_DOWN | RA_MENU_LEFT | RA_MENU_RIGHT | RA_MENU_L | RA_MENU_R;
	while (ok && !g_quit.load(std::memory_order_acquire)) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 20 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_nsec -= 1000000000L;
			ts.tv_sec++;
		}
		sem_timedwait(&g_sem, &ts);
		if (g_hotkey_seq.load(std::memory_order_acquire) != hk)
			break;
		uint64_t now = mono_ms();
		uint32_t b = g_menu_btn.load(std::memory_order_acquire);
		uint32_t pressed = b & ~prev;
		prev = b;
		// auto-repeat for the direction / page buttons
		uint32_t rep = b & REPEATABLE;
		if (pressed & REPEATABLE) {
			repeat_btn = pressed & REPEATABLE;
			held_since = now;
			next_repeat = now + 400;
		} else if (repeat_btn && (rep & repeat_btn) && now >= next_repeat && held_since) {
			pressed |= repeat_btn & rep;
			next_repeat = now + 90;
		} else if (!(rep & repeat_btn)) {
			repeat_btn = 0;
		}
		if (!pressed)
			continue;

		if (mode == UI_MENU) {
			int act = g_menu.input(pressed, b);
			g_popups.store(g_menu.options().popups ? 1 : 0, std::memory_order_relaxed);
			if (act == RA_MACT_RESUME)
				break;
			if (act == RA_MACT_ACHIEVEMENTS) {
				mode = UI_LIST;
				v = RaListView();
				v.per_page = ra_list_per_page(r.w);
				v.in_menu = true;
				draw();   // "Loading..." while the RA thread builds the snapshot
				data = fetch_list();
				if (!data)
					mode = UI_MENU;
				draw();
				continue;
			}
			if (act == RA_MACT_EXIT) {
				draw();   // "Exiting..."
				ra_menu_log("%s requested, handing over to the emulation thread",
					g_menu.request_action() == "load" ? "load" : "exit");
				g_exit_ack.store(0, std::memory_order_relaxed);
				g_exit_req.store(1, std::memory_order_release);
				// on the target the process ends in there; host tests come back
				while (!g_exit_ack.load(std::memory_order_acquire) && !g_quit.load(std::memory_order_acquire))
					usleep(10000);
				break;
			}
			if (act == RA_MACT_REDRAW)
				draw();
			continue;
		}

		// achievement list
		RaListView nv = v;
		bool close = false;
		int count = data ? (int)data->count : 0;
		if (pressed & RA_MENU_UP)
			ra_list_nav(&nv, count, RA_NAV_UP);
		if (pressed & RA_MENU_DOWN)
			ra_list_nav(&nv, count, RA_NAV_DOWN);
		if (pressed & (RA_MENU_LEFT | RA_MENU_L))
			ra_list_nav(&nv, count, RA_NAV_PGUP);
		if (pressed & (RA_MENU_RIGHT | RA_MENU_R))
			ra_list_nav(&nv, count, RA_NAV_PGDN);
		if (nv.detail) {
			if (pressed & (RA_MENU_A | RA_MENU_B))
				nv.detail = false;
		} else {
			if ((pressed & RA_MENU_A) && count)
				nv.detail = true;
			if (pressed & RA_MENU_B)
				close = true;
		}
		if (close) {
			if (!menu)
				break;
			// back to the menu
			ra_list_free(data);
			data = NULL;
			mode = UI_MENU;
			draw();
			continue;
		}
		if (nv.sel == v.sel && nv.first == v.first && nv.detail == v.detail)
			continue;
		v = nv;
		draw();
	}

	if (t.have) {
		RaOpStats st = {};
		ra_touched_restore(&t, g_vm, &st);
		ra_vram_flush();
		if (st.kept_units)
			ra_log("%s: %u pixel(s) changed under it were kept", what, st.kept_units);
	}
	ra_touched_free(&t);
	ra_pop_free(&pw);
	ra_canvas_free(&c);
	ra_list_free(data);
	guard.paused = false;
	tr.mark_all_dirty();
	ra_log("%s: closed after %llu ms", what, (unsigned long long)(mono_ms() - t0));
	g_pause.store(PAUSE_DONE, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// overlay thread
// ---------------------------------------------------------------------------

static void* overlay_main(void*)
{
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);
	cpu_set_t cpus;
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0)
		ra_log("overlay: could not pin to cpu0");
	pthread_setname_np(pthread_self(), "ra_ovl");

	OvlGuard guard;
	RaTracker tr;
	tr.init(g_vm, &guard);

	bool showing = false, shown = false;
	RaPopupText txt = {};
	uint64_t start_ms = 0, end_ms = 0;
	bool content_dirty = false;
	RaGeom lay = {};
	bool have_lay = false;
	RaCanvas canvas = {};
	uint32_t last_vbl = g_vbl.load(), last_done = g_rend_done.load(), last_rev = g_rev_seq.load();
	uint32_t popups_shown = 0;

	while (!g_quit.load(std::memory_order_acquire)) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_nsec -= 1000000000L;
			ts.tv_sec++;
		}
		while (sem_timedwait(&g_sem, &ts) != 0 && errno == EINTR)
			;
		// collapse a burst of posts into one pass
		while (sem_trywait(&g_sem) == 0)
			;
		if (g_quit.load(std::memory_order_acquire))
			break;

		if (g_pause.load(std::memory_order_acquire) == PAUSE_PARKED) {
			run_ui(tr, guard);
			last_vbl = g_vbl.load();
			last_done = g_rend_done.load();
			last_rev = g_rev_seq.load();
			continue;
		}

		uint64_t now = mono_ms();
		consume_renders(tr, &last_rev);

		// popup lifecycle: shown for g_popup_ms from the first frame it is
		// visible; dropped if the display never shows it within 15 s
		bool popups_on = g_popups.load(std::memory_order_relaxed) != 0;
		if (showing && (!popups_on || (shown && now >= end_ms) || (!shown && now - start_ms > 15000))) {
			if (!shown && popups_on)
				ra_log("popup: never displayed, dropped: \"%s\"", txt.line1);
			showing = false;
			tr.set_content(lay, RaRegion(), NULL);
		}
		if (!showing) {
			ra_notification_t n;
			while (ra_notification_pop(&n)) {
				if (popups_on && popup_text(n, &txt)) {
					showing = true;
					shown = false;
					start_ms = now;
					content_dirty = true;
					break;
				}
			}
		}

		bool want = showing || tr.count() > 0;
		if (want)
			ra_ovl_flags.fetch_or(RA_OVL_ACTIVE, std::memory_order_relaxed);
		else
			ra_ovl_flags.fetch_and(~RA_OVL_ACTIVE, std::memory_order_relaxed);
		if (!want)
			continue;

		RaGeom g;
		if (!read_snap(&g))
			continue;   // no vblank seen yet
		uint32_t vbl = g_vbl.load(std::memory_order_acquire);
		bool new_vbl = vbl != last_vbl;
		last_vbl = vbl;

		if (showing && (content_dirty || !have_lay || !ra_geom_same_layout(g, lay))) {
			if (have_lay && !ra_geom_same_layout(g, lay))
				log_geom("popup: display mode changed", g);
			RaRegion r = ra_popup_layout(g.src_w, g_position);
			if (canvas.w != r.w || canvas.h != r.h) {
				ra_canvas_free(&canvas);
				ra_canvas_init(&canvas, r.w, r.h);
			}
			if (canvas.px && ra_popup_render(&canvas, g.src_w, r, txt) && tr.set_content(g, r, &canvas)) {
				lay = g;
				have_lay = true;
				content_dirty = false;
			}
		}

		uint32_t done = g_rend_done.load(std::memory_order_acquire);
		if (done != last_done) {
			last_done = done;
			tr.render_done();
		}
		if (new_vbl) {
			if (showing && tr.has_content() && g.enable) {
				if (tr.vblank(g.sof, vbl) && !shown) {
					shown = true;
					end_ms = now + (uint64_t)g_popup_ms;
					popups_shown++;
					ra_log("popup: \"%s\" / \"%s\"", txt.line1, txt.line2);
					if (popups_shown == 1)
						log_geom("popup: first one", g);
				}
			} else if (!showing) {
				tr.vblank(g.sof, vbl);   // retries restores of old layouts
			}
		}
		if (!showing && tr.count()) {
			if (!tr.restore_all()) {
				const RaOpStats& st = tr.st;
				ra_log("popup: removed; totals: %u draws, %u restores, %u pixels found rewritten by the game, "
					"%u kept at restore, %u guard retries; render starts delayed %u times (max %u us, %u timeouts)",
					st.refreshes, st.restores, st.fresh_units, st.kept_units, tr.guard_skips,
					g_rs_waits.load(), g_rs_wait_max_us.load(), g_rs_timeouts.load());
			}
		}
	}

	// best effort: the emulator is stopping
	tr.restore_all();
	tr.shutdown();
	ra_canvas_free(&canvas);
	ra_ovl_flags.store(0);
	g_thread_alive.store(false, std::memory_order_release);
	return NULL;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

static void overlay_start(uint8_t* vram, uint32_t vram_mask, bool (*gpu_idle)(void));

void ra_overlay_set_game(const char* path)
{
	snprintf(g_game_path, sizeof(g_game_path), "%s", path ? path : "");
}

static const RaMenuHw g_menu_hw = { ra_frontend_reg_read, ra_frontend_reg_write };

void ra_overlay_init(uint8_t* vram, uint32_t vram_mask, bool (*gpu_idle)(void))
{
	if (g_running.load() || !vram)
		return;
	// MINICAST_MENU=0: no in-game menu (the hotkey opens the achievement
	// list as before, if RetroAchievements runs)
	const char* m = getenv("MINICAST_MENU");
	g_menu_on = !(m && m[0] == '0');
	bool ra = ra_active() != 0;
	g_popups.store(ra && ra_cfg_popups() ? 1 : 0);
	g_position = ra_cfg_popup_position();
	g_list_enabled = ra ? ra_cfg_list_hotkey() : 0;
	const char* e = getenv("MINICAST_RA_POPUP_MS");
	if (e && atoi(e) > 0)
		g_popup_ms = atoi(e);
	if (g_menu_on) {
		// options.json may override the retroachievements.cfg popups setting
		g_menu.init(getenv("MINICAST_MENU_DIR"), g_game_path, &g_menu_hw, g_popups.load() != 0);
		if (ra)
			g_popups.store(g_menu.options().popups ? 1 : 0);
	} else if (!ra) {
		return;   // nothing to show
	} else if (!g_popups.load() && !g_list_enabled) {
		ra_log("overlay: popups and list hotkey both off, no menu");
		return;
	}
	overlay_start(vram, vram_mask, gpu_idle);
}

// host tests: no RA thread needed (notifications via ra_notify_local)
void ra_overlay_test_start(uint8_t* vram, uint32_t vram_mask, int popup_ms, int position,
	int menu, const char* menu_dir)
{
	if (g_running.load())
		return;
	g_popups.store(1);
	g_list_enabled = 1;
	g_popup_ms = popup_ms;
	g_position = position;
	g_menu_on = menu != 0;
	if (g_menu_on)
		g_menu.init(menu_dir, g_game_path, &g_menu_hw, true);
	overlay_start(vram, vram_mask, NULL);
}

static void overlay_start(uint8_t* vram, uint32_t vram_mask, bool (*gpu_idle)(void))
{
	g_vm.base = vram;
	g_vm.mask = vram_mask;
	g_gpu_idle = gpu_idle;
	sem_init(&g_sem, 0, 0);
	g_quit.store(false);
	g_running.store(true, std::memory_order_release);   // render tracking on from here

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
	struct sched_param sp;
	memset(&sp, 0, sizeof(sp));
	pthread_attr_setschedparam(&attr, &sp);
	g_thread_alive.store(true);
	int err = pthread_create(&g_thread, &attr, overlay_main, NULL);
	pthread_attr_destroy(&attr);
	if (err) {
		g_thread_alive.store(false);
		g_running.store(false);
		ra_log("overlay: thread failed to start (%d)", err);
		return;
	}
	g_exit_req.store(0);
	g_exit_ack.store(0);
	ra_log("overlay: popups %s (%s, %d ms), hotkey (Start+Select / Start+R held 0.5 s, F6) opens %s",
		g_popups.load() ? "on" : "off", ra_position_name(g_position), g_popup_ms,
		g_menu_on ? "the in-game menu" : (g_list_enabled ? "the achievement list" : "nothing"));
}

bool ra_overlay_running(void)
{
	return g_running.load(std::memory_order_relaxed);
}

void ra_overlay_shutdown(void)
{
	if (!g_running.load())
		return;
	g_quit.store(true, std::memory_order_release);
	sem_post(&g_sem);
	for (int i = 0; i < 200 && g_thread_alive.load(std::memory_order_acquire); i++)
		usleep(5000);
	if (!g_thread_alive.load(std::memory_order_acquire))
		pthread_join(g_thread, NULL);
	else
		ra_log("overlay: thread did not stop");
	g_running.store(false, std::memory_order_release);
	ra_ovl_flags.store(0);
}
