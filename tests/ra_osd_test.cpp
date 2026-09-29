// ra_osd_test -- host tests for the RetroAchievements overlay:
//   - pixel encode/decode for the four framebuffer depths
//   - split-VRAM addressing + save/draw/restore against an independent
//     reference model, for every depth x pixel/line doubling x 240/480 x
//     both 32-bit halves, popup and full-screen regions (byte-identical
//     VRAM after restore, the other bank's half never touched)
//   - pixels the game rewrites under/over the popup survive the restore
//   - the buffer tracker: double/triple buffering, static screens,
//     CPU-drawn frames, display-address shake, layout change, render guard
//   - a threaded end-to-end run of the real overlay thread against a fake
//     SH4 thread rendering double-buffered frames, popups and the
//     achievement list (pause, navigation, restore)
//   - text helpers, list sort/paging
//   - preview images (PPM) of popups and list pages
//
//   ra_osd_test [preview_dir]

#include "ra_osd.h"
#include "ra_overlay.h"
#include "ra_dc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <atomic>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// ---------------------------------------------------------------------------
// stubs for ra_overlay.cpp (frontend input + audio)
// ---------------------------------------------------------------------------

static std::atomic<uint32_t> g_stub_btn{0};
static std::atomic<int> g_stub_suppress{0}, g_stub_audio_paused{0}, g_stub_polls{0};
uint32_t ra_frontend_menu_input(void) { g_stub_polls++; return g_stub_btn.load(); }
void ra_frontend_input_suppress(void) { g_stub_suppress++; }
void AudioSetPaused(bool p) { g_stub_audio_paused = p; }
void AudioPausedTick(void) {}

// ---------------------------------------------------------------------------
// reference model (independent of ra_osd)
// ---------------------------------------------------------------------------

static const uint32_t VSIZE = 8u << 20, VMASK = VSIZE - 1;
static uint8_t* vram;

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

struct Mode {
	const char* name;
	int depth;
	bool pixdbl, linedbl_bit, vga, ilace;
};

static const Mode MODES[] = {
	{ "565 640x240 (240p)", 1, false, false, false, false },
	{ "565 640x480 (480i)", 1, false, false, false, true },
	{ "0555 640x480 (480i)", 0, false, false, false, true },
	{ "888 640x480 (VGA)", 2, false, false, true, false },
	{ "0888 640x480 (VGA)", 3, false, false, true, false },
	{ "565 320x240 (pixel double, 240p)", 1, true, false, false, false },
	{ "0888 320x240 (pixel+line double, VGA)", 3, true, true, true, false },
	{ "888 320x480 (pixel double, 480i)", 2, true, false, false, true },
	{ "0555 640x240 (line double bit, 480i)", 0, false, true, false, true },
	{ "888 640x240 (240p)", 2, false, false, false, false },
};
static const int NMODES = sizeof(MODES) / sizeof(MODES[0]);

static void mode_regs(const Mode& m, uint32_t sof, uint32_t r[4])
{
	r[0] = sof;
	r[1] = 1u | ((uint32_t)m.linedbl_bit << 1) | ((uint32_t)m.depth << 2) | ((uint32_t)m.vga << 23);
	r[2] = (uint32_t)m.pixdbl << 8;
	r[3] = (uint32_t)m.ilace << 4;
}

static RaGeom mode_geom(const Mode& m, uint32_t sof)
{
	uint32_t r[4];
	mode_regs(m, sof, r);
	return ra_geom_from_regs(r[0], r[1], r[2], r[3]);
}

struct RefDims { int w, h, bpp, stride; };
static RefDims ref_dims(const Mode& m)
{
	RefDims d;
	d.w = m.pixdbl ? 320 : 640;
	d.h = (m.linedbl_bit || (!m.vga && !m.ilace)) ? 240 : 480;
	d.bpp = m.depth == 2 ? 3 : m.depth == 3 ? 4 : 2;
	d.stride = 640 * d.bpp / (m.pixdbl ? 2 : 1);
	return d;
}

// FB-view byte v of the half selected by SOF bit 22 -> VRAM byte
static inline uint32_t ref_off(uint32_t v, uint32_t half)
{
	return ((((v >> 2) << 3) | (half << 2)) & VMASK) | (v & 3);
}

static uint32_t ref_get(const uint8_t* mem, uint32_t sof, const RefDims& d, int x, int y)
{
	uint32_t half = (sof >> 22) & 1, v = (sof & 0x3FFFFC) + (uint32_t)y * d.stride + (uint32_t)x * d.bpp, p = 0;
	for (int k = 0; k < d.bpp; k++)
		p |= (uint32_t)mem[ref_off(v + k, half)] << (8 * k);
	return p;
}

static void ref_set(uint8_t* mem, uint32_t sof, const RefDims& d, int x, int y, uint32_t p)
{
	uint32_t half = (sof >> 22) & 1, v = (sof & 0x3FFFFC) + (uint32_t)y * d.stride + (uint32_t)x * d.bpp;
	for (int k = 0; k < d.bpp; k++)
		mem[ref_off(v + k, half)] = (uint8_t)(p >> (8 * k));
}

static uint32_t ref_encode(int depth, const uint8_t* c)
{
	uint32_t r = c[0], g = c[1], b = c[2];
	switch (depth) {
	case 0: return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
	case 1: return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	default: return b | (g << 8) | (r << 16);
	}
}

static uint32_t pix_mask(int bpp) { return bpp == 4 ? 0xFFFFFFFFu : (1u << (8 * bpp)) - 1; }

// a "game" pixel value that is none of the overlay colours
static uint32_t game_value(int depth, uint32_t v)
{
	int bpp = depth == 2 ? 3 : depth == 3 ? 4 : 2;
	v &= pix_mask(bpp);
	for (;;) {
		bool clash = false;
		for (int i = 0; i < RA_C_COUNT; i++)
			if ((v & pix_mask(bpp)) == (ref_encode(depth, ra_palette_rgb[i]) & pix_mask(bpp)))
				clash = true;
		if (!clash)
			return v;
		v = (v + 0x0101) & pix_mask(bpp);
	}
}

