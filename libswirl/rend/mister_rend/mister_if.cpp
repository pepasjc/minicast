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
void rend_init_renderer(u8* vram) {
    ::vram = vram;
    // initialize renderer
    printf("rend_init_renderer\n");
	
	polly2_mmio_init();
    polly2_reset();
    polly2_set_vram_base(0x32000000);
    printf("polly2: ClockSel set to %u\n", (unsigned)settings.polly2.ClockSel);
    polly2_set_clock(settings.polly2.ClockSel);
}

void rend_term_renderer() {
    // terminate renderer
    // printf("rend_term_renderer\n");
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

static std::atomic<bool> polly_gone;

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

	// per-frame render stats; mister_support tracks min/max per OSD window
	ReportRendererStats((now_ns() - WaitTime) / 1e6, polly2_frame_cycles());

	if ( (FrameCount&0xf) == 0x0) {
		uint64_t DoneTime = now_ns();
		printf("GPU: %1.1fms, Wait: %1.1fms, Frame to Frame: %1.1fms, Frame Slack: %1.1fms, %u cycles\n", (DoneTime - RenderTime)/1e6, (DoneTime - WaitTime)/1e6, (RenderTime - DelayTime)/1e6, int64_t(RenderTime - DelayTime - (DoneTime - RenderTime))/1e6, polly2_frame_cycles());
	}
}
