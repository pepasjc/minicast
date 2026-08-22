/*
	This file is part of libswirl
*/
#include "license/bsd"


// libswirl.cpp: this is not nullDC anymore. Not that anyone has noticed.
//

//initialse Emu
#include "types.h"
#include "libswirl.h"
#include "oslib/oslib.h"
#include "oslib/audiostream.h"
#include "hw/mem/_vmem.h"
#include "stdclass.h"
#include "cfg/cfg.h"

#include "hw/maple/maple_cfg.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/dyna/ngen.h"

#include "hw/naomi/naomi_cart.h"
#include "reios/reios.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/spg.h"
#include "hw/aica/dsp.h"
// #include "gui/gui.h"
// #include "gui/gui_renderer.h"
#include "profiler/profiler.h"
#include "input/gamepad_device.h"
// #include "rend/TexCache.h"

#include "hw/gdrom/gdromv3.h"

#include "hw/maple/maple_if.h"
#include "hw/modem/modem.h"
#include "hw/bba/bba.h"
#include "hw/holly/holly_intc.h"
#include "hw/aica/aica_mmio.h"
#include "hw/arm7/SoundCPU.h"
#include "hw/pvr/ta_ring.h"

#define fault_printf(...)

#ifdef SCRIPTING
// #include "scripting/lua_bindings.h"
#endif

unique_ptr<VirtualDreamcast> virtualDreamcast;
unique_ptr<GDRomDisc> g_GDRDisc;


void LoadCustom();

settings_t settings;
// Set if game has corresponding option by default, so that it's not saved in the config
static bool rtt_to_buffer_game;
static bool safemode_game;
static bool tr_poly_depth_mask_game;
static bool extra_depth_game;


MMIODevice* Create_BiosDevice();
MMIODevice* Create_FlashDevice();
MMIODevice* Create_NaomiDevice(SuperH4Mmr* sh4mmr, SystemBus* sb, ASIC* asic);
SystemBus* Create_SystemBus();
MMIODevice* Create_PVRDevice(SuperH4Mmr* sh4mmr, SystemBus* sb, ASIC* asic, SPG* spg, u8* mram, u8* vram);
MMIODevice* Create_ExtDevice_006();
MMIODevice* Create_ExtDevice_010();



#if HOST_OS==OS_WINDOWS
#include <windows.h>
#endif

/**
 * cpu_features_get_time_usec:
 *
 * Gets time in microseconds.
 *
 * Returns: time in microseconds.
 **/
int64_t get_time_usec(void)
{
#if HOST_OS==OS_WINDOWS
    static LARGE_INTEGER freq;
    LARGE_INTEGER count;

    /* Frequency is guaranteed to not change. */
    if (!freq.QuadPart && !QueryPerformanceFrequency(&freq))
        return 0;

    if (!QueryPerformanceCounter(&count))
        return 0;
    return count.QuadPart * 1000000 / freq.QuadPart;
#elif HOST_OS==OS_XIL_BARE
    verify(false); // TODO
    return 0;
#elif defined(_POSIX_MONOTONIC_CLOCK) || defined(__QNX__) || defined(ANDROID) || defined(__MACH__) || HOST_OS==OS_LINUX
    struct timespec tv = { 0 };
    if (clock_gettime(CLOCK_MONOTONIC, &tv) < 0)
        return 0;
    return tv.tv_sec * INT64_C(1000000) + (tv.tv_nsec + 500) / 1000;
#elif defined(EMSCRIPTEN)
    return emscripten_get_now() * 1000;
#elif defined(__mips__) || defined(DJGPP)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (1000000 * tv.tv_sec + tv.tv_usec);    
#else
#error "Your platform does not have a timer function implemented in cpu_features_get_time_usec(). Cannot continue."
#endif
}


int GetFile(char* szFileName)
{
    cfgLoadStr("config", "image", szFileName, "");

    return stricmp(szFileName, "nodisk") != 0;
}


s32 plugins_Init()
{

#ifndef TARGET_DISPFRAME
    g_GDRDisc.reset(GDRomDisc::Create());

    if (s32 rv = g_GDRDisc->Init())
        return rv;
#endif

    return rv_ok;
}

void plugins_Term()
{
    g_GDRDisc.reset();
}

void plugins_Reset(bool Manual)
{
    reios_reset();
    g_GDRDisc->Reset(Manual);
    //libExtDevice_Reset(Manual);
}

