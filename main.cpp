#include "math.h"
#include "types.h"
#include "libswirl.h"
// #include "xparameters.h"
// #include "xdebug.h"
// #include "sleep.h"
#include <stdio.h>
#include <stdarg.h>

#include "cfg/cfg.h"

//Joystick stuff
#include <fcntl.h>
#include <unistd.h>

#include <atomic>

#include "hw/pvr/pvr_regs.h"
#include "rend/mister_rend/mister_support.h"

std::atomic<bool> vram_dump_pending;
static unsigned vram_dump_number;

u32 pvr_map32(u32 offset32);

void do_vram_dump(u8* vram, u8* pvr_regs) {
	if (vram_dump_pending.exchange(false)) {
		FILE* f = fopen(("vram_" + std::to_string(vram_dump_number) + ".bin").c_str(), "wb");
		if (!f) {
			printf("Failed to open vram_dump.bin\n");
			return;
		}
		for (unsigned i = 0; i < VRAM_SIZE; i+=4) {
			if (fwrite(&vram[pvr_map32(i)], 4, 1, f) != 1) {
				printf("Failed to write vram_dump.bin\n");
				fclose(f);
				return;
			}
		}
		fclose(f);
		f = fopen(("pvr_regs_" + std::to_string(vram_dump_number) + ".bin").c_str(), "wb");
		if (!f) {
			printf("Failed to open pvr_regs.bin\n");
			return;
		}
		if (fwrite(pvr_regs, pvr_RegSize, 1, f) != 1) {
			printf("Failed to write pvr_regs.bin\n");
			fclose(f);
			return;
		}
		fclose(f);
		printf("Frame dumped -> vram_%d.bin/pvr_regs_%d.bin\n", vram_dump_number, vram_dump_number);
		vram_dump_number++;
	}
}

#include "hw/sh4/sh4_mem.h"   // SaveRomFiles
#include <signal.h>
#include <pthread.h>
#include "stdclass.h"
#include "ra_dc.h"
#include "ra_overlay.h"

// Upper bound for letting RetroAchievements submissions still in flight
// finish on exit. The launcher's Start+L quit sends SIGTERM, waits 1.5 s,
// taps ESC, waits 2 s more, then SIGKILLs - stay inside that. Only spent
// when a request is actually pending (normally the exit is immediate).
static const int RA_EXIT_FLUSH_MS = 3000;

// The fast exits below (_exit on ESC / mapped Exit, SIGTERM from the
// launcher) skip the normal teardown, which is what writes the Dreamcast
// flash (BIOS settings: language, audio mode, ...) back to dc_nvmem.bin.
static void save_before_exit()
{
	// First caller wins: with a RetroAchievements flush in progress (SIGTERM),
	// the launcher's fallback ESC tap must not _exit() underneath it - a
	// second caller parks here until the first one exits the process.
	static std::atomic<bool> exiting(false);
	if (exiting.exchange(true)) {
		for (;;)
			pause();
	}
	// same directory LoadRomFiles reads (libswirl.cpp), where dc_nvmem.bin
	// takes precedence over dc_flash.bin on the next boot
	SaveRomFiles("/media/fat/games/Dreamcast/");
	fflush(stdout);
	sync();
	// after the flash: an achievement unlocked right before quitting
	ra_shutdown(RA_EXIT_FLUSH_MS);
}

// In-game menu Load Game / Exit Emulator (ra_overlay.h): request.json is
// written already; called on the emulation thread, parked in the vblank hook
// with the "Exiting..." page on screen - the same exit as ESC / SIGTERM.
void ra_frontend_exit(void)
{
	printf("minicast: exit requested from the in-game menu, saving and exiting%c", 10);
	fflush(stdout);
	save_before_exit();
	_exit(0);
}

// SIGTERM is blocked in every thread and taken here synchronously, so the
// save runs in a normal thread context, not inside a signal handler.
static void* sigterm_thread(void* arg)
{
	sigset_t* set = (sigset_t*)arg;
	int sig = 0;
	if (sigwait(set, &sig) == 0) {
		printf("minicast: SIGTERM, saving flash and exiting%c", 10);
		fflush(stdout);
		save_before_exit();
		_exit(0);
	}
	return nullptr;
}

int main(int argc, char* argv[])
{
	static sigset_t term_set;
	sigemptyset(&term_set);
	sigaddset(&term_set, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &term_set, nullptr);
	pthread_t term_thread;
	pthread_create(&term_thread, nullptr, sigterm_thread, &term_set);

	#if HOST_OS == OS_LINUX
	void common_linux_setup();
	common_linux_setup();
	#endif
	
	set_user_config_dir(".");
	set_user_data_dir(".");
	add_system_config_dir(".");
	add_system_data_dir(".");

	ParseCommandLine(argc, argv);
	cfgOpen();

	const char* game = argc == 1 ? "" : argv[1];

	// RetroAchievements: off (and free) unless /media/fat/retroachievements.cfg
	// has a username and MINICAST_RA != 0; hashes the disc on its own thread
	ra_init(game);
	// in-game menu (ra_overlay / ra_menu): title + "current game" fallback
	ra_overlay_set_game(game);

	libswirl_init();
	libswirl_loop(game);

	// normal teardown: stop reading emulated RAM before it is released
	ra_shutdown(RA_EXIT_FLUSH_MS);

	return 0;
}

void os_DebugBreak()
{
    for(;;);
}

int msgboxf(char const* msg, unsigned int d, ...) {
	char buffer[512];
	va_list args;
	va_start (args, d);
	vsnprintf (buffer,512,msg, args);
	puts (buffer);
	va_end (args);
    return 0;
}

bool rc_serialize(void* src, unsigned int src_size, void** dest, unsigned int* total_size) { return false; }
bool rc_unserialize(void* src, unsigned int src_size, void** dest, unsigned int* total_size) { return false; }
bool dc_serialize(void** data, unsigned int* total_size) { return false; }
bool dc_unserialize(void** data, unsigned int* total_size) { return false; }
struct RegisterStruct;
bool register_serialize(RegisterStruct* regs, size_t size, void** data, unsigned int* total_size) { return false; }
bool register_unserialize(RegisterStruct* regs, size_t size, void** data, unsigned int* total_size) { return false; }

#if defined(FAUX96)
#include <sys/time.h>
#endif

#if HOST_OS==OS_XIL_BARE
double os_GetSeconds()
{
	#if defined(FAUX96)
		timeval a;
		gettimeofday (&a,0);
		static u64 tvs_base=a.tv_sec;
		return a.tv_sec-tvs_base+a.tv_usec/1000000.0;
	#else
    die("os_GetSeconds()");
    return 0;
	#endif
}
#endif

