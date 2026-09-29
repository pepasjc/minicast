// ra_menu_test -- host tests for the in-game menu (ra_menu + ra_json):
//   - JSON reader: types, nesting, escapes (\uXXXX, surrogate pairs),
//     rejects of malformed input, depth limit; quote -> parse round trips
//   - options.json parse / options_changed.json write
//   - CRT_CTRL read-modify-write over every register value
//   - games.json parse, first-letter groups
//   - protocol files: atomic write, request.json, stale files removed
//   - the menu state machine against fake registers: pages, navigation,
//     option changes (register writes + options_changed.json), Load Game /
//     Exit confirmation -> request.json, missing / invalid launcher files
//   - rendering of every page at 640 and 320 wide (smoke)
//
//   ra_menu_test

#include "ra_menu.h"
#include "ra_json.h"
#include "ra_overlay.h"   // RA_MENU_* buttons
#include "ra_dc.h"        // RA_STATE_*

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static bool parse(const char* s, RaJson* j) { return ra_json_parse(s, strlen(s), j); }

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

static void test_json()
{
	RaJson j;
	CHECK(parse("{\"a\":1,\"b\":[true,false,null,-2.5e3,\"x\"],\"c\":{\"d\":\"e\"}}", &j));
	CHECK(j.type == RaJson::OBJ && j.keys.size() == 3);
	CHECK(j.get("a") && j.get("a")->type == RaJson::NUM && j.get("a")->num == 1);
	const RaJson* b = j.get("b");
	CHECK(b && b->type == RaJson::ARR && b->items.size() == 5);
	if (b && b->items.size() == 5) {
		CHECK(b->items[0].type == RaJson::BOOL && b->items[0].b);
		CHECK(b->items[1].type == RaJson::BOOL && !b->items[1].b);
		CHECK(b->items[2].type == RaJson::NUL);
		CHECK(b->items[3].type == RaJson::NUM && b->items[3].num == -2500);
		CHECK(b->items[4].type == RaJson::STR && b->items[4].str == "x");
	}
	CHECK(j.get("c") && j.get("c")->get_str("d") && !strcmp(j.get("c")->get_str("d"), "e"));
	CHECK(!j.get("zz") && !j.get_str("a"));

	// whitespace, BOM, empty containers, trailing NULs
	CHECK(parse("\xEF\xBB\xBF \n\t{ } \r\n", &j) && j.type == RaJson::OBJ && j.keys.empty());
	CHECK(ra_json_parse("[]\0\0", 4, &j) && j.type == RaJson::ARR && j.items.empty());
	// a repeated key: the last one wins
	CHECK(parse("{\"k\":\"1\",\"k\":\"2\"}", &j) && !strcmp(j.get_str("k"), "2"));

	// escapes
	CHECK(parse("\"q\\\"b\\\\s\\/n\\nt\\tr\\rb\\bf\\f\"", &j) && j.str == "q\"b\\s/n\nt\tr\rb\bf\f");
	CHECK(parse("\"caf\\u00e9 \\u00C9\"", &j) && j.str == "caf\xC3\xA9 \xC3\x89");
	CHECK(parse("\"\\u30bd\"", &j) && j.str == "\xE3\x82\xBD");
	CHECK(parse("\"\\ud83d\\ude00!\"", &j) && j.str == "\xF0\x9F\x98\x80!");
	CHECK(parse("\"\\ud83dx\"", &j) && j.str == "?x");          // lone high surrogate
	CHECK(parse("\"\\ude00\"", &j) && j.str == "?");            // lone low surrogate
	CHECK(parse("\"\\ud83d\\u0041\"", &j) && j.str == "?A");    // high + not a low one
	CHECK(parse("\"raw \xC3\xA9 utf8\"", &j) && j.str == "raw \xC3\xA9 utf8");

	// malformed
	const char* bad[] = {
		"", "{", "}", "{\"a\":}", "{\"a\" 1}", "{a:1}", "{\"a\":1,}", "[1,]", "[1 2]",
		"\"unterminated", "\"bad \\x escape\"", "\"\\u12\"", "\"\\uzzzz\"", "tru", "nul", "01x",
		"-", "1.2.3", "{\"a\":1} x", "\"ctl \x01 char\"", "[\"a\"", "{\"a\":[1,{\"b\":2]}", "+1",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		bool ok = parse(bad[i], &j);
		if (ok)
			printf("  json: accepted bad input #%zu: %s\n", i, bad[i]);
		CHECK(!ok);
		CHECK(j.type == RaJson::NUL);
	}

	// depth limit (32)
	std::string deep(32, '['), deeper(33, '[');
	deep += std::string(32, ']');
	deeper += std::string(33, ']');
	CHECK(parse(deep.c_str(), &j));
	CHECK(!parse(deeper.c_str(), &j));

	// quote -> parse round trips
	const char* samples[] = {
		"", "plain", "with \"quotes\" and \\backslash\\", "line\nbreak\ttab\r",
		"ctl \x01\x02\x1f end", "/media/fat/games/Dreamcast/Sonic Adventure (USA)/disc.gdi",
		"Caf\xC3\xA9 \xE3\x82\xBD\xE3\x83\x8B\xE3\x83\x83\xE3\x82\xAF \xF0\x9F\x98\x80",
	};
	for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		std::string q;
		ra_json_quote(&q, samples[i]);
		CHECK(parse(q.c_str(), &j) && j.type == RaJson::STR && j.str == samples[i]);
	}
	std::string q;
	ra_json_quote(&q, "a\x01" "b");
	CHECK(q == "\"a\\u0001b\"");
	printf("json: ok\n");
}

