// ra_osd.cpp -- framebuffer drawing for the RetroAchievements overlay; see
// ra_osd.h for the framebuffer model and the save/restore rules.

#include "ra_osd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <atomic>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define RA_OSD_NEON 1
#endif

// ---------------------------------------------------------------------------
// geometry
// ---------------------------------------------------------------------------

RaGeom ra_geom_from_regs(uint32_t sof1, uint32_t ctrl, uint32_t vo, uint32_t spg)
{
	static const uint8_t bpp_by_depth[4] = { 2, 2, 3, 4 };
	static const uint32_t stride640[4] = { 1280, 1280, 1920, 2560 };
	RaGeom g;
	memset(&g, 0, sizeof(g));
	g.sof = sof1 & 0x7FFFFCu;
	g.enable = ctrl & 1u;
	g.depth = (ctrl >> 2) & 3u;
	g.concat = (ctrl >> 4) & 7u;
	g.pixdbl = (vo >> 8) & 1u;
	// sys_top.v: fb_disp_linedbl = fb_line_double || (!vclk_div && !interlace)
	bool vga = (ctrl >> 23) & 1u;
	bool ilace = (spg >> 4) & 1u;
	bool linedbl = ((ctrl >> 1) & 1u) || (!vga && !ilace);
	g.src_w = g.pixdbl ? 320 : 640;
	g.src_h = linedbl ? 240 : 480;
	g.vs = (uint8_t)(g.src_h / 240);
	g.bpp = bpp_by_depth[g.depth];
	g.stride = stride640[g.depth] >> (g.pixdbl ? 1 : 0);
	return g;
}

bool ra_geom_same_layout(const RaGeom& a, const RaGeom& b)
{
	return a.depth == b.depth && a.pixdbl == b.pixdbl && a.src_h == b.src_h &&
		a.src_w == b.src_w && a.stride == b.stride;
}

// ---------------------------------------------------------------------------
// VRAM access
// ---------------------------------------------------------------------------

void ra_vram_read(const RaVram& vm, uint32_t vword, uint32_t half, uint32_t n, uint32_t* out)
{
	__asm__ volatile("" ::: "memory");
	uint64_t first = (uint64_t)vword << 3, last = ((uint64_t)vword + n) << 3;
	if (n && last - 1 <= vm.mask) {
		// no wrap inside the run: a straight de-interleave of our halves
		const uint32_t* p = (const uint32_t*)(vm.base + first);
		uint32_t i = 0;
#ifdef RA_OSD_NEON
		// 32-byte loads: one burst for four of our words on the
		// write-combined (normal non-cacheable) mapping
		if (half) {
			for (; i + 4 <= n; i += 4)
				vst1q_u32(out + i, vld2q_u32(p + 2 * i).val[1]);
		} else {
			for (; i + 4 <= n; i += 4)
				vst1q_u32(out + i, vld2q_u32(p + 2 * i).val[0]);
		}
#endif
		const volatile uint32_t* vp = p;
		for (; i < n; i++)
			out[i] = vp[2 * i + half];
		return;
	}
	for (uint32_t i = 0; i < n; i++)
		out[i] = *(const volatile uint32_t*)(vm.base + ra_vram_off(vm, vword + i, half));
}

void ra_vram_flush(void)
{
#if defined(__arm__) || defined(__aarch64__)
	__asm__ volatile("dsb sy" ::: "memory");
#else
	std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

// ---------------------------------------------------------------------------
// colours
// ---------------------------------------------------------------------------

const uint8_t ra_palette_rgb[RA_C_COUNT][3] = {
	{  16,  24,  56 },   // BG
	{ 232, 180,  40 },   // BORDER
	{ 240, 240, 240 },   // TEXT
	{ 255, 208,  64 },   // GOLD
	{ 150, 158, 180 },   // DIM
	{  48,  72, 150 },   // HILITE
	{ 255,  96,  88 },   // RED
	{ 120, 184, 255 },   // BLUE
	{  96, 224, 112 },   // GREEN
	{  80,  96, 140 },   // SEP
};

uint32_t ra_encode_rgb(int depth, uint8_t r, uint8_t g, uint8_t b)
{
	switch (depth) {
	case 0: return ((uint32_t)(r >> 3) << 10) | ((uint32_t)(g >> 3) << 5) | (b >> 3);
	case 1: return ((uint32_t)(r >> 3) << 11) | ((uint32_t)(g >> 2) << 5) | (b >> 3);
	default: return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16);   // 888 / 0888
	}
}

void ra_decode_pixel(int depth, int concat, uint32_t p, uint8_t* r, uint8_t* g, uint8_t* b)
{
	uint32_t p16 = p & 0xFFFFu;
	switch (depth) {
	case 0:
		*r = (uint8_t)((((p16 >> 10) & 0x1F) << 3) | concat);
		*g = (uint8_t)((((p16 >> 5) & 0x1F) << 3) | concat);
		*b = (uint8_t)(((p16 & 0x1F) << 3) | concat);
		break;
	case 1:
		*r = (uint8_t)((((p16 >> 11) & 0x1F) << 3) | concat);
		*g = (uint8_t)((((p16 >> 5) & 0x3F) << 2) | (concat >> 1));
		*b = (uint8_t)(((p16 & 0x1F) << 3) | concat);
		break;
	default:
		*r = (uint8_t)(p >> 16);
		*g = (uint8_t)(p >> 8);
		*b = (uint8_t)p;
		break;
	}
}