static void fill_random(uint8_t* mem, size_t n)
{
	for (size_t i = 0; i < n; i += 4) {
		uint32_t v = rnd();
		memcpy(mem + i, &v, 4);
	}
}

static long first_diff(const uint8_t* a, const uint8_t* b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (a[i] != b[i])
			return (long)i;
	return -1;
}

// expected VRAM after drawing canvas c at region r
static void ref_draw(uint8_t* mem, const Mode& m, uint32_t sof, const RaRegion& r, const RaCanvas& c)
{
	RefDims d = ref_dims(m);
	int vs = d.h / 240;
	for (int y = r.y * vs; y < (r.y + r.h) * vs; y++)
		for (int x = r.x; x < r.x + r.w; x++)
			ref_set(mem, sof, d, x, y, ref_encode(m.depth, ra_palette_rgb[c.px[(y / vs - r.y) * c.w + (x - r.x)]]));
}

static RaVram test_vm() { RaVram v; v.base = vram; v.mask = VMASK; return v; }

// ---------------------------------------------------------------------------
// sample content
// ---------------------------------------------------------------------------

static const char* const SAMPLE_TITLES[] = {
	"First Steps", "Sonic Boom", "Ring Collector", "Chaos Emerald Hunter", "Speed Demon",
	"No Damage Run - Emerald Coast", "Big's Fishing Trip", "Tails' Flight School",
	"A Very Long Achievement Title That Keeps Going And Going Past The Edge",
	"Knuckles' Treasure", "Amy's Hammer Time", "E-102 Gamma", "Chao Garden Master",
	"Perfect Chaos", "Super Sonic", "Emblem Collector I", "Emblem Collector II",
	"Emblem Collector III", "Twinkle Park Speedrun", "Casinopolis Jackpot",
	"Ice Cap Snowboarder", "Red Mountain Climber", "Sky Deck Survivor", "Lost World Explorer",
	"Final Egg Infiltrator",
};

static ra_list_data* sample_list(int n)
{
	ra_list_data* d = (ra_list_data*)calloc(1, sizeof(ra_list_data));
	d->items = (ra_list_item*)calloc((size_t)n, sizeof(ra_list_item));
	snprintf(d->game, sizeof(d->game), "Sonic Adventure");
	for (int i = 0; i < n; i++) {
		ra_list_item& it = d->items[d->count++];
		it.id = 1000 + i;
		it.points = (uint32_t)((i * 7) % 25 + 1);
		it.unlocked = (i % 3) == 1;
		it.unlock_time = it.unlocked ? 1790000000 + i * 3600 : 0;
		snprintf(it.title, sizeof(it.title), "%s", SAMPLE_TITLES[i % 25]);
		snprintf(it.desc, sizeof(it.desc),
			i % 4 == 0 ? "Clear the stage without losing a single ring, then find the hidden emblem behind the waterfall near the second checkpoint of the level."
			           : "Collect %d rings in one run", 50 + i * 10);
		if (!it.unlocked && i % 5 == 2)
			snprintf(it.progress, sizeof(it.progress), "%d/100", i * 3);
	}
	ra_list_sort(d);
	ra_list_totals(d);
	return d;
}

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------

static void test_encode()
{
	CHECK(ra_encode_rgb(1, 255, 0, 0) == 0xF800);
	CHECK(ra_encode_rgb(1, 0, 255, 0) == 0x07E0);
	CHECK(ra_encode_rgb(1, 0, 0, 255) == 0x001F);
	CHECK(ra_encode_rgb(0, 255, 0, 0) == 0x7C00);
	CHECK(ra_encode_rgb(0, 0, 255, 0) == 0x03E0);
	CHECK(ra_encode_rgb(2, 0x12, 0x34, 0x56) == 0x123456);
	CHECK(ra_encode_rgb(3, 0x12, 0x34, 0x56) == 0x123456);
	for (int depth = 0; depth < 4; depth++)
		for (int i = 0; i < RA_C_COUNT; i++) {
			const uint8_t* c = ra_palette_rgb[i];
			CHECK(ra_encode_rgb(depth, c[0], c[1], c[2]) == ref_encode(depth, c));
			uint8_t r, g, b;
			ra_decode_pixel(depth, 0, ra_encode_rgb(depth, c[0], c[1], c[2]), &r, &g, &b);
			int tol = depth <= 1 ? 8 : 0;
			CHECK(abs(r - c[0]) <= tol && abs(g - c[1]) <= tol && abs(b - c[2]) <= tol);
		}
	// the palette stays distinct in every depth (restore relies on it)
	for (int depth = 0; depth < 4; depth++)
		for (int i = 0; i < RA_C_COUNT; i++)
			for (int j = i + 1; j < RA_C_COUNT; j++)
				CHECK(ref_encode(depth, ra_palette_rgb[i]) != ref_encode(depth, ra_palette_rgb[j]));
	printf("encode: ok\n");
}

static void test_geometry()
{
	for (int mi = 0; mi < NMODES; mi++) {
		const Mode& m = MODES[mi];
		RefDims d = ref_dims(m);
		RaGeom g = mode_geom(m, 0x412344);
		CHECK(g.sof == 0x412344);
		CHECK(g.src_w == d.w && g.src_h == d.h && g.bpp == d.bpp && (int)g.stride == d.stride);
		CHECK(g.vs == d.h / 240 && g.depth == m.depth && g.enable);
	}
	RaGeom g = ra_geom_from_regs(0x1234567, 0, 0, 0);
	CHECK(g.sof == (0x1234567u & 0x7FFFFC) && !g.enable);
	printf("geometry: ok\n");
}