// ---------------------------------------------------------------------------
// options + CRT_CTRL
// ---------------------------------------------------------------------------

static void test_options()
{
	RaMenuOptions o;
	const char* full = "{\"volume\":\"3\",\"vfilter\":\"light\",\"field\":\"original\",\"popups\":\"off\"}";
	CHECK(ra_menu_parse_options(full, strlen(full), &o));
	CHECK(o.have_volume && o.vol_att == 3 && !o.mute);
	CHECK(o.have_vfilter && o.vfilter == RA_VF_LIGHT);
	CHECK(o.have_field && !o.field_correct);
	CHECK(o.have_popups && !o.popups);
	std::string w = ra_menu_options_json(o);
	CHECK(w == "{\"volume\":\"3\",\"vfilter\":\"light\",\"field\":\"original\",\"popups\":\"off\"}\n");
	RaMenuOptions back;
	CHECK(ra_menu_parse_options(w.data(), w.size(), &back));
	CHECK(back.vol_att == 3 && !back.mute && back.vfilter == RA_VF_LIGHT && !back.field_correct && !back.popups);

	RaMenuOptions m;
	const char* mute = "{\"volume\":\"mute\",\"vfilter\":\"smooth\",\"field\":\"correct\",\"popups\":\"on\"}";
	CHECK(ra_menu_parse_options(mute, strlen(mute), &m));
	CHECK(m.mute && m.vfilter == RA_VF_SMOOTH && m.field_correct && m.popups);
	CHECK(ra_menu_options_json(m) == std::string(mute) + "\n");

	// partial / unknown values: only the good keys count
	RaMenuOptions p;
	const char* part = "{\"volume\":\"9\",\"vfilter\":\"auto\",\"field\":\"correct\",\"extra\":[1,2]}";
	CHECK(ra_menu_parse_options(part, strlen(part), &p));
	CHECK(!p.have_volume && !p.have_vfilter && p.have_field && !p.have_popups);
	RaMenuOptions n;
	const char* num = "{\"volume\":5,\"popups\":true}";
	CHECK(ra_menu_parse_options(num, strlen(num), &n) && n.have_volume && n.vol_att == 5 && n.have_popups && n.popups);
	// not an object / not JSON
	RaMenuOptions x;
	CHECK(!ra_menu_parse_options("[1]", 3, &x));
	CHECK(!ra_menu_parse_options("volume=3", 8, &x));
	CHECK(!x.have_volume && !x.have_vfilter);

	// AUDIO_VOL encoding (MiSTer: [3:0] attenuation, [4] mute)
	CHECK(ra_audio_vol_value(0, false) == 0x00);
	CHECK(ra_audio_vol_value(7, false) == 0x07);
	CHECK(ra_audio_vol_value(9, false) == 0x07);
	CHECK(ra_audio_vol_value(3, true) == 0x10);

	// CRT_CTRL read-modify-write: every 6-bit value, plus junk in the
	// reserved bits (a build without the register reads back stale data)
	int bad = 0;
	for (uint32_t cur = 0; cur < 0x40; cur++) {
		uint32_t junk = cur | 0xA5A5A5C0u;
		for (int vf = 0; vf <= 2; vf++) {
			uint32_t r = ra_crt_ctrl_set_vfilter(junk, vf);
			if (((r >> 1) & 3u) != (uint32_t)vf || (r & 0x39u) != (cur & 0x39u) || (r & ~0x3Fu))
				bad++;
		}
		for (int f = 0; f <= 1; f++) {
			uint32_t r = ra_crt_ctrl_set_field(junk, f != 0);
			if ((r & 1u) != (uint32_t)f || (r & 0x3Eu) != (cur & 0x3Eu) || (r & ~0x3Fu))
				bad++;
		}
	}
	CHECK(bad == 0);
	CHECK(ra_crt_ctrl_set_vfilter(0x19, RA_VF_SMOOTH) == 0x1D);   // field + 240p + YPbPr kept
	CHECK(ra_crt_ctrl_set_vfilter(0x3F, RA_VF_OFF) == 0x39);
	CHECK(ra_crt_ctrl_set_vfilter(0x00, 7) == 0x04);                // clamped to smooth
	CHECK(ra_crt_ctrl_set_field(0x3E, true) == 0x3F);
	CHECK(ra_crt_ctrl_set_field(0x3F, false) == 0x3E);
	printf("options + CRT_CTRL: ok\n");
}

