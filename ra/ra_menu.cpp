// ra_menu.cpp -- the DreamSTer in-game menu (see ra_menu.h)

#include "ra_menu.h"
#include "ra_json.h"
#include "ra_overlay.h"   // RA_MENU_* buttons
#include "ra_dc.h"        // RA_STATE_*

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

void ra_menu_log(const char* fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	flockfile(stdout);
	fputs("menu: ", stdout);
	fputs(line, stdout);
	fputc('\n', stdout);
	fflush(stdout);
	funlockfile(stdout);
}

// ---------------------------------------------------------------------------
// options + registers
// ---------------------------------------------------------------------------

static bool str_is(const char* a, const char* b)
{
	return a && !strcasecmp(a, b);
}

bool ra_menu_parse_options(const char* text, size_t len, RaMenuOptions* o)
{
	RaJson j;
	if (!ra_json_parse(text, len, &j) || j.type != RaJson::OBJ)
		return false;
	const RaJson* v = j.get("volume");
	if (v && v->type == RaJson::STR) {
		if (str_is(v->str.c_str(), "mute")) {
			o->mute = true;
			o->vol_att = 0;
			o->have_volume = true;
		} else if (v->str.size() == 1 && v->str[0] >= '0' && v->str[0] <= '7') {
			o->mute = false;
			o->vol_att = v->str[0] - '0';
			o->have_volume = true;
		}
	} else if (v && v->type == RaJson::NUM && v->num >= 0 && v->num <= 7) {
		o->mute = false;
		o->vol_att = (int)v->num;
		o->have_volume = true;
	}
	const char* s = j.get_str("vfilter");
	if (str_is(s, "off") || str_is(s, "light") || str_is(s, "smooth")) {
		o->vfilter = str_is(s, "off") ? RA_VF_OFF : str_is(s, "light") ? RA_VF_LIGHT : RA_VF_SMOOTH;
		o->have_vfilter = true;
	}
	s = j.get_str("field");
	if (str_is(s, "correct") || str_is(s, "original")) {
		o->field_correct = str_is(s, "correct");
		o->have_field = true;
	}
	v = j.get("popups");
	if (v && v->type == RaJson::STR && (str_is(v->str.c_str(), "on") || str_is(v->str.c_str(), "off"))) {
		o->popups = str_is(v->str.c_str(), "on");
		o->have_popups = true;
	} else if (v && v->type == RaJson::BOOL) {
		o->popups = v->b;
		o->have_popups = true;
	}
	return true;
}

std::string ra_menu_options_json(const RaMenuOptions& o)
{
	static const char* const vf[3] = { "off", "light", "smooth" };
	char vol[8];
	if (o.mute)
		snprintf(vol, sizeof(vol), "mute");
	else
		snprintf(vol, sizeof(vol), "%d", o.vol_att < 0 ? 0 : (o.vol_att > 7 ? 7 : o.vol_att));
	std::string s = "{\"volume\":";
	ra_json_quote(&s, vol);
	s += ",\"vfilter\":";
	ra_json_quote(&s, vf[o.vfilter >= 0 && o.vfilter <= 2 ? o.vfilter : 0]);
	s += ",\"field\":";
	ra_json_quote(&s, o.field_correct ? "correct" : "original");
	s += ",\"popups\":";
	ra_json_quote(&s, o.popups ? "on" : "off");
	s += "}\n";
	return s;
}

// CRT_CTRL bits (pvr_mmio.sv): [0] field order, [2:1] vertical filter,
// [3] 240p, [4] YPbPr, [5] subcarrier; the rest reserved (write 0)
static const uint32_t CRT_CTRL_MASK = 0x3Fu;

uint32_t ra_crt_ctrl_set_vfilter(uint32_t cur, int vfilter)
{
	uint32_t vf = vfilter < 0 ? 0u : (vfilter > 2 ? 2u : (uint32_t)vfilter);
	return ((cur & ~0x6u) | (vf << 1)) & CRT_CTRL_MASK;
}

uint32_t ra_crt_ctrl_set_field(uint32_t cur, bool correct)
{
	return ((cur & ~0x1u) | (correct ? 1u : 0u)) & CRT_CTRL_MASK;
}

// ---------------------------------------------------------------------------
// game list + protocol files
// ---------------------------------------------------------------------------

static std::string path_basename(const std::string& p, bool strip_ext)
{
	size_t s = p.find_last_of('/');
	std::string b = s == std::string::npos ? p : p.substr(s + 1);
	if (strip_ext) {
		size_t d = b.find_last_of('.');
		if (d != std::string::npos && d > 0)
			b = b.substr(0, d);
	}
	return b;
}

