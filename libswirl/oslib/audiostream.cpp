/*
	This file is part of libswirl
*/
#include "license/bsd"


#include <limits.h>
#include "hw/holly/sb.h" // for STATIC_FORWARD
#include "cfg/cfg.h"
#include "oslib/threading.h"
#include "oslib/oslib.h"
#include "audiostream.h"

struct SoundFrame { s16 l; s16 r; };

#ifdef LOG_SOUND
// TODO Only works on Windows!
WaveWriter rawout("d:\\aica_out.wav");
#endif

static unsigned int audiobackends_num_max = 1;
static unsigned int audiobackends_num_registered = 0;
static audiobackend_t **audiobackends = NULL;

static float InterpolateCatmull4pt3oX(float x0, float x1, float x2, float x3, float t) {
	return 0.45f* ((2 * x1) + t * ((-x0 + x2) + t * ((2 * x0 - 5 * x1 + 4 * x2 - x3) + t * (-x0 + 3 * x1 - 3 * x2 + x3))));
}

static s16 SaturateToS16(float v)
{
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (s16)v;
}

class PullBuffer_t {
private:
	static const u32 SAMPLE_COUNT = 4096;
	static const u32 OVERRUN_SKIP = 128;

	SoundFrame RingBuffer[SAMPLE_COUNT];
	const u32 RingBufferByteSize = sizeof(RingBuffer);
	const u32 RingBufferSampleCount = SAMPLE_COUNT;

	volatile u32 WritePtr;  //last WRITEN sample
	volatile u32 ReadPtr;   //next sample to read

	u32 asRingUsedCount()
	{
		if (WritePtr > ReadPtr)
			return WritePtr - ReadPtr;
		else
			return RingBufferSampleCount - (ReadPtr - WritePtr);
		//s32 sz=(WritePtr+1)%RingBufferSampleCount-ReadPtr;
		//return sz<0?sz+RingBufferSampleCount:sz;
	}

	u32 asRingFreeCount()
	{
		return RingBufferSampleCount - asRingUsedCount();
	}

	SoundFrame Status[4] = { { 0 } };
	float current_partial_pos = 0;
public:
	u32 ReadAudioResampling(void* buffer, u32 buffer_size, u32 amt, u32 target_rate)
	{
		if (buffer == nullptr)
			return asRingUsedCount() * target_rate / 44100;

		SoundFrame* outbuf = (SoundFrame*)buffer;

		u32 read = min(min(amt, buffer_size), asRingUsedCount());

		float inc = 44100.0f / target_rate;

		for (int i = 0; i < read; i++)
		{
			SoundFrame res = {
				SaturateToS16(InterpolateCatmull4pt3oX(Status[0].l,Status[1].l,Status[2].l,Status[3].l,current_partial_pos)),
				SaturateToS16(InterpolateCatmull4pt3oX(Status[0].r,Status[1].r,Status[2].r,Status[3].r,current_partial_pos))
			};
			outbuf[i] = res;
			current_partial_pos += inc;
			while (current_partial_pos >= 1)
			{
				Status[0] = Status[1];
				Status[1] = Status[2];
				Status[2] = Status[3];
				Status[3] = RingBuffer[ReadPtr];
				ReadPtr = (ReadPtr + 1) % RingBufferSampleCount;
				current_partial_pos -= 1;
			}
		}

		return read;
	}

	u32 ReadAudio(void* buffer, u32 buffer_size, u32 amt)
	{
		if (buffer == nullptr)
			return asRingUsedCount();

		u32 read = min(min(amt, buffer_size), asRingUsedCount());
		u32 first = min(RingBufferSampleCount - ReadPtr, read);
		memcpy(buffer, RingBuffer + ReadPtr, first * sizeof(SoundFrame));
		if (read > first)
		{
			memcpy(((SoundFrame*)buffer) + first, RingBuffer,(read - first) * sizeof(SoundFrame));
		}
		ReadPtr = (ReadPtr + read) % RingBufferSampleCount;
		return read;
	}

