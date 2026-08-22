// Mocks of the emulator surface constprop()/rw_related() touch, so they can be
// compiled and tested on the host. Kept deliberately close to the real thing:
// the register numbering, the FMT_* values, the shilop order and the
// shil_param predicates all have to match libswirl or the test proves nothing.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

using namespace std;

typedef uint64_t u64;
typedef int64_t s64;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint8_t u8;
typedef int8_t s8;

#define verify(x) do { if (!(x)) { printf("verify failed %s:%d: %s\n", __FILE__, __LINE__, #x); fflush(stdout); abort(); } } while (0)

#define PAGE_SIZE 4096

// 16MB, matching a stock dreamcast
#define RAM_SIZE  (16 * 1024 * 1024)
#define RAM_MASK  (RAM_SIZE - 1)

enum Sh4RegType
{
	reg_r0, reg_r1, reg_r2, reg_r3, reg_r4, reg_r5, reg_r6, reg_r7,
	reg_r8, reg_r9, reg_r10, reg_r11, reg_r12, reg_r13, reg_r14, reg_r15,

	reg_r0_Bank, reg_r1_Bank, reg_r2_Bank, reg_r3_Bank,
	reg_r4_Bank, reg_r5_Bank, reg_r6_Bank, reg_r7_Bank,

	reg_gbr, reg_ssr, reg_spc, reg_sgr, reg_dbr, reg_vbr,
	reg_mach, reg_macl, reg_pr, reg_fpul,

	reg_fr_0, reg_fr_1, reg_fr_2, reg_fr_3, reg_fr_4, reg_fr_5, reg_fr_6, reg_fr_7,
	reg_fr_8, reg_fr_9, reg_fr_10, reg_fr_11, reg_fr_12, reg_fr_13, reg_fr_14, reg_fr_15,

	reg_xf_0, reg_xf_1, reg_xf_2, reg_xf_3, reg_xf_4, reg_xf_5, reg_xf_6, reg_xf_7,
	reg_xf_8, reg_xf_9, reg_xf_10, reg_xf_11, reg_xf_12, reg_xf_13, reg_xf_14, reg_xf_15,

	reg_sr_T, reg_sr_status, reg_old_sr_status, reg_fpscr, reg_old_fpscr,
	reg_pc_dyn,

	sh4_reg_count
};

enum shil_param_type
{
	FMT_NULL,
	FMT_IMM,
	FMT_I32,
	FMT_F32,
	FMT_F64,
	FMT_V2,
	FMT_V3,
	FMT_V4,
	FMT_V8,
	FMT_V16,

	FMT_REG_BASE = FMT_I32,
	FMT_VECTOR_BASE = FMT_V2,
};

// same order as shil_canonical.h -- keep in sync
enum shilop
{
	shop_mov32,
	shop_mov64,
	shop_jdyn,
	shop_jcond,
	shop_ifb,
	shop_readm,
	shop_writem,
	shop_sync_sr,
	shop_sync_fpscr,
	shop_and,
	shop_or,
	shop_xor,
	shop_not,
	shop_add,
	shop_sub,
	shop_neg,
	shop_shl,
	shop_shr,
	shop_sar,
	shop_adc,
	shop_sbc,
	shop_ror,
	shop_rocl,
	shop_rocr,
	shop_swaplb,
	shop_swap,
	shop_shld,
	shop_shad,
	shop_ext_s8,
	shop_ext_s16,
	shop_mul_u16,
	shop_mul_s16,
	shop_mul_i32,
	shop_mul_u64,
	shop_mul_s64,
	shop_div32u,
	shop_div32s,
	shop_div32p2,
	shop_debug_3,
	shop_debug_1,
	shop_cvt_f2i_t,
	shop_cvt_i2f_n,
	shop_cvt_i2f_z,
	shop_pref,
	shop_test,
	shop_seteq,
	shop_setge,
	shop_setgt,
	shop_setae,
	shop_setab,
	shop_setpeq,
	shop_fadd,
	shop_fsub,
	shop_fmul,
	shop_fdiv,
	shop_fabs,
	shop_fneg,
	shop_fsqrt,
	shop_fipr,
	shop_ftrv,
	shop_fmac,
	shop_fsrra,
	shop_fsca,
	shop_fseteq,
	shop_fsetgt,
	shop_frswap,
	shop_max,
};

// same values as decoder.h (mkbet packing)
enum BlockEndType
{
	BET_StaticJump  = 0x00,
	BET_StaticCall  = 0x02,
	BET_StaticIntr  = 0x06,
	BET_DynamicJump = 0x08,
	BET_DynamicCall = 0x0A,
	BET_DynamicRet  = 0x0C,
	BET_DynamicIntr = 0x0E,
	BET_Cond_0      = 0x10,
	BET_Cond_1      = 0x11,
};

struct shil_param
{
	u32 type = FMT_NULL;
	union {
		u32 _imm = 0;
		Sh4RegType _reg;
	};