static char game_letter(const std::string& name)
{
	char a[64];
	ra_to_ascii(name.c_str(), a, sizeof(a));
	for (const char* p = a; *p; p++) {
		char c = *p;
		if (c >= 'a' && c <= 'z')
			return (char)(c - 'a' + 'A');
		if (c >= 'A' && c <= 'Z')
			return c;
		if (c >= '0' && c <= '9')
			return '#';
	}
	return '#';
}

bool ra_menu_parse_games(const char* text, size_t len, RaGameList* out)
{
	out->games.clear();
	out->current.clear();
	RaJson j;
	if (!ra_json_parse(text, len, &j) || j.type != RaJson::OBJ)
		return false;
	const RaJson* g = j.get("games");
	if (!g || g->type != RaJson::ARR)
		return false;
	out->games.reserve(g->items.size());
	for (const RaJson& e : g->items) {
		if (e.type != RaJson::OBJ)
			continue;
		const char* path = e.get_str("path");
		if (!path || !path[0])
			continue;
		RaGame gm;
		gm.path = path;
		const char* name = e.get_str("name");
		gm.name = name && name[0] ? name : path_basename(gm.path, true);
		gm.letter = game_letter(gm.name);
		out->games.push_back(gm);
	}
	const char* cur = j.get_str("current");
	if (cur)
		out->current = cur;
	return true;
}

bool ra_menu_read_file(const char* path, std::string* out, size_t max_bytes)
{
	out->clear();
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	char buf[16384];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (out->size() + n > max_bytes) {
			ok = false;
			break;
		}
		out->append(buf, n);
	}
	if (ferror(f))
		ok = false;
	fclose(f);
	if (!ok)
		out->clear();
	return ok;
}

