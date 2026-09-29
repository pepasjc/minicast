#pragma once

// ra_osd -- drawing for the RetroAchievements overlay (popups + achievement
// list) straight into the Dreamcast framebuffer the polly2 SPG scans out.
//
// Everything here is plain computation on memory it is handed: no threads,
// no globals besides constant tables, so the host tests drive it against a
// fake VRAM. ra_overlay.cpp owns the threads and the SH4 hand-off.
//
// Framebuffer model (polly2 spg.sv / sys_top.v, same as spg_screenshot in
// mister_support.cpp): the SPG scans ONE progressive image at FB_R_SOF1 -
// FB_R_SOF2 is not used; in 480i field 0/1 show the even/odd lines of it.
//   depth    FB_R_CTRL[3:2]: 0555 / 565 / 888 packed / 0888 (2/2/3/4 bytes)
//   width    640, or 320 with VO_CONTROL.pixel_double
//   height   240 lines when FB_R_CTRL.fb_line_double is set or the mode is
//            240p (not VGA, not interlaced), else 480
//   stride   640 px * bytes/px, halved with pixel_double (not FB_R_SIZE)
//   layout   split VRAM: FB-view 32-bit word W lives at VRAM byte
//            W*8 + SOF1[22]*4 (the other half of each 64-bit word belongs
//            to the other bank)
//
// Drawing is done on a "canvas" of palette indices whose x unit is one
// source pixel and whose y unit is one 240p line: a 480-line source repeats
// every canvas row on two lines, so text looks the same in 240p, 480i (both
// fields carry every row - no interlace flicker) and 480p shown line-skipped
// on a 15 kHz CRT.
//
// Save / restore works on whole FB-view words through the split layout. The
// "under" copy is refreshed from what is in VRAM each time the popup is
// (re)drawn, per pixel (packed 888 pixels straddle words): pixels that no longer show
// what was drawn have been rewritten by the game, so the fresh content
// becomes the new "under" and a restore never puts stale pixels back over
// it - it only reverts pixels that still show the overlay.

#include <stdint.h>
#include <stddef.h>
#include "ra_font8x8.h"

// ---------------------------------------------------------------------------
// display geometry
// ---------------------------------------------------------------------------

struct RaGeom {
	uint32_t sof;       // FB_R_SOF1 & 0x7FFFFC: FB-view byte address, bit 22 = 32-bit half
	uint8_t depth;      // 0 = 0555, 1 = 565, 2 = 888 packed, 3 = 0888
	uint8_t concat;     // FB_R_CTRL.fb_concat (decoding only)
	uint8_t pixdbl;     // 320-wide source
	uint8_t enable;     // FB_R_CTRL.fb_enable
	uint16_t src_w;     // 640 / 320
	uint16_t src_h;     // 480 / 240 source lines
	uint8_t vs;         // source lines per canvas row: src_h / 240
	uint8_t bpp;        // bytes per pixel 2/2/3/4
	uint32_t stride;    // FB-view bytes per source line
};

RaGeom ra_geom_from_regs(uint32_t fb_r_sof1, uint32_t fb_r_ctrl, uint32_t vo_control, uint32_t spg_control);
// same pixel layout (everything but the address)
bool ra_geom_same_layout(const RaGeom& a, const RaGeom& b);

// ---------------------------------------------------------------------------
// VRAM access (mapped DDR on the target, a plain buffer in the host tests)
// ---------------------------------------------------------------------------

struct RaVram {
	uint8_t* base;
	uint32_t mask;      // VRAM_MASK: offsets wrap like spg_screenshot's
};

// VRAM byte offset of FB-view word `vword` in `half`
static inline uint32_t ra_vram_off(const RaVram& vm, uint32_t vword, uint32_t half)
{
	return ((vword << 3) | (half << 2)) & vm.mask;
}
// n consecutive FB-view words starting at vword (NEON de-interleave on ARM)
void ra_vram_read(const RaVram& vm, uint32_t vword, uint32_t half, uint32_t n, uint32_t* out);
static inline void ra_vram_write(const RaVram& vm, uint32_t vword, uint32_t half, uint32_t v)
{
	*(volatile uint32_t*)(vm.base + ra_vram_off(vm, vword, half)) = v;
}
// write barrier: everything stored so far is on its way to DDR (dsb)
void ra_vram_flush(void);

// ---------------------------------------------------------------------------
// colours
// ---------------------------------------------------------------------------

enum RaColor {
	RA_C_BG = 0,     // box background (dark blue)
	RA_C_BORDER,     // popup border (gold)
	RA_C_TEXT,       // white
	RA_C_GOLD,       // headings, achievement titles
	RA_C_DIM,        // locked / secondary text
	RA_C_HILITE,     // selected list entry background
	RA_C_RED,        // errors
	RA_C_BLUE,       // info popups
	RA_C_GREEN,      // unlocked mark
	RA_C_SEP,        // separator lines
	RA_C_COUNT
};
extern const uint8_t ra_palette_rgb[RA_C_COUNT][3];