// draw + restore for every mode / address / region
static void test_modes()
{
	static const uint32_t SOFS[] = { 0x000000, 0x0C8040, 0x400000 | 0x012340 };
	uint8_t* orig = (uint8_t*)malloc(VSIZE);
	uint8_t* exp = (uint8_t*)malloc(VSIZE);
	RaVram vm = test_vm();
	ra_list_data* list = sample_list(25);
	int cases = 0;
	for (int mi = 0; mi < NMODES; mi++) {
		const Mode& m = MODES[mi];
		RefDims d = ref_dims(m);
		for (uint32_t sof : SOFS) {
			fill_random(vram, VSIZE);
			memcpy(orig, vram, VSIZE);
			RaGeom g = mode_geom(m, sof);
			RaRegion regions[5] = {
				ra_popup_layout(d.w, RA_POS_LEFT), ra_popup_layout(d.w, RA_POS_CENTER),
				ra_popup_layout(d.w, RA_POS_RIGHT), { 5, 3, 37, 11 }, { 0, 0, d.w, 240 },
			};
			for (int ri = 0; ri < 5; ri++) {
				const RaRegion& r = regions[ri];
				CHECK(r.x >= 0 && r.x + r.w <= d.w && r.y >= 0 && r.y + r.h <= 240);
				RaCanvas c;
				ra_canvas_init(&c, r.w, r.h);
				if (ri < 3) {
					RaPopupText t = {};
					snprintf(t.line1, sizeof(t.line1), "Achievement unlocked");
					snprintf(t.line2, sizeof(t.line2), "Chaos Emerald Hunter (25)");
					t.accent = RA_C_BORDER;
					CHECK(ra_popup_render(&c, d.w, r, t));
				} else if (ri == 3) {
					for (int i = 0; i < r.w * r.h; i++)
						c.px[i] = (uint8_t)((i * 7 + i / 13) % RA_C_COUNT);
				} else {
					RaListView v = {};
					v.per_page = ra_list_per_page(d.w);
					CHECK(ra_list_render(&c, list, v, true));
				}
				RaTouched t;
				RaPopWords pw = {};
				RaOpStats st = {};
				CHECK(ra_touched_init(&t, g, r));
				CHECK(ra_pop_build(&pw, g, r, c));
				ra_touched_refresh(&t, vm, pw, true, &st);
				memcpy(exp, orig, VSIZE);
				ref_draw(exp, m, sof, r, c);
				long diff = first_diff(vram, exp, VSIZE);
				if (diff >= 0)
					printf("  %s sof %06x region %d: draw differs at VRAM byte 0x%lx\n", m.name, sof, ri, diff);
				CHECK(diff < 0);
				CHECK(ra_touched_intact(&t, vm));
				CHECK(st.fresh_units == 0);

				// redraw other content without reading (paused list path)
				for (int i = 0; i < r.w * r.h; i++)
					c.px[i] = (uint8_t)((c.px[i] + 3) % RA_C_COUNT);
				CHECK(ra_pop_build(&pw, g, r, c));
				ra_touched_refresh(&t, vm, pw, false, &st);
				memcpy(exp, orig, VSIZE);
				ref_draw(exp, m, sof, r, c);
				CHECK(first_diff(vram, exp, VSIZE) < 0);

				ra_touched_restore(&t, vm, &st);
				diff = first_diff(vram, orig, VSIZE);
				if (diff >= 0)
					printf("  %s sof %06x region %d: restore differs at VRAM byte 0x%lx\n", m.name, sof, ri, diff);
				CHECK(diff < 0);
				CHECK(st.kept_units == 0);
				ra_touched_free(&t);
				ra_pop_free(&pw);
				ra_canvas_free(&c);
				cases++;
			}
		}
	}
	ra_list_free(list);
	free(orig);
	free(exp);
	printf("modes: %d draw/restore cases\n", cases);
}

// the game rewrites pixels in and around the popup between draws: those
// pixels keep the game's values, everything else comes back as it was
static void test_fresh_pixels()
{
	uint8_t* orig = (uint8_t*)malloc(VSIZE);
	uint8_t* exp = (uint8_t*)malloc(VSIZE);
	RaVram vm = test_vm();
	for (int mi = 0; mi < NMODES; mi++) {
		const Mode& m = MODES[mi];
		RefDims d = ref_dims(m);
		int vs = d.h / 240;
		uint32_t sof = mi & 1 ? 0x400000 | 0x020000 : 0x010000;
		fill_random(vram, VSIZE);
		memcpy(orig, vram, VSIZE);
		memcpy(exp, orig, VSIZE);
		RaGeom g = mode_geom(m, sof);
		// odd x/width: partial words at both ends
		RaRegion r = { 7, 10, 45, 9 };
		RaCanvas c;
		ra_canvas_init(&c, r.w, r.h);
		for (int i = 0; i < r.w * r.h; i++)
			c.px[i] = (uint8_t)(i % RA_C_COUNT);
		RaTouched t;
		RaPopWords pw = {};
		RaOpStats st = {};
		ra_touched_init(&t, g, r);
		ra_pop_build(&pw, g, r, c);
		ra_touched_refresh(&t, vm, pw, true, &st);

		for (int round = 0; round < 2; round++) {
			for (int k = 0; k < 60; k++) {
				// inside the region, or just around it
				int x = r.x - 2 + (int)(rnd() % (uint32_t)(r.w + 4));
				int y = r.y * vs - 1 + (int)(rnd() % (uint32_t)(r.h * vs + 2));
				if (x < 0 || y < 0 || x >= d.w || y >= d.h)
					continue;
				uint32_t v = game_value(m.depth, rnd());
				ref_set(vram, sof, d, x, y, v);
				ref_set(exp, sof, d, x, y, v);
			}
			if (round == 0)
				ra_touched_refresh(&t, vm, pw, true, &st);   // redraw over the fresh pixels
		}
		CHECK(st.fresh_units > 0);
		ra_touched_restore(&t, vm, &st);
		long diff = first_diff(vram, exp, VSIZE);
		if (diff >= 0)
			printf("  %s: fresh pixels: differs at VRAM byte 0x%lx\n", m.name, diff);
		CHECK(diff < 0);
		ra_touched_free(&t);
		ra_pop_free(&pw);
		ra_canvas_free(&c);
	}
	free(orig);
	free(exp);
	printf("fresh pixels: ok\n");
}

