/*
	This file is part of libswirl
*/
#include "license/bsd"

#include "../../../polly2-rtl/driver/polly2_mmio.h"


#include "SuperH4_impl.h"
#include "sh4_interpreter.h"
#include "sh4_core.h"
#include "sh4_mmio.h"
#include "sh4_mem.h"
#include "sh4_sched.h"
#include "sh4_interrupts.h"

#include "hw/pvr/pvr_mem.h" // for TAWriteSQ_STTA / TAWriteSQ_MTTA

#if HOST_OS == OS_LINUX
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#endif

/*
    Freerunning mode: instead of advancing emulated time per executed
    timeslice, slave the scheduler to wall clock time read from the CycloneV
    A9 global timer. It runs at mpu_periph_clk = mpu_clk/4 - 200MHz at the
    stock 800MHz mpu_clk (one tick == one SH4 cycle), but the CPU clock is
    not fixed (e.g. DreamSTer's 1GHz overclock -> 250MHz timer), so init
    reads cpufreq and freerun_now() rescales raw ticks into the SH4's
    200MHz cycle domain.
*/

#define CV_GLOBALTMR_PHYS 0xFFFEC000u   /* A9 private peripheral space */
#define CV_GLOBALTMR_LO   0x200u        /* counter low word */
#define CV_GLOBALTMR_CTRL 0x208u        /* bit0: timer enable */

// never advance more this
#define FREERUN_MAX_BEHIND (SH4_MAIN_CLOCK / 60)

static volatile u8* freerun_gt;
static u32 freerun_last;

static u32 freerun_raw_last;                  /* last raw timer read */
static u32 freerun_scale_q24 = 1u << 24;      /* timer ticks -> SH4 cycles */
static u32 freerun_frac;                      /* Q24 scaling remainder */
static u32 freerun_cycles;                    /* virtual 200MHz counter */

static inline u32 freerun_now()
{
    u32 raw = *(volatile u32*)(freerun_gt + CV_GLOBALTMR_LO);
    u32 d = raw - freerun_raw_last;
    freerun_raw_last = raw;
    u64 s = (u64)d * freerun_scale_q24 + freerun_frac;
    freerun_frac = (u32)s & ((1u << 24) - 1);
    freerun_cycles += (u32)(s >> 24);
    return freerun_cycles;
}

static bool freerun_init()
{
    static int state;   /* 0 = untried, 1 = ok, -1 = unavailable */
    if (state) return state > 0;
    state = -1;

#if HOST_OS == OS_LINUX
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        printf("freerunning: cannot open /dev/mem, using normal timing\n");
        return false;
    }
    void* m = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   CV_GLOBALTMR_PHYS);
    close(fd);
    if (m == MAP_FAILED) {
        printf("freerunning: global timer mmap failed, using normal timing\n");
        return false;
    }
    freerun_gt = (volatile u8*)m;

    volatile u32* ctrl = (volatile u32*)(freerun_gt + CV_GLOBALTMR_CTRL);
    if (!(*ctrl & 1))
        *ctrl |= 1;     /* linux normally has it enabled already */

    /* the timer ticks at mpu_clk/4; mpu_clk from cpufreq (kHz, 800000 at
       stock). Q24 ratio of that to the SH4's 200MHz. */
    u32 cpu_khz = 800000;
    int cf = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
                  O_RDONLY | O_CLOEXEC);
    if (cf >= 0) {
        char buf[32];
        ssize_t n = read(cf, buf, sizeof(buf) - 1);
        close(cf);
        if (n > 0) {
            buf[n] = 0;
            u32 v = (u32)strtoul(buf, NULL, 10);
            if (v)
                cpu_khz = v;
        }
    }
    u64 timer_hz = (u64)cpu_khz * 250;   /* kHz * 1000 / 4 */
    freerun_scale_q24 = (u32)(((u64)SH4_MAIN_CLOCK << 24) / timer_hz);
    printf("freerunning: cpu %u kHz -> global timer %u Hz, scale %.6f\n",
           cpu_khz, (u32)timer_hz, freerun_scale_q24 / (double)(1u << 24));

    freerun_raw_last = *(volatile u32*)(freerun_gt + CV_GLOBALTMR_LO);
    freerun_last = freerun_now();
    state = 1;
    return true;
#else
    return false;
#endif
}

u64 compile_cycles;
u32 compile_start;
void UpdateSystem_StartBlockCompile() {
    compile_start = freerun_now();
}
void UpdateSystem_EndBlockCompile() {
    compile_cycles += freerun_now() - compile_start;
}

