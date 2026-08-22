/*
	This file is part of libswirl
*/
#include "license/bsd"


#pragma once

#include "ta_ctx.h"

extern u32 FrameCount;


void rend_init_renderer(u8* vram);
void rend_term_renderer();
void rend_vblank();

void rend_start_render(u8* vram);
void rend_end_render();

// freerunning mode: rend_end_sch polls the renderer for actual completion at
// 100kHz instead of raising RENDER_DONE at the fixed FPSTarget latency
#define REND_DONE_POLL_CYCLES (SH4_MAIN_CLOCK / (100 * 1000))
bool rend_render_done(u32 GuestRenderCycles);