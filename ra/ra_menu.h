#pragma once

// ra_menu -- the DreamSTer in-game menu: pages, navigation, live options and
// the files exchanged with the launcher. Plain computation + file I/O: the
// overlay thread (ra_overlay.cpp) pauses the emulation, feeds the buttons in
// and puts the rendered canvas on screen; the registers are reached through
// the RaMenuHw callbacks (host tests stub them).
//
// Pages: Main (Resume / Load Game / Achievements / Options / Exit Emulator),
// the game list, Options (Volume, Vertical filter, 480i field order,
// Achievement popups; applied as they change), a Yes/No confirmation for
// loading a game or exiting, and the "Exiting..." page shown while the
// emulator shuts down.
//
// Launcher protocol, directory RA_MENU_DIR (/tmp/dreamster, created if
// missing; MINICAST_MENU_DIR overrides it for tests):
//   games.json            launcher -> minicast, before the start:
//                         {"games":[{"name":"..","path":".."},..],"current":"<path>"}
//                         missing/invalid: Load Game says "No game list"
//   options.json          launcher -> minicast, before the start:
//                         {"volume":"0".."7"|"mute","vfilter":"off|light|smooth",
//                          "field":"correct|original","popups":"on|off"}
//                         missing keys: read from the registers (popups: the
//                         retroachievements.cfg setting)
//   request.json          minicast -> launcher, written (tmp + rename) right
//                         before the graceful exit the menu starts:
//                         {"action":"load","path":"<gdi>"} or {"action":"exit"}
//   options_changed.json  minicast -> launcher, the full option set (same
//                         keys/values as options.json), rewritten (tmp +
//                         rename) after every change
// A request.json / options_changed.json left over from an earlier run is
// removed when the menu starts.

#include "ra_osd.h"
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#define RA_MENU_DIR "/tmp/dreamster"

void ra_menu_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ---------------------------------------------------------------------------
// options + registers
// ---------------------------------------------------------------------------

enum { RA_VF_OFF = 0, RA_VF_LIGHT = 1, RA_VF_SMOOTH = 2 };

struct RaMenuOptions {
	int vol_att = 0;              // AUDIO_VOL[3:0]: attenuation, 6 dB steps (0 = max, 7 = -42 dB)
	bool mute = false;            // AUDIO_VOL[4]
	int vfilter = RA_VF_OFF;      // CRT_CTRL[2:1]
	bool field_correct = true;    // CRT_CTRL[0]
	bool popups = true;           // achievement popups
	bool have_volume = false, have_vfilter = false, have_field = false, have_popups = false;
};

// options.json: sets the recognized keys (and their have_* flags); false if
// the text is not a JSON object
bool ra_menu_parse_options(const char* text, size_t len, RaMenuOptions* o);
// the full set, options.json format
std::string ra_menu_options_json(const RaMenuOptions& o);

// polly2 cfg page registers (the frontend maps them to their offsets:
// AUDIO_VOL 0x30 on REVISION >= 6, 0x28 on REVISION 3; CRT_CTRL 0x34)
enum { RA_REG_AUDIO_VOL = 0, RA_REG_CRT_CTRL = 1 };

static inline uint32_t ra_audio_vol_value(int att, bool mute)
{
	return mute ? 0x10u : (uint32_t)(att < 0 ? 0 : (att > 7 ? 7 : att));
}
// CRT_CTRL read-modify-write: only the field changes, [0] field order /
// [3] 240p / [4] YPbPr / [5] subcarrier are kept; reserved bits written 0
uint32_t ra_crt_ctrl_set_vfilter(uint32_t cur, int vfilter);
uint32_t ra_crt_ctrl_set_field(uint32_t cur, bool correct);

struct RaMenuHw {
	bool (*reg_read)(int reg, uint32_t* value);   // false: not available
	bool (*reg_write)(int reg, uint32_t value);
};

// ---------------------------------------------------------------------------
// game list + protocol files
// ---------------------------------------------------------------------------

struct RaGame {
	std::string name, path;   // UTF-8 as in games.json
	char letter;              // 'A'..'Z', '#' (digits / other), for the letter jump
};