	bool is_null() const { return type == FMT_NULL; }
	bool is_imm() const { return type == FMT_IMM; }
	bool is_reg() const { return type >= FMT_REG_BASE; }
	bool is_r32i() const { return type == FMT_I32; }
	bool is_r32f() const { return type == FMT_F32; }
	bool is_r64f() const { return type == FMT_F64; }
	bool is_r32() const { return is_r32i() || is_r32f(); }
	u32 is_r32fv() const { return type >= FMT_V2 ? count() : 0; }

	bool is_imm_s16() const { return is_imm() && (s32)_imm >= -32768 && (s32)_imm <= 32767; }

	u32 count() const
	{
		switch (type)
		{
		case FMT_I32: case FMT_F32: return 1;
		case FMT_F64: case FMT_V2: return 2;
		case FMT_V4: return 4;
		case FMT_V16: return 16;
		default: return 0;
		}
	}
};

// shop_ftrv flags: known-constant w, the loading op was removed
#define FTRV_W_ZERO 0x1
#define FTRV_W_ONE  0x2

struct shil_opcode
{
	shilop op = shop_add;
	u32 Flow = 0;
	u32 flags = 0;
	u32 flags2 = 0;
	shil_param rd, rd2, rs1, rs2, rs3;
	u16 host_offs = 0;
	u16 guest_offs = 0;
	u16 host_body = 0;
	u16 host_body_end = 0;
	std::string dissasm() { return ""; }
};

struct RuntimeBlockInfo
{
	u32 addr = 0;
	u32 sh4_code_size = 4;
	u32 BranchBlock = 0xFFFFFFFF;
	u32 NextBlock = 0xFFFFFFFF;
	BlockEndType BlockType = BET_StaticJump;
	bool static_from_constprop = false;
	u32 memops = 0;
	u32 linkedmemops = 0;
	vector<shil_opcode> oplist;
};

// ---- controllable emulator state -------------------------------------------

// dc boot rom: 2MB at physical 0
#define BOOT_ROM_SIZE (2 * 1024 * 1024)

// fake ram, indexed by physical offset
extern u8 mock_ram[RAM_SIZE];
// fake boot rom
extern u8 mock_rom[BOOT_ROM_SIZE];
// when true for a page, bm_RamPageHasData() reports it as data-containing
extern bool mock_page_has_data[RAM_SIZE / PAGE_SIZE];
// counts ReadMem* calls that fell outside ram/rom -- must stay zero
extern u32 mock_bad_reads;

inline void mock_reset()
{
	memset(mock_ram, 0, sizeof(mock_ram));
	memset(mock_rom, 0, BOOT_ROM_SIZE);
	memset(mock_page_has_data, 0, sizeof(mock_page_has_data));
	mock_bad_reads = 0;
}

inline bool IsOnRam(u32 addr)
{
	// same region test as sh4_mem.cpp
	if (((addr >> 26) & 0x7) == 3)
		if ((((addr >> 29) & 0x7) != 7) && (((addr >> 29) & 0x7) != 3))
			return true;
	return false;
}

inline bool mock_on_rom(u32 addr)
{
	if (((addr >> 29) & 7) == 7) return false; // p4 / store queues
	return (addr & 0x1FFFFFFF) < BOOT_ROM_SIZE;
}

inline u32 ReadMem32(u32 addr)
{
	if (mock_on_rom(addr))
	{
		u32 v;
		memcpy(&v, &mock_rom[addr & 0x1FFFFFFF], 4);
		return v;
	}
	if (!IsOnRam(addr))
	{
		// the real one would dispatch into an mmio handler here
		mock_bad_reads++;
		return 0xDEADBEEF;
	}
	u32 v;
	memcpy(&v, &mock_ram[addr & RAM_MASK], 4);
	return v;
}

inline u16 ReadMem16(u32 addr)
{
	if (mock_on_rom(addr))
	{
		u16 v;
		memcpy(&v, &mock_rom[addr & 0x1FFFFFFF], 2);
		return v;
	}
	if (!IsOnRam(addr))
	{
		mock_bad_reads++;
		return 0xDEAD;
	}
	u16 v;
	memcpy(&v, &mock_ram[addr & RAM_MASK], 2);
	return v;
}

inline u8 ReadMem8(u32 addr)
{
	if (mock_on_rom(addr))
		return mock_rom[addr & 0x1FFFFFFF];
	if (!IsOnRam(addr))
	{
		mock_bad_reads++;
		return 0xDD;
	}
	return mock_ram[addr & RAM_MASK];
}

// fuse_readm_pairs only runs with the nvmem fastpath available
inline bool _nvmem_enabled() { return true; }

inline bool bm_RamPageHasData(u32 guest_addr, u32 len)
{
	auto page_base = (guest_addr & RAM_MASK) / PAGE_SIZE;
	auto page_top = ((guest_addr + len - 1) & RAM_MASK) / PAGE_SIZE;

	bool rv = false;
	for (auto i = page_base; i <= page_top; i++)
		rv |= mock_page_has_data[i];
	return rv;
}
