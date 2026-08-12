/*
	This file is part of libswirl

	Stats OSD in the polly2 SPG top border band: a 640x30 RGB565 linear
	framebuffer in DDR right after the VRAM window, displayed 2x-doubled
	as a 1280x60 band above the game image (see polly2-rtl FB_TOP).

	ReportEmulatorStats() stores the numbers; ReportRendererStats() is
	called once per rendered frame and min/max accumulate here, so each
	OSD update shows the extremes of exactly the window since the last
	draw. UpdateStatsOSD() redraws the band and resets that window:

	  S <speed%> V <vblank/s> R <renders/s>  W <max wait ms> R <max render ms>  <mode>

	R converts the window's max render cycles to ms using the core clock
	the CLK register reads back at draw time (75/90/100/112.5 MHz).

	Drawn with a built-in 8x8 font (5x7 glyphs); the 2x band doubling makes
	it 16x16 on screen. This TU maps its own polly2 MMIO view (polly2_mmio
	is per-TU static, same pattern as AudioStream_Mister).
*/
#include "types.h"
#include "mister_support.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "../../hw/pvr/pvr_regs.h"
#include "../../../../polly2-rtl/driver/polly2_mmio.h"

/* PNG screenshot writer (public domain, header-only - this TU carries the
 * single implementation) */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../deps/stb/stb_image_write.h"

/* Physical DDR home of the band FB: the first 128-byte-aligned byte after
 * the 16MB VRAM window at 0x32000000 (HW_FPGA_VRAM_OFST + SPAN in
 * _vmem.cpp) - outside everything the core or Linux touches. */
#define STATS_FB_PHYS 0x33000000u
#define STATS_FB_MAP  (64u * 1024u)   /* page-multiple >= 1280*30 bytes */

static volatile u16* stats_fb;        /* 640x30, stride 640 pixels */

static struct {
	double speed_pct, vbs, rps;
	char   mode[24];
	// per-OSD-window accumulators, merged by ReportRendererStats() each
	// frame and reset by UpdateStatsOSD() after drawing
	double wait_min_ms, wait_max_ms;
	u32    cycles_min, cycles_max, cycles_total, frames;
} stats = { 0, 0, 0, {0}, 1e12, 0, ~0u, 0 };

static void reset_render_window()
{
	stats.wait_min_ms = 1e12;
	stats.wait_max_ms = 0;
	stats.cycles_min  = ~0u;
	stats.cycles_max  = 0;
	stats.cycles_total = 0;
	stats.frames = 0;
}

// ---------------------------------------------------------------- font ---
// 5x7 glyphs in 8x8 cells, MSB = leftmost pixel, rows top to bottom.

static const u8 font_digit[10][8] = {
	{0x70,0x88,0x98,0xA8,0xC8,0x88,0x70,0x00},  // 0
	{0x20,0x60,0x20,0x20,0x20,0x20,0x70,0x00},  // 1
	{0x70,0x88,0x08,0x30,0x40,0x80,0xF8,0x00},  // 2
	{0x70,0x88,0x08,0x30,0x08,0x88,0x70,0x00},  // 3
	{0x10,0x30,0x50,0x90,0xF8,0x10,0x10,0x00},  // 4
	{0xF8,0x80,0xF0,0x08,0x08,0x88,0x70,0x00},  // 5
	{0x30,0x40,0x80,0xF0,0x88,0x88,0x70,0x00},  // 6
	{0xF8,0x08,0x10,0x20,0x40,0x40,0x40,0x00},  // 7
	{0x70,0x88,0x88,0x70,0x88,0x88,0x70,0x00},  // 8
	{0x70,0x88,0x88,0x78,0x08,0x10,0x60,0x00},  // 9
};