	void WriteSample(s16 r, s16 l, bool wait)
	{
		const u32 ptr = (WritePtr + 1) % RingBufferSampleCount;
		RingBuffer[ptr].r = r;
		RingBuffer[ptr].l = l;
		WritePtr = ptr;

		if (asRingFreeCount() < 16)
		{
			if (wait)
			{
				// infinite?
				while (asRingFreeCount() < 512)
					SleepMs(1);
			}
			else
			{
				ReadPtr = (ReadPtr + OVERRUN_SKIP) % RingBufferSampleCount;
			}
		}
	}
};

class PushBuffer_t {
private:
	static const u32 SAMPLE_COUNT = 512;

	SoundFrame RingBuffer[SAMPLE_COUNT];
	const u32 RingBufferByteSize = sizeof(RingBuffer);
	const u32 RingBufferSampleCount = SAMPLE_COUNT;

	volatile u32 WritePtr;  //last WRITEN sample

public:
	// set by creator
	audiobackend_t* audiobackend_current;

	u32 PushAudio(u32 amt, bool wait)
	{
		if (audiobackend_current != NULL) {
			return audiobackend_current->push(RingBuffer, amt, wait);
		}
		return 0;
	}

	void WriteSample(s16 r, s16 l, bool wait)
	{
		const u32 ptr = (WritePtr + 1) % RingBufferSampleCount;
		RingBuffer[ptr].r = r;
		RingBuffer[ptr].l = l;
		WritePtr = ptr;

		if (WritePtr == (SAMPLE_COUNT - 1))
		{
			PushAudio(SAMPLE_COUNT, wait);
		}
	}
};

u32 GetAudioBackendCount()
{
	return audiobackends_num_registered;
}

audiobackend_t* GetAudioBackend(int num)
{
	return audiobackends[num];
}

bool RegisterAudioBackend(audiobackend_t *backend)
{
	/* This function announces the availability of an audio backend to reicast. */
	// Check if backend is valid
	if (backend == NULL)
	{
		printf("ERROR: Tried to register invalid audio backend (NULL pointer).\n");
		return false;
	}

	if (backend->slug == "auto" || backend->slug == "none") {
		printf("ERROR: Tried to register invalid audio backend (slug \"%s\" is a reserved keyword).\n", backend->slug.c_str());
		return false;
	}

	// First call to RegisterAudioBackend(), create the backend structure;
	if (audiobackends == NULL)
		audiobackends = static_cast<audiobackend_t**>(calloc(audiobackends_num_max, sizeof(audiobackend_t*)));

	// Check if we need to allocate addition memory for storing the pointers and allocate if neccessary
	if (audiobackends_num_registered == audiobackends_num_max)
	{
		// Check for integer overflows
		if (audiobackends_num_max == UINT_MAX)
		{
			printf("ERROR: Registering audio backend \"%s\" (%s) failed. Cannot register more than %u backends\n", backend->slug.c_str(), backend->name.c_str(), audiobackends_num_max);
			return false;
		}
		audiobackends_num_max++;
		audiobackend_t **new_ptr = static_cast<audiobackend_t**>(realloc(audiobackends, audiobackends_num_max * sizeof(audiobackend_t*)));
		// Make sure that allocation worked
		if (new_ptr == NULL)
		{
			printf("ERROR: Registering audio backend \"%s\" (%s) failed. Cannot allocate additional memory.\n", backend->slug.c_str(), backend->name.c_str());
			return false;
		}
		audiobackends = new_ptr;
	}

	audiobackends[audiobackends_num_registered] = backend;
	audiobackends_num_registered++;
	return true;
}

static audiobackend_t* SearchAudioBackend(const std::string& s) {
	for (unsigned int i = 0; i < audiobackends_num_registered;++i) {
		if (audiobackends[i]->slug == s)
			return audiobackends[i];
	}
	
	return nullptr;
}

audiobackend_t* GetAudioBackend(std::string slug)
{
	if (slug == "none")
	{
		printf("WARNING: Audio backend set to \"none\"!\n");
	}
	else if (audiobackends_num_registered > 0)
	{
		#if BUILD_RETROARCH_CORE==1
			audiobackend_t* rv = SearchAudioBackend("libretro");
			if (rv != nullptr) {
				printf("Auto-selected audio backend \"%s\" (%s).\n", rv->slug.c_str(), rv->name.c_str());
				return rv;
			}
		#endif
		if (slug == "auto")
		{
			/* FIXME: At some point, one might want to insert some intelligent
				 algorithm for autoselecting the approriate audio backend here.
				 I'm too lazy right now. */
			printf("Auto-selected audio backend \"%s\" (%s).\n", audiobackends[0]->slug.c_str(), audiobackends[0]->name.c_str());
			return audiobackends[0];
		}
		else
		{
			audiobackend_t* rv = SearchAudioBackend(slug);
			if (rv != nullptr)
				return rv;

			printf("WARNING: Audio backend \"%s\" not found!\n", slug.c_str());
		}
	}
	else
	{
		printf("WARNING: No audio backends available!\n");
	}
	return NULL;
}

