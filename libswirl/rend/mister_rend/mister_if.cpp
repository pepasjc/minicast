#include "types.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <arm_neon.h>

//#include "../../hw/pvr/pvr_mem.h"
#include "../../hw/pvr/pvr_regs.h"
#include "../../hw/pvr/Renderer_if.h"
#include "../../hw/mem/_vmem.h"
#include "../../../../polly2-rtl/driver/polly2_mmio.h"

#include <time.h>

#include "hw/pvr/ta_ring.h"
#include "mister_support.h"
#include "ra_dc.h"
#include "ra_overlay.h"
#include "ra_menu.h"   // RA_REG_*

u32 FrameCount;
static uint64_t RenderTime = 0;
static uint64_t DelayTime = 0;

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void SetREP(u32 render_end_pending_cycles);
static u8* vram;
static std::atomic<bool> polly_gone;

// In-game menu options (ra_menu): the polly2 cfg page registers the launcher
// also programs. Only touched while the LW HPS-to-FPGA bridge is out of
// reset (an access into a bridge in reset is a bus error); it always is
// while minicast runs, the check is a guard against surprises.
//   AUDIO_VOL  0xFF202030 on REVISION >= 6, 0xFF202028 on REVISION 3 (the
//              r6-based builds); REVISION 4/5 have VRAM_CFG at 0x28: none
//   CRT_CTRL   0xFF202034, REVISION >= 6 (builds without it ignore writes)
static const uint32_t RSTMGR_BASE = 0xFFD05000u, RSTMGR_BRGMODRST = 0x1C;
static volatile uint32_t* rstmgr_page;

static bool lw_bridge_up() {
    if (!rstmgr_page) {
        int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
        if (fd < 0)
            return false;
        void* m = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, RSTMGR_BASE);
        close(fd);
        if (m == MAP_FAILED)
            return false;
        rstmgr_page = (volatile uint32_t*)m;
    }
    return !(rstmgr_page[RSTMGR_BRGMODRST / 4] & 2u);   // [1] lwhps2fpga in reset
}

static int menu_reg_offset(int reg) {
    if (!polly2_mmio || !lw_bridge_up())
        return -1;
    uint32_t rev = polly2_revision();
    if (reg == RA_REG_AUDIO_VOL)
        return rev >= 6 ? 0x2030 : (rev == 3 ? 0x2028 : -1);
    if (reg == RA_REG_CRT_CTRL)
        return rev >= 6 ? 0x2034 : -1;
    return -1;
}

bool ra_frontend_reg_read(int reg, uint32_t* value) {
    int off = menu_reg_offset(reg);
    if (off < 0)
        return false;
    *value = polly2_mmio_rd((uint32_t)off);
    return true;
}

bool ra_frontend_reg_write(int reg, uint32_t value) {
    int off = menu_reg_offset(reg);
    if (off < 0)
        return false;
    polly2_mmio_wr((uint32_t)off, value);
    return true;
}

// RA overlay (achievement list): PowerVR idle before drawing into VRAM with
// the SH4 thread parked (a render it started may still be finishing)
static bool ra_gpu_idle() {
    return !polly_gone || polly2_done();
}

void rend_init_renderer(u8* vram) {
    ::vram = vram;
    // initialize renderer
    printf("rend_init_renderer\n");

	polly2_mmio_init();
    polly2_reset();
    polly2_set_vram_base(0x32000000);
    printf("polly2: ClockSel set to %u\n", (unsigned)settings.polly2.ClockSel);
    polly2_set_clock(settings.polly2.ClockSel);

    // RetroAchievements popups and the in-game menu draw into the displayed
    // framebuffer
    ra_overlay_init(vram, VRAM_MASK, ra_gpu_idle);
}

void rend_term_renderer() {
    // terminate renderer
    // printf("rend_term_renderer\n");
    // before VRAM is unmapped: the overlay thread reads/writes it
    ra_overlay_shutdown();
}

void rend_vblank() {
    // present framebuffer to video out
    // take a queued Print Screen screenshot at the vblank boundary, when the
    // scanout registers describe a coherent displayed frame
    ScreenshotVBlank(vram);
}

// freerunning REP poll: report the FPGA's actual completion, with the same
// 500ms watchdog + AutoReset the blocking wait in rend_end_render has
static uint64_t poll_deadline_ns;

extern std::atomic<bool> vram_dump_pending;
extern void do_vram_dump(u8* vram, u8* pvr_regs);