void LoadSpecialSettings()
{
#if DC_PLATFORM == DC_PLATFORM_DREAMCAST
    printf("Game ID is [%s]\n", reios_product_number);

    // Pro Pinball Trilogy
    if (!strncmp("T30701D", reios_product_number, 7)
        // Demolition Racer
        || !strncmp("T15112N", reios_product_number, 7)
        // Star Wars - Episode I - Racer (United Kingdom)
        || !strncmp("T23001D", reios_product_number, 7)
        // Star Wars - Episode I - Racer (USA)
        || !strncmp("T23001N", reios_product_number, 7)
        // Record of Lodoss War (EU)
        || !strncmp("T7012D", reios_product_number, 6)
        // Record of Lodoss War (USA)
        || !strncmp("T40218N", reios_product_number, 7)
        // Surf Rocket Racers
        || !strncmp("T40216N", reios_product_number, 7))
    {
        printf("Disabling div1_som for game %s\n", reios_product_number);
        settings.dynarec.opt_div1_som = 0;
        safemode_game = true;
    }

#elif DC_PLATFORM == DC_PLATFORM_NAOMI || DC_PLATFORM == DC_PLATFORM_ATOMISWAVE
    printf("Game ID is [%s]\n", naomi_game_id);

    if (!strcmp("METAL SLUG 6", naomi_game_id) || !strcmp("WAVE RUNNER GP", naomi_game_id))
    {
        printf("Enabling Dynarec safe mode for game %s\n", naomi_game_id);
        settings.dynarec.safemode = 1;
        safemode_game = true;
    }
    if (!strcmp("SAMURAI SPIRITS 6", naomi_game_id))
    {
        printf("Enabling Extra depth scaling for game %s\n", naomi_game_id);
        settings.rend.ExtraDepthScale = 1e26;
        extra_depth_game = true;
    }
    if (!strcmp("DYNAMIC GOLF", naomi_game_id)
        || !strcmp("SHOOTOUT POOL", naomi_game_id)
        || !strcmp("OUTTRIGGER     JAPAN", naomi_game_id)
        || !strcmp("CRACKIN'DJ  ver JAPAN", naomi_game_id)
        || !strcmp("CRACKIN'DJ PART2  ver JAPAN", naomi_game_id)
        || !strcmp("KICK '4' CASH", naomi_game_id))
    {
        printf("Enabling JVS rotary encoders for game %s\n", naomi_game_id);
        settings.input.JammaSetup = 2;
    }
    else if (!strcmp("POWER STONE 2 JAPAN", naomi_game_id)		// Naomi
        || !strcmp("GUILTY GEAR isuka", naomi_game_id))		// AW
    {
        printf("Enabling 4-player setup for game %s\n", naomi_game_id);
        settings.input.JammaSetup = 1;
    }
    else if (!strcmp("SEGA MARINE FISHING JAPAN", naomi_game_id)
        || !strcmp(naomi_game_id, "BASS FISHING SIMULATOR VER.A"))	// AW
    {
        printf("Enabling specific JVS setup for game %s\n", naomi_game_id);
        settings.input.JammaSetup = 3;
    }
    else if (!strcmp("RINGOUT 4X4 JAPAN", naomi_game_id))
    {
        printf("Enabling specific JVS setup for game %s\n", naomi_game_id);
        settings.input.JammaSetup = 4;
    }
    else if (!strcmp("NINJA ASSAULT", naomi_game_id)
        || !strcmp(naomi_game_id, "Sports Shooting USA")	// AW
        || !strcmp(naomi_game_id, "SEGA CLAY CHALLENGE"))	// AW
    {
        printf("Enabling lightgun setup for game %s\n", naomi_game_id);
        settings.input.JammaSetup = 5;
    }
    else if (!strcmp(" BIOHAZARD  GUN SURVIVOR2", naomi_game_id))
    {
        printf("Enabling specific JVS setup for game %s\n", naomi_game_id);
        settings.input.JammaSetup = 7;
    }
    if (!strcmp("COSMIC SMASH IN JAPAN", naomi_game_id))
    {
        printf("Enabling translucent depth multipass for game %s\n", naomi_game_id);
        settings.rend.TranslucentPolygonDepthMask = true;
        tr_poly_depth_mask_game = true;
    }
#endif
}


void InitSettings()
{
    settings.dynarec.Enable = true;
    settings.dynarec.idleskip = true;
    
    settings.dynarec.opt_cyclecheck_backwards_only = false;
    settings.dynarec.opt_div1_som = true;  
    settings.dynarec.opt_sqw_on_pref = false;
    settings.dynarec.opt_sqw_rewrite = false;
    settings.dynarec.opt_constprop = 1;
    settings.dynarec.opt_readm_pairs = 0;
    settings.dynarec.opt_fipr_w = 1;
    settings.dynarec.opt_self_loops = 0;

    settings.dynarec.ScpuEnable = true;
    settings.dynarec.DspEnable = true;
    settings.dynarec.SmcCheckLevel = FullCheck;

    settings.dreamcast.cable = 3;	// TV composite
    settings.dreamcast.region = 3;	// default
    settings.dreamcast.broadcast = 4;	// default
    settings.dreamcast.language = 6;	// default
    settings.dreamcast.FullMMU = false;
    settings.aica.LimitFPS = true;
    settings.aica.NoBatch = false;	// This also controls the DSP. Disabled by default
    settings.aica.NoSound = false;
    

    settings.pvr.MultithreadedTA = 0;
    settings.pvr.FPSTarget = 0;

    settings.polly2.AutoReset = true;
    settings.polly2.DumpOnLockup = false;
    settings.polly2.ClockSel = 0;   // Default to no OC

    settings.gdrom.async_dma = true;

    settings.freerunning = false;

    settings.debug.PrintTaErrors = false;
    settings.debug.SerialConsole = false;
    settings.debug.VirtualSerialPort = false;
    settings.debug.VirtualSerialPortFile = "";

    settings.bios.UseReios = 0;
    settings.reios.ElfFile = "";

    settings.input.MouseSensitivity = 100;
    settings.input.JammaSetup = 0;
    settings.input.VirtualGamepadVibration = 20;
    for (int i = 0; i < MAPLE_PORTS; i++)
    {
        settings.input.maple_devices[i] = i == 0 ? MDT_SegaController : MDT_None;
        settings.input.maple_expansion_devices[i][0] = i == 0 ? MDT_SegaVMU : MDT_None;
        settings.input.maple_expansion_devices[i][1] = i == 0 ? MDT_SegaVMU : MDT_None;
    }

#if SUPPORT_DISPMANX
    settings.dispmanx.Width = 640;
    settings.dispmanx.Height = 480;
    settings.dispmanx.Keep_Aspect = true;
#endif
}