// ---------------------------------------------------------------------------
// tracker simulation (single thread)
// ---------------------------------------------------------------------------

struct SimGuard : RaGuard {
	bool inflight = false;
	RaRange a = {}, b = {};
	int fails = 0;
	bool begin(const RaRange& r) override
	{
		if (inflight && (ra_range_overlap(a, r) || ra_range_overlap(b, r))) {
			fails++;
			return false;
		}
		return true;
	}
	void end() override {}
};

static uint32_t pattern(int depth, uint32_t frame, int x, int y)
{
	uint32_t h = frame * 2654435761u ^ (uint32_t)(x * 40503) ^ (uint32_t)(y * 9973) ^ (uint32_t)(x * y);
	h ^= h >> 15;
	h *= 2246822519u;
	h ^= h >> 13;
	return game_value(depth, h);
}

static void render_full(uint8_t* mem, const Mode& m, uint32_t sof, uint32_t frame)
{
	RefDims d = ref_dims(m);
	for (int y = 0; y < d.h; y++)
		for (int x = 0; x < d.w; x++)
			ref_set(mem, sof, d, x, y, pattern(m.depth, frame, x, y));
}

static bool buffer_is_frame(const uint8_t* mem, const Mode& m, uint32_t sof, uint32_t frame)
{
	RefDims d = ref_dims(m);
	for (int y = 0; y < d.h; y++)
		for (int x = 0; x < d.w; x++)
			if (ref_get(mem, sof, d, x, y) != pattern(m.depth, frame, x, y)) {
				printf("  buffer %06x frame %u: pixel %d,%d differs\n", sof, frame, x, y);
				return false;
			}
	return true;
}

static bool shows_popup(const Mode& m, uint32_t sof, const RaRegion& r, const RaCanvas& c)
{
	RefDims d = ref_dims(m);
	int vs = d.h / 240;
	for (int y = r.y * vs; y < (r.y + r.h) * vs; y++)
		for (int x = r.x; x < r.x + r.w; x++)
			if (ref_get(vram, sof, d, x, y) != ref_encode(m.depth, ra_palette_rgb[c.px[(y / vs - r.y) * c.w + (x - r.x)]]))
				return false;
	return true;
}

static void make_popup(const RaGeom& g, int pos, const char* l2, RaRegion* r, RaCanvas* c)
{
	*r = ra_popup_layout(g.src_w, pos);
	ra_canvas_free(c);
	ra_canvas_init(c, r->w, r->h);
	RaPopupText t = {};
	snprintf(t.line1, sizeof(t.line1), "Achievement unlocked");
	snprintf(t.line2, sizeof(t.line2), "%s", l2);
	t.accent = RA_C_BORDER;
	ra_popup_render(c, g.src_w, *r, t);
}

// the other bank's halves of the VRAM words must never change
static bool other_half_intact(const uint8_t* orig, uint32_t half_used)
{
	for (uint32_t off = 0; off < VSIZE; off += 8) {
		uint32_t o = off + (half_used ? 0 : 4);
		if (memcmp(vram + o, orig + o, 4))
			return false;
	}
	return true;
}