bool ra_menu_write_atomic(const char* path, const std::string& data)
{
	std::string tmp = std::string(path) + ".tmp";
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return false;
	size_t off = 0;
	while (off < data.size()) {
		ssize_t w = write(fd, data.data() + off, data.size() - off);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0) {
			close(fd);
			unlink(tmp.c_str());
			return false;
		}
		off += (size_t)w;
	}
	fsync(fd);
	if (close(fd) != 0 || rename(tmp.c_str(), path) != 0) {
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

std::string ra_menu_request_json(const char* action, const char* path)
{
	std::string s = "{\"action\":";
	ra_json_quote(&s, action);
	if (path) {
		s += ",\"path\":";
		ra_json_quote(&s, path);
	}
	s += "}\n";
	return s;
}

// ---------------------------------------------------------------------------
// menu state
// ---------------------------------------------------------------------------

static const size_t GAMES_MAX_BYTES = 16u << 20;

void RaMenu::init(const char* dir, const char* game_path, const RaMenuHw* hw, bool popups_default)
{
	dir_ = dir && dir[0] ? dir : RA_MENU_DIR;
	game_path_ = game_path ? game_path : "";
	hw_ = hw;
	opt_ = RaMenuOptions();
	opt_.popups = popups_default;
	if (mkdir(dir_.c_str(), 0755) != 0 && errno != EEXIST)
		ra_menu_log("cannot create %s (%s)", dir_.c_str(), strerror(errno));
	// leftovers of an earlier run must not be taken for this one's
	std::string req = dir_ + "/request.json", chg = dir_ + "/options_changed.json";
	if (unlink(req.c_str()) == 0)
		ra_menu_log("removed a stale %s", req.c_str());
	if (unlink(chg.c_str()) == 0)
		ra_menu_log("removed a stale %s", chg.c_str());

	std::string text, path = dir_ + "/options.json";
	if (!ra_menu_read_file(path.c_str(), &text, 65536))
		ra_menu_log("no %s: options from the registers", path.c_str());
	else if (!ra_menu_parse_options(text.data(), text.size(), &opt_))
		ra_menu_log("%s is not valid JSON: options from the registers", path.c_str());
	else
		ra_menu_log("options.json: volume %s, vfilter %s, field %s, popups %s",
			opt_.have_volume ? (opt_.mute ? "mute" : std::to_string(opt_.vol_att).c_str()) : "-",
			opt_.have_vfilter ? (opt_.vfilter == RA_VF_OFF ? "off" : opt_.vfilter == RA_VF_LIGHT ? "light" : "smooth") : "-",
			opt_.have_field ? (opt_.field_correct ? "correct" : "original") : "-",
			opt_.have_popups ? (opt_.popups ? "on" : "off") : "-");
	games_loaded_ = games_ok_ = false;
	games_ = RaGameList();
	current_idx_ = -1;
	page_ = P_MAIN;
}

void RaMenu::load_games()
{
	if (games_loaded_)
		return;
	games_loaded_ = true;
	std::string text, path = dir_ + "/games.json";
	if (!ra_menu_read_file(path.c_str(), &text, GAMES_MAX_BYTES)) {
		ra_menu_log("no game list (%s missing or too big)", path.c_str());
		return;
	}
	if (!ra_menu_parse_games(text.data(), text.size(), &games_)) {
		ra_menu_log("no game list (%s is not a valid game list)", path.c_str());
		return;
	}
	games_ok_ = true;
	const std::string& cur = games_.current.empty() ? game_path_ : games_.current;
	current_idx_ = -1;
	for (size_t i = 0; i < games_.games.size() && current_idx_ < 0; i++)
		if (games_.games[i].path == cur || games_.games[i].path == game_path_)
			current_idx_ = (int)i;
	ra_menu_log("game list: %zu games, current %s", games_.games.size(),
		current_idx_ >= 0 ? games_.games[current_idx_].name.c_str() : "(not in the list)");
}

void RaMenu::read_registers()
{
	uint32_t v = 0;
	vol_ok_ = hw_ && hw_->reg_read && hw_->reg_write && hw_->reg_read(RA_REG_AUDIO_VOL, &v);
	if (vol_ok_) {
		// the register is the truth: the launcher's Start+X / Start+Y and
		// volume keys change it while the game runs
		int att = (int)(v & 0xF);
		opt_.vol_att = att > 7 ? 7 : att;
		opt_.mute = (v & 0x10) != 0;
		if (opt_.mute)
			opt_.vol_att = 0;
	}
	crt_ok_ = hw_ && hw_->reg_read && hw_->reg_write && hw_->reg_read(RA_REG_CRT_CTRL, &v);
	if (crt_ok_) {
		// builds without CRT_CTRL read back stale data: options.json first
		if (!opt_.have_vfilter) {
			int vf = (int)((v >> 1) & 3);
			opt_.vfilter = vf > 2 ? RA_VF_SMOOTH : vf;
		}
		if (!opt_.have_field)
			opt_.field_correct = (v & 1) != 0;
	}
}

void RaMenu::open(int ra_state)
{
	ra_state_ = ra_state;
	load_games();
	read_registers();
	page_ = P_MAIN;
	main_sel_ = M_RESUME;
	error_.clear();
}

bool RaMenu::main_enabled(int item) const
{
	if (item == M_ACHIEVEMENTS)
		return ra_state_ == RA_STATE_READY;
	return item >= 0 && item < M_COUNT;
}

const char* RaMenu::main_reason(int item) const
{
	if (item != M_ACHIEVEMENTS)
		return "";
	switch (ra_state_) {
	case RA_STATE_READY: return "";
	case RA_STATE_STARTING: return "still loading";
	case RA_STATE_NOT_RECOGNIZED: return "game not recognized";
	case RA_STATE_LOGIN_FAILED: return "login failed";
	case RA_STATE_FAILED: return "not available";
	default: return "RetroAchievements off";
	}
}

bool RaMenu::opt_enabled(int row) const
{
	switch (row) {
	case O_VOLUME: return vol_ok_;
	case O_VFILTER:
	case O_FIELD: return crt_ok_;
	case O_POPUPS: return ra_state_ != RA_STATE_OFF;
	default: return false;
	}
}

const char* RaMenu::opt_reason(int row) const
{
	if (opt_enabled(row))
		return "";
	return row == O_POPUPS ? "RetroAchievements off" : "not available";
}

static const char* const VFILTER_NAME[3] = { "Off", "Light (2-tap)", "Smooth (3-tap)" };

// volume as one index: 0 = mute, 1..8 = -42 dB .. 0 dB
static int vol_index(const RaMenuOptions& o) { return o.mute ? 0 : 8 - o.vol_att; }

void RaMenu::opt_value(int row, char* buf, size_t n, bool* can_left, bool* can_right) const
{
	*can_left = *can_right = true;
	switch (row) {
	case O_VOLUME: {
		int i = vol_index(opt_);
		*can_left = i > 0;
		*can_right = i < 8;
		if (opt_.mute)
			snprintf(buf, n, "Mute");
		else if (!opt_.vol_att)
			snprintf(buf, n, "0 dB (max)");
		else
			snprintf(buf, n, "-%d dB", 6 * opt_.vol_att);
		break;
	}
	case O_VFILTER:
		*can_left = opt_.vfilter > 0;
		*can_right = opt_.vfilter < 2;
		snprintf(buf, n, "%s", VFILTER_NAME[opt_.vfilter >= 0 && opt_.vfilter <= 2 ? opt_.vfilter : 0]);
		break;
	case O_FIELD:
		*can_left = !opt_.field_correct;
		*can_right = opt_.field_correct;
		snprintf(buf, n, "%s", opt_.field_correct ? "Correct" : "Original");
		break;
	case O_POPUPS:
		*can_left = opt_.popups;
		*can_right = !opt_.popups;
		snprintf(buf, n, "%s", opt_.popups ? "On" : "Off");
		break;
	default:
		buf[0] = '\0';
	}
}

void RaMenu::write_options_changed()
{
	std::string path = dir_ + "/options_changed.json";
	if (!ra_menu_write_atomic(path.c_str(), ra_menu_options_json(opt_)))
		ra_menu_log("could not write %s (%s)", path.c_str(), strerror(errno));
}

// dir -1 / +1; wrap: A cycles the choices (not the volume)
bool RaMenu::opt_change(int row, int dir, bool wrap)
{
	if (!opt_enabled(row))
		return false;
	switch (row) {
	case O_VOLUME: {
		int i = vol_index(opt_) + dir;
		if (i < 0 || i > 8)
			return false;
		opt_.mute = i == 0;
		opt_.vol_att = i == 0 ? 0 : 8 - i;
		opt_.have_volume = true;
		uint32_t v = ra_audio_vol_value(opt_.vol_att, opt_.mute);
		bool ok = hw_->reg_write(RA_REG_AUDIO_VOL, v);
		ra_menu_log("volume %s (AUDIO_VOL 0x%02x)%s", opt_.mute ? "mute" : std::to_string(opt_.vol_att).c_str(),
			v, ok ? "" : " - write failed");
		break;
	}
	case O_VFILTER:
	case O_FIELD: {
		if (row == O_VFILTER) {
			int vf = opt_.vfilter + dir;
			if (vf < 0 || vf > 2) {
				if (!wrap)
					return false;
				vf = vf < 0 ? 2 : 0;
			}
			opt_.vfilter = vf;
			opt_.have_vfilter = true;
		} else {
			bool c = dir > 0 ? false : true;   // list order: Correct, Original
			if (c == opt_.field_correct) {
				if (!wrap)
					return false;
				c = !c;
			}
			opt_.field_correct = c;
			opt_.have_field = true;
		}
		uint32_t cur = 0;
		if (!hw_->reg_read(RA_REG_CRT_CTRL, &cur)) {
			ra_menu_log("CRT_CTRL not readable, not changed");
			break;
		}
		uint32_t nv = row == O_VFILTER ? ra_crt_ctrl_set_vfilter(cur, opt_.vfilter)
		                               : ra_crt_ctrl_set_field(cur, opt_.field_correct);
		bool ok = hw_->reg_write(RA_REG_CRT_CTRL, nv);
		ra_menu_log("%s %s: CRT_CTRL 0x%02x -> 0x%02x%s", row == O_VFILTER ? "vertical filter" : "field order",
			row == O_VFILTER ? VFILTER_NAME[opt_.vfilter] : (opt_.field_correct ? "correct" : "original"),
			cur & 0xFF, nv, ok ? "" : " - write failed");
		break;
	}
	case O_POPUPS: {
		bool p = dir > 0;   // Left = Off, Right = On
		if (p == opt_.popups) {
			if (!wrap)
				return false;
			p = !p;
		}
		opt_.popups = p;
		opt_.have_popups = true;
		ra_menu_log("achievement popups %s", p ? "on" : "off");
		break;
	}
	default:
		return false;
	}
	write_options_changed();
	return true;
}

void RaMenu::game_view_clamp()
{
	gv_.per_page = (216 - 4 - 38) / 12;
	ra_list_nav(&gv_, (int)games_.games.size(), -1);   // no move, just clamp
}

void RaMenu::letter_jump(int dir)
{
	const std::vector<RaGame>& g = games_.games;
	int n = (int)g.size();
	if (n <= 0)
		return;
	int sel = gv_.sel < 0 ? 0 : (gv_.sel >= n ? n - 1 : gv_.sel);
	char l = g[sel].letter;
	int target = sel;
	if (dir > 0) {
		int i = sel;
		while (i < n && g[i].letter == l)
			i++;
		target = i < n ? i : n - 1;
	} else {
		int s = sel;
		while (s > 0 && g[s - 1].letter == l)
			s--;
		if (s < sel || s == 0) {
			target = s;
		} else {
			char p = g[s - 1].letter;
			int t = s - 1;
			while (t > 0 && g[t - 1].letter == p)
				t--;
			target = t;
		}
	}
	gv_.sel = target;
	gv_.first = target;   // the letter's first game on top of the page
	game_view_clamp();
}

int RaMenu::do_confirm()
{
	if (confirm_sel_ != 1) {
		page_ = confirm_load_ ? P_GAMES : P_MAIN;
		error_.clear();
		return RA_MACT_REDRAW;
	}
	std::string path = dir_ + "/request.json";
	if (confirm_load_) {
		const RaGame& g = games_.games[gv_.sel];
		if (!ra_menu_write_atomic(path.c_str(), ra_menu_request_json("load", g.path.c_str()))) {
			ra_menu_log("could not write %s (%s)", path.c_str(), strerror(errno));
			error_ = "Could not write the request file";
			return RA_MACT_REDRAW;
		}
		req_action_ = "load";
		req_path_ = g.path;
		ra_menu_log("request: load %s", g.path.c_str());
	} else {
		// exiting works without the file too (the launcher's default)
		if (!ra_menu_write_atomic(path.c_str(), ra_menu_request_json("exit", NULL)))
			ra_menu_log("could not write %s (%s), exiting anyway", path.c_str(), strerror(errno));
		else
			ra_menu_log("request: exit");
		req_action_ = "exit";
		req_path_.clear();
	}
	page_ = P_EXITING;
	return RA_MACT_EXIT;
}

int RaMenu::input(uint32_t pressed, uint32_t held)
{
	switch (page_) {
	case P_MAIN: {
		if (pressed & RA_MENU_B)
			return RA_MACT_RESUME;
		int d = (pressed & RA_MENU_DOWN) ? 1 : (pressed & RA_MENU_UP) ? -1 : 0;
		if (d) {
			int s = main_sel_;
			for (int i = 0; i < M_COUNT; i++) {
				s = (s + d + M_COUNT) % M_COUNT;
				if (main_enabled(s))
					break;
			}
			if (s == main_sel_)
				return RA_MACT_NONE;
			main_sel_ = s;
			return RA_MACT_REDRAW;
		}
		if (!(pressed & RA_MENU_A) || !main_enabled(main_sel_))
			return RA_MACT_NONE;
		switch (main_sel_) {
		case M_RESUME:
			return RA_MACT_RESUME;
		case M_LOAD:
			page_ = P_GAMES;
			gv_ = RaListView();
			gv_.sel = gv_.first = current_idx_ >= 0 ? current_idx_ : 0;
			game_view_clamp();
			if (current_idx_ >= 0) {
				// the running game in the middle of the page when possible
				gv_.first = current_idx_ - gv_.per_page / 2;
				game_view_clamp();
			}
			return RA_MACT_REDRAW;
		case M_ACHIEVEMENTS:
			return RA_MACT_ACHIEVEMENTS;
		case M_OPTIONS:
			page_ = P_OPTIONS;
			opt_sel_ = O_VOLUME;
			for (int i = 0; i < O_COUNT && !opt_enabled(opt_sel_); i++)
				opt_sel_ = (opt_sel_ + 1) % O_COUNT;
			return RA_MACT_REDRAW;
		case M_EXIT:
			page_ = P_CONFIRM;
			confirm_load_ = false;
			confirm_sel_ = 0;
			error_.clear();
			return RA_MACT_REDRAW;
		}
		return RA_MACT_NONE;
	}
	case P_GAMES: {
		if (pressed & RA_MENU_B) {
			page_ = P_MAIN;
			return RA_MACT_REDRAW;
		}
		int n = games_ok_ ? (int)games_.games.size() : 0;
		if (!n)
			return RA_MACT_NONE;
		RaListView v = gv_;
		if (pressed & RA_MENU_UP)
			ra_list_nav(&gv_, n, RA_NAV_UP);
		if (pressed & RA_MENU_DOWN)
			ra_list_nav(&gv_, n, RA_NAV_DOWN);
		if (pressed & (RA_MENU_LEFT | RA_MENU_L))
			ra_list_nav(&gv_, n, RA_NAV_PGUP);
		if (pressed & (RA_MENU_RIGHT | RA_MENU_R))
			ra_list_nav(&gv_, n, RA_NAV_PGDN);
		// X / Y: previous / next first letter (not with Start held: the
		// launcher's Start+X / Start+Y volume hotkeys)
		if (!(held & RA_MENU_START)) {
			if (pressed & RA_MENU_X)
				letter_jump(-1);
			if (pressed & RA_MENU_Y)
				letter_jump(1);
		}
		if (pressed & RA_MENU_A) {
			page_ = P_CONFIRM;
			confirm_load_ = true;
			confirm_sel_ = 0;
			error_.clear();
			return RA_MACT_REDRAW;
		}
		return (v.sel != gv_.sel || v.first != gv_.first) ? RA_MACT_REDRAW : RA_MACT_NONE;
	}
	case P_OPTIONS: {
		if (pressed & RA_MENU_B) {
			page_ = P_MAIN;
			return RA_MACT_REDRAW;
		}
		int d = (pressed & RA_MENU_DOWN) ? 1 : (pressed & RA_MENU_UP) ? -1 : 0;
		if (d) {
			int s = opt_sel_;
			for (int i = 0; i < O_COUNT; i++) {
				s = (s + d + O_COUNT) % O_COUNT;
				if (opt_enabled(s))
					break;
			}
			if (s == opt_sel_ || !opt_enabled(s))
				return RA_MACT_NONE;
			opt_sel_ = s;
			return RA_MACT_REDRAW;
		}
		bool changed = false;
		if (pressed & RA_MENU_LEFT)
			changed |= opt_change(opt_sel_, -1, false);
		if (pressed & RA_MENU_RIGHT)
			changed |= opt_change(opt_sel_, 1, false);
		if (pressed & RA_MENU_A)
			changed |= opt_change(opt_sel_, 1, opt_sel_ != O_VOLUME);
		return changed ? RA_MACT_REDRAW : RA_MACT_NONE;
	}
	case P_CONFIRM: {
		if (pressed & RA_MENU_B) {
			confirm_sel_ = 0;
			return do_confirm();
		}
		if (pressed & (RA_MENU_UP | RA_MENU_DOWN | RA_MENU_LEFT | RA_MENU_RIGHT)) {
			confirm_sel_ ^= 1;
			return RA_MACT_REDRAW;
		}
		if (pressed & RA_MENU_A)
			return do_confirm();
		return RA_MACT_NONE;
	}
	case P_EXITING:
	default:
		return RA_MACT_NONE;
	}
}

// ---------------------------------------------------------------------------
// rendering: same layout grid as the achievement list (ra_osd.cpp): title
// at row 12, subtitle 22, rule 32, footer rule 216, help line 219 - all in
// 240p rows, clear of the CRT overscan
// ---------------------------------------------------------------------------

static const int MENU_TOP = 12, MENU_ROWS_Y = 48, MENU_ROW_H = 20;
static const int GAME_ROWS_Y = 38, GAME_ROW_H = 12;
static const int FOOT_RULE = 216, FOOT_Y = 219;

static void text_right(RaCanvas* c, int right_x, int y, const char* s, uint8_t color)
{
	ra_canvas_text(c, right_x - 8 * (int)strlen(s), y, s, color, -1);
}

static void text_center(RaCanvas* c, int y, const char* s, uint8_t color)
{
	ra_canvas_text(c, (c->w - 8 * (int)strlen(s)) / 2, y, s, color, -1);
}

static void header(RaCanvas* c, int mx, int cols, const char* title, const char* sub, uint8_t sub_color)
{
	char buf[256];
	ra_to_ascii(title, buf, sizeof(buf));
	char fit[256];
	ra_fit(buf, cols, fit, sizeof(fit));
	ra_canvas_text(c, mx, MENU_TOP, fit, RA_C_GOLD, cols);
	if (sub && sub[0]) {
		ra_fit(sub, cols, fit, sizeof(fit));
		ra_canvas_text(c, mx, MENU_TOP + 10, fit, sub_color, cols);
	}
	ra_canvas_fill(c, mx, MENU_TOP + 20, c->w - 2 * mx, 1, RA_C_SEP);
}

static void footer(RaCanvas* c, int mx, int cols, const char* help, const char* pos)
{
	ra_canvas_fill(c, mx, FOOT_RULE, c->w - 2 * mx, 1, RA_C_SEP);
	char buf[160];
	ra_fit(help, cols - (int)strlen(pos) - 1, buf, sizeof(buf));
	ra_canvas_text(c, mx, FOOT_Y, buf, RA_C_DIM, -1);
	text_right(c, c->w - mx, FOOT_Y, pos, RA_C_DIM);
}

static void hilite(RaCanvas* c, int mx, int y, int h)
{
	ra_canvas_fill(c, mx - 4, y, c->w - 2 * mx + 8, h, RA_C_HILITE);
}

std::string RaMenu::title() const
{
	if (games_ok_ && current_idx_ >= 0)
		return games_.games[current_idx_].name;
	if (!game_path_.empty())
		return path_basename(game_path_, true);
	return "DreamSTer";
}

void RaMenu::render_main(RaCanvas* c, int mx, int cols) const
{
	static const char* const LABEL[M_COUNT] = { "Resume", "Load Game", "Achievements", "Options", "Exit Emulator" };
	header(c, mx, cols, title().c_str(), "Game paused", RA_C_TEXT);
	for (int i = 0; i < M_COUNT; i++) {
		int y = MENU_ROWS_Y + i * MENU_ROW_H;
		bool en = main_enabled(i);
		if (i == main_sel_)
			hilite(c, mx, y - 6, MENU_ROW_H);
		ra_canvas_text(c, mx + 8, y, LABEL[i], en ? RA_C_TEXT : RA_C_DIM, -1);
		const char* why = main_reason(i);
		char extra[48] = "";
		if (why[0])
			snprintf(extra, sizeof(extra), "%s", why);
		else if (i == M_LOAD && games_ok_)
			snprintf(extra, sizeof(extra), "%zu game%s", games_.games.size(), games_.games.size() == 1 ? "" : "s");
		if (extra[0] && (int)(strlen(LABEL[i]) + strlen(extra)) + 3 <= cols)
			text_right(c, c->w - mx - 8, y, extra, RA_C_DIM);
	}
	footer(c, mx, cols, cols >= 60 ? "Up/Down: select   A: choose   B: resume" : "A:choose B:resume", "");
}

void RaMenu::render_games(RaCanvas* c, int mx, int cols) const
{
	int n = games_ok_ ? (int)games_.games.size() : 0;
	char sub[64] = "";
	if (games_ok_)
		snprintf(sub, sizeof(sub), "%d game%s", n, n == 1 ? "" : "s");
	header(c, mx, cols, "Load Game", sub, RA_C_TEXT);
	if (!n) {
		text_center(c, 116, games_ok_ ? "No games in the list" : "No game list", RA_C_DIM);
		footer(c, mx, cols, cols >= 60 ? "B: back" : "B:back", "");
		return;
	}
	int pp = gv_.per_page > 0 ? gv_.per_page : 1;
	int name_cols = cols - 2;
	for (int k = 0; k < pp; k++) {
		int idx = gv_.first + k;
		if (idx < 0 || idx >= n)
			break;
		const RaGame& g = games_.games[idx];
		int y = GAME_ROWS_Y + k * GAME_ROW_H;
		if (idx == gv_.sel)
			hilite(c, mx, y - 2, GAME_ROW_H);
		bool cur = idx == current_idx_;
		if (cur)
			ra_canvas_glyph(c, mx, y, RA_GLYPH_RIGHT, RA_C_GOLD);
		char a[256], fit[256];
		ra_to_ascii(g.name.c_str(), a, sizeof(a));
		ra_fit(a, name_cols, fit, sizeof(fit));
		ra_canvas_text(c, mx + 16, y, fit, cur ? RA_C_GOLD : RA_C_TEXT, name_cols);
	}
	char pos[40];
	snprintf(pos, sizeof(pos), "%s%s %d/%d", gv_.first > 0 ? "\x04" : " ", gv_.first + pp < n ? "\x05" : " ",
		gv_.sel + 1, n);
	footer(c, mx, cols, cols >= 60 ? "Up/Down: select  L/R: page  X/Y: letter  A: load  B: back"
	                               : "L/R:page A:load B:back", pos);
}

void RaMenu::render_options(RaCanvas* c, int mx, int cols) const
{
	static const char* const LABEL[O_COUNT] = { "Volume", "Vertical filter", "480i field order", "Achievement popups" };
	static const char* const HELP[O_COUNT] = {
		"Audio output level, in 6 dB steps.",
		"Blends the lines of 480-line images on a 15 kHz CRT: less 480i flicker, softer picture.",
		"Which field shows the even lines in 480i. Original = the first CRT builds' order.",
		"Show a popup when an achievement unlocks.",
	};
	header(c, mx, cols, "Options", "Changes apply immediately", RA_C_DIM);
	for (int i = 0; i < O_COUNT; i++) {
		int y = MENU_ROWS_Y + i * MENU_ROW_H;
		bool en = opt_enabled(i);
		if (i == opt_sel_ && en)
			hilite(c, mx, y - 6, MENU_ROW_H);
		ra_canvas_text(c, mx + 8, y, LABEL[i], en ? RA_C_TEXT : RA_C_DIM, -1);
		int right = c->w - mx - 8;
		if (!en) {
			text_right(c, right, y, opt_reason(i), RA_C_DIM);
			continue;
		}
		char val[32];
		bool cl, cr;
		opt_value(i, val, sizeof(val), &cl, &cr);
		// "< value >": the arrows dim at the ends of the range
		int len = (int)strlen(val);
		int x = right - 8 * (len + 4);
		ra_canvas_glyph(c, x, y, '<', cl ? RA_C_GOLD : RA_C_SEP);
		ra_canvas_text(c, x + 16, y, val, RA_C_TEXT, -1);
		ra_canvas_glyph(c, right - 8, y, '>', cr ? RA_C_GOLD : RA_C_SEP);
	}
	// what the selected option does
	if (opt_sel_ >= 0 && opt_sel_ < O_COUNT && opt_enabled(opt_sel_)) {
		char buf[512];
		int n = ra_wrap(HELP[opt_sel_], cols, 3, buf, sizeof(buf));
		char* line = buf;
		int y = MENU_ROWS_Y + O_COUNT * MENU_ROW_H + 8;
		ra_canvas_fill(c, mx, y - 6, c->w - 2 * mx, 1, RA_C_SEP);
		y += 4;
		for (int i = 0; i < n; i++) {
			char* nl = strchr(line, '\n');
			if (nl)
				*nl = '\0';
			ra_canvas_text(c, mx, y, line, RA_C_DIM, cols);
			y += 10;
			line = nl ? nl + 1 : line + strlen(line);
		}
	}
	footer(c, mx, cols, cols >= 60 ? "Up/Down: select   Left/Right: change   B: back" : "U/D:select L/R:change B:back", "");
}

void RaMenu::render_confirm(RaCanvas* c, int mx, int cols) const
{
	char buf[512], a[256], fit[256];
	int y = MENU_ROWS_Y;
	if (confirm_load_) {
		header(c, mx, cols, "Load Game", "", RA_C_TEXT);
		ra_canvas_text(c, mx + 8, y, "Load this game?", RA_C_TEXT, -1);
		ra_to_ascii(games_.games[gv_.sel].name.c_str(), a, sizeof(a));
		ra_fit(a, cols - 1, fit, sizeof(fit));
		ra_canvas_text(c, mx + 8, y + 14, fit, RA_C_GOLD, cols - 1);
		y += 32;
		snprintf(buf, sizeof(buf), "The running game is closed: progress since the last save is lost.");
	} else {
		header(c, mx, cols, "Exit Emulator", "", RA_C_TEXT);
		ra_canvas_text(c, mx + 8, y, "Quit the game?", RA_C_TEXT, -1);
		y += 18;
		snprintf(buf, sizeof(buf), "The emulator closes: progress since the last save is lost.");
	}
	char wrapped[512];
	int n = ra_wrap(buf, cols - 1, 2, wrapped, sizeof(wrapped));
	char* line = wrapped;
	for (int i = 0; i < n; i++) {
		char* nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		ra_canvas_text(c, mx + 8, y, line, RA_C_DIM, cols - 1);
		y += 10;
		line = nl ? nl + 1 : line + strlen(line);
	}
	static const char* const CHOICE[2] = { "No", "Yes" };
	int cy = y + 22;
	for (int i = 0; i < 2; i++) {
		int yy = cy + i * MENU_ROW_H;
		if (i == confirm_sel_)
			hilite(c, mx, yy - 6, MENU_ROW_H);
		ra_canvas_text(c, mx + 8, yy, CHOICE[i], RA_C_TEXT, -1);
	}
	if (!error_.empty()) {
		ra_fit(error_.c_str(), cols, fit, sizeof(fit));
		ra_canvas_text(c, mx, cy + 2 * MENU_ROW_H + 6, fit, RA_C_RED, cols);
	}
	footer(c, mx, cols, cols >= 60 ? "Up/Down: select   A: confirm   B: cancel" : "A:confirm B:cancel", "");
}

void RaMenu::render_exiting(RaCanvas* c, int mx, int cols) const
{
	bool load = req_action_ == "load";
	header(c, mx, cols, load ? "Load Game" : "Exit Emulator", "", RA_C_TEXT);
	text_center(c, 108, load ? "Loading..." : "Exiting...", RA_C_GOLD);
	if (load) {
		char a[256], fit[256];
		std::string name = games_ok_ && gv_.sel >= 0 && gv_.sel < (int)games_.games.size()
			? games_.games[gv_.sel].name : path_basename(req_path_, true);
		ra_to_ascii(name.c_str(), a, sizeof(a));
		ra_fit(a, cols, fit, sizeof(fit));
		text_center(c, 122, fit, RA_C_TEXT);
	}
	text_center(c, load ? 136 : 122, cols >= 40 ? "Saving and closing the emulator" : "Saving...", RA_C_DIM);
}

bool RaMenu::render(RaCanvas* c) const
{
	if (!c->px || c->h != 240)
		return false;
	int mx = c->w >= 640 ? 32 : 12;
	int cols = (c->w - 2 * mx) / 8;
	ra_canvas_fill(c, 0, 0, c->w, c->h, RA_C_BG);
	switch (page_) {
	case P_MAIN: render_main(c, mx, cols); break;
	case P_GAMES: render_games(c, mx, cols); break;
	case P_OPTIONS: render_options(c, mx, cols); break;
	case P_CONFIRM: render_confirm(c, mx, cols); break;
	case P_EXITING: render_exiting(c, mx, cols); break;
	}
	return true;
}