void LoadSettings(bool game_specific)
{
    const char* config_section = game_specific ? cfgGetGameId() : "config";
    const char* input_section = game_specific ? cfgGetGameId() : "input";
    const char* audio_section = game_specific ? cfgGetGameId() : "audio";

    settings.dynarec.Enable = cfgLoadBool(config_section, "Dynarec.Enabled", settings.dynarec.Enable);
    settings.dynarec.idleskip = cfgLoadBool(config_section, "Dynarec.idleskip", settings.dynarec.idleskip);
    
    settings.dynarec.opt_cyclecheck_backwards_only = cfgLoadBool(config_section, "Dynarec.opt_cyclecheck_backwards_only", settings.dynarec.opt_cyclecheck_backwards_only);
    settings.dynarec.opt_div1_som = cfgLoadBool(config_section, "Dynarec.opt_div1_som", settings.dynarec.opt_div1_som);
    settings.dynarec.opt_sqw_on_pref = cfgLoadBool(config_section, "Dynarec.opt_sqw_on_pref", settings.dynarec.opt_sqw_on_pref);
    settings.dynarec.opt_sqw_rewrite = cfgLoadBool(config_section, "Dynarec.opt_sqw_rewrite", settings.dynarec.opt_sqw_rewrite);
    settings.dynarec.opt_constprop = cfgLoadInt(config_section, "Dynarec.opt_constprop", settings.dynarec.opt_constprop);
    settings.dynarec.opt_readm_pairs = cfgLoadBool(config_section, "Dynarec.opt_readm_pairs", settings.dynarec.opt_readm_pairs);
    settings.dynarec.opt_fipr_w = cfgLoadBool(config_section, "Dynarec.opt_fipr_w", settings.dynarec.opt_fipr_w);
    settings.dynarec.opt_self_loops = cfgLoadBool(config_section, "Dynarec.opt_self_loops", settings.dynarec.opt_self_loops);

    settings.dynarec.SmcCheckLevel = (SmcCheckEnum)cfgLoadInt(config_section, "Dynarec.SmcCheckLevel", settings.dynarec.SmcCheckLevel);
    settings.dynarec.ScpuEnable = cfgLoadInt(config_section, "Dynarec.ScpuEnabled", settings.dynarec.ScpuEnable);
    settings.dynarec.DspEnable = cfgLoadInt(config_section, "Dynarec.DspEnabled", settings.dynarec.DspEnable);

    //disable_nvmem can't be loaded, because nvmem init is before cfg load
    settings.dreamcast.cable = cfgLoadInt(config_section, "Dreamcast.Cable", settings.dreamcast.cable);
    settings.dreamcast.region = cfgLoadInt(config_section, "Dreamcast.Region", settings.dreamcast.region);
    settings.dreamcast.broadcast = cfgLoadInt(config_section, "Dreamcast.Broadcast", settings.dreamcast.broadcast);
    settings.dreamcast.language = cfgLoadInt(config_section, "Dreamcast.Language", settings.dreamcast.language);
    settings.dreamcast.FullMMU = cfgLoadBool(config_section, "Dreamcast.FullMMU", settings.dreamcast.FullMMU);
    settings.aica.LimitFPS = cfgLoadBool(config_section, "aica.LimitFPS", settings.aica.LimitFPS);
    settings.aica.NoBatch = cfgLoadBool(config_section, "aica.NoBatch", settings.aica.NoBatch);
    settings.aica.NoSound = cfgLoadBool(config_section, "aica.NoSound", settings.aica.NoSound);
    
    settings.pvr.MultithreadedTA = cfgLoadInt(config_section, "pvr.MultithreadedTA", settings.pvr.MultithreadedTA);

    settings.pvr.FPSTarget = cfgLoadInt(config_section, "pvr.FPSTarget", settings.pvr.FPSTarget);

    settings.polly2.AutoReset = cfgLoadBool(config_section, "polly2.AutoReset", settings.polly2.AutoReset);
    settings.polly2.DumpOnLockup = cfgLoadBool(config_section, "polly2.DumpOnLockup", settings.polly2.DumpOnLockup);
    settings.polly2.ClockSel = cfgLoadInt(config_section, "polly2.ClockSel", settings.polly2.ClockSel);

    settings.gdrom.async_dma = cfgLoadBool(config_section, "gdrom.AsyncDMA", settings.gdrom.async_dma);

    settings.freerunning = cfgLoadBool(config_section, "Freerunning", settings.freerunning);
    
    settings.debug.SerialConsole = cfgLoadBool(config_section, "Debug.SerialConsoleEnabled", settings.debug.SerialConsole);
    settings.debug.VirtualSerialPort = cfgLoadBool(config_section, "Debug.VirtualSerialPort", settings.debug.VirtualSerialPort);
    settings.debug.VirtualSerialPortFile = cfgLoadStr(config_section, "Debug.VirtualSerialPortFile", settings.debug.VirtualSerialPortFile.c_str());
    
    settings.bios.UseReios = cfgLoadBool(config_section, "bios.UseReios", settings.bios.UseReios);
    settings.reios.ElfFile = cfgLoadStr(game_specific ? cfgGetGameId() : "reios", "ElfFile", settings.reios.ElfFile.c_str());


    settings.input.MouseSensitivity = cfgLoadInt(input_section, "MouseSensitivity", settings.input.MouseSensitivity);
    settings.input.JammaSetup = cfgLoadInt(input_section, "JammaSetup", settings.input.JammaSetup);
    settings.input.VirtualGamepadVibration = cfgLoadInt(input_section, "VirtualGamepadVibration", settings.input.VirtualGamepadVibration);
    for (int i = 0; i < MAPLE_PORTS; i++)
    {
        char device_name[32];
        sprintf(device_name, "device%d", i + 1);
        settings.input.maple_devices[i] = (MapleDeviceType)cfgLoadInt(input_section, device_name, settings.input.maple_devices[i]);
        sprintf(device_name, "device%d.1", i + 1);
        settings.input.maple_expansion_devices[i][0] = (MapleDeviceType)cfgLoadInt(input_section, device_name, settings.input.maple_expansion_devices[i][0]);
        sprintf(device_name, "device%d.2", i + 1);
        settings.input.maple_expansion_devices[i][1] = (MapleDeviceType)cfgLoadInt(input_section, device_name, settings.input.maple_expansion_devices[i][1]);
    }

    if (!game_specific)
    {
        settings.dreamcast.ContentPath.clear();
        std::string paths = cfgLoadStr(config_section, "Dreamcast.ContentPath", "");
        std::string::size_type start = 0;
        while (true)
        {
            std::string::size_type end = paths.find(';', start);
            if (end == std::string::npos)
                end = paths.size();
            if (start != end)
                settings.dreamcast.ContentPath.push_back(paths.substr(start, end - start));
            if (end == paths.size())
                break;
            start = end + 1;
        }
    }
    /*
        //make sure values are valid
        settings.dreamcast.cable		= min(max(settings.dreamcast.cable,    0),3);
        settings.dreamcast.region		= min(max(settings.dreamcast.region,   0),3);
        settings.dreamcast.broadcast	= min(max(settings.dreamcast.broadcast,0),4);
    */
}

