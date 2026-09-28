/*
	This file is part of libswirl
*/
#include "license/bsd"

#pragma once

#include "types.h"
#include <string.h>
#include <atomic>
#include <map>

/*
	TA data ring buffer

	Producer: the "ta fsm" (ta.cpp) parses TA data, copies each 32-byte block
	into this ring and, on end-of-list, spin-waits for it to drain before
	raising the list interrupt.

	Consumer: lxdream's tacore drains the ring on its own thread/core and feeds
	pvr2_ta_process_block().

	This is a Cortex-A9 (dual core) target, so the single-producer /
	single-consumer hand-off is ordered with DMB ISH. The barrier is NOT taken
	per block - on an A9 a dmb ish is a serializing ~30-cycle bubble, and taking
	one per 32-byte block dominated the SQ path. Instead the producer copies
	blocks and bumps a private write cursor with plain stores, then publishes
	the whole batch with a single barrier via ta_ring_publish():
	  Producer: write data..., then DMB ISH, then store the shared write index.
	  Consumer: read the shared write index, then DMB ISH, then read the data.

	False sharing is avoided by keeping the producer- and consumer-owned indices
	on separate cache lines, and by each side caching the other's index so the
	steady-state fill/empty checks don't touch the shared line.
*/

// Must stay a power of two.
#define TA_RING_BYTES    (8 * 1024 * 1024)
#define TA_RING_BLOCK    32
#define TA_RING_BLOCKS   (TA_RING_BYTES / TA_RING_BLOCK)
#define TA_RING_MASK     (TA_RING_BLOCKS - 1)

#define TA_RING_LINE     64  // A9 cache line

// Publish to the consumer once this many blocks have been staged. Amortizes the
// dmb ish across a batch instead of paying it per block, while still keeping the
// consumer fed so it runs concurrently with the producer. Must be a power of two.
#define TA_RING_BATCH    32

// How far ahead of the write cursor ta_ring_push prefetches, in blocks. The
// ring is far bigger than the L2, so every new cache line the producer writes
// is a write-allocate miss that has to come from DDR; a few lines of lead
// hide that even for back-to-back pushes (ta_vtx_data's DMA loop).
#define TA_RING_PREFETCH 8

// Register and control ops
#define TA_RING_DECOUPLED_MAGIC 0xDEADBEEDCAFEBEBEULL
#define TA_RING_DECOUPLED_OP_REGWRITE 0
#define TA_RING_DECOUPLED_OP_LISTINIT 1
#define TA_RING_DECOUPLED_OP_SOFTRESET 2
#define TA_RING_DECOUPLED_OP_STARTPOLLY 3

struct ta_ring_t
{
	// Producer-owned. write_pub is the batch of blocks published to the
	// consumer; write_priv is the producer's private cursor (unpublished
	// blocks it has already copied in). Own cache line.
	DECL_ALIGN(TA_RING_LINE) volatile u32 write_pub;
	u32 write_priv;            // producer-private, not shared
	u32 read_cache;            // producer's cached copy of read_pub

	// Consumer-owned. Own cache line.
	DECL_ALIGN(TA_RING_LINE) volatile u32 read_pub;
	u32 write_cache;           // consumer's cached copy of write_pub

	DECL_ALIGN(TA_RING_LINE) u8 data[TA_RING_BYTES];
};

extern ta_ring_t ta_ring;
extern std::atomic<u64> ta_eol_interrupt_mark; // counts EOLs
extern std::map<u32, u64> ta_contexts;

extern u8 pvr_regs_mtta[];

// DMB ISH - inner shareable data memory barrier (A9 SMP hand-off)
#if HOST_CPU == CPU_ARM || HOST_CPU == CPU_ARM64
#define TA_RING_DMB() __asm__ __volatile__("dmb ish" ::: "memory")
#else
#define TA_RING_DMB() __sync_synchronize()
#endif