// ---------------------------------------------------------------------------
// games.json + files
// ---------------------------------------------------------------------------

static const char* GAMES_JSON =
	"{\"games\":["
	"{\"name\":\"18 Wheeler: American Pro Trucker (USA)\",\"path\":\"/g/18w.gdi\"},"
	"{\"name\":\"Crazy Taxi (USA)\",\"path\":\"/g/ct.gdi\"},"
	"{\"name\":\"Dead or Alive 2 (USA)\",\"path\":\"/media/fat/games/Dreamcast/DOA2/Dead or Alive 2 (USA).gdi\"},"
	"{\"name\":\"\\u00c9cco the Dolphin\",\"path\":\"/g/ecco.gdi\"},"
	"{\"path\":\"/g/Jet Grind Radio (USA).cue\"},"
	"{\"name\":\"no path\"},"
	"{\"name\":\"Sega Rally 2\",\"path\":\"/g/sr2.gdi\"},"
	"{\"name\":\"Sonic Adventure (USA)\",\"path\":\"/g/sa1.gdi\"},"
	"{\"name\":\"Soulcalibur (USA)\",\"path\":\"/g/sc.gdi\"},"
	"7,"
	"{\"name\":\"Virtua Tennis\",\"path\":\"/g/vt.gdi\"}"
	"],\"current\":\"/media/fat/games/Dreamcast/DOA2/Dead or Alive 2 (USA).gdi\"}";

