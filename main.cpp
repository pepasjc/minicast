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

int main(int argc, char* argv[])
{
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

	libswirl_init();
	libswirl_loop(argc == 1 ? "": argv[1]);

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

#include <linux/input.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <glob.h>
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

struct EvdevPad {
	int fd;
	std::string path, id;
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

static void evdev_init() {
	// device id -> mappings, from [input0]..[inputN]
	struct CfgPad { InMapping map[TGT_COUNT]; bool any; int port; };
	std::map<std::string, CfgPad> cfgpads;
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
		cfgpads[devid] = pad;
	}

	glob_t g;
	if (glob("/dev/input/event*", 0, NULL, &g) != 0) {
		printf("evdev: no /dev/input/event* devices\n");
		return;
	}
	std::vector<std::string> paths(g.gl_pathv, g.gl_pathv + g.gl_pathc);
	globfree(&g);
	std::sort(paths.begin(), paths.end(), evdev_path_less);

	std::map<std::string, int> seen;
	for (size_t i = 0; i < paths.size(); i++) {
		int fd = open(paths[i].c_str(), O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		std::string id = evdev_device_id(fd);
		if (id.empty()) {
			close(fd);
			continue;
		}
		int dup = seen[id]++;
		if (dup) {  // identical devices with no unique id
			char suffix[16];
			snprintf(suffix, sizeof(suffix), "#%d", dup + 1);
			id += suffix;
		}
		// unmatched devices stay open in monitor-only mode: no mappings,
		// but the hardcoded ESC exit still watches them
		std::map<std::string, CfgPad>::iterator it = cfgpads.find(id);
		bool mapped = it != cfgpads.end() && it->second.any;

		// rumble (purupuru pack): mapped controllers advertising FF_RUMBLE are
		// reopened read-write so effects can be uploaded and played
		bool ffRumble = false;
		if (mapped) {
			unsigned long ffbits[(FF_MAX + 8 * sizeof(unsigned long)) /
			                     (8 * sizeof(unsigned long))] = {0};
			if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffbits)), ffbits) >= 0 &&
				(ffbits[FF_RUMBLE / (8 * sizeof(unsigned long))] >>
				 (FF_RUMBLE % (8 * sizeof(unsigned long)))) & 1) {
				int rw = open(paths[i].c_str(), O_RDWR | O_NONBLOCK);
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
		pad.path = paths[i];
		pad.id = id;
		pad.port = 0;
		pad.ffRumble = ffRumble;
		pad.ffEffect = -1;
		pad.feedKb = false;
		pad.feedMouse = false;
		if (it != cfgpads.end()) {
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
			}
		}
		// drain events queued before we started
		struct input_event drain[16];
		while (read(fd, drain, sizeof(drain)) > 0)
			;
		g_evdev_pads.push_back(pad);
		if (pad.feedKb || pad.feedMouse)
			printf("evdev: mapped %s [%s] -> port %c (DC %s)\n",
				   pad.path.c_str(), id.c_str(), 'A' + pad.port,
				   pad.feedKb ? "keyboard" : "mouse");
		else if (mapped)
			printf("evdev: mapped %s [%s] -> port %c%s\n",
				   pad.path.c_str(), id.c_str(), 'A' + pad.port,
				   ffRumble ? ", rumble" : "");
		else
			printf("evdev: monitoring %s [%s]\n", pad.path.c_str(), id.c_str());
	}
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

static void evdev_read_pad(EvdevPad& p) {
	if (p.fd < 0)
		return;
	struct input_event ev[64];
	for (;;) {
		ssize_t n = read(p.fd, ev, sizeof(ev));
		if (n < (ssize_t)sizeof(ev[0])) {
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				break;
			printf("evdev: lost %s [%s]\n", p.path.c_str(), p.id.c_str());
			close(p.fd);
			p.fd = -1;
			break;
		}
		for (ssize_t e = 0; e < n / (ssize_t)sizeof(ev[0]); e++) {
			if (ev[e].type == EV_KEY && ev[e].code == KEY_ESC && ev[e].value == 1) {
				printf("evdev: ESC pressed on %s, exiting\n", p.path.c_str());
				fflush(stdout);
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
			evdev_feed(p, ev[e].type, ev[e].code, ev[e].value);
		}
	}
}

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
		evdev_tick(p);

		// mapped emulator actions, on the PRESS edge (in addition to the
		// hardcoded ESC exit / SYSRQ screenshot in evdev_read_pad)
		if (p.held[TGT_BTN_EXIT] && !p.actPrev[0]) {
			printf("evdev: exit button pressed on %s, exiting\n", p.path.c_str());
			fflush(stdout);
			_exit(0); // Ungraceful termination, as the hardcoded ESC
		}
		if (p.held[TGT_BTN_SCREENSHOT] && !p.actPrev[1])
			QueueScreenshot();
		p.actPrev[0] = p.held[TGT_BTN_EXIT];
		p.actPrev[1] = p.held[TGT_BTN_SCREENSHOT];

		// buttons OR across same-port devices; biggest absolute axis wins
		int q = p.port & 3;
		for (int t = 0; t < TGT_COUNT; t++)
			if (TARGET_BTN_BIT[t] && p.held[t])
				buttons[q] &= ~TARGET_BTN_BIT[t];
		if (abs(p.analog[0]) > abs(ax[q])) ax[q] = p.analog[0];
		if (abs(p.analog[1]) > abs(ay[q])) ay[q] = p.analog[1];
		if (p.trig[0] > l[q]) l[q] = p.trig[0];
		if (p.trig[1] > r[q]) r[q] = p.trig[1];
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
		evdev_init();
	}
	if (g_evdev_mode) {
		evdev_update();   // refreshes all four ports
		return;
	}

	// legacy js0 mode: evdev devices are still monitored for the ESC exit
	for (size_t i = 0; i < g_evdev_pads.size(); i++)
		evdev_read_pad(g_evdev_pads[i]);
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
