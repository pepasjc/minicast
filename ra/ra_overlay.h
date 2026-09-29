#pragma once

// RetroAchievements overlay for minicast: unlock popups drawn into the
// displayed Dreamcast framebuffer, and the in-game achievement list (which
// pauses the emulation). Drawing (ra_osd) happens on the "ra_ovl" thread on
// cpu0; the SH4 thread only snapshots a few registers and signals.
//
//   rend_init_renderer  ra_overlay_init() once VRAM is mapped
//   SH4 vblank          ra_overlay_vblank() when ra_overlay_wants_vblank();
//                       the SH4 thread parks in there while the list is open
//   STARTRENDER         ra_overlay_render_start(): where the PowerVR writes
//   render end          ra_overlay_render_end()
//   evdev (SH4 thread)  ra_overlay_hotkey(): Start+Select held / F6
//
// Rules that keep the game image intact (details in ra_osd.h/.cpp):
//   - a buffer is only read/written while no render into an overlapping
//     range is in flight; render_start waits (rarely, ~1 ms) for an overlay
//     operation on its target range to finish (seq_cst flag handshake)
//   - "under" pixels are refreshed from VRAM whenever the popup is redrawn,
//     and a restore only reverts pixels that still show the overlay

#include <stdint.h>
#include <atomic>

// Starts the overlay thread if RetroAchievements is running. gpu_idle
// reports whether the PowerVR is idle (may be NULL).
void ra_overlay_init(uint8_t* vram, uint32_t vram_mask, bool (*gpu_idle)(void));
bool ra_overlay_running(void);
// Stops the thread (restores what it can). Tests / normal teardown.
void ra_overlay_shutdown(void);
// host tests: start without the RA thread / retroachievements.cfg
void ra_overlay_test_start(uint8_t* vram, uint32_t vram_mask, int popup_ms, int position);

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

// The list hotkey fired (Start+Select held 0.5 s on a mapped pad, or F6):
// opens the list at the next vblank, or closes it while it is open.
void ra_overlay_hotkey(void);
// list hotkey enabled (retroachievements.cfg dreamster_list_hotkey)
bool ra_overlay_list_enabled(void);

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
// after the list closes: the game sees no buttons until all are released
void ra_frontend_input_suppress(void);

// audio: while paused the FIFO feeder fades to silence and keeps the FIFO
// topped up with zeros (no underrun, no stutter); resume fades back in.
void AudioSetPaused(bool paused);
// SH4 thread, while paused: unthreaded audio mode keeps the FIFO fed
void AudioPausedTick(void);