u16 kcode[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
u8 rt[4] = {0, 0, 0, 0};
u8 lt[4] = {0, 0, 0, 0};
u32 vks[4];
s8 joyx[4] = {0, 0, 0, 0};
s8 joyy[4] = {0, 0, 0, 0};

#include "oslib/threading.h"

#include <pthread.h>

#if !defined(HOST_NO_THREADS)
void cThread::Start() {
	hThread = new pthread_t;
	// pthread_create( hThread, NULL, entry, param);
}
void cThread::WaitToEnd() {
	if (hThread) {
		// pthread_join(*hThread,0);
		delete hThread;
		hThread = NULL;
	}
}

cMutex::cMutex() {
	// pthread_mutex_init(&mutx, NULL);
}
cMutex::~cMutex() {
	// pthread_mutex_destroy(&mutx);
}
void cMutex::Lock() {
	// pthread_mutex_lock(&mutx);
}
bool cMutex::TryLock() {
	// return pthread_mutex_trylock(&mutx)==0;
    return false;
}
void cMutex::Unlock() {
	// pthread_mutex_unlock(&mutx);
}

cResetEvent::cResetEvent() {
	// pthread_mutex_init(&mutx, NULL);
	// pthread_cond_init(&cond, NULL);
}
cResetEvent::~cResetEvent() {
}
void cResetEvent::Set()//Signal
{
	// pthread_mutex_lock( &mutx );
	// state=true;
    // pthread_cond_signal( &cond);
	// pthread_mutex_unlock( &mutx );
}
void cResetEvent::Reset()//reset
{
	// pthread_mutex_lock( &mutx );
	// state=false;
	// pthread_mutex_unlock( &mutx );
}
bool cResetEvent::Wait(unsigned msec)//Wait for signal , then reset
{
	// pthread_mutex_lock( &mutx );
	// if (!state)
	// {
	// 	struct timespec ts;
	// 	#if HOST_OS == OS_DARWIN
	// 		// OSX doesn't have clock_gettime.
	// 		clock_serv_t cclock;
	// 		mach_timespec_t mts;

	// 		host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
	// 		clock_get_time(cclock, &mts);
	// 		mach_port_deallocate(mach_task_self(), cclock);
	// 		ts.tv_sec = mts.tv_sec;
	// 		ts.tv_nsec = mts.tv_nsec;
	// 	#else
	// 		clock_gettime(CLOCK_REALTIME, &ts);
	// 	#endif
	// 	ts.tv_sec += msec / 1000;
	// 	ts.tv_nsec += (msec % 1000) * 1000000;
	// 	while (ts.tv_nsec > 1000000000)
	// 	{
	// 		ts.tv_nsec -= 1000000000;
	// 		ts.tv_sec++;
	// 	}
	// 	pthread_cond_timedwait( &cond, &mutx, &ts );
	// }
	// bool rc = state;
	// state=false;
	// pthread_mutex_unlock( &mutx );

	// return rc;
    return false;
}
void cResetEvent::Wait()//Wait for signal , then reset
{
	// pthread_mutex_lock( &mutx );
	// if (!state)
	// {
	// 	pthread_cond_wait( &cond, &mutx );
	// }
	// state=false;
	// pthread_mutex_unlock( &mutx );
}

#endif

void SleepMs(unsigned count) {
	// usleep(count * 1000);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
	*memptr=aligned_alloc(alignment, size);
    return *memptr == 0;
}

void prof_periodical() { }

char naomi_game_id[33];
struct InputDescriptors;
InputDescriptors *NaomiGameInputs;
u8 *naomi_default_eeprom;


u32 PVR_VTXC;

void os_SetWindowText(char const* msg) {
    //die("os_SetWindowText(char const*)");
	msgboxf("SetWindowText: %s\n", 0, msg);
}

void push_vmu_screen(int, int, unsigned char*) {

}

int get_mic_data(unsigned char*) {  // no microphone on this build
	return 0;
}

bool bios_loaded;

#if HOST_OS == OS_XIL_BARE
void VLockedMemory::LockRegion(unsigned offset, unsigned size_bytes) {
	#ifndef TARGET_NO_EXCEPTIONS
	
	#endif
}

void VLockedMemory::UnLockRegion(unsigned offset, unsigned size_bytes) {
	#ifndef TARGET_NO_EXCEPTIONS
	
	#endif
}
#endif

#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

int kbhit(void) {
    struct termios oldt, newt;
    int ch;
    int oldf;

    // Get current terminal settings
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    // Disable canonical mode and echo
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    // Remember old file status flags, then set non-blocking
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

    // Try to read
    ch = getchar();

    // Restore old settings
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);

    if (ch != EOF) {
        // If we got a character, push it back so we can read it later
        ungetc(ch, stdin);
        return 1;
    }

    return 0;
}

#include "input/gamepad.h"

// ---------------------------------------------------------------------------
// evdev input mapping, driven by the emu.cfg [input0]..[inputN] sections
// written by Scripts/DreamSTer.sh. Mapping value grammar (colon separated):
//   key:<code>              button/key press
//   keys:<neg>:<pos>        key pair driving an axis (-1 = unset)
//   abs:<code>:<dir>        absolute axis, dir 1 or -1
//   rel:<code>:<dir>        relative axis (mouse motion)
// Devices are matched by DeviceId = bus:vid:pid:ver[:uniq][#n] (all hex,
// #n disambiguates identical ids in /dev/input/event* numeric order).
// If emu.cfg has no [inputN] sections, the legacy /dev/input/js0 mapping
// below is used instead.
//
// Each device feeds the maple port its [inputN] `Port` key selects (0..3 =
// A..D, default 0/A). Devices sharing a port merge: buttons are logically
// OR'd, for axes the value with the biggest absolute value wins. The
// matching maple bus must have a controller ([input] deviceN = 0;
// DreamSTer.sh enables it for every port in use).
//
// ESC on any evdev device (mapped or not) is a hardcoded exit; Print Screen
// queues an spg_screenshot the same way (taken at the next SPG vblank).
// ButtonExit / ButtonScreenshot in an [inputN] section additionally map a
// per-device button (or key) to the same two actions.
//
// In-game menu (ra_overlay / ra_menu; with MINICAST_MENU=0 the RetroAchievements
// achievement list instead): Start + Select or Start + R
// held 0.5 s on a mapped pad, or F6 on any keyboard. Select is the pad's
// BTN_SELECT (or the [inputN] ButtonSelect = key:<code> mapping); the Dreamcast
// has no Select, so while it is held the pad's Start is kept from the game.
// Start + R is for pads without a Select (arcade sticks, where the coin button
// is usually the Exit button): R is the mapped R trigger (digital, or analog
// past ~80%); R and Start still reach the game (a quick Start tap pauses as
// usual - racing games hold R), only a 0.5 s hold of both opens the menu. While
// the menu is open the arrow keys / Enter / Backspace / PgUp / PgDn of any
// keyboard work too.

#include <linux/input.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <glob.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

enum InTarget {
	TGT_DPAD_UP, TGT_DPAD_DOWN, TGT_DPAD_LEFT, TGT_DPAD_RIGHT,
	TGT_BTN_A, TGT_BTN_B, TGT_BTN_X, TGT_BTN_Y, TGT_BTN_START,
	TGT_ANALOG_X, TGT_ANALOG_Y, TGT_TRIG_L, TGT_TRIG_R,
	// emulator actions (no DC pad bit): exit + screenshot, mappable per
	// device via DreamSTer.sh, IN ADDITION to the hardcoded ESC / SYSRQ
	TGT_BTN_EXIT, TGT_BTN_SCREENSHOT,
	TGT_COUNT
};

static const char* TARGET_CFG_KEYS[TGT_COUNT] = {
	"DpadUp", "DpadDown", "DpadLeft", "DpadRight",
	"ButtonA", "ButtonB", "ButtonX", "ButtonY", "ButtonStart",
	"AxisX", "AxisY", "TriggerL", "TriggerR",
	"ButtonExit", "ButtonScreenshot",
};

static const u16 TARGET_BTN_BIT[TGT_COUNT] = {
	DC_DPAD_UP, DC_DPAD_DOWN, DC_DPAD_LEFT, DC_DPAD_RIGHT,
	DC_BTN_A, DC_BTN_B, DC_BTN_X, DC_BTN_Y, DC_BTN_START,
	0, 0, 0, 0,
	0, 0,
};

struct InMapping {
	enum Kind { NONE, KEY, KEYPAIR, ABS, REL } kind;
	int code;       // KEY/ABS/REL
	int dir;        // ABS/REL: 1 or -1
	int neg, pos;   // KEYPAIR key codes, -1 = unset
};

static bool decode_mapping(const char* text, InMapping* m) {
	int a, b;
	memset(m, 0, sizeof(*m));
	m->kind = InMapping::NONE;
	m->neg = m->pos = -1;
	m->dir = 1;
	if (sscanf(text, "key:%d", &a) == 1) {
		m->kind = InMapping::KEY; m->code = a;
	} else if (sscanf(text, "keys:%d:%d", &a, &b) == 2) {
		m->kind = InMapping::KEYPAIR; m->neg = a; m->pos = b;
	} else if (sscanf(text, "abs:%d:%d", &a, &b) == 2) {
		m->kind = InMapping::ABS; m->code = a; m->dir = b >= 0 ? 1 : -1;
	} else if (sscanf(text, "rel:%d:%d", &a, &b) == 2) {
		m->kind = InMapping::REL; m->code = a; m->dir = b >= 0 ? 1 : -1;
	}
	return m->kind != InMapping::NONE;
}

// ------------------------------------------------------------------------
// maple keyboard / mouse feeding: a device whose configured port carries a
// DC keyboard or mouse ([input] deviceN = 4/5, set by DreamSTer.sh's
// Peripherals page) drives the maple device state polled by maple_devs.cpp
// (this is the only writer on the MiSTer build - the libswirl/input and
// linux-dist frontends that normally feed these globals are compiled out).