static bool backends_sorted = false;
void SortAudioBackends()
{
	if (backends_sorted)
		return;

	// Sort backends by slug
	for (int n = audiobackends_num_registered; n > 0; n--)
	{
		for (int i = 0; i < n - 1; i++)
		{
			if (audiobackends[i]->slug > audiobackends[i + 1]->slug)
			{
				audiobackend_t* swap = audiobackends[i];
				audiobackends[i] = audiobackends[i + 1];
				audiobackends[i + 1] = swap;
			}
		}
	}
}


#include "../../../polly2-rtl/driver/polly2_mmio.h"
#include <atomic>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>

/*
	Threaded FIFO feeder ("audio", "threaded" = 1, the default)

	Every AUDIO_DATA store goes uncached over the lightweight HPS-FPGA
	bridge, and the SH4 thread used to eat that write latency ~48000
	times a second inside AICA_Sample32. Now the SH4 thread only resamples
	and drops the packed output words into this single-producer /
	single-consumer ring; a helper thread on cpu0 moves them to the FIFO.

	Producer (SH4 thread, cpu1): plain stores into data[] and a private
	cursor; the cursor is published with one release store every
	AUDIO_RING_PUBLISH samples (sub-batch leftovers go out with the next
	AICA_Sample32 call, ~0.7ms later - the FIFO holds 42ms).

	Consumer (audio thread, cpu0): every AUDIO_POLL_NS it loads write_pub
	(acquire), reads the FIFO level once and pushes only what fits, so its
	stores never stall; then retires them with a store to read_pub. It
	never sleeps on the FIFO: sleeping 1ms between polls is what keeps it
	off cpu0 (see below), and the 2048-deep FIFO covers that 40 times over.

	Pacing: the old path was paced by the FIFO store stalling while the FIFO
	was full. Now a full ring is the limiter: with LimitFPS the producer
	futex-waits on read_pub until the consumer retires something (the
	consumer only issues FUTEX_WAKE when prod_waiting is set, so it costs
	nothing unless the emulator is ahead of realtime). Without LimitFPS a
	full ring drops the sample, like the old `polly2_audio_space() > 1`
	check. prod_waiting/read_pub use the Dekker pattern (seq_cst store,
	then seq_cst load on each side) so a wake can't be lost; the futex
	also has a 2ms timeout as a backstop.

	cpu0 sharing: the TA consumer (tacore.inl) is pinned to cpu0 and parks
	in WFE, which the scheduler can't see - to CFS it is a 100% busy nice 0
	task that never sleeps. The audio thread is also SCHED_OTHER nice 0 and
	sleeps ~1ms between short bursts, so its vruntime stays far behind the
	spinner's and every hrtimer wakeup preempts it immediately (and the
	resched IPI/timer IRQ also knocks the core out of WFE).
*/
static const u32 AUDIO_RING_MAX     = 2048;   // array size; ring may be smaller
static const u32 AUDIO_RING_PUBLISH = 32;     // power of two
static const long AUDIO_POLL_NS     = 1000000;
static const long AUDIO_WAIT_NS     = 2000000; // producer futex timeout

struct AudioRing {
	// producer-owned
	alignas(64) u32 write_pub;     // published write cursor (free-running)
	u32 write_priv;                // producer-private cursor
	u32 read_cache;                // producer's copy of read_pub
	u32 size, mask;                // power of two <= AUDIO_RING_MAX
	// consumer-owned; also the producer's futex word
	alignas(64) u32 read_pub;
	// set by the producer while it is waiting for room
	alignas(64) u32 prod_waiting;
	alignas(64) u32 data[AUDIO_RING_MAX]; // (r << 16) | l, as AUDIO_DATA wants
};