void LoadCustom()
{
#if DC_PLATFORM == DC_PLATFORM_DREAMCAST
    char* reios_id = reios_disk_id();

    char* p = reios_id + strlen(reios_id) - 1;
    while (p >= reios_id && *p == ' ')
        * p-- = '\0';
    if (*p == '\0')
        return;
#elif DC_PLATFORM == DC_PLATFORM_NAOMI || DC_PLATFORM == DC_PLATFORM_ATOMISWAVE
    char* reios_id = naomi_game_id;
    char* reios_software_name = naomi_game_id;
#endif

    // Default per-game settings
    LoadSpecialSettings();

    cfgSetGameId(reios_id);

    // Reload per-game settings
    LoadSettings(true);
}

void SaveSettings()
{
    cfgSaveBool("config", "Dynarec.Enabled", settings.dynarec.Enable);
    cfgSaveInt("config", "Dynarec.ScpuEnabled", settings.dynarec.ScpuEnable);
    cfgSaveInt("config", "Dynarec.DspEnabled", settings.dynarec.DspEnable);
    cfgSaveInt("config", "Dreamcast.Cable", settings.dreamcast.cable);
    cfgSaveInt("config", "Dreamcast.Region", settings.dreamcast.region);
    cfgSaveInt("config", "Dreamcast.Broadcast", settings.dreamcast.broadcast);
    cfgSaveBool("config", "Dreamcast.FullMMU", settings.dreamcast.FullMMU);
    cfgSaveBool("config", "Dynarec.idleskip", settings.dynarec.idleskip);
    cfgSaveBool("config", "Dynarec.opt_cyclecheck_backwards_only", settings.dynarec.opt_cyclecheck_backwards_only);
	cfgSaveBool("config", "Dynarec.opt_div1_som", settings.dynarec.opt_div1_som);
	cfgSaveBool("config", "Dynarec.opt_sqw_on_pref", settings.dynarec.opt_sqw_on_pref);
    cfgSaveBool("config", "Dynarec.opt_sqw_rewrite", settings.dynarec.opt_sqw_rewrite);
	cfgSaveInt("config", "Dynarec.opt_constprop", settings.dynarec.opt_constprop);
	cfgSaveBool("config", "Dynarec.opt_readm_pairs", settings.dynarec.opt_readm_pairs);
	cfgSaveBool("config", "Dynarec.opt_fipr_w", settings.dynarec.opt_fipr_w);
	cfgSaveBool("config", "Dynarec.opt_self_loops", settings.dynarec.opt_self_loops);
    cfgSaveInt("config", "Dynarec.SmcCheckLevel", (int)settings.dynarec.SmcCheckLevel);

    cfgSaveInt("config", "Dreamcast.Language", settings.dreamcast.language);
    cfgSaveBool("config", "aica.LimitFPS", settings.aica.LimitFPS);
    cfgSaveBool("config", "aica.NoBatch", settings.aica.NoBatch);
    cfgSaveBool("config", "aica.NoSound", settings.aica.NoSound);

    cfgSaveInt("config", "pvr.MultithreadedTA", settings.pvr.MultithreadedTA);
    cfgSaveInt("config", "pvr.FPSTarget", settings.pvr.FPSTarget);

    cfgSaveBool("config", "polly2.AutoReset", settings.polly2.AutoReset);
    cfgSaveBool("config", "polly2.DumpOnLockup", settings.polly2.DumpOnLockup);
    cfgSaveInt("config", "polly2.ClockSel", settings.polly2.ClockSel);

    cfgSaveBool("config", "gdrom.AsyncDMA", settings.gdrom.async_dma);

    cfgSaveBool("config", "Freerunning", settings.freerunning);

    cfgSaveBool("config", "Debug.SerialConsoleEnabled", settings.debug.SerialConsole);
    cfgSaveBool("config", "Debug.VirtualSerialPort", settings.debug.VirtualSerialPort);
    cfgSaveStr("config", "Debug.VirtualSerialPortFile", settings.debug.VirtualSerialPortFile.c_str());
    
    cfgSaveInt("input", "MouseSensitivity", settings.input.MouseSensitivity);
    cfgSaveInt("input", "VirtualGamepadVibration", settings.input.VirtualGamepadVibration);
    for (int i = 0; i < MAPLE_PORTS; i++)
    {
        char device_name[32];
        sprintf(device_name, "device%d", i + 1);
        cfgSaveInt("input", device_name, (s32)settings.input.maple_devices[i]);
        sprintf(device_name, "device%d.1", i + 1);
        cfgSaveInt("input", device_name, (s32)settings.input.maple_expansion_devices[i][0]);
        sprintf(device_name, "device%d.2", i + 1);
        cfgSaveInt("input", device_name, (s32)settings.input.maple_expansion_devices[i][1]);
    }
    // FIXME This should never be a game-specific setting
    std::string paths;
    for (auto path : settings.dreamcast.ContentPath)
    {
        if (!paths.empty())
            paths += ";";
        paths += path;
    }
    cfgSaveStr("config", "Dreamcast.ContentPath", paths.c_str());

    // GamepadDevice::SaveMaplePorts();

#ifdef _ANDROID
    void SaveAndroidSettings();
    SaveAndroidSettings();
#endif
}