static void test_tracker()
{
	const Mode& m = MODES[1];   // 565 640x480
	RefDims d = ref_dims(m);
	RaVram vm = test_vm();
	uint8_t* orig = (uint8_t*)malloc(VSIZE);
	const uint32_t FB = 640 * 480 * 2;
	uint32_t sofs[3] = { 0x000000, FB, 2 * FB };

	// A: double buffering, every frame re-rendered, content change, removal
	{
		fill_random(vram, VSIZE);
		memcpy(orig, vram, VSIZE);
		SimGuard guard;
		RaTracker tr;
		tr.init(vm, &guard);
		RaGeom g = mode_geom(m, 0);
		RaRegion r;
		RaCanvas c = {};
		make_popup(g, RA_POS_LEFT, "Speed Demon (10)", &r, &c);
		CHECK(tr.set_content(g, r, &c));
		uint32_t last[2] = { 0, 0 };
		render_full(vram, m, sofs[0], 1);
		render_full(vram, m, sofs[1], 2);
		last[0] = 1;
		last[1] = 2;
		int shown = 0, frames_with_content = 0;
		for (uint32_t f = 3; f < 60; f++) {
			int back = f & 1;
			RaRange rr = { sofs[back], sofs[back] + FB };
			RaRange none = { 0, 0 };
			tr.render_event(rr, none);
			guard.inflight = true;
			guard.a = rr;
			tr.render_done();          // an overlay pass during the render: must back off
			render_full(vram, m, sofs[back], f);
			last[back] = f;
			guard.inflight = false;
			tr.render_done();
			if (f == 30) {
				make_popup(g, RA_POS_LEFT, "Ring Collector (5)", &r, &c);
				CHECK(tr.set_content(g, r, &c));
			}
			if (f == 45)
				tr.set_content(g, r, NULL);
			bool ok = tr.vblank(sofs[back], f);
			if (tr.has_content()) {
				frames_with_content++;
				if (ok && shows_popup(m, sofs[back], r, c))
					shown++;
			} else {
				tr.restore_all();
			}
		}
		CHECK(shown == frames_with_content);
		CHECK(guard.fails > 0);
		CHECK(tr.count() == 0);
		CHECK(buffer_is_frame(vram, m, sofs[0], last[0]));
		CHECK(buffer_is_frame(vram, m, sofs[1], last[1]));
		CHECK(other_half_intact(orig, 0));
		printf("tracker double buffering: popup in %d/%d displayed frames, %u draws, %u guard back-offs\n",
			shown, frames_with_content, tr.st.refreshes, guard.fails);
		tr.shutdown();
		ra_canvas_free(&c);
	}

	// B: static screen flipping between two buffers without renders
	{
		fill_random(vram, VSIZE);
		memcpy(orig, vram, VSIZE);
		RaTracker tr;
		tr.init(vm, NULL);
		RaGeom g = mode_geom(m, 0);
		RaRegion r;
		RaCanvas c = {};
		make_popup(g, RA_POS_CENTER, "Static (1)", &r, &c);
		tr.set_content(g, r, &c);
		for (uint32_t f = 0; f < 10; f++) {
			CHECK(tr.vblank(sofs[f & 1], f));
			CHECK(shows_popup(m, sofs[f & 1], r, c));
		}
		CHECK(tr.count() == 2);
		tr.set_content(g, r, NULL);
		CHECK(tr.restore_all() == 0);
		CHECK(first_diff(vram, orig, VSIZE) < 0);
		tr.shutdown();
		ra_canvas_free(&c);
		printf("tracker static flip: ok\n");
	}

	// C: CPU-drawn frames (FMV): the game writes the displayed buffer
	// directly, no render events; everything it wrote must survive
	{
		fill_random(vram, VSIZE);
		uint8_t* truth = (uint8_t*)malloc(VSIZE);
		memcpy(truth, vram, VSIZE);
		RaTracker tr;
		tr.init(vm, NULL);
		RaGeom g = mode_geom(m, 0);
		RaRegion r;
		RaCanvas c = {};
		make_popup(g, RA_POS_LEFT, "FMV (1)", &r, &c);
		tr.set_content(g, r, &c);
		int redraws0 = 0;
		for (uint32_t f = 0; f < 40; f++) {
			tr.vblank(sofs[0], f);
			if (f == 0)
				redraws0 = (int)tr.st.refreshes;
			// the "decoder" rewrites a band of lines through the popup
			int y0 = (int)(rnd() % 60);
			for (int y = y0; y < y0 + 8; y++)
				for (int x = 0; x < d.w; x += 1 + (int)(rnd() % 3)) {
					uint32_t v = game_value(m.depth, rnd());
					ref_set(vram, sofs[0], d, x, y, v);
					ref_set(truth, sofs[0], d, x, y, v);
				}
		}
		CHECK((int)tr.st.refreshes > redraws0);   // the spot check noticed
		tr.set_content(g, r, NULL);
		tr.restore_all();
		long diff = first_diff(vram, truth, VSIZE);
		if (diff >= 0)
			printf("  FMV: differs at 0x%lx\n", diff);
		CHECK(diff < 0);
		printf("tracker CPU-drawn frames: %u redraws, ok\n", tr.st.refreshes);
		tr.shutdown();
		ra_canvas_free(&c);
		free(truth);
	}

	// D: display address shake (overlapping popup regions) + triple
	// buffering + a render-to-texture elsewhere every frame
	{
		fill_random(vram, VSIZE);
		memcpy(orig, vram, VSIZE);
		RaTracker tr;
		tr.init(vm, NULL);
		RaGeom g = mode_geom(m, 0);
		RaRegion r;
		RaCanvas c = {};
		make_popup(g, RA_POS_RIGHT, "Shake (2)", &r, &c);
		tr.set_content(g, r, &c);
		const uint32_t shake = 4 * 1280;
		for (uint32_t f = 0; f < 30; f++) {
			uint32_t s = (f & 1) ? sofs[0] + shake : sofs[0];
			CHECK(tr.vblank(s, f));
			CHECK(shows_popup(m, s, r, c));
			CHECK(tr.count() == 1);
		}
		for (uint32_t f = 30; f < 60; f++) {
			RaRange tex = { 0x700000, 0x710000 }, none = { 0, 0 };
			tr.render_event(tex, none);
			tr.render_done();
			CHECK(tr.vblank(sofs[f % 3], f));
		}
		CHECK(tr.count() == 3);
		tr.set_content(g, r, NULL);
		CHECK(tr.restore_all() == 0);
		CHECK(first_diff(vram, orig, VSIZE) < 0);
		printf("tracker shake + triple buffering: ok\n");
		tr.shutdown();
		ra_canvas_free(&c);
	}

	// E: display mode change while shown (640x480 565 -> 320x240 0888)
	{
		fill_random(vram, VSIZE);
		memcpy(orig, vram, VSIZE);
		RaTracker tr;
		tr.init(vm, NULL);
		RaGeom g1 = mode_geom(m, 0);
		RaRegion r;
		RaCanvas c = {};
		make_popup(g1, RA_POS_LEFT, "Mode change (3)", &r, &c);
		tr.set_content(g1, r, &c);
		CHECK(tr.vblank(sofs[0], 0));
		CHECK(tr.vblank(sofs[1], 1));
		const Mode& m2 = MODES[6];
		RaGeom g2 = mode_geom(m2, 0);
		make_popup(g2, RA_POS_LEFT, "Mode change (3)", &r, &c);
		tr.set_content(g2, r, &c);
		CHECK(tr.vblank(sofs[2], 2));   // old-layout buffers restored first
		CHECK(shows_popup(m2, sofs[2], r, c));
		CHECK(tr.count() == 1);
		tr.set_content(g2, r, NULL);
		tr.restore_all();
		CHECK(first_diff(vram, orig, VSIZE) < 0);
		printf("tracker display mode change: ok\n");
		tr.shutdown();
		ra_canvas_free(&c);
	}
	free(orig);
}

// ---------------------------------------------------------------------------
// threaded end to end: the real overlay thread + a fake SH4 thread
// ---------------------------------------------------------------------------

static const Mode& E2E_MODE = MODES[1];   // 565 640x480
static const uint32_t E2E_FB = 640 * 480 * 2;
static uint32_t e2e_regs[4];