// ---------------------------------------------------------------------------
// canvas
// ---------------------------------------------------------------------------

bool ra_canvas_init(RaCanvas* c, int w, int h)
{
	c->w = w;
	c->h = h;
	c->px = (uint8_t*)calloc((size_t)w * h, 1);
	return c->px != NULL;
}

void ra_canvas_free(RaCanvas* c)
{
	free(c->px);
	c->px = NULL;
	c->w = c->h = 0;
}

void ra_canvas_fill(RaCanvas* c, int x, int y, int w, int h, uint8_t color)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
	for (int yy = y0; yy < y1; yy++)
		if (x1 > x0)
			memset(c->px + (size_t)yy * c->w + x0, color, (size_t)(x1 - x0));
}

void ra_canvas_glyph(RaCanvas* c, int x, int y, uint8_t code, uint8_t color)
{
	if (code >= 0x7F)
		code = '?';
	const uint8_t* g = ra_font8x8[code];
	for (int r = 0; r < 8; r++) {
		int yy = y + r;
		if (yy < 0 || yy >= c->h || !g[r])
			continue;
		uint8_t* line = c->px + (size_t)yy * c->w;
		for (int b = 0; b < 8; b++) {
			int xx = x + b;
			if ((g[r] & (0x80u >> b)) && xx >= 0 && xx < c->w)
				line[xx] = color;
		}
	}
}

int ra_canvas_text(RaCanvas* c, int x, int y, const char* s, uint8_t color, int max_chars)
{
	int n = 0;
	for (; s[n] && (max_chars < 0 || n < max_chars); n++)
		ra_canvas_glyph(c, x + 8 * n, y, (uint8_t)s[n], color);
	return n;
}

// ---------------------------------------------------------------------------
// text
// ---------------------------------------------------------------------------

// U+00C0..U+00FF -> ASCII (Latin-1 letters without their accents)
static const char* const latin1_map[64] = {
	"A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
	"D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
	"a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
	"d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y",
};

static const char* map_codepoint(uint32_t cp, char* one)
{
	if (cp >= 0xC0 && cp <= 0xFF)
		return latin1_map[cp - 0xC0];
	if (cp >= 0xFF01 && cp <= 0xFF5E) {   // full-width ASCII
		one[0] = (char)(cp - 0xFEE0);
		one[1] = 0;
		return one;
	}
	switch (cp) {
	case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x3000: return " ";
	case 0xA9: return "(C)";
	case 0xAE: return "(R)";
	case 0xB0: return "o";
	case 0xB7: case 0x2022: case 0x30FB: return "*";
	case 0xAB: case 0xBB: case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
	case 0xB4: case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
	case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
	case 0x2026: return "...";
	case 0x2122: return "TM";
	case 0x0152: return "OE";
	case 0x0153: return "oe";
	case 0x2190: return "<-";
	case 0x2192: return "->";
	case 0x00D7: return "x";
	default: return "?";
	}
}

void ra_to_ascii(const char* utf8, char* out, size_t out_size)
{
	if (!out_size)
		return;
	size_t o = 0;
	const unsigned char* p = (const unsigned char*)(utf8 ? utf8 : "");
	while (*p && o + 1 < out_size) {
		unsigned c = *p;
		if (c < 0x80) {
			out[o++] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
			p++;
			continue;
		}
		int len = c >= 0xF0 && c <= 0xF7 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c <= 0xDF ? 2 : 0;
		if (c >= 0xF8)
			len = 0;
		uint32_t cp = len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
		int ok = len > 0;
		for (int k = 1; ok && k < len; k++) {
			if ((p[k] & 0xC0) != 0x80)
				ok = 0;
			else
				cp = (cp << 6) | (p[k] & 0x3F);
		}
		char one[2];
		const char* rep = ok ? map_codepoint(cp, one) : "?";
		p += ok ? len : 1;
		for (; *rep && o + 1 < out_size; rep++)
			out[o++] = *rep;
	}
	out[o] = '\0';
}

void ra_fit(const char* s, int width, char* out, size_t out_size)
{
	if (!out_size)
		return;
	size_t len = strlen(s);
	if (width < 0)
		width = 0;
	if (len <= (size_t)width) {
		snprintf(out, out_size, "%s", s);
		return;
	}
	if (width < 4) {
		snprintf(out, out_size, "%.*s", width, s);
		return;
	}
	int keep = width - 3;
	while (keep > 0 && s[keep - 1] == ' ')
		keep--;
	snprintf(out, out_size, "%.*s...", keep, s);
}