static bool reset_requested;

void libswirl_init() {
    InitSettings();
    LoadSettings(false);
}
void libswirl_loop(const char* game) {
    virtualDreamcast.reset(VirtualDreamcast::Create());

    virtualDreamcast->Init();

    int rc = virtualDreamcast->StartGame(game);
    if (rc >= 0)
        virtualDreamcast->MainLoop();
}
/*
int reicast_init(int argc, char* argv[])
{
#ifdef _WIN32
    setbuf(stdout, 0);
    setbuf(stderr, 0);
#endif

    // TODO: Move vmem_reserve back here
    if (ParseCommandLine(argc, argv))
    {
        return 69;
    }
    printf("Config dir is: %s\n", get_writable_config_path("/").c_str());
	printf("Data dir is:   %s\n", get_writable_data_path("/").c_str());

    InitSettings();
    bool showOnboarding = false;
    if (!cfgOpen())
    {
        printf("Config directory is not set. Starting onboarding\n");
        showOnboarding = true;
    }
    else
        LoadSettings(false);

#if BUILD_RETROARCH_CORE == 0
    os_CreateWindow();
#endif
    os_SetupInput();

    g_GUI.reset(GUI::Create());
    g_GUI->Init();

#if BUILD_RETROARCH_CORE == 0
    g_GUIRenderer.reset(GUIRenderer::Create(g_GUI.get()));
#endif

    if (showOnboarding)
        g_GUI->OpenOnboarding();

    return 0;
}

void reicast_ui_loop() {
    g_GUIRenderer->UILoop();
}

void reicast_term() {
    g_GUIRenderer.reset();

    g_GUI.reset();
}
*/

//General update

//3584 Cycles
#define AICA_SAMPLE_GCM 441
#define AICA_SAMPLE_CYCLES (SH4_MAIN_CLOCK/(44100/AICA_SAMPLE_GCM)*32)


//14336 Cycles

const int AICA_TICK = 145124;

struct Dreamcast_impl : VirtualDreamcast {

    unique_ptr<AicaContext> aica_ctx;
    unique_ptr<AudioStream> audio_stream;
#if !defined(HOST_NO_THREADS)
    cMutex callback_lock;
#endif
    function<void()> callback;
    
    int aica_schid = -1;
    int ds_schid = -1;

