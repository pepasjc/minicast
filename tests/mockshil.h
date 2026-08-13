// Minimal mocks of types.h/shil.h so regalloc.h can be fuzzed on the host.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <set>
#include <deque>

using namespace std;

typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef uint8_t u8;

#define verify(x) do { if (!(x)) { printf("verify failed %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static inline void die(const char* msg) { printf("die: %s\n", msg); abort(); }

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
	FMT_V4,
	FMT_V16,
};

enum shilop
{
	shop_mov32,
	shop_mov64,
	shop_ifb,
	shop_sync_sr,
	shop_sync_fpscr,
	shop_readm,
	shop_writem,
	shop_jcond,
	shop_jdyn,
	// generic ops for fuzzing; the allocator doesn't special-case these
	shop_add,
	shop_sbc,
	shop_fadd,
	// vector ops: the test allocator explodes their V4 and F64 params (like
	// arm32 does for ftrv/fipr and for the f64 movs/memops); V16 stays a
	// memory operand
	shop_ftrv,
	shop_fipr,
	shop_fsca,	// exploded F64 pair write from a scalar source
	shop_frswap,	// V16 read-write of both banks: spans must flush and die
};

struct shil_param
{
	u32 type = FMT_NULL;
	Sh4RegType _reg = reg_r0;
	u32 _imm = 0;

	bool is_null() const { return type == FMT_NULL; }
	bool is_imm() const { return type == FMT_IMM; }
	bool is_reg() const { return type >= FMT_I32; }
	bool is_r32i() const { return type == FMT_I32; }
	bool is_r32f() const { return type == FMT_F32; }
	bool is_r64f() const { return type == FMT_F64; }
	bool is_r32() const { return is_r32i() || is_r32f(); }

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

struct shil_opcode
{
	shilop op = shop_add;
	shil_param rd, rd2, rs1, rs2, rs3;
};

struct RuntimeBlockInfo
{
	u32 addr = 0;
	vector<shil_opcode> oplist;
};

// only dereferenced on the shop_ifb path, which the fuzzer never generates
struct MockOpDesc { u32 mask; };
static MockOpDesc* OpDesc[0x10000];