static AudioRing audio_ring;
static bool audio_threaded;                   // ring + thread active
static std::atomic<bool> audio_thread_stop;
static std::atomic<bool> audio_thread_alive;
static pthread_t audio_thread_handle;

static inline long audio_futex(u32* addr, int op, u32 val, const struct timespec* ts)
{
	return syscall(SYS_futex, addr, op, val, ts, NULL, 0);
}

static void* audio_thread_main(void*)
{
	// Don't take process-directed signals: the polly2 module SIGUSR1s every
	// /dev/polly2 holder, SIGTERM is sigwait()ed by main.cpp. Inherited mask
	// already blocks SIGTERM; block the rest too (same as gdrom's fill thread).
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);

	// cpu0, with the TA consumer; cpu1 belongs to the SH4 thread.
	unsigned long cpumask = 1ul << 0;
	if (syscall(SYS_sched_setaffinity, 0, sizeof(cpumask), &cpumask) != 0)
		printf("audio: failed to pin feeder thread to cpu0\n");
	else
		printf("audio: FIFO feeder thread pinned to cpu0, ring %u\n", audio_ring.size);

	AudioRing& R = audio_ring;
	u32 rd = R.read_pub;
	const struct timespec poll = { 0, AUDIO_POLL_NS };

	while (!audio_thread_stop.load(std::memory_order_relaxed))
	{
		u32 wr = __atomic_load_n(&R.write_pub, __ATOMIC_ACQUIRE);
		u32 avail = wr - rd;
		if (avail)
		{
			u32 space = polly2_audio_space();
			u32 n = avail < space ? avail : space;
			for (u32 i = 0; i < n; i++)
				polly2_mmio_wr(POLLY2_MMIO_AUDIO_DATA, R.data[(rd + i) & R.mask]);

			if (n)
			{
				rd += n;
				__atomic_store_n(&R.read_pub, rd, __ATOMIC_SEQ_CST);
				if (__atomic_load_n(&R.prod_waiting, __ATOMIC_SEQ_CST))
					audio_futex(&R.read_pub, FUTEX_WAKE_PRIVATE, 1, NULL);
			}
		}
		nanosleep(&poll, NULL);
	}

	audio_thread_alive.store(false, std::memory_order_release);
	// in case the producer is parked (it isn't at TermAudio, but be safe)
	audio_futex(&R.read_pub, FUTEX_WAKE_PRIVATE, 1, NULL);
	return NULL;
}

// Producer slow path: the ring is full. Returns true once there is room,
// false if the feeder thread is gone (caller drops the sample).
static __attribute__((noinline)) bool audio_ring_wait(u32 wr)
{
	AudioRing& R = audio_ring;
	// everything written so far must be visible for the consumer to drain it
	__atomic_store_n(&R.write_pub, wr, __ATOMIC_RELEASE);

	const struct timespec to = { 0, AUDIO_WAIT_NS };
	for (;;)
	{
		__atomic_store_n(&R.prod_waiting, 1, __ATOMIC_SEQ_CST);
		u32 rd = __atomic_load_n(&R.read_pub, __ATOMIC_SEQ_CST);
		if (wr - rd < R.size || !audio_thread_alive.load(std::memory_order_acquire))
		{
			__atomic_store_n(&R.prod_waiting, 0, __ATOMIC_RELAXED);
			R.read_cache = rd;
			return wr - rd < R.size;
		}
		audio_futex(&R.read_pub, FUTEX_WAIT_PRIVATE, rd, &to);
	}
}

static inline void audio_ring_put(u32 v, bool wait)
{
	AudioRing& R = audio_ring;
	u32 wr = R.write_priv;

	// the cached read cursor says full: refresh it, then wait or drop
	if (wr - R.read_cache >= R.size)
	{
		R.read_cache = __atomic_load_n(&R.read_pub, __ATOMIC_ACQUIRE);
		if (wr - R.read_cache >= R.size)
		{
			if (!wait || !audio_ring_wait(wr))
				return;
		}
	}

	R.data[wr & R.mask] = v;
	wr++;
	R.write_priv = wr;
	if ((wr & (AUDIO_RING_PUBLISH - 1)) == 0)
		__atomic_store_n(&R.write_pub, wr, __ATOMIC_RELEASE);
}