    Dreamcast_impl() { }

#ifndef TARGET_DISPFRAME
    void MainLoop()
    {
        // sh4/producer on cpu1 -- linux routes IRQs/softirqs to cpu0 by
        // default, and the jit is far more interference-sensitive than the
        // spinning ta consumer, which pins itself to cpu0
        ta_ring_pin_thread(1);

#if FEAT_HAS_NIXPROF
        install_prof_handler(0);
#endif

#ifdef SCRIPTING
        luabindings_onstart();
#endif

        if (settings.dynarec.Enable && sh4_cpu->setBackend(SH4BE_DYNAREC))
        {
            printf("Using MCPU Recompiler\n");
        }
        else
        {
            sh4_cpu->setBackend(SH4BE_INTERPRETER);
            printf("Using MCPU Interpreter\n");
        }

        auto scpu = sh4_cpu->GetA0H<SoundCPU>(A0H_SCPU);

        if (settings.dynarec.ScpuEnable && scpu->setBackend(ARM7BE_DYNAREC))
        {
            printf("Using SCPU Recompiler\n");
        }
        else
        {
            scpu->setBackend(ARM7BE_INTERPRETER);
            printf("Using SCPU Interpreter\n");
        }

        auto dsp = sh4_cpu->GetA0H<DSP>(A0H_DSP);

        if (settings.dynarec.DspEnable && dsp->setBackend(DSPBE_DYNAREC))
        {
            printf("Using DSP Recompiler\n");
        }
        else
        {
            dsp->setBackend(DSPBE_INTERPRETER);
            printf("Using DSP Interpreter\n");
        }

        if (!settings.aica.DSPEnabled)
        {
            printf("DSP is disabled\n");
        }

        sh4_cpu->Start();


        do {
            reset_requested = false;

            sh4_cpu->Run();

            SaveRomFiles(get_writable_data_path(DATA_PATH));
            if (reset_requested)
            {
                virtualDreamcast->Reset();
                sh4_cpu->Start();
#ifdef SCRIPTING
                luabindings_onreset();
#endif
            }
        } while (reset_requested);

#ifdef SCRIPTING
            luabindings_onstop();
#endif

            // g_GUIRenderer->WaitQueueEmpty();

#if !defined(HOST_NO_THREADS)
            callback_lock.Lock();
#endif
            verify(callback != nullptr);
            callback();
            callback = nullptr;
#if !defined(HOST_NO_THREADS)
            callback_lock.Unlock();
#endif
        }
#endif

    ~Dreamcast_impl() {
        Term();
    }


    int AicaUpdate(int tag, int c, int j)
    {
        //gpc_counter=0;
        //bm_Periodical_14k();

        //static int aica_sample_cycles=0;
        //aica_sample_cycles+=14336*AICA_SAMPLE_GCM;

        //if (aica_sample_cycles>=AICA_SAMPLE_CYCLES)
        {
            sh4_cpu->GetA0H<SoundCPU>(A0H_SCPU)->Update(512 * 32);
            sh4_cpu->GetA0H<AICA>(A0H_AICA)->Update(1 * 32);
            //aica_sample_cycles-=AICA_SAMPLE_CYCLES;
        }

        return AICA_TICK;
    }


    int DreamcastSecond(int tag, int c, int j)
    {
#if 1 //HOST_OS==OS_WINDOWS
        prof_periodical();
#endif

#if FEAT_SHREC != DYNAREC_NONE
        bm_Periodical_1s();
#endif

        //printf("%d ticks\n",sh4_sched_intr);
        sh4_sched_intr = 0;
        return SH4_MAIN_CLOCK;
    }

    void Reset()
    {
        plugins_Reset(false);

        mem_Reset(sh4_cpu, false);

        sh4_cpu->Reset(false);

        sh4_cpu->vram.Zero();
        sh4_cpu->aica_ram.Zero();
    }

    int StartGame(const string& path)
    {
        cfgSetVirtual("config", "image", path.c_str());

        if (settings.bios.UseReios || !LoadRomFiles("/media/fat/games/Dreamcast/")) // get_readonly_data_path(DATA_PATH)
        {
#ifdef USE_REIOS
            if (!LoadHle(get_readonly_data_path(DATA_PATH)))
            {
                return -5;
            }
            else
            {
                printf("Did not load bios, using reios\n");
            }
#else
            printf("Cannot find BIOS files\n");
            return -5;
#endif
        }

        rend_init_renderer(sh4_cpu->vram.data);

        if (plugins_Init())
            return -3;

#if DC_PLATFORM == DC_PLATFORM_NAOMI || DC_PLATFORM == DC_PLATFORM_ATOMISWAVE
        if (!naomi_cart_SelectFile())
            return -6;
#endif

        LoadCustom();

        mem_Init(sh4_cpu);

        mem_map_default(sh4_cpu);

#if DC_PLATFORM == DC_PLATFORM_NAOMI
        mcfg_CreateNAOMIJamma();
#elif DC_PLATFORM == DC_PLATFORM_ATOMISWAVE
        mcfg_CreateAtomisWaveControllers();
#endif

        Reset();

        Resume();

        return 0;
    }

    bool IsRunning()
    {
        return sh4_cpu && sh4_cpu->IsRunning();
    }