// one frame rendered into `sof`, in four slices with gaps (the overlay
// thread runs meanwhile), announced like rend_start_render / rend_end_render
static void e2e_render(uint32_t sof, uint32_t frame)
{
	RefDims d = ref_dims(E2E_MODE);
	ra_overlay_render_start(sof, 0, false, 1280, 480);
	for (int part = 0; part < 4; part++) {
		for (int y = part * 120; y < (part + 1) * 120; y++)
			for (int x = 0; x < d.w; x++)
				ref_set(vram, sof, d, x, y, pattern(E2E_MODE.depth, frame, x, y));
		usleep(150);
	}
	ra_overlay_render_end();
}

static void e2e_vblank(uint32_t sof)
{
	if (ra_overlay_wants_vblank())
		ra_overlay_vblank(sof, e2e_regs[1], e2e_regs[2], e2e_regs[3]);
}

struct ListScript {
	std::atomic<int> phase{0};
	bool list_seen = false;
	bool detail_seen = false;
};

static void press(uint32_t b, int ms = 60)
{
	g_stub_btn = b;
	usleep(ms * 1000);
	g_stub_btn = 0;
	usleep(80 * 1000);
}

static void* list_driver(void* arg)
{
	ListScript* s = (ListScript*)arg;
	uint16_t bg = (uint16_t)ra_encode_rgb(1, ra_palette_rgb[RA_C_BG][0], ra_palette_rgb[RA_C_BG][1], ra_palette_rgb[RA_C_BG][2]);
	RefDims d = ref_dims(E2E_MODE);
	// wait for the list to be drawn (full screen: BG at the corner)
	for (int i = 0; i < 300 && !s->list_seen; i++) {
		usleep(10000);
		if (ref_get(vram, 0, d, 2, 2) == bg && ref_get(vram, 0, d, 637, 477) == bg)
			s->list_seen = true;
	}
	press(RA_MENU_DOWN);
	press(RA_MENU_DOWN);
	press(RA_MENU_R);         // page down
	press(RA_MENU_A);         // details
	usleep(100000);
	s->detail_seen = ref_get(vram, 0, d, 2, 2) == bg;
	press(RA_MENU_B);         // back to the list
	press(RA_MENU_B);         // close
	s->phase = 1;
	return NULL;
}

static void test_e2e()
{
	RefDims d = ref_dims(E2E_MODE);
	fill_random(vram, VSIZE);
	uint8_t* orig = (uint8_t*)malloc(VSIZE);
	memcpy(orig, vram, VSIZE);
	mode_regs(E2E_MODE, 0, e2e_regs);
	uint32_t sofs[2] = { 0x000000, E2E_FB };

	ra_overlay_test_start(vram, VMASK, 300, RA_POS_LEFT);
	CHECK(ra_overlay_running());

	uint32_t last[2] = { 0, 0 };
	e2e_render(sofs[0], 1);
	e2e_render(sofs[1], 2);
	last[0] = 1;
	last[1] = 2;
	ra_notify_local(RA_NOTIF_INFO, "Sonic Adventure", "7 of 25 achievements unlocked");
	ra_notify_local(RA_NOTIF_ACHIEVEMENT, "Ring Collector", "Collect 100 rings");
	ra_notify_local(RA_NOTIF_ERROR, "RetroAchievements login failed", "Invalid password");

	RaRegion r = ra_popup_layout(640, RA_POS_LEFT);
	uint16_t border = (uint16_t)ra_encode_rgb(1, ra_palette_rgb[RA_C_BORDER][0], ra_palette_rgb[RA_C_BORDER][1], ra_palette_rgb[RA_C_BORDER][2]);
	uint16_t red = (uint16_t)ra_encode_rgb(1, ra_palette_rgb[RA_C_RED][0], ra_palette_rgb[RA_C_RED][1], ra_palette_rgb[RA_C_RED][2]);
	uint16_t blue = (uint16_t)ra_encode_rgb(1, ra_palette_rgb[RA_C_BLUE][0], ra_palette_rgb[RA_C_BLUE][1], ra_palette_rgb[RA_C_BLUE][2]);
	int seen_blue = 0, seen_gold = 0, seen_red = 0, frames = 0;
	uint64_t t0 = 0;
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		t0 = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	}
	// ~60 fps worth of double-buffered frames while the three popups run
	for (uint32_t f = 3; ; f++) {
		int back = f & 1;
		e2e_render(sofs[back], f);
		last[back] = f;
		e2e_vblank(sofs[back]);
		usleep(2000);
		uint32_t corner = ref_get(vram, sofs[back], d, r.x, r.y * 2);
		seen_blue += corner == blue;
		seen_gold += corner == border;
		seen_red += corner == red;
		frames++;
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
		if (now - t0 > 1500 && !ra_ovl_flags.load())
			break;
		if (now - t0 > 15000) {
			printf("  e2e: overlay still active after 15 s\n");
			CHECK(false);
			break;
		}
	}
	printf("e2e popups: %d frames, popup corner seen: info %d, achievement %d, error %d\n",
		frames, seen_blue, seen_gold, seen_red);
	CHECK(seen_blue > 0 && seen_gold > 0 && seen_red > 0);
	CHECK(buffer_is_frame(vram, E2E_MODE, sofs[0], last[0]));
	CHECK(buffer_is_frame(vram, E2E_MODE, sofs[1], last[1]));
	CHECK(other_half_intact(orig, 0));

	// achievement list: hotkey -> pause at the next vblank -> scripted
	// navigation on another thread -> close -> resume
	ra_test_set_game_ready(1);
	ra_test_publish_list(sample_list(25));
	e2e_render(sofs[0], 1000);
	last[0] = 1000;
	e2e_vblank(sofs[0]);
	uint8_t* before = (uint8_t*)malloc(VSIZE);
	memcpy(before, vram, VSIZE);
	ListScript script;
	pthread_t drv;
	pthread_create(&drv, NULL, list_driver, &script);
	ra_overlay_hotkey();
	int polls0 = g_stub_polls.load();
	e2e_vblank(sofs[0]);   // parks here until the list is closed
	pthread_join(drv, NULL);
	CHECK(script.phase == 1);
	CHECK(script.list_seen);
	CHECK(script.detail_seen);
	CHECK(g_stub_polls.load() > polls0 + 10);
	CHECK(g_stub_suppress.load() == 1);
	CHECK(!g_stub_audio_paused.load());
	long diff = first_diff(vram, before, VSIZE);
	if (diff >= 0)
		printf("  list: VRAM differs after close at 0x%lx\n", diff);
	CHECK(diff < 0);
	printf("e2e list: opened, navigated, detail view, closed, VRAM restored\n");

	// hotkey with no game: no pause, an info popup instead
	ra_test_set_game_ready(0);
	ra_overlay_hotkey();
	e2e_vblank(sofs[0]);
	CHECK(g_stub_suppress.load() == 1);

	ra_overlay_shutdown();
	CHECK(!ra_overlay_running());
	free(before);
	free(orig);
}