// WFE/SEV pairing for the idle edges: a waiter parks in wfe instead of
// hammering the shared cache lines (and heating a core the other thread
// shares an L2 with); the other side sevs after publishing its index.
// The event register makes check-then-wait race-free: a sev arriving
// between the check and the wfe leaves the event pending, so the wfe
// falls straight through and the loop re-checks. wfe also wakes on any
// interrupt, so a lost sev can only cost one tick, never a hang.
#if HOST_CPU == CPU_ARM || HOST_CPU == CPU_ARM64
#define TA_RING_WFE() __asm__ __volatile__("wfe" ::: "memory")
#define TA_RING_SEV() __asm__ __volatile__("sev" ::: "memory")
#else
#define TA_RING_WFE()
#define TA_RING_SEV()
#endif

// Producer: copy one 32-byte block into the ring, spinning while full. No
// barrier and no shared-index store here - just a plain copy + private cursor
// bump. Call ta_ring_publish() to make the batch visible to the consumer.
static inline void ta_ring_push(const void* block)
{
	u32 wr = ta_ring.write_priv;

	// Wait until there is room. Check against the cached read index first and
	// only refresh from the shared line when the cache says we're full, so the
	// common case never touches the consumer's cache line. A full ring parks
	// in wfe; the consumer sevs after retiring each batch.
	if ((wr - ta_ring.read_cache) >= TA_RING_BLOCKS)
	{
		for (;;)
		{
			ta_ring.read_cache = ta_ring.read_pub;
			if ((wr - ta_ring.read_cache) < TA_RING_BLOCKS)
				break;
			TA_RING_WFE();
		}
	}

	memcpy(&ta_ring.data[(wr & TA_RING_MASK) * TA_RING_BLOCK], block, TA_RING_BLOCK);

	// Pull a later slot's line in now so the copy that lands there doesn't
	// stall on the line fill. Only a hint: it can't fault, and it is a plain
	// coherent line fetch, so it changes nothing for the consumer even when
	// the ring is nearly full and that slot hasn't been retired yet.
	__builtin_prefetch(&ta_ring.data[((wr + TA_RING_PREFETCH) & TA_RING_MASK) * TA_RING_BLOCK], 1);

	wr += 1;
	ta_ring.write_priv = wr;

	// Publish on a batch boundary so the consumer stays fed. One barrier per
	// TA_RING_BATCH blocks instead of one per block.
	if ((wr & (TA_RING_BATCH - 1)) == 0)
	{
		TA_RING_DMB();
		ta_ring.write_pub = wr;
		TA_RING_SEV();
	}
}

// Producer: publish everything pushed since the last publish. One barrier for
// the whole batch: all block stores are ordered before the index store.
static inline void ta_ring_publish()
{
	if (ta_ring.write_pub != ta_ring.write_priv)
	{
		TA_RING_DMB();
		ta_ring.write_pub = ta_ring.write_priv;
		TA_RING_SEV();
	}
}

// Producer: publish, then wait until the consumer has drained everything.
static inline void ta_ring_drain()
{
	ta_ring_publish();
	while (ta_ring.read_pub != ta_ring.write_priv)
	{
		// wait for the consumer to drain the ring; it sevs per retired batch
		TA_RING_WFE();
	}
}

void ta_ring_consumer_start(u8* vram);
void ta_ring_consumer_start_decoupled(u8* vram);

// Pin the calling thread to one core: sh4/producer on cpu0, ta consumer on
// cpu1, so the SPSC hand-off above always runs core-to-core instead of
// migrating and false-sharing. Raw syscall: the CPU_SET macros need
// _GNU_SOURCE before the first glibc header, which this build doesn't define.
#if HOST_OS == OS_LINUX
#include <unistd.h>
#include <sys/syscall.h>

static inline void ta_ring_pin_thread(int cpu)
{
	unsigned long mask = 1ul << cpu;

	if (syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask) != 0)
		printf("ta_ring: failed to pin thread to cpu%d\n", cpu);
	else
		printf("ta_ring: pinned thread to cpu%d\n", cpu);
}
#else
static inline void ta_ring_pin_thread(int cpu) { }
#endif