    bool Init()
    {
        audio_stream.reset(AudioStream::Create());
        audio_stream->InitAudio();

        sh4_cpu = SuperH4::Create();

        if (!_vmem_reserve(&sh4_cpu->mram, &sh4_cpu->vram , &sh4_cpu->aica_ram, INTERNAL_ARAM_SIZE))
        {
            printf("Failed to alloc mem\n");
            return false;
        }

        auto sh4mmr = SuperH4Mmr::Create(sh4_cpu);

        MMIODevice* biosDevice = Create_BiosDevice();
        MMIODevice* flashDevice = Create_FlashDevice();
        
        SystemBus* systemBus = Create_SystemBus();
        ASIC* asic = Create_ASIC(systemBus);

        MMIODevice* gdromOrNaomiDevice =
        
#if DC_PLATFORM == DC_PLATFORM_NAOMI || DC_PLATFORM == DC_PLATFORM_ATOMISWAVE
            Create_NaomiDevice(sh4mmr, systemBus, asic)
#else
            Create_GDRomDevice(sh4mmr, systemBus, asic)
#endif
        ;

        SPG* spg = SPG::Create(asic);

        if (settings.pvr.MultithreadedTA == TA_MTTA) {
            ta_ring_consumer_start(sh4_cpu->vram.data);
        } else if (settings.pvr.MultithreadedTA == TA_MTTA_DECOUPLED) {
            ta_ring_consumer_start_decoupled(sh4_cpu->vram.data);
        }
        MMIODevice* pvrDevice = Create_PVRDevice(sh4mmr, systemBus, asic, spg, sh4_cpu->mram.data, sh4_cpu->vram.data);

        aica_ctx.reset(AICA::CreateContext());

        DSP* dsp = DSP::Create(aica_ctx.get(), sh4_cpu->aica_ram.data, sh4_cpu->aica_ram.size);
        AICA* aicaDevice = AICA::Create(audio_stream.get(), systemBus, asic, dsp, aica_ctx.get(), sh4_cpu->aica_ram.data, sh4_cpu->aica_ram.size);
        SoundCPU* soundCPU = SoundCPU::Create(aicaDevice, sh4_cpu->aica_ram.data, sh4_cpu->aica_ram.size);

        MMIODevice* mapleDevice = Create_MapleDevice(systemBus, asic);
        

        MMIODevice* extDevice_006 =

#if DC_PLATFORM == DC_PLATFORM_DREAMCAST && defined(ENABLE_MODEM)
            Create_Modem(asic)
#else
            Create_ExtDevice_006()
#endif
        ;

        MMIODevice* extDevice_010 = 
        #if DC_PLATFORM == DC_PLATFORM_DREAMCAST && defined(ENABLE_BBA)
            Create_BBA(asic, CreateNullNetwork());
        #else
            Create_ExtDevice_010();
        #endif

        MMIODevice* rtcDevice = AICA::CreateRTC();

        sh4_cpu->sh4mmr.reset(sh4mmr);

        sh4_cpu->SetA0Handler(A0H_BIOS, biosDevice);
        sh4_cpu->SetA0Handler(A0H_FLASH, flashDevice);
        sh4_cpu->SetA0Handler(A0H_GDROM, gdromOrNaomiDevice);
        sh4_cpu->SetA0Handler(A0H_SB, systemBus);
        sh4_cpu->SetA0Handler(A0H_PVR, pvrDevice);
        sh4_cpu->SetA0Handler(A0H_EXTDEV_006, extDevice_006);
        sh4_cpu->SetA0Handler(A0H_AICA, aicaDevice);
        sh4_cpu->SetA0Handler(A0H_RTC, rtcDevice);
        sh4_cpu->SetA0Handler(A0H_EXTDEV_010, extDevice_010);

        sh4_cpu->SetA0Handler(A0H_MAPLE, mapleDevice);
        sh4_cpu->SetA0Handler(A0H_ASIC, asic);
        sh4_cpu->SetA0Handler(A0H_SPG, spg);
        sh4_cpu->SetA0Handler(A0H_SCPU, soundCPU);
        sh4_cpu->SetA0Handler(A0H_DSP, dsp);
        
#if DC_PLATFORM == DC_PLATFORM_DREAMCAST
        mcfg_CreateDevices();
#endif

        aica_schid = sh4_sched_register(this, 0, STATIC_FORWARD(Dreamcast_impl, AicaUpdate));
        sh4_sched_request(aica_schid, AICA_TICK);

        ds_schid = sh4_sched_register(this, 0, STATIC_FORWARD(Dreamcast_impl, DreamcastSecond));
        sh4_sched_request(ds_schid, SH4_MAIN_CLOCK);

        return sh4_cpu->Init();
    }


    void Term()
    {
        sh4_cpu->Term();
        
#if DC_PLATFORM != DC_PLATFORM_DREAMCAST
        naomi_cart_Close();
#endif
        plugins_Term();
        rend_term_renderer();

        _vmem_release(&sh4_cpu->mram, &sh4_cpu->vram, &sh4_cpu->aica_ram);

        mcfg_DestroyDevices();

        SaveSettings();

        delete sh4_cpu;
        sh4_cpu = nullptr;

        audio_stream->TermAudio();
        audio_stream.reset();
    }

    void Stop(function<void()> callback)
    {
        verify(sh4_cpu->IsRunning());

#if !defined(HOST_NO_THREADS)
        callback_lock.Lock();
        #endif
        verify(this->callback == nullptr);
        this->callback = callback;
        #if !defined(HOST_NO_THREADS)
        callback_lock.Unlock();
        #endif
        sh4_cpu->Stop();
    }

    // Called on the emulator thread for soft reset
    void RequestReset()
    {
        reset_requested = true;
        sh4_cpu->Stop();
    }

    void Resume()
    {
        verify(!sh4_cpu->IsRunning());
    }

    void cleanup_serialize(void* data)
    {
        if (data != NULL)
            free(data);
    }