static const u8 font_upper[26][8] = {
	{0x70,0x88,0x88,0xF8,0x88,0x88,0x88,0x00},  // A
	{0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0,0x00},  // B
	{0x70,0x88,0x80,0x80,0x80,0x88,0x70,0x00},  // C
	{0xF0,0x88,0x88,0x88,0x88,0x88,0xF0,0x00},  // D
	{0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8,0x00},  // E
	{0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0x00},  // F
	{0x70,0x88,0x80,0xB8,0x88,0x88,0x78,0x00},  // G
	{0x88,0x88,0x88,0xF8,0x88,0x88,0x88,0x00},  // H
	{0x70,0x20,0x20,0x20,0x20,0x20,0x70,0x00},  // I
	{0x38,0x10,0x10,0x10,0x10,0x90,0x60,0x00},  // J
	{0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88,0x00},  // K
	{0x80,0x80,0x80,0x80,0x80,0x80,0xF8,0x00},  // L
	{0x88,0xD8,0xA8,0xA8,0x88,0x88,0x88,0x00},  // M
	{0x88,0xC8,0xA8,0x98,0x88,0x88,0x88,0x00},  // N
	{0x70,0x88,0x88,0x88,0x88,0x88,0x70,0x00},  // O
	{0xF0,0x88,0x88,0xF0,0x80,0x80,0x80,0x00},  // P
	{0x70,0x88,0x88,0x88,0xA8,0x90,0x68,0x00},  // Q
	{0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0x00},  // R
	{0x78,0x80,0x80,0x70,0x08,0x08,0xF0,0x00},  // S
	{0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0x00},  // T
	{0x88,0x88,0x88,0x88,0x88,0x88,0x70,0x00},  // U
	{0x88,0x88,0x88,0x88,0x88,0x50,0x20,0x00},  // V
	{0x88,0x88,0x88,0xA8,0xA8,0xA8,0x50,0x00},  // W
	{0x88,0x88,0x50,0x20,0x50,0x88,0x88,0x00},  // X
	{0x88,0x88,0x50,0x20,0x20,0x20,0x20,0x00},  // Y
	{0xF8,0x08,0x10,0x20,0x40,0x80,0xF8,0x00},  // Z
};

static const u8 font_pct[8]   = {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00};  // %
static const u8 font_dot[8]   = {0x00,0x00,0x00,0x00,0x00,0x60,0x60,0x00};  // .
static const u8 font_slash[8] = {0x04,0x08,0x10,0x20,0x40,0x80,0x00,0x00};  // /
static const u8 font_colon[8] = {0x00,0x60,0x60,0x00,0x60,0x60,0x00,0x00};  // :
static const u8 font_minus[8] = {0x00,0x00,0x00,0xF8,0x00,0x00,0x00,0x00};  // -
static const u8 font_plus[8]  = {0x00,0x20,0x20,0xF8,0x20,0x20,0x00,0x00};  // +
static const u8 font_blank[8] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

static const u8* glyph(char c)
{
	if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
	if (c >= '0' && c <= '9') return font_digit[c - '0'];
	if (c >= 'A' && c <= 'Z') return font_upper[c - 'A'];
	switch (c) {
		case '%': return font_pct;
		case '.': return font_dot;
		case '/': return font_slash;
		case ':': return font_colon;
		case '-': return font_minus;
		case '+': return font_plus;
		default:  return font_blank;
	}
}

// ------------------------------------------------------------- drawing ---

/* the OSD stats line, from the CURRENT accumulation window (shared by the
 * band redraw and the screenshot band re-render; only UpdateStatsOSD
 * resets the window) */
static void format_stats_line(char* buf, size_t n)
{
	u32 hz = polly2_clock_hz();

	double render_max_ms = stats.cycles_max * 1e3 / hz;
	double rps_polly2 = stats.cycles_total ?
		(double)stats.frames * hz / (double)stats.cycles_total :
		0;

	snprintf(buf, n,
	         "S %5.1f%% V %4.1f R %4.1f %s WT %4.1f RT %4.1f PS %4.1f",
	         stats.speed_pct, stats.vbs, stats.rps, stats.mode,
	         stats.wait_max_ms, render_max_ms, rps_polly2);
}


static void draw_text(volatile u16* dst, int x, int y, const char* text, u16 color)
{
	for (; *text && x <= (int)POLLY2_BAND_W - 8; text++, x += 8) {
		const u8* g = glyph(*text);
		for (int r = 0; r < 8; r++) {
			volatile u16* line = dst + (y + r) * POLLY2_BAND_W + x;
			for (int b = 0; b < 8; b++)
				if (g[r] & (0x80u >> b)) line[b] = color;
		}
	}
}