// every SH4_TIMESLICE cycles
int UpdateSystem()
{
    //this is an optimisation (mostly for ARM)
    //makes scheduling easier !
    //update_fp* tmu=pUpdateTMU;

    if (settings.freerunning)
    {
        u32 elapsed = freerun_now() - freerun_last;

        if (compile_cycles > elapsed) {
            compile_cycles -= elapsed;
            return Sh4cntx.interrupt_pend | (sh4_int_bCpuRun == false);
        } else {
            elapsed -= compile_cycles;
            compile_cycles = 0;
        }

        // if buffer is nearly full, don't generate more audio / update timers
        if (polly2_audio_space() < 128) {
            freerun_last = freerun_now();
            return Sh4cntx.interrupt_pend | (sh4_int_bCpuRun == false);
        }

        if (elapsed > FREERUN_MAX_BEHIND) {
            freerun_last += elapsed - FREERUN_MAX_BEHIND;
            elapsed = FREERUN_MAX_BEHIND;
        }

        s32 cycles = elapsed;
        while (cycles>=SH4_TIMESLICE) {
            cycles -= SH4_TIMESLICE+1;
            freerun_last += SH4_TIMESLICE;
            Sh4cntx.sh4_sched_next -= SH4_TIMESLICE;
            if (Sh4cntx.sh4_sched_next < 0)
                sh4_sched_tick(SH4_TIMESLICE);
        }
        if (cycles > 1) {
            Sh4cntx.sh4_sched_next -= cycles - 1;
            freerun_last += cycles - 1;
        }
        if (Sh4cntx.sh4_sched_next < 0)
                sh4_sched_tick(SH4_TIMESLICE);
    }
    else
    {
        Sh4cntx.sh4_sched_next -= SH4_TIMESLICE;
        if (Sh4cntx.sh4_sched_next < 0)
            sh4_sched_tick(SH4_TIMESLICE);
    }

    // Force an interrupt check if the cpu has been stopped
    // ngen is required to only check the bCpuRun on interrupt processing
    return Sh4cntx.interrupt_pend | (sh4_int_bCpuRun == false);
}

int UpdateSystem_INTC()
{
    if (UpdateSystem())
        return UpdateINTC();
    else
        return 0;
}


/// SuperH4_impl



void SuperH4_impl::SetA0Handler(Area0Hanlders slot, MMIODevice* dev) {
    devices[slot].reset(dev);
}

MMIODevice* SuperH4_impl::GetA0Handler(Area0Hanlders slot) {
    return devices[slot].get();
}

bool SuperH4_impl::setBackend(SuperH4Backends backend) {
    switch (backend)
    {
    case SH4BE_INTERPRETER:
        sh4_backend.reset(Get_Sh4Interpreter());
        break;
#if FEAT_SHREC != DYNAREC_NONE
    case SH4BE_DYNAREC:
        sh4_backend.reset(Get_Sh4Recompiler());
        break;
#endif

    default:
        return false;
    }

    return sh4_backend->Init();
}

void SuperH4_impl::Run() {
    sh4_backend->Loop();
}

void SuperH4_impl::Stop()
{
    verify(sh4_int_bCpuRun);

    sh4_int_bCpuRun = false;
}

void SuperH4_impl::Start()
{
    verify(!sh4_int_bCpuRun);
    
    sh4_int_bCpuRun = true;
}

void SuperH4_impl::Step()
{
    if (sh4_int_bCpuRun)
    {
        printf("Sh4 Is running , can't step\n");
    }
    else
    {
        u32 op = ReadMem16(next_pc);
        next_pc += 2;
        ExecuteOpcode(op);
    }
}

void SuperH4_impl::Skip()
{
    if (sh4_int_bCpuRun)
    {
        printf("Sh4 Is running, can't Skip\n");
    }
    else
    {
        next_pc += 2;
    }
}

void SuperH4_impl::Reset(bool Manual)
{
    if (sh4_int_bCpuRun)
    {
        printf("Sh4 Is running, can't Reset\n");
    }
    else
    {
        next_pc = 0xA0000000;

        memset(r, 0, sizeof(r));
        memset(r_bank, 0, sizeof(r_bank));

        gbr = ssr = spc = sgr = dbr = vbr = 0;
        mac.full = pr = fpul = 0;

        sh4_sr_SetFull(0x700000F0);
        old_sr.status = sr.status;
        UpdateSR();

        fpscr.full = 0x0004001;
        old_fpscr = fpscr;
        UpdateFPSCR();

        //Any more registers have default value ?
        printf("Sh4 Reset\n");

        //Clear cache
        sh4_backend->ClearCache();

        // reset mmrs/modules
        sh4mmr->Reset();

        // reset devices
        for (const auto& dev : devices)
            dev->Reset(Manual);
    }
}

bool SuperH4_impl::IsRunning()
{
    return sh4_int_bCpuRun;
}

SuperH4_impl::SuperH4_impl() {

}

bool SuperH4_impl::Init()
{
    verify(polly2_mmio_init() == 0);
    verify(freerun_init() == true);

    verify(sizeof(Sh4cntx) == 448);

    memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));
    do_sqw_ta = settings.pvr.MultithreadedTA ? &TAWriteSQ_MTTA : &TAWriteSQ_STTA;

    setBackend(SH4BE_INTERPRETER);

    // init devices
    for (const auto& dev : devices)
        if (!dev->Init())
            return false;

    return true;
}

void SuperH4_impl::Term()
{
    verify(!sh4_cpu->IsRunning());

    for (const auto& dev : devices)
        dev->Term();
    
    for (auto& dev : devices)
        dev.reset();
    
    sh4mmr.reset();

    sh4_sched_cleanup();

    sh4_backend.reset();

    printf("Sh4 Term\n");
}

void SuperH4_impl::ResetCache() {
    sh4_backend->ClearCache();
}

void SuperH4_impl::serialize(void** data, unsigned int* total_size) {
    for (int i = 0; i < A0H_MAX; i++) {
        sh4_cpu->GetA0Handler((Area0Hanlders)i)->serialize(data, total_size);
    }

    sh4mmr->serialize(data, total_size);
}

void SuperH4_impl::unserialize(void** data, unsigned int* total_size) {
    for (int i = 0; i < A0H_MAX; i++) {
        sh4_cpu->GetA0Handler((Area0Hanlders)i)->unserialize(data, total_size);
    }

    sh4mmr->unserialize(data, total_size);
}

SuperH4* SuperH4::Create() {

    auto rv = new SuperH4_impl();

    return rv;
}