    string get_savestate_file_path()
    {
        string state_file = cfgLoadStr("config", "image", "noname.chd");
        size_t lastindex = state_file.find_last_of("/");
        if (lastindex != -1)
            state_file = state_file.substr(lastindex + 1);
        lastindex = state_file.find_last_of(".");
        if (lastindex != -1)
            state_file = state_file.substr(0, lastindex);
        state_file = state_file + ".state";
        return get_writable_data_path(DATA_PATH) + state_file;
    }
    void SaveState(){ die("No states"); }
    void LoadState(){ die("No states"); }
/*
    void SaveState()
    {
        string filename;
        unsigned int total_size = 0;
        void* data = NULL;
        void* data_ptr = NULL;
        FILE* f;

        verify(!sh4_cpu->IsRunning());

        if (!dc_serialize(&data, &total_size))
        {
            printf("Failed to save state - could not initialize total size\n");
            g_GUI->DisplayNotification("Save state failed", 2000);
            cleanup_serialize(data);
            return;
        }

        data = malloc(total_size);
        if (data == NULL)
        {
            printf("Failed to save state - could not malloc %d bytes", total_size);
            g_GUI->DisplayNotification("Save state failed - memory full", 2000);
            cleanup_serialize(data);
            return;
        }

        data_ptr = data;

        if (!dc_serialize(&data_ptr, &total_size))
        {
            printf("Failed to save state - could not serialize data\n");
            g_GUI->DisplayNotification("Save state failed", 2000);
            cleanup_serialize(data);
            return;
        }

        filename = get_savestate_file_path();
        f = fopen(filename.c_str(), "wb");

        if (f == NULL)
        {
            printf("Failed to save state - could not open %s for writing\n", filename.c_str());
            g_GUI->DisplayNotification("Cannot open save file", 2000);
            cleanup_serialize(data);
            return;
        }

        fwrite(data, 1, total_size, f);
        fclose(f);

        cleanup_serialize(data);
        printf("Saved state to %s\n size %d", filename.c_str(), total_size);
        g_GUI->DisplayNotification("State saved", 1000);
    }

    void LoadState()
    {
        string filename;
        unsigned int total_size = 0;
        void* data = NULL;
        void* data_ptr = NULL;
        FILE* f;

        verify(!sh4_cpu->IsRunning());

        filename = get_savestate_file_path();
        f = fopen(filename.c_str(), "rb");

        if (f == NULL)
        {
            printf("Failed to load state - could not open %s for reading\n", filename.c_str());
            g_GUI->DisplayNotification("Save state not found", 2000);
            cleanup_serialize(data);
            return;
        }
        fseek(f, 0, SEEK_END);
        total_size = (unsigned int)ftell(f);
        fseek(f, 0, SEEK_SET);
        data = malloc(total_size);
        if (data == NULL)
        {
            printf("Failed to load state - could not malloc %d bytes", total_size);
            g_GUI->DisplayNotification("Failed to load state - memory full", 2000);
            cleanup_serialize(data);
            return;
        }

        fread(data, 1, total_size, f);
        fclose(f);


        data_ptr = data;

        sh4_cpu->ResetCache();
        sh4_cpu->GetA0H<SoundCPU>(A0H_SCPU)->InvalidateJitCache();

        if (!dc_unserialize(&data_ptr, &total_size))
        {
            printf("Failed to load state - could not unserialize data\n");
            g_GUI->DisplayNotification("Invalid save state", 2000);
            cleanup_serialize(data);
            return;
        }

        mmu_set_state();
        sh4_sched_ffts();

        // TODO: save state fix this
        dynamic_cast<SPG*>(sh4_cpu->GetA0Handler(A0H_SPG))->CalculateSync();

        cleanup_serialize(data);
        printf("Loaded state from %s size %d\n", filename.c_str(), total_size);
    }
*/
#if defined(TARGET_NO_EXCEPTIONS)
    bool HandleFault(unat addr, rei_host_context_t* ctx) {
        die("Fault handling disabled");
        return false;
    }
#else
    bool HandleFault(unat addr, rei_host_context_t* ctx)
    {
        if (!sh4_cpu)
            return false;

        fault_printf("dc_handle_fault: %p from %p\n", (u8*)addr, (u8*)ctx->pc);

        bool dyna_cde = ((unat)CC_RX2RW(ctx->pc) > (unat)CodeCache) && ((unat)CC_RX2RW(ctx->pc) < (unat)(CodeCache + CODE_SIZE));

        u8* address = (u8*)addr;

        /*
        // no texcache -> no vram locks
        if (VramLockedWrite(sh4_cpu->vram.data, address))
        {
            fault_printf("VramLockedWrite!\n");

            return true;
        }
        else */ if (_vmem_bm_LockedWrite(address))
        {
            fault_printf("dc_handle_fault: _vmem_bm_LockedWrite!\n");

            return true;
        }
        else if (bm_LockedWrite(address))
        {
            fault_printf("dc_handle_fault: bm_LockedWrite!\n");
            return true;
        }
        else if (dyna_cde && rdv_ngen && rdv_ngen->Rewrite(ctx))
        {
            fault_printf("dc_handle_fault: rdv_ngen->Rewrite!\n");
            return true;
        }
        else
        {
            fault_printf("dc_handle_fault: not handled!\n");
            return false;
        }
    }
#endif
};

VirtualDreamcast* VirtualDreamcast::Create() {
    return new Dreamcast_impl();
}