static bool osd_init()
{
	static int state;   /* 0 = untried, 1 = ok, -1 = unavailable */
	if (state) return state > 0;
	state = -1;

	if (!polly2_mmio && polly2_mmio_init() != 0) {
		printf("mister_support: no polly2 MMIO, stats OSD disabled\n");
		return false;
	}
	if (!polly2_has_bands()) {
		printf("mister_support: bitstream REVISION %u has no border bands, "
		       "stats OSD disabled\n", polly2_revision());
		return false;
	}

	int fd = open("/dev/mem_wc", O_RDWR | O_SYNC);
	if (fd < 0) fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("mister_support: cannot open /dev/mem(_wc), stats OSD disabled\n");
		return false;
	}
	void* m = mmap(0, STATS_FB_MAP, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	               STATS_FB_PHYS);
	close(fd);
	if (m == MAP_FAILED) {
		printf("mister_support: band FB mmap failed, stats OSD disabled\n");
		return false;
	}
	stats_fb = (volatile u16*)m;

	for (u32 i = 0; i < POLLY2_BAND_W * POLLY2_BAND_H; i++) stats_fb[i] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
	polly2_set_fb_top(STATS_FB_PHYS);   /* band on, black until first draw */

	state = 1;
	return true;
}

// ----------------------------------------------------------------- api ---

void ReportEmulatorStats(double speed_pct, double vblank_per_sec,
                         const char* display_mode, double renders_per_sec)
{
	stats.speed_pct = speed_pct;
	stats.vbs       = vblank_per_sec;
	stats.rps       = renders_per_sec;
	snprintf(stats.mode, sizeof(stats.mode), "%s",
	         display_mode ? display_mode : "");
}

void ReportRendererStats(double wait_ms, u32 cycles)
{
	stats.cycles_total += cycles;
	stats.frames++;

	if (wait_ms < stats.wait_min_ms) stats.wait_min_ms = wait_ms;
	if (wait_ms > stats.wait_max_ms) stats.wait_max_ms = wait_ms;
	if (cycles < stats.cycles_min)   stats.cycles_min  = cycles;
	if (cycles > stats.cycles_max)   stats.cycles_max  = cycles;
}

// ---------------------------------------------------------- screenshot ---
// spg_screenshot: software replica of the polly2 SPG scanout (spg.sv) at
// SOURCE resolution - a 640x540 image, no 2x doubling and no side borders:
//   * top band    (lines   0..29 ): the stats OSD, RE-RENDERED into a local
//     640x30 RGB565 band fb (same clear + draw_text as UpdateStatsOSD, from
//     the current stats window - nothing is read back from DDR), then
//     MSB-replicated 565->888, 1:1. Always present, even when the on-screen
//     band is disabled.
//   * game window (lines  30..509): the 640x480 FB_R_SOF1 framebuffer
//     through the split-VRAM layout (FB 32-bit word W -> DDR byte
//     W*8 + SOF1[22]*4), all four FB_R_CTRL fb_depth formats with
//     fb_concat appended below the 5/6-bit channels (refsw2 Present
//     semantics). A VO_CONTROL.pixel_double 320-wide source fills the 640
//     width at 2x; an FB_R_CTRL.fb_line_double 240-line source fills the
//     480 height at 2x (half the SPG's 4x, since this canvas is half its
//     1280x960 window). fb_enable=0 blanks the window (bands unaffected).
//   * bottom band (lines 510..539): black for now (its renderer lands soon).
// The line stride mirrors sys_top's fb_disp_stride (640 px x 2/2/3/4 bytes
// by depth, halved by pixel_double) - NOT FB_R_SIZE.
// Output: 24bpp RGB PNG (stb_image_write) at
// /media/fat/screenshot/Dreamcast/<datetime>.png.

#define SHOT_DIR       "/media/fat/screenshots/Dreamcast"
#define SHOT_W         640
#define SHOT_H         540
#define SHOT_Y0        30            /* game window y 30..509 */
#define SHOT_Y1        510