// ---------------------------------------------------------------------------
// text + list logic
// ---------------------------------------------------------------------------

static void test_text()
{
	char out[128];
	ra_to_ascii("Caf\xC3\xA9 \xE2\x80\x9CQuoted\xE2\x80\x9D \xE2\x80\x94 it\xE2\x80\x99s\xE2\x80\xA6", out, sizeof(out));
	CHECK(!strcmp(out, "Cafe \"Quoted\" - it's..."));
	ra_to_ascii("\xE3\x82\xBD\xE3\x83\x8B\xE3\x83\x83\xE3\x82\xAF!", out, sizeof(out));   // 4 katakana
	CHECK(!strcmp(out, "?\?\?\?!"));
	ra_to_ascii("\xEF\xBC\xA1\xEF\xBC\xA2 bad\xFF\xC3", out, sizeof(out));   // full-width, invalid bytes
	CHECK(!strcmp(out, "AB bad??"));
	ra_to_ascii("tab\there\nnl", out, sizeof(out));
	CHECK(!strcmp(out, "tab here nl"));
	ra_to_ascii("Stra\xC3\x9F" "e", out, sizeof(out));
	CHECK(!strcmp(out, "Strasse"));
	ra_to_ascii("Stra\xC3\x9F" "e", out, 6);   // a replacement cut by the buffer end
	CHECK(!strcmp(out, "Stras"));

	ra_fit("Short", 10, out, sizeof(out));
	CHECK(!strcmp(out, "Short"));
	ra_fit("A long title here", 10, out, sizeof(out));
	CHECK(!strcmp(out, "A long..."));
	CHECK(strlen(out) <= 10);

	int n = ra_wrap("The quick brown fox jumps over the lazy dog", 10, 10, out, sizeof(out));
	CHECK(n == 5 && !strcmp(out, "The quick\nbrown fox\njumps over\nthe lazy\ndog"));
	n = ra_wrap("The quick brown fox jumps over the lazy dog", 10, 2, out, sizeof(out));
	CHECK(n == 2 && !strcmp(out, "The quick\nbrown f..."));
	n = ra_wrap("Supercalifragilistic", 8, 3, out, sizeof(out));
	CHECK(n == 3 && !strcmp(out, "Supercal\nifragili\nstic"));
	printf("text: ok\n");
}

static void test_list_logic()
{
	ra_list_data* d = sample_list(25);
	// unlocked first, stable within each group
	bool seen_locked = false;
	uint32_t prev_id[2] = { 0, 0 };
	for (uint32_t i = 0; i < d->count; i++) {
		if (!d->items[i].unlocked)
			seen_locked = true;
		CHECK(!(seen_locked && d->items[i].unlocked));
		int grp = d->items[i].unlocked ? 1 : 0;
		CHECK(d->items[i].id > prev_id[grp]);
		prev_id[grp] = d->items[i].id;
	}
	uint32_t unl = 0, pts = 0, upts = 0;
	for (uint32_t i = 0; i < d->count; i++) {
		pts += d->items[i].points;
		if (d->items[i].unlocked) {
			unl++;
			upts += d->items[i].points;
		}
	}
	CHECK(d->num_unlocked == unl && d->points_total == pts && d->points_unlocked == upts);

	RaListView v = {};
	v.per_page = ra_list_per_page(640);
	CHECK(v.per_page == 8);
	ra_list_nav(&v, 25, RA_NAV_UP);
	CHECK(v.sel == 0 && v.first == 0);
	for (int i = 0; i < 9; i++)
		ra_list_nav(&v, 25, RA_NAV_DOWN);
	CHECK(v.sel == 9 && v.first == 2);
	ra_list_nav(&v, 25, RA_NAV_PGDN);
	CHECK(v.sel == 17 && v.first == 10);
	ra_list_nav(&v, 25, RA_NAV_PGDN);
	CHECK(v.sel == 24 && v.first == 17);
	ra_list_nav(&v, 25, RA_NAV_PGUP);
	CHECK(v.sel == 16 && v.first == 9);
	ra_list_nav(&v, 25, RA_NAV_HOME);
	CHECK(v.sel == 0 && v.first == 0);
	ra_list_nav(&v, 25, RA_NAV_END);
	CHECK(v.sel == 24 && v.first == 17);
	RaListView e = {};
	e.per_page = 8;
	ra_list_nav(&e, 3, RA_NAV_PGDN);
	CHECK(e.sel == 2 && e.first == 0);
	ra_list_nav(&e, 0, RA_NAV_DOWN);
	CHECK(e.sel == 0 && e.first == 0);

	// rendering edge cases
	RaCanvas c;
	ra_canvas_init(&c, 320, 240);
	CHECK(ra_list_render(&c, NULL, v, true));
	ra_list_data empty = {};
	CHECK(ra_list_render(&c, &empty, e, true));
	v.detail = true;
	CHECK(ra_list_render(&c, d, v, true));
	ra_canvas_free(&c);
	ra_list_free(d);
	printf("list logic: ok\n");
}

// ---------------------------------------------------------------------------
// previews
// ---------------------------------------------------------------------------