static void test_games()
{
	RaGameList gl;
	CHECK(ra_menu_parse_games(GAMES_JSON, strlen(GAMES_JSON), &gl));
	CHECK(gl.games.size() == 9);
	CHECK(gl.current == "/media/fat/games/Dreamcast/DOA2/Dead or Alive 2 (USA).gdi");
	if (gl.games.size() == 9) {
		CHECK(gl.games[0].letter == '#');
		CHECK(gl.games[1].letter == 'C');
		CHECK(gl.games[3].name == "\xC3\x89" "cco the Dolphin" && gl.games[3].letter == 'E');
		CHECK(gl.games[4].name == "Jet Grind Radio (USA)" && gl.games[4].letter == 'J');   // name from the path
		CHECK(gl.games[5].name == "Sega Rally 2" && gl.games[5].letter == 'S');
	}
	RaGameList e;
	CHECK(ra_menu_parse_games("{\"games\":[]}", 12, &e) && e.games.empty() && e.current.empty());
	CHECK(!ra_menu_parse_games("{\"games\":{}}", 12, &e));
	CHECK(!ra_menu_parse_games("{\"list\":[]}", 11, &e));
	CHECK(!ra_menu_parse_games("{\"games\":[", 10, &e));
	printf("games: ok\n");
}

static char g_dir[64];

static std::string path_in(const char* name) { return std::string(g_dir) + "/" + name; }

static void put_file(const char* name, const char* text)
{
	CHECK(ra_menu_write_atomic(path_in(name).c_str(), text));
}

static std::string get_file(const char* name)
{
	std::string s;
	ra_menu_read_file(path_in(name).c_str(), &s, 1 << 20);
	return s;
}

static bool exists(const char* name)
{
	struct stat st;
	return stat(path_in(name).c_str(), &st) == 0;
}

static void test_files()
{
	CHECK(ra_menu_write_atomic(path_in("a.json").c_str(), "hello"));
	CHECK(get_file("a.json") == "hello");
	CHECK(ra_menu_write_atomic(path_in("a.json").c_str(), "replaced"));
	CHECK(get_file("a.json") == "replaced");
	CHECK(!exists("a.json.tmp"));
	std::string s;
	CHECK(!ra_menu_read_file(path_in("a.json").c_str(), &s, 4));   // over the limit
	CHECK(!ra_menu_read_file(path_in("missing.json").c_str(), &s, 100));
	CHECK(!ra_menu_write_atomic("/nonexistent-dir/x.json", "x"));
	unlink(path_in("a.json").c_str());

	RaJson j;
	std::string r = ra_menu_request_json("load", "/media/fat/games/Dreamcast/Caf\xC3\xA9 \"x\"/g.gdi");
	CHECK(r.back() == '\n');
	CHECK(ra_json_parse(r.data(), r.size(), &j) && j.keys.size() == 2);
	CHECK(j.get_str("action") && !strcmp(j.get_str("action"), "load"));
	CHECK(j.get_str("path") && !strcmp(j.get_str("path"), "/media/fat/games/Dreamcast/Caf\xC3\xA9 \"x\"/g.gdi"));
	r = ra_menu_request_json("exit", NULL);
	CHECK(r == "{\"action\":\"exit\"}\n");
	printf("files: ok\n");
}

// ---------------------------------------------------------------------------
// the menu against fake registers
// ---------------------------------------------------------------------------

static uint32_t fake_reg[2];
static bool fake_ok[2] = { true, true };
static int fake_writes[2];
static bool fake_read(int reg, uint32_t* v)
{
	if (reg < 0 || reg > 1 || !fake_ok[reg])
		return false;
	*v = fake_reg[reg];
	return true;
}
static bool fake_write(int reg, uint32_t v)
{
	if (reg < 0 || reg > 1 || !fake_ok[reg])
		return false;
	fake_reg[reg] = v;
	fake_writes[reg]++;
	return true;
}
static const RaMenuHw FAKE_HW = { fake_read, fake_write };

