/*
	This file is part of libswirl
*/
#include "license/bsd"

#pragma once

#include "types.h"

/*
	Writes a text map of jit blocks (runtime address, size, guest pc) so that
	dynarec frames can be symbolized after a perf run.

	perf will not consume this itself. The usual /tmp/perf-<pid>.map fallback only
	applies to addresses in *anonymous* executable mappings, and our code cache
	lives inside SH4_TCB, a static array in .text of the main binary -- so perf
	resolves jit addresses against minicast.elf and never reads the map. jitdump
	does not work either: perf inject --jit fails to decode a MiSTer-recorded
	perf.data on a different host arch.

	Instead, tools/symbolize_perf.py joins `perf script` text output against this
	map, which avoids perf's symbolization path entirely.

	Block symbols are "sh4_<guest-pc>+<guest-bytes>". The guest span matters
	because a block's cost belongs to every SH4 instruction it covers, not just
	its entry pc -- tools/perfheat.py uses it to spread a sample across the
	address range the block actually executes.

	Set MINICAST_PERF_MAP to the file to write; the value is used verbatim
	(relative paths included) so the map lands where you can find it. There is no
	pid-named fallback - captures are normally symbolized on a different machine,
	where /tmp/perf-<pid>.map is both hard to locate and easy to mix up between
	runs. MINICAST_SHOP_MAP works the same way.

	Usage:
		MINICAST_PERF_MAP=/tmp/jit.map ./minicast.elf ...
		perf record -F 999 -g --tid <tid> ...
		perf script -i perf.data -F ip,sym,dso > samples.txt
		tools/symbolize_perf.py samples.txt /tmp/jit.map minicast.elf

	MINICAST_SHOP_MAP additionally writes a second, finer map that splits each block
	into its individual shil opcodes, so cost can be attributed per shop rather than
	per block. This is not perf-map format and only symbolize_perf.py --by-shop
	reads it:

		block <runtime-start-hex> <size-hex> <guest-pc-hex> <relink-offs-hex>
		op <host-offs-hex> <shop name>
		op <host-offs-hex> <shop name>
		...

	The op offsets are relative to the block's runtime start and are sorted
	ascending; an op runs until the next op's offset, the last until the relink
	offset. Code before the first op (prologue) and from relink-offs to size
	(block exit) belongs to no shop.

	Regalloc-emitted code (OpBegin preloads / OpEnd writebacks) is reported as
	its own synthetic shop, <regalloc>, and the smc/cycle-check prologue as
	<sched/smc>, so the fixed per-block overhead and the allocator's spill
	traffic can be read separately from the op bodies.
*/
struct RuntimeBlockInfo;

void perf_map_Init();
void perf_map_AddBlock(void* code, u32 host_code_size, u32 guest_addr, u32 guest_size);
void perf_map_AddShops(void* code, u32 host_code_size, RuntimeBlockInfo* block);
void perf_map_Term();
