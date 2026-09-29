#pragma once

// Overlay for minicast: RetroAchievements unlock popups drawn into the
// displayed Dreamcast framebuffer, and the in-game menu (ra_menu.h: Resume,
// Load Game, Achievements, Options, Exit) which pauses the emulation. With
// the menu off (MINICAST_MENU=0) the hotkey opens the achievement list
// directly, as before. Drawing (ra_osd) happens on the "ra_ovl" thread on
// cpu0; the SH4 thread only snapshots a few registers and signals.
//
//   main                ra_overlay_set_game(argv[1]) before the emulator starts
//   rend_init_renderer  ra_overlay_init() once VRAM is mapped
//   SH4 vblank          ra_overlay_vblank() when ra_overlay_wants_vblank();
//                       the SH4 thread parks in there while the menu is open
//                       (and leaves through ra_frontend_exit() when the menu
//                       loads a game / exits)
//   STARTRENDER         ra_overlay_render_start(): where the PowerVR writes
//   render end          ra_overlay_render_end()
//   evdev (SH4 thread)  ra_overlay_hotkey(): Start+L / Start+Select held / F6
//
// Rules that keep the game image intact (details in ra_osd.h/.cpp):
//   - a buffer is only read/written while no render into an overlapping
//     range is in flight; render_start waits (rarely, ~1 ms) for an overlay
//     operation on its target range to finish (seq_cst flag handshake)
//   - "under" pixels are refreshed from VRAM whenever the popup is redrawn,
//     and a restore only reverts pixels that still show the overlay

#include <stdint.h>
#include <atomic>

// The running image (argv[1]): the menu's title and "current game" fallback.
void ra_overlay_set_game(const char* path);
// Starts the overlay thread if the in-game menu is enabled (MINICAST_MENU
// unset or != 0) or RetroAchievements is running with popups / the list
// hotkey on. gpu_idle reports whether the PowerVR is idle (may be NULL).
void ra_overlay_init(uint8_t* vram, uint32_t vram_mask, bool (*gpu_idle)(void));
bool ra_overlay_running(void);
// Stops the thread (restores what it can). Tests / normal teardown.
void ra_overlay_shutdown(void);
// host tests: start without the RA thread / retroachievements.cfg;
// menu != 0: the hotkey opens the menu (protocol files in menu_dir)
void ra_overlay_test_start(uint8_t* vram, uint32_t vram_mask, int popup_ms, int position,
	int menu, const char* menu_dir);

// ---------------------------------------------------------------------------
// SH4 thread
// ---------------------------------------------------------------------------

enum { RA_OVL_ACTIVE = 1u, RA_OVL_PAUSE = 2u };
extern std::atomic<uint32_t> ra_ovl_flags;

static inline bool ra_overlay_wants_vblank(void)
{
	return ra_ovl_flags.load(std::memory_order_relaxed) != 0;
}

// The display registers at vblank. Blocks while the achievement list is
// open (the emulation is paused there).
void ra_overlay_vblank(uint32_t fb_r_sof1, uint32_t fb_r_ctrl, uint32_t vo_control, uint32_t spg_control);

// A render starts: it writes `lines` lines of `line_bytes` from FB_W_SOF1
// (and FB_W_SOF2 when two_fields). Cheap; waits only while the overlay is
// in the middle of an operation on an overlapping range.
void ra_overlay_render_start(uint32_t fb_w_sof1, uint32_t fb_w_sof2, bool two_fields,
	uint32_t line_bytes, uint32_t lines);
void ra_overlay_render_end(void);

// The hotkey fired (Start+L, or Start+Select held 0.5 s, on a mapped pad, or
// F6): opens the menu (or the achievement list) at the next vblank, or
// closes it while it is open.
void ra_overlay_hotkey(void);
// the hotkey does something: the menu is on, or the achievement list hotkey
// (retroachievements.cfg dreamster_list_hotkey) with RA running
bool ra_overlay_hotkey_enabled(void);
bool ra_overlay_menu_enabled(void);

// ---------------------------------------------------------------------------
// provided by the frontend (main.cpp) and the audio backend (audiostream.cpp)
// ---------------------------------------------------------------------------

enum {
	RA_MENU_UP = 1u << 0,
	RA_MENU_DOWN = 1u << 1,
	RA_MENU_LEFT = 1u << 2,
	RA_MENU_RIGHT = 1u << 3,
	RA_MENU_A = 1u << 4,
	RA_MENU_B = 1u << 5,
	RA_MENU_X = 1u << 6,
	RA_MENU_Y = 1u << 7,
	RA_MENU_START = 1u << 8,
	RA_MENU_L = 1u << 9,
	RA_MENU_R = 1u << 10,
};

// SH4 thread while paused: reads the input devices (hotkeys, ESC exit
// included) and returns the RA_MENU_* buttons held on any pad/keyboard.
uint32_t ra_frontend_menu_input(void);
// after the menu/list closes: the game sees no buttons until all are released
void ra_frontend_input_suppress(void);
// SH4 thread, paused: the menu wrote request.json - graceful exit (flash,
// VMU, RetroAchievements flush) and _exit(0). Never returns on the target.
void ra_frontend_exit(void);
// polly2 cfg page registers for the menu options (RA_REG_* in ra_menu.h):
// false when the register is not there (bitstream revision) or the LW
// bridge is not up
bool ra_frontend_reg_read(int reg, uint32_t* value);
bool ra_frontend_reg_write(int reg, uint32_t value);

// audio: while paused the FIFO feeder fades to silence and keeps the FIFO
// topped up with zeros (no underrun, no stutter); resume fades back in.
void AudioSetPaused(bool paused);
// SH4 thread, while paused: unthreaded audio mode keeps the FIFO fed
void AudioPausedTick(void);