int ra_wrap(const char* s, int width, int max_lines, char* out, size_t out_size)
{
	if (!out_size)
		return 0;
	out[0] = '\0';
	if (width < 1 || max_lines < 1)
		return 0;
	size_t o = 0;
	int lines = 0;
	const char* p = s;
	char line[512];
	while (lines < max_lines) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		size_t len = strlen(p);
		size_t take;
		if (len <= (size_t)width) {
			take = len;
		} else {
			take = 0;
			for (size_t k = 1; k <= (size_t)width; k++)   // break at the last space that fits
				if (p[k] == ' ')
					take = k;
			if (!take)
				take = (size_t)width;   // one long word: hard break
		}
		const char* rest = p + take;
		while (*rest == ' ')
			rest++;
		if (lines == max_lines - 1 && *rest) {
			ra_fit(p, width, line, sizeof(line));   // last line with text left over
			take = len;
		} else {
			size_t n = take < sizeof(line) - 1 ? take : sizeof(line) - 1;
			memcpy(line, p, n);
			while (n && line[n - 1] == ' ')
				n--;
			line[n] = '\0';
		}
		int w = snprintf(out + o, out_size - o, "%s%s", lines ? "\n" : "", line);
		if (w < 0 || (size_t)w >= out_size - o) {
			out[o] = '\0';
			break;
		}
		o += (size_t)w;
		lines++;
		p += take;
	}
	return lines;
}

// ---------------------------------------------------------------------------
// regions
// ---------------------------------------------------------------------------

RaSpan ra_region_span(const RaGeom& g, const RaRegion& r)
{
	RaSpan s;
	s.y0 = (uint32_t)r.y * g.vs;
	s.ny = (uint32_t)r.h * g.vs;
	uint32_t b0 = (uint32_t)r.x * g.bpp, b1 = (uint32_t)(r.x + r.w) * g.bpp;
	s.wl0 = b0 / 4;
	s.nw = (b1 + 3) / 4 - s.wl0;
	s.mask_first = 0xFFFFFFFFu << (8 * (b0 & 3));
	s.mask_last = (b1 & 3) ? (1u << (8 * (b1 & 3))) - 1 : 0xFFFFFFFFu;
	return s;
}

RaRange ra_region_range(const RaGeom& g, const RaSpan& s)
{
	RaRange r;
	r.lo = g.sof + s.y0 * g.stride;
	r.hi = r.lo + s.ny * g.stride;
	return r;
}

bool ra_pop_build(RaPopWords* p, const RaGeom& g, const RaRegion& r, const RaCanvas& c)
{
	if (c.w != r.w || c.h != r.h || !c.px)
		return false;
	RaSpan s = ra_region_span(g, r);
	uint32_t* val = (uint32_t*)malloc((size_t)r.h * s.nw * 4);
	uint8_t* lb = (uint8_t*)malloc((size_t)s.nw * 4);
	if (!val || !lb) {
		free(val);
		free(lb);
		return false;
	}
	uint32_t enc[RA_C_COUNT];
	for (int i = 0; i < RA_C_COUNT; i++)
		enc[i] = ra_encode_rgb(g.depth, ra_palette_rgb[i][0], ra_palette_rgb[i][1], ra_palette_rgb[i][2]);
	for (int row = 0; row < r.h; row++) {
		memset(lb, 0, (size_t)s.nw * 4);
		const uint8_t* src = c.px + (size_t)row * c.w;
		for (int x = 0; x < r.w; x++) {
			uint32_t e = enc[src[x] < RA_C_COUNT ? src[x] : (uint8_t)RA_C_BG];
			uint32_t off = (uint32_t)(r.x + x) * g.bpp - s.wl0 * 4;
			for (uint32_t k = 0; k < g.bpp; k++)
				lb[off + k] = (uint8_t)(e >> (8 * k));
		}
		uint32_t* dst = val + (size_t)row * s.nw;
		for (uint32_t i = 0; i < s.nw; i++)
			dst[i] = (uint32_t)lb[4 * i] | ((uint32_t)lb[4 * i + 1] << 8) |
				((uint32_t)lb[4 * i + 2] << 16) | ((uint32_t)lb[4 * i + 3] << 24);
	}
	free(lb);
	ra_pop_free(p);
	p->rows = (uint32_t)r.h;
	p->nw = s.nw;
	p->val = val;
	return true;
}

void ra_pop_free(RaPopWords* p)
{
	free(p->val);
	p->val = NULL;
	p->rows = p->nw = 0;
}

// Pixels whose content differs, as a byte mask over one word: 16-bit pixels
// for 0555/565, the whole word for 0888. (Packed 888 is done per pixel
// across words in line_changes.)
static inline uint32_t unit_diff(uint32_t d, int depth)
{
	if (depth <= 1)
		return ((d & 0x0000FFFFu) ? 0x0000FFFFu : 0) | ((d & 0xFFFF0000u) ? 0xFFFF0000u : 0);
	return d ? 0xFFFFFFFFu : 0;
}

static inline uint32_t mask_pixels(uint32_t m, int bpp)
{
	uint32_t bytes = ((m & 0xFFu) ? 1 : 0) + ((m & 0xFF00u) ? 1 : 0) + ((m & 0xFF0000u) ? 1 : 0) + ((m >> 24) ? 1 : 0);
	return bpp ? (bytes + bpp - 1) / bpp : 0;
}