static std::atomic<bool> screenshot_pending;

void QueueScreenshot() { screenshot_pending = true; }

/* one FB-view byte of the game framebuffer through the split-VRAM layout
 * (pvr_map32 with the line's fixed 32-bit half): view byte v lives at VRAM
 * byte (v/4)*8 + half*4 + (v&3). */
static inline u8 shot_fb_byte(const u8* vram, u32 view, u32 half)
{
	u32 off = ((view >> 2) << 3) | (half << 2) | (view & 3u);
	return vram[off & VRAM_MASK];
}

/* render one game source line into a 640-wide RGB strip (pixel_double
 * sources are 320 wide and repeat 2x) */
static void shot_game_line(const u8* vram, u32 sof1, u32 sy, u32 depth,
                           u32 concat, bool pixdbl, u8* strip /* 640*3 */)
{
	static const u32 bpp_by_depth[4] = { 2, 2, 3, 4 };
	u32 bpp    = bpp_by_depth[depth];
	u32 stride = (depth == 2) ? 1920u : (depth == 3) ? 2560u : 1280u;
	if (pixdbl) stride >>= 1;
	u32 half   = (sof1 >> 22) & 1u;
	u32 base   = (sof1 & 0x3FFFFCu) + sy * stride;   /* SOF1[1:0] dropped, as HW */
	u32 srcw   = pixdbl ? 320u : 640u;
	u32 rep    = pixdbl ? 2u : 1u;

	for (u32 sx = 0; sx < srcw; sx++) {
		u32 v = base + sx * bpp;
		u32 p32 = shot_fb_byte(vram, v, half)
		        | ((u32)shot_fb_byte(vram, v + 1, half) << 8)
		        | ((bpp > 2) ? ((u32)shot_fb_byte(vram, v + 2, half) << 16) : 0)
		        | ((bpp > 3) ? ((u32)shot_fb_byte(vram, v + 3, half) << 24) : 0);
		u32 p16 = p32 & 0xFFFFu;
		u8 r8, g8, b8;
		switch (depth) {
		case 0:                       /* 0555, fb_concat appended */
			r8 = (u8)((((p16 >> 10) & 0x1F) << 3) | concat);
			g8 = (u8)((((p16 >>  5) & 0x1F) << 3) | concat);
			b8 = (u8)((( p16        & 0x1F) << 3) | concat);
			break;
		case 1:                       /* 565, fb_concat appended */
			r8 = (u8)((((p16 >> 11) & 0x1F) << 3) | concat);
			g8 = (u8)((((p16 >>  5) & 0x3F) << 2) | (concat >> 1));
			b8 = (u8)((( p16        & 0x1F) << 3) | concat);
			break;
		default:                      /* 888 packed / 0888: R,G,B = bytes 2,1,0 */
			r8 = (u8)(p32 >> 16);
			g8 = (u8)(p32 >> 8);
			b8 = (u8)p32;
			break;
		}
		u8* d = strip + (u32)(sx * rep) * 3u;
		for (u32 k = 0; k < rep; k++) { d[0] = r8; d[1] = g8; d[2] = b8; d += 3; }
	}
}

/* one line of a local 640x30 RGB565 band fb -> RGB strip (MSB-replicated) */
static void shot_band_line(const u16* band, u32 sy, u8* strip /* 640*3 */)
{
	const u16* src = band + sy * POLLY2_BAND_W;
	for (u32 sx = 0; sx < 640; sx++) {
		u32 p16 = src[sx];
		u8* d = strip + sx * 3u;
		d[0] = (u8)((((p16 >> 11) & 0x1F) << 3) | ((p16 >> 13) & 0x7));
		d[1] = (u8)((((p16 >>  5) & 0x3F) << 2) | ((p16 >>  9) & 0x3));
		d[2] = (u8)((( p16        & 0x1F) << 3) | ((p16 >>  2) & 0x7));
	}
}