struct RaGameList {
	std::vector<RaGame> games;
	std::string current;      // "current" (may be empty)
};
// false if the text is not {"games":[...]} (entries without a path are skipped)
bool ra_menu_parse_games(const char* text, size_t len, RaGameList* out);

// whole file, at most max_bytes (false if bigger / unreadable)
bool ra_menu_read_file(const char* path, std::string* out, size_t max_bytes);
// path.tmp + fsync + rename
bool ra_menu_write_atomic(const char* path, const std::string& data);
// {"action":"load","path":".."} / {"action":"exit"} (path NULL)
std::string ra_menu_request_json(const char* action, const char* path);

// ---------------------------------------------------------------------------
// the menu
// ---------------------------------------------------------------------------

enum {
	RA_MACT_NONE = 0,        // nothing changed
	RA_MACT_REDRAW,          // render() again
	RA_MACT_RESUME,          // close the menu, resume the game
	RA_MACT_ACHIEVEMENTS,    // show the achievement list (B comes back here)
	RA_MACT_EXIT,            // request.json written, page = Exiting: shut down
};

class RaMenu {
public:
	enum Page { P_MAIN, P_GAMES, P_OPTIONS, P_CONFIRM, P_EXITING };
	enum { M_RESUME, M_LOAD, M_ACHIEVEMENTS, M_OPTIONS, M_EXIT, M_COUNT };
	enum { O_VOLUME, O_VFILTER, O_FIELD, O_POPUPS, O_COUNT };

	// dir: protocol directory; game_path: argv[1]; popups_default: the RA
	// cfg setting, used when options.json has no "popups"
	void init(const char* dir, const char* game_path, const RaMenuHw* hw, bool popups_default);
	// every time the game is paused for the menu: main page, Resume selected;
	// ra_state (RA_STATE_*) decides whether Achievements can be picked
	void open(int ra_state);
	// pressed: RA_MENU_* bits newly pressed (auto-repeat included), held: all held
	int input(uint32_t pressed, uint32_t held);
	// full-screen page (c->h must be 240)
	bool render(RaCanvas* c) const;

	Page page() const { return page_; }
	const RaMenuOptions& options() const { return opt_; }
	const RaGameList& games() const { return games_; }
	bool games_ok() const { return games_ok_; }
	// set once RA_MACT_EXIT was returned
	const std::string& request_action() const { return req_action_; }
	const std::string& request_path() const { return req_path_; }
	std::string dir() const { return dir_; }

private:
	void load_games();
	void read_registers();
	bool main_enabled(int item) const;
	bool opt_enabled(int row) const;
	const char* main_reason(int item) const;
	const char* opt_reason(int row) const;
	void opt_value(int row, char* buf, size_t n, bool* can_left, bool* can_right) const;
	bool opt_change(int row, int dir, bool wrap);
	void write_options_changed();
	int do_confirm();
	void game_view_clamp();
	void letter_jump(int dir);
	std::string title() const;

	void render_main(RaCanvas* c, int mx, int cols) const;
	void render_games(RaCanvas* c, int mx, int cols) const;
	void render_options(RaCanvas* c, int mx, int cols) const;
	void render_confirm(RaCanvas* c, int mx, int cols) const;
	void render_exiting(RaCanvas* c, int mx, int cols) const;

	std::string dir_, game_path_;
	const RaMenuHw* hw_ = nullptr;
	RaMenuOptions opt_;
	bool vol_ok_ = false, crt_ok_ = false;
	int ra_state_ = 0;

	bool games_loaded_ = false, games_ok_ = false;
	RaGameList games_;
	int current_idx_ = -1;

	Page page_ = P_MAIN;
	int main_sel_ = M_RESUME;
	int opt_sel_ = O_VOLUME;
	RaListView gv_ = {};         // game list view
	bool confirm_load_ = false;  // confirm page: load (gv_.sel) or exit
	int confirm_sel_ = 0;        // 0 = No, 1 = Yes
	std::string error_;
	std::string req_action_, req_path_;
};