// pixel bytes (little-endian, low bpp bytes) of an RGB colour in `depth`
uint32_t ra_encode_rgb(int depth, uint8_t r, uint8_t g, uint8_t b);
// the SPG's decode (fb_concat appended below 5/6-bit channels)
void ra_decode_pixel(int depth, int concat, uint32_t p, uint8_t* r, uint8_t* g, uint8_t* b);

// ---------------------------------------------------------------------------
// canvas + text
// ---------------------------------------------------------------------------

enum {
	RA_GLYPH_CHECK = 0x01,
	RA_GLYPH_RIGHT = 0x02,
	RA_GLYPH_LEFT = 0x03,
	RA_GLYPH_UP = 0x04,
	RA_GLYPH_DOWN = 0x05,
	RA_GLYPH_BULLET = 0x06,
	RA_GLYPH_SQUARE = 0x07,
	RA_GLYPH_HLINE = 0x08,
};

struct RaCanvas {
	int w, h;           // source pixels x 240-scale rows
	uint8_t* px;        // RaColor per pixel
};
bool ra_canvas_init(RaCanvas* c, int w, int h);
void ra_canvas_free(RaCanvas* c);
void ra_canvas_fill(RaCanvas* c, int x, int y, int w, int h, uint8_t color);
// 8x8 cells; glyphs < 0x20 are the RA_GLYPH_* symbols, >= 0x7F draw '?'
void ra_canvas_glyph(RaCanvas* c, int x, int y, uint8_t code, uint8_t color);
// at most max_chars (<0: no limit) chars of ASCII text; returns the count
int ra_canvas_text(RaCanvas* c, int x, int y, const char* s, uint8_t color, int max_chars);

// UTF-8 -> printable ASCII: typographic quotes/dashes/ellipsis and accented
// Latin letters map to their plain forms, anything else becomes one '?'.
// Control characters become spaces. Always NUL-terminates.
void ra_to_ascii(const char* utf8, char* out, size_t out_size);
// at most `width` chars; longer text ends in "..."
void ra_fit(const char* s, int width, char* out, size_t out_size);
// word wrap into lines of at most `width` chars, '\n'-separated, at most
// max_lines (the last one ends in "..." if text is left over); returns lines
int ra_wrap(const char* s, int width, int max_lines, char* out, size_t out_size);

// ---------------------------------------------------------------------------
// regions: a canvas-space rectangle mapped onto one framebuffer
// ---------------------------------------------------------------------------

struct RaRegion {
	int x, y, w, h;     // x/w in source pixels, y/h in 240-scale rows
};

struct RaSpan {
	uint32_t y0, ny;    // first source line, line count
	uint32_t wl0, nw;   // first FB-view word within a line, words per line
	uint32_t mask_first, mask_last;   // byte masks of the partial end words
};
RaSpan ra_region_span(const RaGeom& g, const RaRegion& r);
static inline uint32_t ra_span_mask(const RaSpan& s, uint32_t i)
{
	uint32_t m = 0xFFFFFFFFu;
	if (i == 0) m &= s.mask_first;
	if (i + 1 == s.nw) m &= s.mask_last;
	return m;
}

// [lo, hi) in the 32-bit FB address space (bit 22 = half, as SOF registers)
struct RaRange {
	uint32_t lo, hi;
};
static inline bool ra_range_overlap(const RaRange& a, const RaRange& b)
{
	return a.lo < b.hi && b.lo < a.hi;
}
RaRange ra_region_range(const RaGeom& g, const RaSpan& s);

// Encoded overlay words per canvas row (a 480-line source uses each row twice)
struct RaPopWords {
	uint32_t rows, nw;
	uint32_t* val;      // [rows][nw]
};
bool ra_pop_build(RaPopWords* p, const RaGeom& g, const RaRegion& r, const RaCanvas& c);
void ra_pop_free(RaPopWords* p);

struct RaOpStats {
	uint32_t refreshes, restores;
	uint32_t words_read, words_written;
	uint32_t fresh_units;       // pixels found rewritten by the game (under refreshed)
	uint32_t kept_units;        // restore: pixels left alone (no longer the overlay's)
};

// One framebuffer the overlay has drawn into.
struct RaTouched {
	RaGeom geom;        // geom.sof = this buffer
	RaRegion rgn;
	RaSpan span;
	uint32_t* under;    // [ny][nw] what the game had there
	uint32_t* drawn;    // [ny][nw] what we last wrote
	bool have;          // under/drawn valid
};
bool ra_touched_init(RaTouched* t, const RaGeom& g, const RaRegion& r);
void ra_touched_free(RaTouched* t);
// Draws the overlay words into the buffer. read=false trusts drawn[] as the
// current VRAM content (only valid when nothing else can have written it,
// e.g. while the emulation is paused); the first refresh always reads.
void ra_touched_refresh(RaTouched* t, const RaVram& vm, const RaPopWords& p, bool read, RaOpStats* st);
// Puts the saved pixels back where VRAM still shows what we drew.
void ra_touched_restore(RaTouched* t, const RaVram& vm, RaOpStats* st);
// A few sampled words: true while they still show what was drawn.
bool ra_touched_intact(const RaTouched* t, const RaVram& vm);
static inline RaRange ra_touched_range(const RaTouched* t) { return ra_region_range(t->geom, t->span); }