static void spg_screenshot(const u8* vram)
{
	if (!vram)
		return;

	/* display registers, exactly as sys_top feeds the spg */
	u32  sof1    = FB_R_SOF1;
	u32  rctrl   = FB_R_CTRL.full;
	bool enable  = (rctrl >> 0) & 1;
	bool linedbl = (rctrl >> 1) & 1;
	u32  depth   = (rctrl >> 2) & 3;
	u32  concat  = (rctrl >> 4) & 7;
	bool pixdbl  = VO_CONTROL.pixel_double;

	/* Bands are RE-RENDERED into local band fbs, not read back from DDR, so
	 * they are always part of the screenshot even with the on-screen band
	 * disabled. Top = the stats OSD (same clear + draw_text as
	 * UpdateStatsOSD, from the current stats window - which is NOT reset
	 * here); bottom = black for now, its renderer lands soon. */
	static u16 top_band[POLLY2_BAND_W * POLLY2_BAND_H];
	static u16 bot_band[POLLY2_BAND_W * POLLY2_BAND_H];
	memset(top_band, 0, sizeof top_band);
	memset(bot_band, 0, sizeof bot_band);
	if (polly2_mmio || polly2_mmio_init() == 0) {   /* clock_hz needs MMIO */
		char line[POLLY2_BAND_W / 8 + 1];
		format_stats_line(line, sizeof line);
		draw_text(top_band, 2, 11, line, 0xFFFF);
	}

	u8* frame = (u8*)calloc(1, (size_t)SHOT_W * SHOT_H * 3);   /* black borders */
	if (!frame) return;

	u8  strip[SHOT_W * 3];
	int cur_rgn = -1;           /* {region, src line} strip cache (line_double) */
	u32 cur_src = ~0u;
	for (u32 oy = 0; oy < SHOT_H; oy++) {
		int rgn; u32 src; bool on;
		if (oy < SHOT_Y0)      { rgn = 0; src = oy;                                  on = true;   }
		else if (oy < SHOT_Y1) { rgn = 1; src = (oy - SHOT_Y0) >> (linedbl ? 1 : 0); on = enable; }
		else                   { rgn = 2; src = oy - SHOT_Y1;                        on = true;   }
		if (!on) continue;      /* fb_enable=0: game window stays black */

		if (rgn != cur_rgn || src != cur_src) {
			if (rgn == 0)      shot_band_line(top_band, src, strip);
			else if (rgn == 2) shot_band_line(bot_band, src, strip);
			else               shot_game_line(vram, sof1, src, depth, concat, pixdbl, strip);
			cur_rgn = rgn; cur_src = src;
		}
		memcpy(frame + (size_t)oy * SHOT_W * 3, strip, sizeof strip);
	}

	/* ---- write the PNG (24bpp RGB, top-down - stb takes `frame` as is) ---- */
	{	/* mkdir -p SHOT_DIR; existing components (the usual case) are fine */
		char dir[] = SHOT_DIR;
		for (char* p = dir + 1; ; p++) {
			if (*p == '/' || *p == 0) {
				char c = *p;
				*p = 0;
				mkdir(dir, 0777);
				*p = c;
				if (c == 0)
					break;
			}
		}
	}
	time_t t = time(NULL);
	struct tm tmv;
	localtime_r(&t, &tmv);
	char path[128];
	snprintf(path, sizeof path, SHOT_DIR "/%04d-%02d-%02d_%02d-%02d-%02d.png",
	         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
	         tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

	if (stbi_write_png(path, SHOT_W, SHOT_H, 3, frame, SHOT_W * 3))
		printf("mister_support: screenshot -> %s\n", path);
	else
		printf("mister_support: cannot write %s\n", path);
	free(frame);
}

void ScreenshotVBlank(const u8* vram)
{
	if (screenshot_pending.exchange(false))
		spg_screenshot(vram);
}

void UpdateStatsOSD()
{
	if (!osd_init())
		return;

	char line[POLLY2_BAND_W / 8 + 1];
	format_stats_line(line, sizeof line);

	for (u32 i = 0; i < POLLY2_BAND_W * POLLY2_BAND_H; i++) stats_fb[i] = 0;
	draw_text(stats_fb, 2, 11, line, 0xFFFF);
	__asm__ volatile("dsb sy" ::: "memory");

	reset_render_window();   // next OSD update covers exactly this gap
}