#include "hw/maple/maple_devs.h"   // MDT_* device types
#include "hw/sh4/sh4_mem.h"         // SaveRomFiles

// The exit keys below leave via _exit(), skipping the teardown that writes
// the Dreamcast flash back - save it first so BIOS settings (language,
// stereo/mono, ...) persist. Same folder LoadRomFiles reads it from.
static void save_flash_before_exit() {
	SaveRomFiles("/media/fat/games/Dreamcast/");
	sync();
}

extern u8 kb_key[6];      // DC keyboard matrix: up to 6 held keys (HID codes)
extern u8 kb_shift;       // DC keyboard modifier bitmask (HID byte)
extern u32 mo_buttons;    // DC mouse buttons, active low
extern f32 mo_x_delta, mo_y_delta, mo_wheel_delta;

// linux evdev keycode -> DC (USB HID) keyboard usage code; modifiers are
// handled separately via dc_kbd_input, 0 = no DC equivalent
static const u8 LINUX_TO_DC_KEY[128] = {
	/*   0 */ 0,    0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, // ESC, 1..6
	/*   8 */ 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B, // 7..0 - = BS TAB
	/*  16 */ 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, // Q..I
	/*  24 */ 0x12, 0x13, 0x2F, 0x30, 0x28, 0,    0x04, 0x16, // O P [ ] RET, A S
	/*  32 */ 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, // D..L ;
	/*  40 */ 0x34, 0x35, 0,    0x31, 0x1D, 0x1B, 0x06, 0x19, // ' ` \ Z X C V
	/*  48 */ 0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0,    0x55, // B N M , . / KP*
	/*  56 */ 0,    0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, // SPC CAPS F1..F5
	/*  64 */ 0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F, // F6..F10 NUM SCR KP7
	/*  72 */ 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59, // KP8 KP9 KP- KP4..6 KP+ KP1
	/*  80 */ 0x5A, 0x5B, 0x62, 0x63, 0,    0,    0x64, 0x44, // KP2 KP3 KP0 KP. 102ND F11
	/*  88 */ 0x45, 0,    0,    0,    0,    0,    0,    0,    // F12
	/*  96 */ 0x58, 0,    0x54, 0x46, 0,    0,    0x4A, 0x52, // KPRET KP/ PRTSC HOME UP
	/* 104 */ 0x4B, 0x50, 0x4F, 0x4D, 0x51, 0x4E, 0x49, 0x4C, // PGUP LEFT RIGHT END DOWN PGDN INS DEL
	/* 112 */ 0,    0,    0,    0,    0,    0,    0,    0x48, // PAUSE
	/* 120 */ 0,    0,    0,    0,    0,    0,    0,    0,
};

static void dc_kbd_input(u16 code, bool pressed) {
	u8 mod = 0;
	switch (code) {
	case KEY_LEFTCTRL:   mod = 0x01; break;
	case KEY_LEFTSHIFT:  mod = 0x02; break;
	case KEY_LEFTALT:    mod = 0x04; break;
	case KEY_LEFTMETA:   mod = 0x08; break;   // S1
	case KEY_RIGHTCTRL:  mod = 0x10; break;
	case KEY_RIGHTSHIFT: mod = 0x20; break;
	case KEY_RIGHTALT:   mod = 0x40; break;
	case KEY_RIGHTMETA:  mod = 0x80; break;   // S2
	}
	if (mod) {
		if (pressed)
			kb_shift |= mod;
		else
			kb_shift &= ~mod;
		return;
	}
	u8 dc = code < 128 ? LINUX_TO_DC_KEY[code] : 0;
	if (!dc)
		return;
	if (pressed) {  // 6-key rollover, ignore re-press/autorepeat
		for (int i = 0; i < 6; i++)
			if (kb_key[i] == dc)
				return;
		for (int i = 0; i < 6; i++)
			if (kb_key[i] == 0) {
				kb_key[i] = dc;
				return;
			}
	} else {
		for (int i = 0; i < 6; i++)
			if (kb_key[i] == dc) {
				for (; i < 5; i++)
					kb_key[i] = kb_key[i + 1];
				kb_key[5] = 0;
				return;
			}
	}
}

static void dc_mouse_input(u16 type, u16 code, s32 value) {
	if (type == EV_REL) {
		float sens = settings.input.MouseSensitivity / 100.0f;
		if (code == REL_X)
			mo_x_delta += value * sens;
		else if (code == REL_Y)
			mo_y_delta += value * sens;
		else if (code == REL_WHEEL)
			mo_wheel_delta -= value * 16;   // wheel up = negative, as x11.cpp
	} else if (type == EV_KEY) {
		u32 mask = 0;
		if (code == BTN_LEFT)
			mask = 1 << 2;
		else if (code == BTN_RIGHT)
			mask = 1 << 1;
		else if (code == BTN_MIDDLE)
			mask = 1 << 3;
		if (mask) {
			if (value)
				mo_buttons &= ~mask;
			else
				mo_buttons |= mask;
		}
	}
}

#define EVDEV_REL_GAIN       4.0f  // mouse counts -> analog deflection
#define ABS_CODES            64    // ABS_MAX + 1

static int hid_shadow_open(const std::string& evpath);
static void hid_shadow_close(int slot);
static uint64_t mono_ns();

struct EvdevPad {
	int fd;
	std::string path, id;
	std::string baseId;        // id without the #n suffix (rescan numbering)
	dev_t rdev;                // node identity: a replaced node reuses the path
	ino_t ino;
	bool dead;                 // removed: fds closed, dropped by evdev_reap
	int port;                  // maple port 0..3 this device feeds
	bool feedKb;               // port carries a DC keyboard: feed kb_key/kb_shift
	bool feedMouse;            // port carries a DC mouse: feed mo_*
	InMapping map[TGT_COUNT];

	int absLo[ABS_CODES], absHi[ABS_CODES];
	bool absValid[ABS_CODES];

	bool ffRumble;             // supports FF_RUMBLE, fd is open read-write
	int ffEffect;              // uploaded rumble effect id, -1 = none yet

	bool held[TGT_COUNT];      // dpad_*/btn_* targets currently pressed
	bool actPrev[2];           // TGT_BTN_EXIT/SCREENSHOT state last update
	int analog[2];             // TGT_ANALOG_X/Y, -128..127
	int trig[2];               // TGT_TRIG_L/R, 0..255
	bool kpNeg[2], kpPos[2];   // keypair halves per analog axis
	bool relDriven[2];         // analog axis fed by rel (mouse) motion
	float relAccum[2];         // motion accumulator, decays in tick
	bool synDropped;           // SYN_DROPPED seen: skip to SYN_REPORT, then resync
	int hidFd;                 // hidraw node of the same device (GET_REPORT), -1 = none
	uint64_t lastEventNs;      // last key/abs event read from evdev
	uint64_t lastCheckedSample;// shadow sample seq already compared
	int selectCode;            // key code of the pad's Select (RA list hotkey)
	bool selHeld;              // Select currently held
	uint64_t comboSince;       // Start+Select held since (mono ns), 0 = not
	bool comboFired;           // hotkey sent for this hold
};

static bool g_evdev_mode = false;   // any [inputN] section in emu.cfg
static std::vector<EvdevPad> g_evdev_pads;

static std::string evdev_device_id(int fd) {
	struct input_id id;
	if (ioctl(fd, EVIOCGID, &id) < 0)
		return "";
	char buf[64];
	snprintf(buf, sizeof(buf), "%04x:%04x:%04x:%04x",
			 id.bustype, id.vendor, id.product, id.version);
	std::string s = buf;
	char uniq[64] = {0};
	if (ioctl(fd, EVIOCGUNIQ(sizeof(uniq) - 1), uniq) >= 0 && uniq[0]) {
		s += ":";
		s += uniq;
	}
	return s;
}

static int evdev_path_num(const std::string& path) {
	int n = 0;
	for (size_t i = 0; i < path.size(); i++)
		if (path[i] >= '0' && path[i] <= '9')
			n = n * 10 + (path[i] - '0');
	return n;
}

static bool evdev_path_less(const std::string& a, const std::string& b) {
	return evdev_path_num(a) < evdev_path_num(b);
}

// device id -> mappings, from [input0]..[inputN]; kept for hotplug rescans
struct CfgPad { InMapping map[TGT_COUNT]; bool any; int port; int selectCode; };
static std::map<std::string, CfgPad> g_cfgpads;