static void render_both(RaMenu& m)
{
	RaCanvas a, b;
	ra_canvas_init(&a, 640, 240);
	ra_canvas_init(&b, 320, 240);
	CHECK(m.render(&a));
	CHECK(m.render(&b));
	// some text must be there (BG is palette index 0)
	int text = 0;
	for (int i = 0; i < a.w * a.h; i++)
		text += a.px[i] != RA_C_BG;
	CHECK(text > 200);
	ra_canvas_free(&a);
	ra_canvas_free(&b);
}

static RaJson read_json(const char* name)
{
	RaJson j;
	std::string s = get_file(name);
	ra_json_parse(s.data(), s.size(), &j);
	return j;
}

static void test_menu()
{
	put_file("games.json", GAMES_JSON);
	put_file("options.json", "{\"volume\":\"2\",\"vfilter\":\"smooth\",\"field\":\"correct\",\"popups\":\"on\"}");
	put_file("request.json", "{\"action\":\"exit\"}");            // stale: removed by init
	put_file("options_changed.json", "{\"volume\":\"7\"}");      // stale: removed by init
	fake_reg[RA_REG_AUDIO_VOL] = 0x02;
	fake_reg[RA_REG_CRT_CTRL] = 0x1D;   // field correct, smooth, 240p, YPbPr

	RaMenu m;
	m.init(g_dir, "/media/fat/games/Dreamcast/DOA2/Dead or Alive 2 (USA).gdi", &FAKE_HW, true);
	CHECK(!exists("request.json") && !exists("options_changed.json"));
	CHECK(m.options().have_volume && m.options().vol_att == 2);
	CHECK(m.options().vfilter == RA_VF_SMOOTH && m.options().field_correct && m.options().popups);

	// RetroAchievements off: Achievements can't be picked, popups greyed
	m.open(RA_STATE_OFF);
	CHECK(m.page() == RaMenu::P_MAIN && m.games_ok() && m.games().games.size() == 9);
	render_both(m);
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);   // Load Game
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);   // Options (Achievements skipped)
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW);
	CHECK(m.page() == RaMenu::P_OPTIONS);
	render_both(m);

	// Volume: Right = louder (attenuation 2 -> 1)
	CHECK(m.input(RA_MENU_RIGHT, 0) == RA_MACT_REDRAW);
	CHECK(fake_reg[RA_REG_AUDIO_VOL] == 0x01 && m.options().vol_att == 1);
	RaJson j = read_json("options_changed.json");
	CHECK(j.get_str("volume") && !strcmp(j.get_str("volume"), "1"));
	CHECK(j.get_str("vfilter") && !strcmp(j.get_str("vfilter"), "smooth"));
	CHECK(j.get_str("field") && !strcmp(j.get_str("field"), "correct"));
	CHECK(j.get_str("popups") && !strcmp(j.get_str("popups"), "on"));
	CHECK(m.input(RA_MENU_RIGHT, 0) == RA_MACT_REDRAW);   // 0 dB
	CHECK(fake_reg[RA_REG_AUDIO_VOL] == 0x00);
	int w0 = fake_writes[RA_REG_AUDIO_VOL];
	CHECK(m.input(RA_MENU_RIGHT, 0) == RA_MACT_NONE);     // already max
	CHECK(fake_writes[RA_REG_AUDIO_VOL] == w0);
	for (int i = 0; i < 8; i++)
		m.input(RA_MENU_LEFT, 0);
	CHECK(m.options().mute && fake_reg[RA_REG_AUDIO_VOL] == 0x10);
	j = read_json("options_changed.json");
	CHECK(j.get_str("volume") && !strcmp(j.get_str("volume"), "mute"));
	CHECK(m.input(RA_MENU_LEFT, 0) == RA_MACT_NONE);
	CHECK(m.input(RA_MENU_RIGHT, 0) == RA_MACT_REDRAW);   // -42 dB
	CHECK(fake_reg[RA_REG_AUDIO_VOL] == 0x07);

	// Vertical filter: read-modify-write keeps 240p / YPbPr / field
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);
	CHECK(m.input(RA_MENU_LEFT, 0) == RA_MACT_REDRAW);    // smooth -> light
	CHECK(fake_reg[RA_REG_CRT_CTRL] == 0x1B);
	CHECK(m.input(RA_MENU_LEFT, 0) == RA_MACT_REDRAW);    // off
	CHECK(fake_reg[RA_REG_CRT_CTRL] == 0x19);
	CHECK(m.input(RA_MENU_LEFT, 0) == RA_MACT_NONE);
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW);       // A cycles: light
	CHECK(fake_reg[RA_REG_CRT_CTRL] == 0x1B);
	j = read_json("options_changed.json");
	CHECK(j.get_str("vfilter") && !strcmp(j.get_str("vfilter"), "light"));
	// something else changed CRT_CTRL[5] meanwhile: kept
	fake_reg[RA_REG_CRT_CTRL] |= 0x20;
	// field order
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);
	CHECK(m.input(RA_MENU_RIGHT, 0) == RA_MACT_REDRAW);   // correct -> original
	CHECK(fake_reg[RA_REG_CRT_CTRL] == 0x3A);
	CHECK(!m.options().field_correct);
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW);       // back to correct
	CHECK(fake_reg[RA_REG_CRT_CTRL] == 0x3B);
	// popups row is greyed with RA off: Down wraps to Volume
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);
	int wv = fake_writes[RA_REG_AUDIO_VOL];
	CHECK(m.input(RA_MENU_RIGHT, 0) == RA_MACT_REDRAW);   // that is the volume row
	CHECK(fake_writes[RA_REG_AUDIO_VOL] == wv + 1);
	render_both(m);
	CHECK(m.input(RA_MENU_B, 0) == RA_MACT_REDRAW && m.page() == RaMenu::P_MAIN);

	// Achievements with a game loaded; popups row enabled
	m.open(RA_STATE_READY);
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_ACHIEVEMENTS);
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);   // Options
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW);
	CHECK(m.input(RA_MENU_UP, 0) == RA_MACT_REDRAW);     // wraps to popups
	CHECK(m.input(RA_MENU_LEFT, 0) == RA_MACT_REDRAW);   // off
	CHECK(!m.options().popups);
	j = read_json("options_changed.json");
	CHECK(j.get_str("popups") && !strcmp(j.get_str("popups"), "off"));
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW && m.options().popups);
	render_both(m);
	m.input(RA_MENU_B, 0);

	// volume re-read from the register at every open (launcher hotkeys)
	fake_reg[RA_REG_AUDIO_VOL] = 0x14;
	m.open(RA_STATE_READY);
	CHECK(m.options().mute);
	fake_reg[RA_REG_AUDIO_VOL] = 0x05;
	m.open(RA_STATE_READY);
	CHECK(!m.options().mute && m.options().vol_att == 5);

	// Load Game: opens on the running game, letter jumps, confirm No / Yes
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW && m.page() == RaMenu::P_GAMES);
	render_both(m);
	CHECK(m.input(RA_MENU_Y, 0) == RA_MACT_REDRAW);           // D -> E (Ecco)
	CHECK(m.input(RA_MENU_Y, 0) == RA_MACT_REDRAW);           // -> J
	CHECK(m.input(RA_MENU_Y, 0) == RA_MACT_REDRAW);           // -> S (Sega Rally 2)
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);        // Sonic Adventure
	CHECK(m.input(RA_MENU_X, 0) == RA_MACT_REDRAW);           // start of S
	CHECK(m.input(RA_MENU_X, 0) == RA_MACT_REDRAW);           // J
	CHECK(m.input(RA_MENU_Y, RA_MENU_START) == RA_MACT_NONE); // Start held: launcher volume
	CHECK(m.input(RA_MENU_Y, 0) == RA_MACT_REDRAW);           // S again
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);        // Sonic Adventure
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW && m.page() == RaMenu::P_CONFIRM);
	render_both(m);
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW && m.page() == RaMenu::P_GAMES);   // No
	CHECK(!exists("request.json"));
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW && m.page() == RaMenu::P_CONFIRM);
	CHECK(m.input(RA_MENU_B, 0) == RA_MACT_REDRAW && m.page() == RaMenu::P_GAMES);   // B = No
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW);
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);        // Yes
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_EXIT && m.page() == RaMenu::P_EXITING);
	render_both(m);
	j = read_json("request.json");
	CHECK(j.get_str("action") && !strcmp(j.get_str("action"), "load"));
	CHECK(j.get_str("path") && !strcmp(j.get_str("path"), "/g/sa1.gdi"));
	CHECK(m.request_action() == "load" && m.request_path() == "/g/sa1.gdi");
	CHECK(m.input(RA_MENU_B | RA_MENU_A, 0) == RA_MACT_NONE);  // exiting: nothing
	CHECK(!exists("request.json.tmp"));

	// Exit Emulator
	RaMenu x;
	x.init(g_dir, "/g/ct.gdi", &FAKE_HW, true);
	CHECK(!exists("request.json"));
	x.open(RA_STATE_NOT_RECOGNIZED);
	CHECK(x.input(RA_MENU_UP, 0) == RA_MACT_REDRAW);         // wraps to Exit
	CHECK(x.input(RA_MENU_A, 0) == RA_MACT_REDRAW && x.page() == RaMenu::P_CONFIRM);
	CHECK(x.input(RA_MENU_RIGHT, 0) == RA_MACT_REDRAW);
	CHECK(x.input(RA_MENU_A, 0) == RA_MACT_EXIT);
	CHECK(get_file("request.json") == "{\"action\":\"exit\"}\n");
	CHECK(x.request_action() == "exit");
	// Resume / B
	x.open(RA_STATE_STARTING);
	CHECK(x.input(RA_MENU_A, 0) == RA_MACT_RESUME);
	x.open(RA_STATE_STARTING);
	CHECK(x.input(RA_MENU_B, 0) == RA_MACT_RESUME);
	// Achievements not selectable while RA is still loading
	x.open(RA_STATE_STARTING);
	x.input(RA_MENU_DOWN, 0);
	x.input(RA_MENU_DOWN, 0);
	CHECK(x.input(RA_MENU_A, 0) != RA_MACT_ACHIEVEMENTS);

	// no launcher files, no registers: "No game list", options greyed
	unlink(path_in("games.json").c_str());
	unlink(path_in("options.json").c_str());
	fake_ok[0] = fake_ok[1] = false;
	RaMenu n;
	n.init(g_dir, "/media/fat/games/Dreamcast/Some Game/Some Game.gdi", &FAKE_HW, false);
	n.open(RA_STATE_OFF);
	CHECK(!n.games_ok() && !n.options().popups);
	render_both(n);
	CHECK(n.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);
	CHECK(n.input(RA_MENU_A, 0) == RA_MACT_REDRAW && n.page() == RaMenu::P_GAMES);
	render_both(n);
	CHECK(n.input(RA_MENU_A, 0) == RA_MACT_NONE);            // nothing to load
	CHECK(n.input(RA_MENU_B, 0) == RA_MACT_REDRAW);
	CHECK(n.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);       // Options
	CHECK(n.input(RA_MENU_A, 0) == RA_MACT_REDRAW && n.page() == RaMenu::P_OPTIONS);
	CHECK(n.input(RA_MENU_RIGHT, 0) == RA_MACT_NONE);        // all greyed
	CHECK(n.input(RA_MENU_DOWN, 0) == RA_MACT_NONE);
	render_both(n);
	CHECK(!exists("options_changed.json"));   // nothing changed

	// options.json missing: values from the registers (CRT_CTRL too)
	fake_ok[0] = fake_ok[1] = true;
	fake_reg[RA_REG_AUDIO_VOL] = 0x03;
	fake_reg[RA_REG_CRT_CTRL] = 0x02;   // light, field original
	RaMenu r;
	r.init(g_dir, "", &FAKE_HW, true);
	r.open(RA_STATE_OFF);
	CHECK(r.options().vol_att == 3 && r.options().vfilter == RA_VF_LIGHT && !r.options().field_correct);
	render_both(r);

	// invalid JSON files
	put_file("games.json", "{\"games\":[{\"name\":\"x\",\"path\":\"/x.gdi\"},]}");
	put_file("options.json", "volume=3");
	RaMenu v;
	v.init(g_dir, "/x.gdi", &FAKE_HW, true);
	v.open(RA_STATE_OFF);
	CHECK(!v.games_ok() && !v.options().have_volume);
	printf("menu: ok\n");
}