// ---------------------------------------------------------------------------
// popup tracker: keeps one popup visible across the game's buffer swaps
// ---------------------------------------------------------------------------

// Serializes VRAM work against renders the PowerVR (polly2) may be doing:
// begin() fails while a render into an overlapping range is in flight, and
// holds new renders into the range off until end().
struct RaGuard {
	virtual bool begin(const RaRange& r) = 0;
	virtual void end() = 0;
	virtual ~RaGuard() {}
};

class RaTracker {
public:
	static const int MAX_BUFFERS = 4;
	static const uint32_t EVICT_VBLANKS = 20;   // not displayed that long: restore + forget
	static const uint32_t ACTIVE_VBLANKS = 8;   // displayed that recently: redraw right after its render

	void init(const RaVram& vm, RaGuard* guard);
	void shutdown();    // frees memory, no VRAM access

	// New overlay content laid out for g (address ignored); c == NULL drops
	// the content (buffers stay touched until restore_all()).
	bool set_content(const RaGeom& g, const RaRegion& r, const RaCanvas* c);
	bool has_content() const { return content_; }
	const RaGeom& content_geom() const { return geom_; }
	uint32_t content_gen() const { return gen_; }

	// A render started into these ranges: overlapping buffers need a redraw.
	void render_event(const RaRange& a, const RaRange& b);
	void mark_all_dirty();
	// vblank: disp_sof is displayed now. Returns true when it shows the
	// current content after the call.
	bool vblank(uint32_t disp_sof, uint32_t vbl);
	// a render finished: redraw buffers that were re-rendered and are in use,
	// so the popup is already in them when the game flips to them
	void render_done();
	// restores every buffer it can (guarded); returns the number left
	int restore_all();
	int count() const;

	RaOpStats st = {};
	uint32_t guard_skips = 0;

private:
	struct Ent {
		bool used;
		bool dirty;
		uint32_t gen;       // content generation drawn
		uint32_t layout;    // layout generation (old layouts may only be restored)
		uint32_t last_disp;
		RaTouched t;
	};
	bool refresh(Ent& e);
	bool restore(Ent& e);
	Ent* find(uint32_t sof);

	RaVram vm_ = {};
	RaGuard* guard_ = nullptr;
	Ent e_[MAX_BUFFERS] = {};
	RaPopWords pop_ = {};
	RaGeom geom_ = {};
	RaRegion rgn_ = {};
	bool content_ = false;
	uint32_t gen_ = 0, layout_ = 0, vbl_ = 0;
};

// ---------------------------------------------------------------------------
// popup layout
// ---------------------------------------------------------------------------

enum { RA_POS_LEFT = 0, RA_POS_CENTER = 1, RA_POS_RIGHT = 2 };
int ra_parse_position(const char* v);   // left/center/centre/right/0/1/2, -1 = invalid
const char* ra_position_name(int pos);

struct RaPopupText {
	char line1[64];
	char line2[128];
	uint8_t accent;     // RaColor of line 1 + border
};

// Popup box position/size for a source width
RaRegion ra_popup_layout(int src_w, int position);
// Renders into c (must be r.w x r.h, as from ra_popup_layout)
bool ra_popup_render(RaCanvas* c, int src_w, const RaRegion& r, const RaPopupText& t);

// ---------------------------------------------------------------------------
// achievement list
// ---------------------------------------------------------------------------

struct ra_list_item {
	uint32_t id, points;
	uint8_t unlocked;
	uint8_t unsupported;
	int64_t unlock_time;   // unix time, 0 = unknown
	char title[96];        // ASCII (ra_to_ascii)
	char desc[256];
	char progress[24];
};

struct ra_list_data {
	char game[96];
	uint32_t count;
	uint32_t num_unlocked, points_total, points_unlocked;
	ra_list_item* items;
};
void ra_list_free(ra_list_data* d);
// unlocked first, then locked; keeps the order within each group (stable)
void ra_list_sort(ra_list_data* d);
// recount num_unlocked / points_*
void ra_list_totals(ra_list_data* d);

enum { RA_NAV_UP, RA_NAV_DOWN, RA_NAV_PGUP, RA_NAV_PGDN, RA_NAV_HOME, RA_NAV_END };

struct RaListView {
	int sel, first;
	int per_page;       // from ra_list_per_page()
	bool detail;
};
int ra_list_per_page(int src_w);
void ra_list_nav(RaListView* v, int count, int action);
// full-screen page: canvas src_w x 240
bool ra_list_render(RaCanvas* c, const ra_list_data* d, const RaListView& v, bool softcore);