bool rend_render_done(u32 GuestRenderCycles) {
    if (polly_gone && polly2_done()) {
        return true;
    }

    if(polly_gone) {

        if (!settings.freerunning && settings.pvr.FPSTarget == 0) {
            // u64 scaled = (u64)polly2_frame_cycles() * SH4_MAIN_CLOCK / polly2_clock_hz();
            // // hold the guest only while it has run ahead of the hardware's progress
            // while (!polly2_done() && GuestRenderCycles >= scaled)
            //     scaled = (u64)polly2_frame_cycles() * SH4_MAIN_CLOCK / polly2_clock_hz();
            if (polly2_done()) {
                return true;
            }
        }

        if (now_ns() > poll_deadline_ns) {
            poll_deadline_ns = now_ns() + 500000000ULL;
            printf("polly2: Timeout waiting for Frame Done!\n");

            if (settings.polly2.DumpOnLockup) {
                vram_dump_pending = true;
                do_vram_dump(vram, pvr_regs);
            }

            if (settings.polly2.AutoReset) {
                printf("polly2: Auto resetting!\n");
                polly2_reset();
                polly2_set_vram_base(0x32000000);
                for (unsigned reg = 0; reg < pvr_RegSize; reg+=4)
                {
                    polly2_reg_write(reg, (u32&)pvr_regs[reg]);
                }
                polly2_go();
            }
        }
    }

    return false;
}

void startpolly() {
    __asm__ volatile("dsb sy" ::: "memory");

    do_vram_dump(vram, pvr_regs);

    poll_deadline_ns = now_ns() + 500000000ULL;
    polly2_go();
    polly_gone = true;
}

void rend_start_render(u8* vram) {
    // RA overlay: where this render writes (FB_W_SOF1, + SOF2 for field
    // rendering), before anything can reach the PowerVR. Waits only while the
    // overlay is working on an overlapping range (rare, ~1 ms).
    ra_overlay_render_start(FB_W_SOF1, FB_W_SOF2, SCALER_CTL.interlace,
                            FB_W_LINESTRIDE.stride * 8, FB_Y_CLIP.max + 1);

    if (settings.pvr.MultithreadedTA == TA_MTTA_DECOUPLED) {
        // ta_ring_publish();
        // auto goal = ta_contexts[CORE_CURRENT_CTX];

        // while (goal > ta_eol_interrupt_mark) {
        //     ;
        // }
        polly_gone = false;
        DECL_ALIGN(64) u32 ring_op[TA_RING_BLOCK/4];
        (u64&)ring_op = TA_RING_DECOUPLED_MAGIC;
        ring_op[2] = TA_RING_DECOUPLED_OP_STARTPOLLY;
        ta_ring_push(ring_op);
        ta_ring_publish();
    }

    // freerunning: REP polls for real completion; FPSTarget pacing only
    // applies when emulated time is decoupled from wall time
    if (settings.freerunning || settings.pvr.FPSTarget == 0) {
        SetREP(REND_DONE_POLL_CYCLES);
    } else {
        SetREP(200000000/settings.pvr.FPSTarget);
    }
	
    if (settings.pvr.MultithreadedTA != TA_MTTA_DECOUPLED) {
        startpolly();
    }

	DelayTime = RenderTime;
	RenderTime = now_ns();
}

void rend_end_render() {
    // "Wait for FPGA frame done" flag...
    uint64_t WaitTime = now_ns();

    while (true)
    {
        if (polly_gone && polly2_done()) {
            break;
        }

        // 0.1 ms delay
        uint64_t start = now_ns();
        while ((now_ns() - start) < 100000ULL) { }

        if (polly_gone && (now_ns() > poll_deadline_ns)) {
            poll_deadline_ns = now_ns() + 500000000ULL;
            printf("polly2: Timeout waiting for Frame Done!\n");

            if (settings.polly2.DumpOnLockup) {
                vram_dump_pending = true;
                do_vram_dump(vram, pvr_regs);
            }

            if (settings.polly2.AutoReset) {
                printf("polly2: Auto resetting!\n");
                polly2_reset();
                polly2_set_vram_base(0x32000000);
                for (unsigned reg = 0 ; reg < pvr_RegSize; reg+=4)
                {
                    polly2_reg_write(reg, (u32&)pvr_regs[reg]);
                }
                polly2_go();
            }
        }
    }
	
    FrameCount++;

    // RA overlay: the target is complete; a popup can go into it before the
    // game flips to it
    ra_overlay_render_end();

    // RetroAchievements with dreamster_frame_source=render: one
    // rc_client_do_frame per rendered frame instead of per vblank
    ra_frame_hook(RA_FRAME_RENDER);

	// per-frame render stats; mister_support tracks min/max per OSD window
	ReportRendererStats((now_ns() - WaitTime) / 1e6, polly2_frame_cycles());

	if ( (FrameCount&0xf) == 0x0) {
		uint64_t DoneTime = now_ns();
		printf("GPU: %1.1fms, Wait: %1.1fms, Frame to Frame: %1.1fms, Frame Slack: %1.1fms, %u cycles\n", (DoneTime - RenderTime)/1e6, (DoneTime - WaitTime)/1e6, (RenderTime - DelayTime)/1e6, int64_t(RenderTime - DelayTime - (DoneTime - RenderTime))/1e6, polly2_frame_cycles());
	}
}