// hotplug: /dev/input mtime the last scan saw, debounced by one poll period
static struct timespec g_input_mtime_scanned, g_input_mtime_seen;
static bool g_input_change_pending = false;
static uint64_t g_hotplug_next_ns = 0;
static bool g_evdev_any_dead = false;
static const uint64_t HOTPLUG_POLL_NS = 500000000ULL;

// A removed device: close its fds (the hidraw shadow is handed back to the
// shadow thread, which owns its fd), release whatever it held on maple
// keyboard/mouse state, and mark it for evdev_reap. Pad buttons/axes need
// no release: evdev_update rebuilds all four ports from the live pads.
static void evdev_remove_pad(EvdevPad& p, const char* why) {
	if (p.dead)
		return;
	printf("evdev: lost %s [%s]%s\n", p.path.c_str(), p.id.c_str(), why);
	if (p.fd >= 0)
		close(p.fd);
	p.fd = -1;
	if (p.hidFd >= 0)
		hid_shadow_close(p.hidFd);
	p.hidFd = -1;
	if (p.feedKb) {
		memset(kb_key, 0, sizeof(kb_key));
		kb_shift = 0;
	}
	if (p.feedMouse)
		mo_buttons |= (1 << 1) | (1 << 2) | (1 << 3);
	memset(p.held, 0, sizeof(p.held));
	memset(p.analog, 0, sizeof(p.analog));
	memset(p.trig, 0, sizeof(p.trig));
	p.ffRumble = false;
	p.dead = true;
	g_evdev_any_dead = true;
}

// drop removed pads; only called outside any g_evdev_pads iteration (all
// pad users - UpdateInputState / UpdateVibration - run on the maple thread)
static void evdev_reap() {
	if (!g_evdev_any_dead)
		return;
	g_evdev_any_dead = false;
	for (size_t i = 0; i < g_evdev_pads.size(); ) {
		if (g_evdev_pads[i].dead)
			g_evdev_pads.erase(g_evdev_pads.begin() + i);
		else
			i++;
	}
}

static bool evdev_id_live(const std::string& id) {
	for (size_t i = 0; i < g_evdev_pads.size(); i++)
		if (!g_evdev_pads[i].dead && g_evdev_pads[i].id == id)
			return true;
	return false;
}

static void evdev_load_cfg() {
	for (int n = 0; ; n++) {
		char section[32];
		snprintf(section, sizeof(section), "input%d", n);
		if (cfgExists(section, "DeviceId") != 2)
			break;
		g_evdev_mode = true;
		std::string devid = cfgLoadStr(section, "DeviceId", "");
		if (devid.empty())
			continue;
		CfgPad pad;
		pad.any = false;
		pad.port = cfgLoadInt(section, "Port", 0);
		if (pad.port < 0 || pad.port > 3)
			pad.port = 0;
		for (int t = 0; t < TGT_COUNT; t++) {
			pad.map[t].kind = InMapping::NONE;
			std::string val = cfgLoadStr(section, TARGET_CFG_KEYS[t], "");
			if (!val.empty() && decode_mapping(val.c_str(), &pad.map[t]))
				pad.any = true;
		}
		// Select for the RA list hotkey: optional ButtonSelect = key:<code>,
		// else the standard gamepad BTN_SELECT
		pad.selectCode = BTN_SELECT;
		{
			InMapping sm;
			std::string val = cfgLoadStr(section, "ButtonSelect", "");
			if (!val.empty() && decode_mapping(val.c_str(), &sm) && sm.kind == InMapping::KEY)
				pad.selectCode = sm.code;
		}
		g_cfgpads[devid] = pad;
	}
}

static std::string evdev_numbered_id(const std::string& base, int n) {
	if (n <= 1)
		return base;
	char suffix[16];
	snprintf(suffix, sizeof(suffix), "#%d", n);
	return base + suffix;
}

// Sensor side-nodes of modern pads (DualSense / DualShock 4 "Motion Sensors",
// "Touchpad", ...): no controls, and a DualSense's motion node sends ~1000
// events/s - monitoring it cost a read + SYN_DROPPED resync every frame on
// the emulation thread. Left closed unless explicitly mapped (they still
// count for the #N numbering, which the caller has done already).
static bool evdev_noisy_node(int fd) {
	char name[256];
	memset(name, 0, sizeof(name));
	if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
		return false;
	for (char* c = name; *c; c++)
		*c = tolower((unsigned char)*c);
	static const char* const parts[] = { "motion sensor", "touchpad", "accelerometer", "gyro" };
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
		if (strstr(name, parts[i]))
			return true;
	return false;
}

// Builds the EvdevPad for an event node already opened O_RDONLY (fd) whose
// numbered DeviceId is `id`, and appends it to g_evdev_pads.
static void evdev_add_pad(const std::string& path, int fd, const std::string& baseId,
                          const std::string& id, bool hotplug) {
	const char* hp = hotplug ? " (hotplug)" : "";
	{
		// unmatched devices stay open in monitor-only mode: no mappings,
		// but the hardcoded ESC exit still watches them
		std::map<std::string, CfgPad>::iterator it = g_cfgpads.find(id);
		bool mapped = it != g_cfgpads.end() && it->second.any;
		if (!mapped && evdev_noisy_node(fd)) {
			printf("evdev: skipping sensor node %s [%s]%s\n", path.c_str(), id.c_str(), hp);
			close(fd);
			return;
		}
		struct stat st;
		memset(&st, 0, sizeof(st));
		fstat(fd, &st);

		// rumble (purupuru pack): mapped controllers advertising FF_RUMBLE are
		// reopened read-write so effects can be uploaded and played
		bool ffRumble = false;
		if (mapped) {
			unsigned long ffbits[(FF_MAX + 8 * sizeof(unsigned long)) /
			                     (8 * sizeof(unsigned long))] = {0};
			if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffbits)), ffbits) >= 0 &&
				(ffbits[FF_RUMBLE / (8 * sizeof(unsigned long))] >>
				 (FF_RUMBLE % (8 * sizeof(unsigned long)))) & 1) {
				int rw = open(path.c_str(), O_RDWR | O_NONBLOCK);
				struct stat rst;
				if (rw >= 0 && (fstat(rw, &rst) != 0 || rst.st_rdev != st.st_rdev)) {
					close(rw);   // node replaced in between: keep the first fd
					rw = -1;
				}
				if (rw >= 0) {
					close(fd);
					fd = rw;
					ffRumble = true;
				}
			}
		}

		EvdevPad pad;
		memset(&pad.absLo, 0, sizeof(pad.absLo));
		memset(&pad.absHi, 0, sizeof(pad.absHi));
		memset(&pad.absValid, 0, sizeof(pad.absValid));
		memset(&pad.held, 0, sizeof(pad.held));
		memset(&pad.analog, 0, sizeof(pad.analog));
		memset(&pad.trig, 0, sizeof(pad.trig));
		memset(&pad.kpNeg, 0, sizeof(pad.kpNeg));
		memset(&pad.kpPos, 0, sizeof(pad.kpPos));
		memset(&pad.relDriven, 0, sizeof(pad.relDriven));
		memset(&pad.relAccum, 0, sizeof(pad.relAccum));
		memset(&pad.actPrev, 0, sizeof(pad.actPrev));
		pad.fd = fd;
		pad.path = path;
		pad.id = id;
		pad.baseId = baseId;
		pad.rdev = st.st_rdev;
		pad.ino = st.st_ino;
		pad.dead = false;
		pad.port = 0;
		pad.ffRumble = ffRumble;
		pad.ffEffect = -1;
		pad.feedKb = false;
		pad.feedMouse = false;
		pad.synDropped = false;
		pad.hidFd = -1;
		pad.lastEventNs = 0;
		pad.lastCheckedSample = 0;
		pad.selectCode = it != g_cfgpads.end() ? it->second.selectCode : BTN_SELECT;
		pad.selHeld = false;
		pad.comboSince = 0;
		pad.comboFired = false;
		if (it != g_cfgpads.end()) {
			// known device: if its port carries a maple keyboard/mouse
			// (Peripherals page), its events feed that device's state
			pad.port = it->second.port;
			pad.feedKb = settings.input.maple_devices[pad.port & 3] == MDT_Keyboard;
			pad.feedMouse = settings.input.maple_devices[pad.port & 3] == MDT_Mouse;
		}
		for (int t = 0; t < TGT_COUNT; t++)
			pad.map[t].kind = InMapping::NONE;
		if (mapped) {
			pad.port = it->second.port;
			memcpy(pad.map, it->second.map, sizeof(pad.map));
			for (int t = 0; t < TGT_COUNT; t++) {
				const InMapping& m = pad.map[t];
				if (m.kind == InMapping::ABS && m.code >= 0 && m.code < ABS_CODES) {
					struct input_absinfo ai;
					if (ioctl(fd, EVIOCGABS(m.code), &ai) == 0) {
						pad.absLo[m.code] = ai.minimum;
						pad.absHi[m.code] = ai.maximum;
						pad.absValid[m.code] = true;
					}
				}
				if (t == TGT_ANALOG_X || t == TGT_ANALOG_Y)
					pad.relDriven[t - TGT_ANALOG_X] = m.kind == InMapping::REL;
				// Select already drives something: no list hotkey on this pad
				if ((m.kind == InMapping::KEY && m.code == pad.selectCode) ||
					(m.kind == InMapping::KEYPAIR && (m.neg == pad.selectCode || m.pos == pad.selectCode)))
					pad.selectCode = -1;
			}
		}
		if (mapped)
			pad.hidFd = hid_shadow_open(path);
		// drain events queued before we started
		struct input_event drain[16];
		while (read(fd, drain, sizeof(drain)) > 0)
			;
		g_evdev_pads.push_back(pad);
		if (pad.feedKb || pad.feedMouse)
			printf("evdev: mapped %s [%s] -> port %c (DC %s)%s\n",
				   pad.path.c_str(), id.c_str(), 'A' + pad.port,
				   pad.feedKb ? "keyboard" : "mouse", hp);
		else if (mapped)
			printf("evdev: mapped %s [%s] -> port %c%s%s\n",
				   pad.path.c_str(), id.c_str(), 'A' + pad.port,
				   ffRumble ? ", rumble" : "", hp);
		else
			printf("evdev: monitoring %s [%s]%s\n", pad.path.c_str(), id.c_str(), hp);
	}
}