static inline uint32_t word_byte(const uint32_t* w, uint32_t j)
{
	return (w[j >> 2] >> ((j & 3) * 8)) & 0xFFu;
}

// ch[i]: byte mask of the region's pixels (whole pixels) in line word i
// whose bytes differ between a and b
static void line_changes(const RaTouched* t, const uint32_t* a, const uint32_t* b, uint32_t* ch)
{
	const RaSpan& s = t->span;
	const RaGeom& g = t->geom;
	if (g.bpp != 3) {   // 16/32-bit pixels never straddle words
		for (uint32_t i = 0; i < s.nw; i++)
			ch[i] = unit_diff(a[i] ^ b[i], g.depth) & ra_span_mask(s, i);
		return;
	}
	memset(ch, 0, s.nw * 4);
	uint32_t b0 = (uint32_t)t->rgn.x * 3 - s.wl0 * 4;
	for (int px = 0; px < t->rgn.w; px++) {
		uint32_t bo = b0 + (uint32_t)px * 3;
		if (word_byte(a, bo) == word_byte(b, bo) && word_byte(a, bo + 1) == word_byte(b, bo + 1) &&
			word_byte(a, bo + 2) == word_byte(b, bo + 2))
			continue;
		for (uint32_t k = 0; k < 3; k++)
			ch[(bo + k) >> 2] |= 0xFFu << (((bo + k) & 3) * 8);
	}
}

bool ra_touched_init(RaTouched* t, const RaGeom& g, const RaRegion& r)
{
	memset(t, 0, sizeof(*t));
	t->geom = g;
	t->rgn = r;
	t->span = ra_region_span(g, r);
	size_t n = (size_t)t->span.ny * t->span.nw;
	t->under = (uint32_t*)calloc(n, 4);
	t->drawn = (uint32_t*)calloc(n, 4);
	if (!t->under || !t->drawn) {
		ra_touched_free(t);
		return false;
	}
	return true;
}

void ra_touched_free(RaTouched* t)
{
	free(t->under);
	free(t->drawn);
	t->under = t->drawn = NULL;
	t->have = false;
}

void ra_touched_refresh(RaTouched* t, const RaVram& vm, const RaPopWords& p, bool read, RaOpStats* st)
{
	const RaSpan& s = t->span;
	const RaGeom& g = t->geom;
	if (p.nw != s.nw || p.rows * g.vs != s.ny)
		return;
	if (!t->have)
		read = true;
	uint32_t half = (g.sof >> 22) & 1u;
	uint32_t vbase = (g.sof & 0x3FFFFCu) >> 2;
	uint32_t lw = g.stride / 4;
	uint32_t cur[640], chg[640];   // widest line: 640 x 4 bytes
	if (s.nw > 640)
		return;
	for (uint32_t l = 0; l < s.ny; l++) {
		const uint32_t* pv = p.val + (size_t)(l / g.vs) * s.nw;
		uint32_t vw = vbase + (s.y0 + l) * lw + s.wl0;
		uint32_t* under = t->under + (size_t)l * s.nw;
		uint32_t* drawn = t->drawn + (size_t)l * s.nw;
		if (read) {
			ra_vram_read(vm, vw, half, s.nw, cur);
			if (st)
				st->words_read += s.nw;
		} else {
			memcpy(cur, drawn, s.nw * 4);
		}
		if (t->have)
			line_changes(t, cur, drawn, chg);
		for (uint32_t i = 0; i < s.nw; i++) {
			uint32_t m = ra_span_mask(s, i);
			// our pixels the game has rewritten since we drew: new "under"
			uint32_t ch = t->have ? chg[i] : m;
			if (ch) {
				under[i] = (under[i] & ~ch) | (cur[i] & ch);
				if (st && t->have)
					st->fresh_units += mask_pixels(ch, g.bpp);
			}
			uint32_t nd = (cur[i] & ~m) | (pv[i] & m);
			if (nd != cur[i]) {
				ra_vram_write(vm, vw + i, half, nd);
				if (st)
					st->words_written++;
			}
			drawn[i] = nd;
		}
	}
	t->have = true;
	if (st)
		st->refreshes++;
}

void ra_touched_restore(RaTouched* t, const RaVram& vm, RaOpStats* st)
{
	if (!t->have)
		return;
	const RaSpan& s = t->span;
	const RaGeom& g = t->geom;
	uint32_t half = (g.sof >> 22) & 1u;
	uint32_t vbase = (g.sof & 0x3FFFFCu) >> 2;
	uint32_t lw = g.stride / 4;
	uint32_t cur[640], chg[640];
	if (s.nw > 640)
		return;
	for (uint32_t l = 0; l < s.ny; l++) {
		uint32_t vw = vbase + (s.y0 + l) * lw + s.wl0;
		const uint32_t* under = t->under + (size_t)l * s.nw;
		const uint32_t* drawn = t->drawn + (size_t)l * s.nw;
		ra_vram_read(vm, vw, half, s.nw, cur);
		if (st)
			st->words_read += s.nw;
		line_changes(t, cur, drawn, chg);
		for (uint32_t i = 0; i < s.nw; i++) {
			uint32_t m = ra_span_mask(s, i);
			uint32_t changed = chg[i];
			uint32_t intact = m & ~changed;
			if (st && changed)
				st->kept_units += mask_pixels(changed, g.bpp);
			uint32_t nd = (cur[i] & ~intact) | (under[i] & intact);
			if (nd != cur[i]) {
				ra_vram_write(vm, vw + i, half, nd);
				if (st)
					st->words_written++;
			}
		}
	}
	t->have = false;
	if (st)
		st->restores++;
}