// Free output space for pacing (freerunning UpdateSystem): FIFO space minus
// what is still queued in the ring. SH4 thread only (reads write_priv).
u32 audio_output_space()
{
	if (!polly2_mmio)
		return POLLY2_AUDIO_FIFO_DEPTH;

	u32 space = polly2_audio_space();
	if (audio_threaded)
	{
		u32 queued = audio_ring.write_priv - __atomic_load_n(&audio_ring.read_pub, __ATOMIC_RELAXED);
		space = queued >= space ? 0 : space - queued;
	}
	return space;
}

struct AudioStream_Mister : AudioStream {
	// AICA produces 44.1 kHz; the polly2 HDMI I2S side drains at 48 kHz.
	// Push-side mirror of ReadAudioResampling: each input frame shifts the
	// 4-frame Catmull-Rom window by one, then emits the output points
	// (spaced 44100/48000 of an input sample apart) inside that interval -
	// 1 or 2 outputs per input, 48000/44100 on average.
	SoundFrame Status[4] = { { 0 } };
	float current_partial_pos = 0;

	void WriteSample(s16 r, s16 l)
	{
		if (!polly2_mmio)
			return;

		bool wait = settings.aica.LimitFPS;

		Status[0] = Status[1];
		Status[1] = Status[2];
		Status[2] = Status[3];
		Status[3].l = l;
		Status[3].r = r;

		const float inc = 44100.0f / 48000.0f;
		while (current_partial_pos < 1)
		{
			SoundFrame res = {
				SaturateToS16(InterpolateCatmull4pt3oX(Status[0].l,Status[1].l,Status[2].l,Status[3].l,current_partial_pos)),
				SaturateToS16(InterpolateCatmull4pt3oX(Status[0].r,Status[1].r,Status[2].r,Status[3].r,current_partial_pos))
			};
			if (audio_threaded) {
				// ring full: wait (frame limiter) or drop, see above
				audio_ring_put(((u32)(u16)res.r << 16) | (u16)res.l, wait);
			}
			// wait: the fabric stalls the store while the FIFO is full;
			// otherwise drop rather than block the emu thread.
			else if (wait || (polly2_audio_space() > 1)) {
				polly2_audio_push(res.l, res.r);
			}
			current_partial_pos += inc;
		}
		current_partial_pos -= 1;
	}

	void StartThread()
	{
		int ring = cfgLoadInt("audio", "ring", 1024);
		u32 size = 64;
		while (size < AUDIO_RING_MAX && size < (u32)ring)
			size <<= 1;

		audio_ring.write_pub = audio_ring.write_priv = audio_ring.read_cache = 0;
		audio_ring.read_pub = 0;
		audio_ring.prod_waiting = 0;
		audio_ring.size = size;
		audio_ring.mask = size - 1;
		audio_thread_stop.store(false);
		audio_thread_alive.store(true);

		// plain SCHED_OTHER even if the creating thread runs RT
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
		pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
		struct sched_param sp = {};
		pthread_attr_setschedparam(&attr, &sp);
		int err = pthread_create(&audio_thread_handle, &attr, audio_thread_main, NULL);
		pthread_attr_destroy(&attr);

		if (err != 0) {
			printf("audio: feeder thread failed (%d), pushing from the emu thread\n", err);
			audio_thread_alive.store(false);
			return;
		}
		audio_threaded = true;
	}

	void InitAudio()
	{
		if (cfgLoadInt("audio", "disable", 0)) {
			printf("WARNING: Audio disabled in config!\n");
			return;
		}

		if (polly2_mmio_init() != 0)
			return;

		if (cfgLoadInt("audio", "threaded", 1))
			StartThread();
		else
			printf("audio: threaded feeder off, pushing from the emu thread\n");
	}

	void TermAudio()
	{
		// Normal teardown only; the _exit()/SIGTERM exits just take the
		// thread down with the process (it holds nothing that needs saving).
		if (audio_threaded) {
			audio_thread_stop.store(true);
			pthread_join(audio_thread_handle, NULL);
			audio_threaded = false;
		}
	}
};

AudioStream* AudioStream::Create() {
	return new AudioStream_Mister();
}