// Opens every /dev/input/event* node not already open. DeviceId #n numbering
// runs over ALL nodes in numeric path order (already-open ones included), the
// same rule as the launcher and the startup scan. On a rescan, an id that a
// live pad already holds (e.g. identical pads, the lower-numbered one
// replugged into a higher node) falls back to the lowest free #n so two
// devices never share one [inputN] section.
static void evdev_scan(bool hotplug) {
	if (hotplug) {
		// nodes removed or replaced since we opened them
		for (size_t i = 0; i < g_evdev_pads.size(); i++) {
			EvdevPad& p = g_evdev_pads[i];
			struct stat st;
			if (p.dead)
				continue;
			if (stat(p.path.c_str(), &st) != 0 || st.st_rdev != p.rdev || st.st_ino != p.ino)
				evdev_remove_pad(p, " (node gone)");
		}
		evdev_reap();
	}

	glob_t g;
	if (glob("/dev/input/event*", 0, NULL, &g) != 0) {
		if (!hotplug)
			printf("evdev: no /dev/input/event* devices\n");
		return;
	}
	std::vector<std::string> paths(g.gl_pathv, g.gl_pathv + g.gl_pathc);
	globfree(&g);
	std::sort(paths.begin(), paths.end(), evdev_path_less);

	std::map<std::string, int> seen;
	for (size_t i = 0; i < paths.size(); i++) {
		if (hotplug) {
			struct stat st;
			if (stat(paths[i].c_str(), &st) != 0)
				continue;
			const EvdevPad* have = nullptr;
			for (size_t j = 0; j < g_evdev_pads.size() && !have; j++) {
				const EvdevPad& p = g_evdev_pads[j];
				if (p.path == paths[i] && p.rdev == st.st_rdev && p.ino == st.st_ino)
					have = &p;
			}
			if (have) {   // already open: counts for the numbering only
				seen[have->baseId]++;
				continue;
			}
		}
		int fd = open(paths[i].c_str(), O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		std::string base = evdev_device_id(fd);
		if (base.empty()) {
			close(fd);
			continue;
		}
		// identical devices with no unique id get #2, #3, ...
		int n = ++seen[base];
		std::string id = evdev_numbered_id(base, n);
		if (hotplug && evdev_id_live(id)) {
			for (n = 1; evdev_id_live(evdev_numbered_id(base, n)); n++)
				;
			id = evdev_numbered_id(base, n);
		}
		evdev_add_pad(paths[i], fd, base, id, hotplug);
	}
}

static void evdev_init() {
	evdev_load_cfg();
	// mtime before the glob: a node added in between changes it again, so
	// the first hotplug poll catches it
	struct stat st;
	if (stat("/dev/input", &st) == 0)
		g_input_mtime_scanned = st.st_mtim;
	evdev_scan(false);
	g_hotplug_next_ns = mono_ns() + HOTPLUG_POLL_NS;
}

// Cheap hotplug check from the input poll path: at most one stat() per
// HOTPLUG_POLL_NS. A changed /dev/input mtime is only acted on once it has
// stayed unchanged for a whole poll period: the directory has settled (a
// DualSense adds several nodes in a row, the hidraw node the GET_REPORT
// shadow needs comes right after the event node) and, the timestamp being
// jiffy-coarse, no later change can share the mtime the scan is recorded
// under.
static bool timespec_eq(const struct timespec& a, const struct timespec& b) {
	return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

static void evdev_hotplug_poll() {
	evdev_reap();
	uint64_t now = mono_ns();
	if (now < g_hotplug_next_ns)
		return;
	g_hotplug_next_ns = now + HOTPLUG_POLL_NS;
	struct stat st;
	if (stat("/dev/input", &st) != 0)
		return;   // devtmpfs drops the dir with its last node; reads see ENODEV
	if (timespec_eq(st.st_mtim, g_input_mtime_scanned)) {
		g_input_change_pending = false;
		return;
	}
	if (!g_input_change_pending || !timespec_eq(st.st_mtim, g_input_mtime_seen)) {
		g_input_change_pending = true;
		g_input_mtime_seen = st.st_mtim;
		return;
	}
	g_input_change_pending = false;
	g_input_mtime_scanned = st.st_mtim;
	evdev_scan(true);
}

static void evdev_feed(EvdevPad& p, u16 type, u16 code, s32 value) {
	// maple keyboard/mouse ports consume the raw events; pad mappings are
	// still processed below (harmless: a kb/mouse port has no controller)
	if (p.feedKb && type == EV_KEY)
		dc_kbd_input(code, value != 0);
	if (p.feedMouse)
		dc_mouse_input(type, code, value);

	switch (type) {
	case EV_KEY:
		if ((int)code == p.selectCode)
			p.selHeld = value != 0;
		for (int t = 0; t < TGT_COUNT; t++) {
			const InMapping& m = p.map[t];
			if (m.kind == InMapping::KEY && m.code == (int)code) {
				if (t == TGT_TRIG_L || t == TGT_TRIG_R)
					p.trig[t - TGT_TRIG_L] = value ? 255 : 0;
				else if (TARGET_BTN_BIT[t] || t == TGT_BTN_EXIT
				                           || t == TGT_BTN_SCREENSHOT)
					p.held[t] = value != 0;
			} else if (m.kind == InMapping::KEYPAIR &&
					   (t == TGT_ANALOG_X || t == TGT_ANALOG_Y)) {
				int a = t - TGT_ANALOG_X;
				if ((int)code == m.neg)
					p.kpNeg[a] = value != 0;
				else if ((int)code == m.pos)
					p.kpPos[a] = value != 0;
				else
					continue;
				p.analog[a] = p.kpNeg[a] ? -128 : (p.kpPos[a] ? 127 : 0);
			}
		}
		break;

	case EV_ABS:
		for (int t = 0; t < TGT_COUNT; t++) {
			const InMapping& m = p.map[t];
			if (m.kind != InMapping::ABS || m.code != (int)code)
				continue;
			int lo = -32768, hi = 32767;
			if (code < ABS_CODES && p.absValid[code]) {
				lo = p.absLo[code];
				hi = p.absHi[code];
			}
			if (hi <= lo)
				continue;
			bool is_analog = t == TGT_ANALOG_X || t == TGT_ANALOG_Y;
			float norm = (value - lo) / (float)(hi - lo) * 2.0f - 1.0f;
			if (m.dir < 0)
				norm = -norm;
			if (is_analog) {
				int v = (int)lroundf(norm * 127);
				p.analog[t - TGT_ANALOG_X] = std::max(-127, std::min(127, v));
			} else if (t == TGT_TRIG_L || t == TGT_TRIG_R) {
				int v = (int)lroundf((norm + 1) / 2 * 255);
				p.trig[t - TGT_TRIG_L] = std::max(0, std::min(255, v));
			} else {
				p.held[t] = norm > 0.5f;
			}
		}
		break;

	case EV_REL:
		for (int t = 0; t < TGT_COUNT; t++) {
			const InMapping& m = p.map[t];
			if (m.kind == InMapping::REL && m.code == (int)code &&
					(t == TGT_ANALOG_X || t == TGT_ANALOG_Y))
				p.relAccum[t - TGT_ANALOG_X] += value * m.dir * EVDEV_REL_GAIN;
		}
		break;
	}
}

static void evdev_tick(EvdevPad& p) {
	// mouse deflection decays back to center when motion stops
	for (int a = 0; a < 2; a++) {
		if (!p.relDriven[a])
			continue;
		float acc = p.relAccum[a];
		p.analog[a] = std::max(-127, std::min(127, (int)acc));
		p.relAccum[a] = fabsf(acc) >= 1.0f ? acc * 0.8f : 0.0f;
	}
}

// ---- device-state shadow (lost USB reports) ------------------------------
// Full-speed sticks behind the DE10's high-speed hub use split transactions;
// under heavy DDR/CPU load the dwc2 host occasionally loses an interrupt IN
// report the hub already acknowledged. Sticks that only report on change
// (GP2040-CE) then leave a press or release missing until the next change:
// a menu keeps scrolling, a press is ignored. A helper thread re-reads the
// input report over the control pipe (HIDIOCGINPUT, retried by the hub) and
// evdev_update repairs mapped keys when the event stream has been quiet.
#include <linux/hidraw.h>
#include <atomic>
#include <dirent.h>
#ifndef HIDIOCGINPUT    /* Linux 5.11+, newer than the toolchain headers */
#define HIDIOCGINPUT(len) _IOC(_IOC_WRITE | _IOC_READ, 'H', 0x0A, len)
#endif

// Slot lifecycle (hotplug): FREE -> ACTIVE by the input thread (fd and the
// sample fields are written while FREE, then published by the state store);
// ACTIVE -> CLOSING by the input thread when the pad goes away; CLOSING ->
// FREE by the shadow thread, which closes the fd itself. While a slot is
// ACTIVE/CLOSING only the shadow thread uses its fd, so it is never closed
// under a running ioctl and the number can't be reused behind its back.
static const int HID_SHADOW_MAX = 8;
enum { HS_FREE = 0, HS_ACTIVE, HS_CLOSING };
struct HidShadow {
	int fd = -1;
	std::atomic<int> state{HS_FREE};
	std::atomic<uint32_t> buttons{0};    // bit n = HID button n+1
	std::atomic<uint64_t> sampleNs{0};   // when buttons was read
	std::atomic<uint64_t> seq{0};
};
static HidShadow g_hid_shadow[HID_SHADOW_MAX];
static std::atomic<bool> g_hid_thread_started{false};
static bool g_hid_thread_running = false;   // input thread only

static void* hid_shadow_thread(void*);

static void hid_shadow_start_thread() {
	if (g_hid_thread_started.exchange(true))
		return;
	pthread_t th;   // plain pthreads like the rest of minicast
	if (pthread_create(&th, nullptr, hid_shadow_thread, nullptr) == 0) {
		pthread_detach(th);
		g_hid_thread_running = true;
	} else
		printf("evdev: GET_REPORT resync thread failed""\n");
}

// pad removed: hand the slot back (the shadow thread closes the fd)
static void hid_shadow_close(int slot) {
	if (slot < 0 || slot >= HID_SHADOW_MAX)
		return;
	HidShadow& h = g_hid_shadow[slot];
	if (h.state.load() != HS_ACTIVE)
		return;
	if (g_hid_thread_running) {
		h.state.store(HS_CLOSING);
	} else {   // no thread ever touched it
		close(h.fd);
		h.fd = -1;
		h.state.store(HS_FREE);
	}
}

static uint64_t mono_ns() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// GP2040-CE "Generic" report: 32 one-bit buttons first, no report id
static const unsigned char GP2040_DESC_PREFIX[] = {
	0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x20,
	0x15, 0x00, 0x25, 0x01, 0x95, 0x20, 0x75, 0x01, 0x81, 0x02 };

// hidraw node of the HID device behind /dev/input/eventN, if its report
// layout is the known 32-button one; returns a shadow slot or -1
static int hid_shadow_open(const std::string& evpath) {
	int slot = -1;
	for (int i = 0; i < HID_SHADOW_MAX && slot < 0; i++)
		if (g_hid_shadow[i].state.load() == HS_FREE)
			slot = i;
	if (slot < 0)
		return -1;
	std::string ev = evpath.substr(evpath.rfind('/') + 1);
	std::string dir = "/sys/class/input/" + ev + "/device/../../hidraw";
	DIR* d = opendir(dir.c_str());
	if (!d)
		return -1;
	std::string node;
	while (struct dirent* e = readdir(d))
		if (!strncmp(e->d_name, "hidraw", 6))
			node = std::string("/dev/") + e->d_name;
	closedir(d);
	if (node.empty())
		return -1;
	int fd = open(node.c_str(), O_RDWR);
	if (fd < 0)
		return -1;
	int dsz = 0;
	struct hidraw_report_descriptor rd;
	if (ioctl(fd, HIDIOCGRDESCSIZE, &dsz) < 0 || dsz < (int)sizeof(GP2040_DESC_PREFIX)) {
		close(fd);
		return -1;
	}
	rd.size = dsz;
	if (ioctl(fd, HIDIOCGRDESC, &rd) < 0 ||
		memcmp(rd.value, GP2040_DESC_PREFIX, sizeof(GP2040_DESC_PREFIX))) {
		close(fd);
		return -1;
	}
	HidShadow& h = g_hid_shadow[slot];
	h.fd = fd;
	h.buttons.store(0);     // a reused slot must not carry the old device's
	h.sampleNs.store(0);    // sample (the new pad starts at lastEventNs 0)
	h.seq.store(0);
	h.state.store(HS_ACTIVE);
	printf("evdev: %s shadowed by %s (GET_REPORT resync)""\n", evpath.c_str(), node.c_str());
	hid_shadow_start_thread();
	return slot;
}

static void* hid_shadow_thread(void*) {
	for (;;) {
		for (int i = 0; i < HID_SHADOW_MAX; i++) {
			HidShadow& h = g_hid_shadow[i];
			int st = h.state.load();
			if (st == HS_CLOSING) {
				close(h.fd);
				h.fd = -1;
				h.state.store(HS_FREE);
				continue;
			}
			if (st != HS_ACTIVE)
				continue;
			unsigned char buf[64];
			memset(buf, 0, sizeof(buf));
			uint64_t t = mono_ns();
			int n = ioctl(h.fd, HIDIOCGINPUT(sizeof(buf)), buf);
			if (n >= 5) {       // [0] report id (0), [1..4] buttons
				uint32_t b = buf[1] | (buf[2] << 8) | (buf[3] << 16) | ((uint32_t)buf[4] << 24);
				h.buttons.store(b);
				h.sampleNs.store(t);
				h.seq.fetch_add(1);
			}
		}
		usleep(50000);
	}
}

// evdev code the kernel (hid-input, gamepad usage) gives HID button n (1-based)
static int hid_button_code(int n) {
	return n <= 16 ? BTN_GAMEPAD + (n - 1) : BTN_TRIGGER_HAPPY + (n - 17);
}

// After SYN_DROPPED the kernel has thrown events away (the client buffer
// overflowed while the game wasn't polling, e.g. during menu loads), so the
// state built from the event stream can hold a press whose release was lost:
// a "latched" direction that keeps a menu scrolling by itself. Re-read the
// real state of every mapped key and axis.
static void evdev_resync(EvdevPad& p) {
	unsigned char keys[KEY_MAX / 8 + 1];
	memset(keys, 0, sizeof(keys));
	if (ioctl(p.fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
		for (int t = 0; t < TGT_COUNT; t++) {
			const InMapping& m = p.map[t];
			int codes[2] = { -1, -1 };
			if (m.kind == InMapping::KEY)
				codes[0] = m.code;
			else if (m.kind == InMapping::KEYPAIR) {
				codes[0] = m.neg;
				codes[1] = m.pos;
			}
			for (int c : codes)
				if (c >= 0 && c <= KEY_MAX)
					evdev_feed(p, EV_KEY, c, (keys[c / 8] >> (c % 8)) & 1);
		}
		if (p.selectCode >= 0 && p.selectCode <= KEY_MAX)
			p.selHeld = (keys[p.selectCode / 8] >> (p.selectCode % 8)) & 1;
	}
	for (int t = 0; t < TGT_COUNT; t++) {
		const InMapping& m = p.map[t];
		if (m.kind == InMapping::ABS && m.code >= 0 && m.code < ABS_CODES) {
			struct input_absinfo ai;
			if (ioctl(p.fd, EVIOCGABS(m.code), &ai) == 0)
				evdev_feed(p, EV_ABS, m.code, ai.value);
		}
	}
}

// keyboard keys that drive the RA achievement list while it is open
// (RA_MENU_* bits held), from every evdev device
static uint32_t g_kbd_menu = 0;

static void kbd_menu_key(u16 code, s32 value) {
	uint32_t bit = 0;
	switch (code) {
	case KEY_UP:        bit = RA_MENU_UP; break;
	case KEY_DOWN:      bit = RA_MENU_DOWN; break;
	case KEY_LEFT:      bit = RA_MENU_LEFT; break;
	case KEY_RIGHT:     bit = RA_MENU_RIGHT; break;
	case KEY_ENTER:
	case KEY_KPENTER:   bit = RA_MENU_A; break;
	case KEY_BACKSPACE: bit = RA_MENU_B; break;
	case KEY_PAGEUP:    bit = RA_MENU_L; break;
	case KEY_PAGEDOWN:  bit = RA_MENU_R; break;
	default: return;
	}
	if (value == 1)
		g_kbd_menu |= bit;
	else if (value == 0)
		g_kbd_menu &= ~bit;
}

static void evdev_read_pad(EvdevPad& p) {
	if (p.fd < 0)
		return;
	struct input_event ev[64];
	for (;;) {
		ssize_t n = read(p.fd, ev, sizeof(ev));
		if (n < (ssize_t)sizeof(ev[0])) {
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				break;
			// ENODEV (unplugged) or worse: closed and dropped at the next
			// evdev_reap; a replug is picked up by the hotplug rescan
			evdev_remove_pad(p, n < 0 && errno == ENODEV ? " (removed)" : "");
			break;
		}
		for (ssize_t e = 0; e < n / (ssize_t)sizeof(ev[0]); e++) {
			if (ev[e].type == EV_SYN && ev[e].code == SYN_DROPPED) {
				if (!p.synDropped)
					printf("evdev: events dropped on %s, resyncing\n", p.path.c_str());
				p.synDropped = true;
				continue;
			}
			if (p.synDropped) {
				if (ev[e].type == EV_SYN && ev[e].code == SYN_REPORT) {
					p.synDropped = false;
					evdev_resync(p);
				}
				continue;
			}
			if (ev[e].type == EV_KEY && ev[e].code == KEY_ESC && ev[e].value == 1) {
				printf("evdev: ESC pressed on %s, exiting\n", p.path.c_str());
				fflush(stdout);
				save_before_exit();
				_exit(0); // Ungraceful termination
			}
			if (ev[e].type == EV_KEY && ev[e].code == KEY_0 && ev[e].value == 1) {
				vram_dump_pending = true;
			}
			if (ev[e].type == EV_KEY && ev[e].code == KEY_SYSRQ && ev[e].value == 1) {
				// Print Screen: queue a screenshot; taken at the next SPG
				// vblank (rend_vblank -> ScreenshotVBlank -> spg_screenshot)
				QueueScreenshot();
			}
			if (ev[e].type == EV_KEY) {
				// F6: in-game menu / achievement list (open / close)
				if (ev[e].code == KEY_F6 && ev[e].value == 1 && ra_overlay_hotkey_enabled()) {
					printf("evdev: F6 on %s (%s)\n", p.path.c_str(),
					       ra_overlay_menu_enabled() ? "in-game menu" : "achievement list");
					ra_overlay_hotkey();
				}
				kbd_menu_key(ev[e].code, ev[e].value);
			}
			if (ev[e].type == EV_KEY || ev[e].type == EV_ABS)
				p.lastEventNs = mono_ns();
			evdev_feed(p, ev[e].type, ev[e].code, ev[e].value);
		}
	}
}

// Compare mapped keys with the device's own report; only when the report was
// sampled after the last evdev event (so it is the newer truth) and the
// stream has been quiet a moment (no report still in flight).
static void evdev_hid_repair(EvdevPad& p) {
	if (p.hidFd < 0)
		return;
	HidShadow& h = g_hid_shadow[p.hidFd];
	uint64_t seq = h.seq.load();
	if (seq == p.lastCheckedSample)
		return;
	uint64_t sample = h.sampleNs.load();
	uint32_t b = h.buttons.load();
	if (sample < p.lastEventNs + 30000000ULL || mono_ns() - p.lastEventNs < 60000000ULL)
		return;                                   // stream newer / still active
	p.lastCheckedSample = seq;
	for (int t = 0; t < TGT_COUNT; t++) {
		const InMapping& m = p.map[t];
		int codes[2] = { -1, -1 };
		if (m.kind == InMapping::KEY)
			codes[0] = m.code;
		else if (m.kind == InMapping::KEYPAIR) {
			codes[0] = m.neg;
			codes[1] = m.pos;
		}
		for (int c : codes) {
			if (c < 0)
				continue;
			int n = c >= BTN_TRIGGER_HAPPY ? c - BTN_TRIGGER_HAPPY + 17 : c - BTN_GAMEPAD + 1;
			if (n < 1 || n > 32 || hid_button_code(n) != c)
				continue;
			bool dev = (b >> (n - 1)) & 1;
			bool cur;
			if (m.kind == InMapping::KEYPAIR)
				cur = (c == m.neg) ? p.kpNeg[t - TGT_ANALOG_X] : p.kpPos[t - TGT_ANALOG_X];
			else if (t == TGT_TRIG_L || t == TGT_TRIG_R)
				cur = p.trig[t - TGT_TRIG_L] != 0;
			else
				cur = p.held[t];
			if (dev != cur) {
				printf("evdev: %s code %d %s, resync from device""\n", p.path.c_str(), c,
				       dev ? "pressed" : "released");
				evdev_feed(p, EV_KEY, c, dev);
			}
		}
	}
}

static bool g_input_suppress = false;   // see the end of evdev_update

// Reads every pad once and refreshes ALL FOUR maple ports (each device
// lands on its configured port; devices sharing one merge). Called from
// UpdateInputState(0) only - maple polls ports in bus order, so ports 1..3
// consume the state this pass just wrote.
static void evdev_update() {
	u16 buttons[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
	int ax[4] = {0}, ay[4] = {0}, l[4] = {0}, r[4] = {0};

	for (size_t i = 0; i < g_evdev_pads.size(); i++) {
		EvdevPad& p = g_evdev_pads[i];
		evdev_read_pad(p);
		if (p.fd < 0)
			continue;
		evdev_hid_repair(p);
		evdev_tick(p);

		// mapped emulator actions, on the PRESS edge (in addition to the
		// hardcoded ESC exit / SYSRQ screenshot in evdev_read_pad)
		if (p.held[TGT_BTN_EXIT] && !p.actPrev[0]) {
			printf("evdev: exit button pressed on %s, exiting\n", p.path.c_str());
			fflush(stdout);
			save_before_exit();
			_exit(0); // Ungraceful termination, as the hardcoded ESC
		}
		if (p.held[TGT_BTN_SCREENSHOT] && !p.actPrev[1])
			QueueScreenshot();
		p.actPrev[0] = p.held[TGT_BTN_EXIT];
		p.actPrev[1] = p.held[TGT_BTN_SCREENSHOT];

		// in-game menu / achievement list hotkey: Start + Select or Start + R
		// held 0.5 s (once per hold); Select keeps Start from the game while
		// the hotkey is usable, R does not (see the header comment)
		bool ra_on = ra_overlay_hotkey_enabled();
		bool ra_sel = p.selHeld && ra_on;
		bool ra_r = ra_on && p.trig[1] >= 204;
		if ((ra_sel || ra_r) && p.held[TGT_BTN_START]) {
			uint64_t now = mono_ns();
			if (!p.comboSince)
				p.comboSince = now;
			else if (!p.comboFired && now - p.comboSince >= 500000000ULL) {
				p.comboFired = true;
				printf("evdev: Start+%s on %s (%s)\n",
				       ra_sel ? "Select" : "R", p.path.c_str(),
				       ra_overlay_menu_enabled() ? "in-game menu" : "achievement list");
				ra_overlay_hotkey();
			}
		} else {
			p.comboSince = 0;
			p.comboFired = false;
		}

		// buttons OR across same-port devices; biggest absolute axis wins
		int q = p.port & 3;
		for (int t = 0; t < TGT_COUNT; t++)
			if (TARGET_BTN_BIT[t] && p.held[t] && !(t == TGT_BTN_START && ra_sel))
				buttons[q] &= ~TARGET_BTN_BIT[t];
		if (abs(p.analog[0]) > abs(ax[q])) ax[q] = p.analog[0];
		if (abs(p.analog[1]) > abs(ay[q])) ay[q] = p.analog[1];
		if (p.trig[0] > l[q]) l[q] = p.trig[0];
		if (p.trig[1] > r[q]) r[q] = p.trig[1];
	}

	// after the menu / achievement list closed: the button that closed it (and
	// any other held one) must not reach the game - nothing until all released
	if (g_input_suppress) {
		bool any = false;
		for (int q = 0; q < 4; q++)
			any |= buttons[q] != 0xFFFF || l[q] > 64 || r[q] > 64;
		if (any) {
			for (int q = 0; q < 4; q++) {
				buttons[q] = 0xFFFF;
				l[q] = r[q] = 0;
			}
		} else {
			g_input_suppress = false;
		}
	}

	for (int q = 0; q < 4; q++) {
		kcode[q] = buttons[q];
		joyx[q] = (s8)std::max(-128, std::min(127, ax[q]));
		joyy[q] = (s8)std::max(-128, std::min(127, ay[q]));
		lt[q] = (u8)l[q];
		rt[q] = (u8)r[q];
	}
}

void UpdateInputState(u32 port) {

	//kcode[port] = 0xFFFF;
	//rt[port] = 0;
	//lt[port] = 0;
	//joyx[port] = 0;
	//joyy[port] = 0;

	//Scan Keybord
/*	while(kbhit()) {
		int ch = getchar();
		switch(ch) {
			case 'e': lt[port] = 255; break;
			case 'r': rt[port] = 255; break;

			case 'v': kcode[port] &= ~DC_BTN_Y; break;
			case 'c': kcode[port] &= ~DC_BTN_X; break;
			case 'x': kcode[port] &= ~DC_BTN_B; break;
			case 'a': kcode[port] &= ~DC_BTN_A; break;
			
			case 's': kcode[port] &= ~DC_BTN_START; break;

			case 'i': kcode[port] &= ~DC_DPAD_UP; break;
			case 'k': kcode[port] &= ~DC_DPAD_DOWN; break;
			case 'j': kcode[port] &= ~DC_DPAD_LEFT; break;
			case 'l': kcode[port] &= ~DC_DPAD_RIGHT; break;

			case 't': joyy[port] = -128; break;
			case 'g': joyy[port] = 127; break;
			case 'f': joyx[port] = -128; break;
			case 'h': joyx[port] = 127; break;
		}
	}*/

	if (port > 0) return;

	static bool evdev_inited = false;
	if (!evdev_inited) {
		evdev_inited = true;
		evdev_init();   // starts the GET_REPORT shadow thread if needed
	}
	evdev_hotplug_poll();   // reaps removed pads, opens new ones
	if (g_evdev_mode) {
		evdev_update();   // refreshes all four ports
		return;
	}

	// legacy js0 mode: evdev devices are still monitored for the ESC exit
	for (size_t i = 0; i < g_evdev_pads.size(); i++)
		evdev_read_pad(g_evdev_pads[i]);
}

// In-game menu / achievement list (emulation paused, SH4 thread parked in the
// vblank hook): the same device polling as the game's, the buttons go to it.
uint32_t ra_frontend_menu_input(void) {
	evdev_hotplug_poll();
	if (g_evdev_mode)
		evdev_update();
	else
		for (size_t i = 0; i < g_evdev_pads.size(); i++)
			evdev_read_pad(g_evdev_pads[i]);
	uint32_t m = g_kbd_menu;
	if (!g_evdev_mode)
		return m;
	for (int q = 0; q < 4; q++) {
		u16 k = (u16)~kcode[q];   // active-low
		if (k & DC_DPAD_UP) m |= RA_MENU_UP;
		if (k & DC_DPAD_DOWN) m |= RA_MENU_DOWN;
		if (k & DC_DPAD_LEFT) m |= RA_MENU_LEFT;
		if (k & DC_DPAD_RIGHT) m |= RA_MENU_RIGHT;
		if (k & DC_BTN_A) m |= RA_MENU_A;
		if (k & DC_BTN_B) m |= RA_MENU_B;
		if (k & DC_BTN_X) m |= RA_MENU_X;
		if (k & DC_BTN_Y) m |= RA_MENU_Y;
		if (k & DC_BTN_START) m |= RA_MENU_START;
		if (lt[q] > 128) m |= RA_MENU_L;
		if (rt[q] > 128) m |= RA_MENU_R;
		if (joyy[q] < -90) m |= RA_MENU_UP;
		if (joyy[q] > 90) m |= RA_MENU_DOWN;
		if (joyx[q] < -90) m |= RA_MENU_LEFT;
		if (joyx[q] > 90) m |= RA_MENU_RIGHT;
	}
	return m;
}

void ra_frontend_input_suppress(void) {
	g_input_suppress = true;
}

// purupuru pack rumble -> evdev force feedback, for every FF_RUMBLE-capable
// device mapped to the port. inclination (fade slope) has no evdev
// equivalent; the peak power is played for the whole duration.
void UpdateVibration(u32 port, float power, float inclination, u32 duration_ms) {
	for (size_t i = 0; i < g_evdev_pads.size(); i++) {
		EvdevPad& p = g_evdev_pads[i];
		if (!p.ffRumble || (p.port & 3) != (int)port)
			continue;

		struct input_event ie;
		memset(&ie, 0, sizeof(ie));
		ie.type = EV_FF;

		if (power <= 0.0f) {
			if (p.ffEffect >= 0) {   // stop
				ie.code = p.ffEffect;
				ie.value = 0;
				if (write(p.fd, &ie, sizeof(ie)) != sizeof(ie))
					;   // device gone; next evdev read notices
			}
			continue;
		}

		u16 mag = (u16)(std::min(power, 1.0f) * 0xFFFF);
		struct ff_effect ef;
		memset(&ef, 0, sizeof(ef));
		ef.type = FF_RUMBLE;
		ef.id = p.ffEffect;   // -1 allocates, else updates in place
		ef.u.rumble.strong_magnitude = mag;
		ef.u.rumble.weak_magnitude = mag;
		ef.replay.length = duration_ms > 0xFFFF ? 0xFFFF : (u16)duration_ms;
		if (ioctl(p.fd, EVIOCSFF, &ef) < 0)
			continue;
		p.ffEffect = ef.id;

		ie.code = p.ffEffect;
		ie.value = 1;
		if (write(p.fd, &ie, sizeof(ie)) != sizeof(ie))
			;
	}
}

#if HOST_OS == OS_XIL_BARE
void bm_vmem_pagefill(void**, unsigned int) {

}


void vmem_platform_ondemand_page(void*, unsigned int) {

}

void vmem_platform_destroy() {

}

void vmem_platform_reset_mem(void*, unsigned int) {

}

struct vmem_mapping;
void vmem_platform_create_mappings(vmem_mapping const*, unsigned int) {

}

#endif

extern "C" void _gettimeofday() {
    die("gettimeofday()");
}