bool ra_touched_intact(const RaTouched* t, const RaVram& vm)
{
	if (!t->have)
		return false;
	const RaSpan& s = t->span;
	const RaGeom& g = t->geom;
	uint32_t half = (g.sof >> 22) & 1u;
	uint32_t vbase = (g.sof & 0x3FFFFCu) >> 2;
	uint32_t lw = g.stride / 4;
	for (int a = 0; a < 4; a++) {
		uint32_t l = s.ny > 1 ? (uint32_t)a * (s.ny - 1) / 3 : 0;
		for (int b = 0; b < 4; b++) {
			uint32_t i = s.nw > 1 ? (uint32_t)b * (s.nw - 1) / 3 : 0;
			uint32_t cur = *(const volatile uint32_t*)(vm.base +
				ra_vram_off(vm, vbase + (s.y0 + l) * lw + s.wl0 + i, half));
			if (unit_diff(cur ^ t->drawn[(size_t)l * s.nw + i], g.depth) & ra_span_mask(s, i))
				return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// tracker
// ---------------------------------------------------------------------------

void RaTracker::init(const RaVram& vm, RaGuard* guard)
{
	vm_ = vm;
	guard_ = guard;
}

void RaTracker::shutdown()
{
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (e_[i].used) {
			ra_touched_free(&e_[i].t);
			e_[i].used = false;
		}
	ra_pop_free(&pop_);
	content_ = false;
}

bool RaTracker::set_content(const RaGeom& g, const RaRegion& r, const RaCanvas* c)
{
	if (!c) {
		content_ = false;
		ra_pop_free(&pop_);
		return true;
	}
	bool same = layout_ && ra_geom_same_layout(g, geom_) && r.x == rgn_.x && r.y == rgn_.y &&
		r.w == rgn_.w && r.h == rgn_.h;
	RaPopWords np = {};
	if (!ra_pop_build(&np, g, r, *c))
		return false;
	ra_pop_free(&pop_);
	pop_ = np;
	if (!same)
		layout_++;   // buffers drawn with the old layout are only restored
	geom_ = g;
	rgn_ = r;
	gen_++;
	content_ = true;
	return true;
}

RaTracker::Ent* RaTracker::find(uint32_t sof)
{
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (e_[i].used && e_[i].t.geom.sof == sof)
			return &e_[i];
	return nullptr;
}

int RaTracker::count() const
{
	int n = 0;
	for (int i = 0; i < MAX_BUFFERS; i++)
		n += e_[i].used;
	return n;
}

bool RaTracker::refresh(Ent& e)
{
	RaRange rr = ra_touched_range(&e.t);
	if (guard_ && !guard_->begin(rr)) {
		guard_skips++;
		return false;
	}
	ra_touched_refresh(&e.t, vm_, pop_, true, &st);
	ra_vram_flush();
	if (guard_)
		guard_->end();
	e.dirty = false;
	e.gen = gen_;
	return true;
}

bool RaTracker::restore(Ent& e)
{
	RaRange rr = ra_touched_range(&e.t);
	if (guard_ && !guard_->begin(rr)) {
		guard_skips++;
		return false;
	}
	ra_touched_restore(&e.t, vm_, &st);
	ra_vram_flush();
	if (guard_)
		guard_->end();
	ra_touched_free(&e.t);
	e.used = false;
	return true;
}

void RaTracker::render_event(const RaRange& a, const RaRange& b)
{
	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (!e_[i].used)
			continue;
		RaRange r = ra_touched_range(&e_[i].t);
		if (ra_range_overlap(r, a) || ra_range_overlap(r, b))
			e_[i].dirty = true;
	}
}

void RaTracker::mark_all_dirty()
{
	for (int i = 0; i < MAX_BUFFERS; i++)
		e_[i].dirty = true;
}

bool RaTracker::vblank(uint32_t disp_sof, uint32_t vbl)
{
	vbl_ = vbl;
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (e_[i].used && e_[i].layout != layout_)
			restore(e_[i]);
	if (!content_)
		return false;

	Ent* d = find(disp_sof);
	if (!d) {
		RaGeom g = geom_;
		g.sof = disp_sof;
		RaRange nr = ra_region_range(g, ra_region_span(g, rgn_));
		// a buffer overlapping the new one (the game scrolls/shakes the
		// display address): take our pixels out of it first
		for (int i = 0; i < MAX_BUFFERS; i++)
			if (e_[i].used && ra_range_overlap(ra_touched_range(&e_[i].t), nr) && !restore(e_[i]))
				return false;
		if (count() >= MAX_BUFFERS) {
			Ent* lru = nullptr;
			for (int i = 0; i < MAX_BUFFERS; i++)
				if (e_[i].used && (!lru || vbl - e_[i].last_disp > vbl - lru->last_disp))
					lru = &e_[i];
			if (lru && !restore(*lru))
				return false;
		}
		for (int i = 0; i < MAX_BUFFERS && !d; i++)
			if (!e_[i].used)
				d = &e_[i];
		if (!d || !ra_touched_init(&d->t, g, rgn_))
			return false;
		d->used = true;
		d->dirty = true;
		d->gen = gen_ - 1;
		d->layout = layout_;
	}
	d->last_disp = vbl;
	// something else (CPU-drawn FMV / 2D) wrote over the popup
	if (!d->dirty && d->gen == gen_ && !ra_touched_intact(&d->t, vm_))
		d->dirty = true;
	bool ok = true;
	if (d->dirty || d->gen != gen_)
		ok = refresh(*d);

	for (int i = 0; i < MAX_BUFFERS; i++)
		if (e_[i].used && &e_[i] != d && vbl - e_[i].last_disp > EVICT_VBLANKS)
			restore(e_[i]);
	return ok;
}

void RaTracker::render_done()
{
	for (int i = 0; i < MAX_BUFFERS; i++) {
		Ent& e = e_[i];
		if (!e.used)
			continue;
		if (e.layout != layout_) {
			restore(e);
			continue;
		}
		if (content_ && (e.dirty || e.gen != gen_) && vbl_ - e.last_disp <= ACTIVE_VBLANKS)
			refresh(e);
	}
}

int RaTracker::restore_all()
{
	int left = 0;
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (e_[i].used && !restore(e_[i]))
			left++;
	return left;
}

// ---------------------------------------------------------------------------
// popup
// ---------------------------------------------------------------------------

int ra_parse_position(const char* val)
{
	char v[16];
	snprintf(v, sizeof(v), "%s", val ? val : "");
	size_t n = strlen(v);
	while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
		v[--n] = '\0';
	if (!strcasecmp(v, "left") || !strcmp(v, "0"))
		return RA_POS_LEFT;
	if (!strcasecmp(v, "center") || !strcasecmp(v, "centre") || !strcmp(v, "1"))
		return RA_POS_CENTER;
	if (!strcasecmp(v, "right") || !strcmp(v, "2"))
		return RA_POS_RIGHT;
	return -1;
}

const char* ra_position_name(int pos)
{
	return pos == RA_POS_CENTER ? "center" : pos == RA_POS_RIGHT ? "right" : "left";
}

// Box: 36 text columns + 8 px padding each side, 24 rows (2 text rows),
// 16 rows down from the top and 32 px in from the side (640-wide sources;
// 8 px on 320-wide ones) - inside the title-safe area of a CRT. Width and x
// are multiples of 4 pixels, so every depth covers whole VRAM words.
static const int POPUP_COLS = 36, POPUP_H = 24, POPUP_TOP = 16;

RaRegion ra_popup_layout(int src_w, int position)
{
	int margin = src_w >= 640 ? 32 : 8;
	int cols = (src_w - 2 * margin - 16) / 8;
	if (cols > POPUP_COLS)
		cols = POPUP_COLS;
	RaRegion r;
	r.w = cols * 8 + 16;
	r.h = POPUP_H;
	r.y = POPUP_TOP;
	if (position == RA_POS_CENTER)
		r.x = (src_w - r.w) / 2;
	else if (position == RA_POS_RIGHT)
		r.x = src_w - margin - r.w;
	else
		r.x = margin;
	r.x &= ~3;
	return r;
}

bool ra_popup_render(RaCanvas* c, int src_w, const RaRegion& r, const RaPopupText& t)
{
	if (c->w != r.w || c->h != r.h)
		return false;
	int cols = (r.w - 16) / 8;
	int bw = src_w >= 640 ? 2 : 1;   // side border: about one 240p line wide on screen
	ra_canvas_fill(c, 0, 0, r.w, r.h, RA_C_BG);
	ra_canvas_fill(c, 0, 0, r.w, 1, t.accent);
	ra_canvas_fill(c, 0, r.h - 1, r.w, 1, t.accent);
	ra_canvas_fill(c, 0, 0, bw, r.h, t.accent);
	ra_canvas_fill(c, r.w - bw, 0, bw, r.h, t.accent);
	char buf[160];
	ra_fit(t.line1, cols, buf, sizeof(buf));
	ra_canvas_text(c, 8, 4, buf, t.accent == RA_C_BORDER ? (uint8_t)RA_C_GOLD : t.accent, cols);
	ra_fit(t.line2, cols, buf, sizeof(buf));
	ra_canvas_text(c, 8, 13, buf, RA_C_TEXT, cols);
	return true;
}

// ---------------------------------------------------------------------------
// achievement list
// ---------------------------------------------------------------------------

void ra_list_free(ra_list_data* d)
{
	if (!d)
		return;
	free(d->items);
	free(d);
}

void ra_list_sort(ra_list_data* d)
{
	if (!d || d->count < 2)
		return;
	ra_list_item* tmp = (ra_list_item*)malloc(sizeof(ra_list_item) * d->count);
	if (!tmp)
		return;
	uint32_t o = 0;
	for (int pass = 0; pass < 2; pass++)
		for (uint32_t i = 0; i < d->count; i++)
			if ((d->items[i].unlocked != 0) == (pass == 0))
				tmp[o++] = d->items[i];
	memcpy(d->items, tmp, sizeof(ra_list_item) * d->count);
	free(tmp);
}

void ra_list_totals(ra_list_data* d)
{
	d->num_unlocked = d->points_total = d->points_unlocked = 0;
	for (uint32_t i = 0; i < d->count; i++) {
		d->points_total += d->items[i].points;
		if (d->items[i].unlocked) {
			d->num_unlocked++;
			d->points_unlocked += d->items[i].points;
		}
	}
}

// Page layout, in 240p rows, 12+ rows clear of the top and bottom edges
// (CRT overscan): title 12, totals 22, rule 32, entries from 36 (20 rows
// each: title line, description line), rule 216, help line 219.
static const int LIST_TOP = 12, LIST_ENTRIES_Y = 36, LIST_ENTRY_H = 20;
static const int LIST_FOOT_RULE = 216, LIST_FOOT_Y = 219;

static int list_margin(int w) { return w >= 640 ? 32 : 12; }

int ra_list_per_page(int src_w)
{
	(void)src_w;
	return (LIST_FOOT_RULE - 4 - LIST_ENTRIES_Y) / LIST_ENTRY_H;
}

void ra_list_nav(RaListView* v, int count, int action)
{
	int pp = v->per_page > 0 ? v->per_page : 1;
	if (count <= 0) {
		v->sel = v->first = 0;
		return;
	}
	switch (action) {
	case RA_NAV_UP: v->sel--; break;
	case RA_NAV_DOWN: v->sel++; break;
	case RA_NAV_PGUP: v->sel -= pp; v->first -= pp; break;
	case RA_NAV_PGDN: v->sel += pp; v->first += pp; break;
	case RA_NAV_HOME: v->sel = 0; break;
	case RA_NAV_END: v->sel = count - 1; break;
	}
	if (v->sel < 0) v->sel = 0;
	if (v->sel > count - 1) v->sel = count - 1;
	int max_first = count > pp ? count - pp : 0;
	if (v->first > max_first) v->first = max_first;
	if (v->first < 0) v->first = 0;
	if (v->sel < v->first) v->first = v->sel;
	if (v->sel >= v->first + pp) v->first = v->sel - pp + 1;
}

static void text_right(RaCanvas* c, int right_x, int y, const char* s, uint8_t color)
{
	ra_canvas_text(c, right_x - 8 * (int)strlen(s), y, s, color, -1);
}

static void list_footer(RaCanvas* c, int mx, int cols, const char* help, const char* pos)
{
	ra_canvas_fill(c, mx, LIST_FOOT_RULE, c->w - 2 * mx, 1, RA_C_SEP);
	char buf[160];
	ra_fit(help, cols - (int)strlen(pos) - 1, buf, sizeof(buf));
	ra_canvas_text(c, mx, LIST_FOOT_Y, buf, RA_C_DIM, -1);
	text_right(c, c->w - mx, LIST_FOOT_Y, pos, RA_C_DIM);
}

static void render_detail(RaCanvas* c, const ra_list_data* d, const RaListView& v, int mx, int cols)
{
	const ra_list_item& it = d->items[v.sel];
	char buf[1024];
	int y = LIST_TOP;
	int n = ra_wrap(it.title, cols, 2, buf, sizeof(buf));
	char* line = buf;
	for (int i = 0; i < n; i++) {
		char* nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		ra_canvas_text(c, mx, y, line, RA_C_GOLD, cols);
		y += 10;
		line = nl ? nl + 1 : line + strlen(line);
	}
	y += 2;
	char status[96];
	if (it.unlocked) {
		ra_canvas_glyph(c, mx, y, RA_GLYPH_CHECK, RA_C_GREEN);
		if (it.unlock_time > 0) {
			time_t tt = (time_t)it.unlock_time;
			struct tm tmv;
			localtime_r(&tt, &tmv);
			char when[32];
			strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
			snprintf(status, sizeof(status), "Unlocked %s", when);
		} else {
			snprintf(status, sizeof(status), "Unlocked");
		}
		ra_canvas_text(c, mx + 16, y, status, RA_C_GREEN, cols - 2);
	} else {
		ra_canvas_glyph(c, mx, y, RA_GLYPH_SQUARE, RA_C_DIM);
		ra_canvas_text(c, mx + 16, y, it.unsupported ? "Locked (not supported)" : "Locked", RA_C_DIM, cols - 2);
	}
	snprintf(status, sizeof(status), "%u point%s", it.points, it.points == 1 ? "" : "s");
	text_right(c, c->w - mx, y, status, it.unlocked ? RA_C_GOLD : RA_C_DIM);
	y += 10;
	if (it.progress[0] && !it.unlocked) {
		snprintf(status, sizeof(status), "Progress: %s", it.progress);
		ra_canvas_text(c, mx + 16, y, status, RA_C_BLUE, cols - 2);
		y += 10;
	}
	y += 2;
	ra_canvas_fill(c, mx, y, c->w - 2 * mx, 1, RA_C_SEP);
	y += 6;
	int max_lines = (LIST_FOOT_RULE - 4 - y) / 10;
	n = ra_wrap(it.desc, cols, max_lines, buf, sizeof(buf));
	line = buf;
	for (int i = 0; i < n; i++) {
		char* nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		ra_canvas_text(c, mx, y, line, RA_C_TEXT, cols);
		y += 10;
		line = nl ? nl + 1 : line + strlen(line);
	}
	char pos[24];
	snprintf(pos, sizeof(pos), "%d/%u", v.sel + 1, d->count);
	list_footer(c, mx, cols, cols >= 60 ? "Up/Down: previous/next   A/B: back to the list" : "U/D:prev/next A/B:back", pos);
}

bool ra_list_render(RaCanvas* c, const ra_list_data* d, const RaListView& v, bool softcore)
{
	if (c->h != 240)
		return false;
	int mx = list_margin(c->w);
	int cols = (c->w - 2 * mx) / 8;
	ra_canvas_fill(c, 0, 0, c->w, c->h, RA_C_BG);

	if (d && d->count && v.detail && v.sel >= 0 && (uint32_t)v.sel < d->count) {
		render_detail(c, d, v, mx, cols);
		return true;
	}

	char buf[256], tmp[256];
	ra_fit(d && d->game[0] ? d->game : "RetroAchievements", cols, buf, sizeof(buf));
	ra_canvas_text(c, mx, LIST_TOP, buf, RA_C_GOLD, cols);
	if (d)
		snprintf(tmp, sizeof(tmp), cols >= 60 ? "Unlocked %u/%u achievements   Points %u/%u%s"
		                                       : "Unlocked %u/%u  Points %u/%u%s",
			d->num_unlocked, d->count, d->points_unlocked, d->points_total,
			softcore ? (cols >= 60 ? "   (softcore)" : "") : "");
	else
		snprintf(tmp, sizeof(tmp), "Loading...");
	ra_fit(tmp, cols, buf, sizeof(buf));
	ra_canvas_text(c, mx, LIST_TOP + 10, buf, RA_C_TEXT, cols);
	ra_canvas_fill(c, mx, LIST_TOP + 20, c->w - 2 * mx, 1, RA_C_SEP);

	const char* help = cols >= 60 ? "Up/Down: select   L/R: page   A: details   B: close"
	                              : "L/R:page A:info B:close";
	if (!d || !d->count) {
		const char* msg = d ? "This game has no achievements" : "";
		ra_canvas_text(c, (c->w - 8 * (int)strlen(msg)) / 2, 116, msg, RA_C_DIM, cols);
		list_footer(c, mx, cols, cols >= 60 ? "B: close" : "B:close", "");
		return true;
	}

	int pp = v.per_page > 0 ? v.per_page : ra_list_per_page(c->w);
	for (int k = 0; k < pp; k++) {
		int idx = v.first + k;
		if (idx < 0 || (uint32_t)idx >= d->count)
			break;
		const ra_list_item& it = d->items[idx];
		int y = LIST_ENTRIES_Y + k * LIST_ENTRY_H;
		if (idx == v.sel)
			ra_canvas_fill(c, mx - 4, y - 2, c->w - 2 * mx + 8, LIST_ENTRY_H + 1, RA_C_HILITE);
		if (it.unlocked)
			ra_canvas_glyph(c, mx, y, RA_GLYPH_CHECK, RA_C_GREEN);
		else
			ra_canvas_glyph(c, mx, y, RA_GLYPH_SQUARE, RA_C_DIM);
		char pts[16];
		snprintf(pts, sizeof(pts), "%u", it.points);
		int title_cols = cols - 2 - (int)strlen(pts) - 1;
		ra_fit(it.title, title_cols, buf, sizeof(buf));
		ra_canvas_text(c, mx + 16, y, buf, it.unlocked ? RA_C_TEXT : RA_C_DIM, title_cols);
		text_right(c, c->w - mx, y, pts, it.unlocked ? RA_C_GOLD : RA_C_DIM);

		int desc_cols = cols - 2;
		if (!it.unlocked && it.progress[0]) {
			snprintf(tmp, sizeof(tmp), "%s", it.progress);
			desc_cols -= (int)strlen(tmp) + 1;
			text_right(c, c->w - mx, y + 10, tmp, RA_C_BLUE);
		}
		ra_fit(it.desc, desc_cols, buf, sizeof(buf));
		ra_canvas_text(c, mx + 16, y + 10, buf, RA_C_DIM, desc_cols);
	}
	char pos[32];
	snprintf(pos, sizeof(pos), "%s%s %d/%u", v.first > 0 ? "\x04" : " ",
		(uint32_t)(v.first + pp) < d->count ? "\x05" : " ", v.sel + 1, d->count);
	list_footer(c, mx, cols, help, pos);
	return true;
}