// Exit Game (back to the launcher's list) vs Exit Emulator, reached by
// wrapping up from Resume
static void test_exit_items()
{
	put_file("games.json", GAMES_JSON);
	RaMenu m;
	m.init(g_dir, "/media/fat/games/Dreamcast/DOA2/Dead or Alive 2 (USA).gdi", &FAKE_HW, true);

	m.open(RA_STATE_OFF);
	CHECK(m.input(RA_MENU_UP, 0) == RA_MACT_REDRAW);     // wraps: Exit Emulator
	CHECK(m.input(RA_MENU_UP, 0) == RA_MACT_REDRAW);     // Exit Game
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW && m.page() == RaMenu::P_CONFIRM);
	render_both(m);
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW && m.page() == RaMenu::P_MAIN);  // No
	CHECK(!exists("request.json"));
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_REDRAW && m.page() == RaMenu::P_CONFIRM);
	CHECK(m.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);   // Yes
	CHECK(m.input(RA_MENU_A, 0) == RA_MACT_EXIT);
	CHECK(m.request_action() == "list");
	RaJson j = read_json("request.json");
	CHECK(j.get_str("action") && !strcmp(j.get_str("action"), "list"));
	render_both(m);
	unlink(path_in("request.json").c_str());

	RaMenu e;
	e.init(g_dir, "/media/fat/games/Dreamcast/DOA2/Dead or Alive 2 (USA).gdi", &FAKE_HW, true);
	e.open(RA_STATE_OFF);
	CHECK(e.input(RA_MENU_UP, 0) == RA_MACT_REDRAW);     // Exit Emulator
	CHECK(e.input(RA_MENU_A, 0) == RA_MACT_REDRAW && e.page() == RaMenu::P_CONFIRM);
	CHECK(e.input(RA_MENU_DOWN, 0) == RA_MACT_REDRAW);
	CHECK(e.input(RA_MENU_A, 0) == RA_MACT_EXIT && e.request_action() == "exit");
	j = read_json("request.json");
	CHECK(j.get_str("action") && !strcmp(j.get_str("action"), "exit"));
	printf("exit items: ok\n");
}

int main()
{
	snprintf(g_dir, sizeof(g_dir), "/tmp/ra_menu_test.XXXXXX");
	if (!mkdtemp(g_dir)) {
		printf("mkdtemp failed\n");
		return 1;
	}
	test_json();
	test_options();
	test_games();
	test_files();
	test_menu();
	test_exit_items();
	// clean up
	const char* files[] = { "games.json", "options.json", "request.json", "options_changed.json" };
	for (const char* f : files)
		unlink(path_in(f).c_str());
	rmdir(g_dir);
	printf(failures ? "ra_menu_test: %d FAILURE(S)\n" : "ra_menu_test: all ok\n", failures);
	return failures ? 1 : 0;
}
