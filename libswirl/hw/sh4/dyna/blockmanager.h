/*
	This file is part of libswirl
*/
#include "license/bsd"


/*
	In case you wonder, the extern "C" stuff are for the assembly code on beagleboard/pandora
*/
#include <map>
#include "types.h"
#include "decoder.h"
#pragma once
#include <set>

typedef void (*DynarecCodeEntryPtr)();

struct RuntimeBlockInfo_Core
{
	u32 addr;
	DynarecCodeEntryPtr code;
	u32 lookups;
};

struct RuntimeBlockInfo: RuntimeBlockInfo_Core
{
	void Setup(u32 pc,fpscr_t fpu_cfg);
	const char* hash(bool full=true, bool reloc=false);

	u32 host_code_size;	//in bytes
	u32 sh4_code_size; //in bytes

	u32 runs;
	s32 staging_runs;

	/*
		
	*/
	fpscr_t fpu_cfg;
	u32 guest_cycles;
	u32 guest_opcodes;
	u32 host_opcodes;


	u32 BranchBlock; //if not 0xFFFFFFFF then jump target
	u32 NextBlock;   //if not 0xFFFFFFFF then next block (by position)

	//0 if not available
	RuntimeBlockInfo* pBranchBlock;
	RuntimeBlockInfo* pNextBlock; 

	u32 relink_offset;
	u32 relink_data;
	u32 csc_RetCache; //only for stats for now

	BlockEndType BlockType;
	bool has_jcond;
	// set when constprop turned a dynamic end into a static one. Such a block
	// looks statically-targeted but stands in for an indirect jump, so the
	// backwards-only cycle check must not treat it as provably forward.
	bool static_from_constprop;

	// op 0's regalloc preloads are emitted before the smc/cycle-check code;
	// this range lets the shop map attribute them (per-shop profiling only)
	u16 host_preload0;
	u16 host_preload0_end;

	vector<shil_opcode> oplist;

	bool contains_code(u8* ptr)
	{
		return ((unat)(ptr-(u8*)code))<host_code_size;
	}

	virtual ~RuntimeBlockInfo();

	virtual u32 Relink()=0;
	virtual void Relocate(void* dst)=0;
	
	//predecessors references
	set<RuntimeBlockInfo*> pre_refs;

	void AddRef(RuntimeBlockInfo* other);
	void RemRef(RuntimeBlockInfo* other);

	void Discard();
	void UpdateRefs();

	u32 memops;
	u32 linkedmemops;
	// key is a host pc for a load/store in a jit block
	// value contains information for mem-op rewriting purposes.
	struct memop_info {
		uint16_t opid;
		uint8_t  rewrite_offset;
		uint8_t  emitted_bytes;
	};		
	std::map<void*, memop_info> memory_accesses;
};

struct CachedBlockInfo: RuntimeBlockInfo_Core
{
	RuntimeBlockInfo* block;
};

void bm_WriteBlockMap(const string& file);


DynarecCodeEntryPtr DYNACALL bm_GetCode(u32 addr);

extern "C" {
ATTR_USED DynarecCodeEntryPtr DYNACALL bm_GetCode2(u32 addr);
}

RuntimeBlockInfo* bm_GetBlock(void* dynarec_code);
RuntimeBlockInfo* bm_GetStaleBlock(void* dynarec_code);
RuntimeBlockInfo* DYNACALL bm_GetBlock(u32 addr);

void bm_AddBlock(RuntimeBlockInfo* blk, bool lockRam);
void bm_DiscardBlock(RuntimeBlockInfo* blk);

void bm_Reset();
void bm_Periodical_1s();
void bm_Periodical_14k();
void bm_Sort();

//Loop-idiom detection (shil.cpp): walk static edges from the anchor and look
//for a cycle back through it. true = settled (loop found and reported, or
//provably no loop); false = graph incomplete, retry once more code compiled.
//On a find, loop_out (if given) receives the cycle members keyed by guest
//address - the lowest address is the header - ready for ngen->CompileLoop().
bool optimize_anchor(RuntimeBlockInfo* anchor_rbi, std::map<u32, RuntimeBlockInfo*>* loop_out = nullptr);

//Region-growth probe (shil.cpp): true when a static path from 'start' reaches
//an edge targeting 'header' - i.e. a later-compiled loop arm closed the cycle
//and the region at 'header' should be regrown.
bool anchor_reaches_header(u32 start, u32 header);

//Multiblock SQ store/pref marking over a loop's member set (shil.cpp); call
//before ngen->CompileLoop(). Gated on opt_sqw_on_pref.
void sq_pref_multiblock(std::map<u32, RuntimeBlockInfo*>& blocks);

void bm_Init();
void bm_Term();

void bm_vmem_pagefill(void** ptr,u32 PAGE_SZ);

void bm_sh4_jitsym(FILE* out);
bool bm_LockedWrite(u8* addy);
bool bm_RamPageHasData(u32 guest_addr, u32 len);

void bm_CleanupDeletedBlocks();