// a stand-in game frame: sky gradient, ground, a few blocks
static void paint_scene(const Mode& m, uint32_t sof)
{
	RefDims d = ref_dims(m);
	for (int y = 0; y < d.h; y++) {
		float fy = (float)y / d.h;
		for (int x = 0; x < d.w; x++) {
			float fx = (float)x / d.w;
			uint8_t c[3];
			if (fy < 0.62f) {
				c[0] = (uint8_t)(70 + 80 * fy);
				c[1] = (uint8_t)(130 + 90 * fy);
				c[2] = (uint8_t)(230 - 20 * fy);
			} else {
				bool chk = (((int)(fx * 16) + (int)(fy * 24)) & 1) != 0;
				c[0] = chk ? 60 : 90;
				c[1] = chk ? 150 : 180;
				c[2] = chk ? 50 : 70;
			}
			float cx = fx - 0.7f, cy = fy - 0.35f;
			if (cx * cx * 1.8f + cy * cy < 0.012f) {
				c[0] = 250; c[1] = 220; c[2] = 60;
			}
			if (fx > 0.15f && fx < 0.3f && fy > 0.45f && fy < 0.62f) {
				c[0] = 170; c[1] = 90; c[2] = 50;
			}
			ref_set(vram, sof, d, x, y, ref_encode(m.depth, c));
		}
	}
}

// the displayed image scaled to 640x480 (as a 4:3 screen shows it)
static bool write_ppm(const char* path, const Mode& m, uint32_t sof)
{
	RefDims d = ref_dims(m);
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	fprintf(f, "P6\n640 480\n255\n");
	for (int oy = 0; oy < 480; oy++) {
		int y = oy * d.h / 480;
		for (int ox = 0; ox < 640; ox++) {
			int x = ox * d.w / 640;
			uint8_t rgb[3];
			ra_decode_pixel(m.depth, 0, ref_get(vram, sof, d, x, y), &rgb[0], &rgb[1], &rgb[2]);
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	printf("preview: %s\n", path);
	return true;
}

static void preview_popup(const char* dir, const char* name, const Mode& m, int pos, const RaPopupText& t)
{
	RaVram vm = test_vm();
	uint32_t sof = 0x010000;
	paint_scene(m, sof);
	RaGeom g = mode_geom(m, sof);
	RaRegion r = ra_popup_layout(g.src_w, pos);
	RaCanvas c;
	ra_canvas_init(&c, r.w, r.h);
	ra_popup_render(&c, g.src_w, r, t);
	RaTouched tt;
	RaPopWords pw = {};
	ra_touched_init(&tt, g, r);
	ra_pop_build(&pw, g, r, c);
	ra_touched_refresh(&tt, vm, pw, true, NULL);
	char path[512];
	snprintf(path, sizeof(path), "%s/ra_popup_preview_%s.ppm", dir, name);
	CHECK(write_ppm(path, m, sof));
	ra_touched_free(&tt);
	ra_pop_free(&pw);
	ra_canvas_free(&c);
}

static void preview_list(const char* dir, const char* name, const Mode& m, RaListView v)
{
	RaVram vm = test_vm();
	uint32_t sof = 0x010000;
	paint_scene(m, sof);
	RaGeom g = mode_geom(m, sof);
	RaRegion r = { 0, 0, g.src_w, 240 };
	RaCanvas c;
	ra_canvas_init(&c, r.w, r.h);
	ra_list_data* d = sample_list(25);
	v.per_page = ra_list_per_page(g.src_w);
	ra_list_render(&c, d, v, true);
	RaTouched tt;
	RaPopWords pw = {};
	ra_touched_init(&tt, g, r);
	ra_pop_build(&pw, g, r, c);
	ra_touched_refresh(&tt, vm, pw, true, NULL);
	char path[512];
	snprintf(path, sizeof(path), "%s/ra_popup_preview_%s.ppm", dir, name);
	CHECK(write_ppm(path, m, sof));
	ra_touched_free(&tt);
	ra_pop_free(&pw);
	ra_canvas_free(&c);
	ra_list_free(d);
}

static void previews(const char* dir)
{
	RaPopupText ach = {};
	snprintf(ach.line1, sizeof(ach.line1), "Achievement unlocked");
	snprintf(ach.line2, sizeof(ach.line2), "Chaos Emerald Hunter (25)");
	ach.accent = RA_C_BORDER;
	RaPopupText game = {};
	snprintf(game.line1, sizeof(game.line1), "Sonic Adventure");
	snprintf(game.line2, sizeof(game.line2), "7 of 25 achievements unlocked");
	game.accent = RA_C_BLUE;
	RaPopupText err = {};
	snprintf(err.line1, sizeof(err.line1), "RetroAchievements login failed");
	snprintf(err.line2, sizeof(err.line2), "Invalid User/Password combination. Please try again");
	err.accent = RA_C_RED;

	preview_popup(dir, "popup_240p_565_left", MODES[0], RA_POS_LEFT, ach);
	preview_popup(dir, "popup_480i_0888_center", MODES[4], RA_POS_CENTER, game);
	preview_popup(dir, "popup_320x240_565_right", MODES[5], RA_POS_RIGHT, err);
	RaListView v = {};
	v.sel = 2;
	preview_list(dir, "list_480i_565", MODES[1], v);
	v.sel = 12;
	v.first = 8;
	preview_list(dir, "list_320x240_565", MODES[5], v);
	v.sel = 0;
	v.first = 0;
	v.detail = true;
	preview_list(dir, "list_detail_240p_565", MODES[0], v);
}

int main(int argc, char** argv)
{
	vram = (uint8_t*)malloc(VSIZE);
	test_encode();
	test_geometry();
	test_text();
	test_list_logic();
	test_modes();
	test_fresh_pixels();
	test_tracker();
	test_e2e();
	if (argc > 1)
		previews(argv[1]);
	free(vram);
	printf(failures ? "ra_osd_test: %d FAILURE(S)\n" : "ra_osd_test: all ok\n", failures);
	return failures ? 1 : 0;
}
