// Tests for the constprop pass (extracted verbatim from shil.cpp).
//
// Three layers:
//  1. an arm32-form validator: every opcode constprop emits must be encodable
//     by rec_arm.cpp (which register/immediate slots each opcode allows)
//  2. directed tests for each transformation and each safety rule
//  3. a differential fuzzer: random blocks are interpreted with canonical
//     (shil_canonical.h) semantics before and after constprop; register state,
//     the memory-write trace and the block exit target must match exactly.
//
// args: <base seed> <fuzz iterations>

#include "constprop_under_test.h"

#include <random>
#include <unordered_map>

u8 mock_ram[RAM_SIZE];
u8 mock_rom[BOOT_ROM_SIZE];
bool mock_page_has_data[RAM_SIZE / PAGE_SIZE];
u32 mock_bad_reads;

// ---- op construction helpers -----------------------------------------------

static shil_param P() { return shil_param(); }
static shil_param I(u32 v) { shil_param p; p.type = FMT_IMM; p._imm = v; return p; }
static shil_param R(Sh4RegType r) { shil_param p; p.type = FMT_I32; p._reg = r; return p; }
static shil_param F(Sh4RegType r) { shil_param p; p.type = FMT_F32; p._reg = r; return p; }
static shil_param D(Sh4RegType r) { shil_param p; p.type = FMT_F64; p._reg = r; return p; }
static shil_param V(Sh4RegType r, u32 fmt) { shil_param p; p.type = fmt; p._reg = r; return p; }

static shil_opcode OP(shilop op, shil_param rd, shil_param rs1, shil_param rs2 = P(),
                      u32 flags = 0, shil_param rs3 = P(), shil_param rd2 = P())
{
	shil_opcode o;
	o.op = op;
	o.rd = rd;
	o.rd2 = rd2;
	o.rs1 = rs1;
	o.rs2 = rs2;
	o.rs3 = rs3;
	o.flags = flags;
	return o;
}

static int count_op(const vector<shil_opcode>& v, shilop op)
{
	int n = 0;
	for (auto& o : v)
		if (o.op == op)
			n++;
	return n;
}

static const shil_opcode* find_op(const vector<shil_opcode>& v, shilop op)
{
	for (auto& o : v)
		if (o.op == op)
			return &o;
	return nullptr;
}

// index of first mov32 <rd>, #<imm> -- -1 if absent
static int find_mov_imm(const vector<shil_opcode>& v, Sh4RegType rd, u32 imm)
{
	for (size_t i = 0; i < v.size(); i++)
		if (v[i].op == shop_mov32 && v[i].rd.is_r32i() && v[i].rd._reg == rd &&
		    v[i].rs1.is_imm() && v[i].rs1._imm == imm)
			return (int)i;
	return -1;
}

// ---- arm32 form validator ---------------------------------------------------
// Encodes what rec_arm.cpp accepts; anything else would die in the backend.

static void check_arm32_forms(const vector<shil_opcode>& ops)
{
	for (auto& o : ops)
	{
		if (o.rs1.is_imm())
			verify(o.op == shop_mov32 || o.op == shop_readm || o.op == shop_ifb);

		if (o.rs2.is_imm())
			verify(o.op == shop_add || o.op == shop_sub || o.op == shop_and ||
			       o.op == shop_or || o.op == shop_xor || o.op == shop_shl ||
			       o.op == shop_shr || o.op == shop_sar || o.op == shop_ror ||
			       o.op == shop_test || o.op == shop_seteq || o.op == shop_setge ||
			       o.op == shop_setgt || o.op == shop_setae || o.op == shop_setab ||
			       o.op == shop_jdyn || o.op == shop_ifb);

		if (o.rs3.is_imm())
			verify(o.op == shop_readm || o.op == shop_writem || o.op == shop_ifb);

		if (o.op == shop_shld || o.op == shop_shad)
			verify(!o.rs2.is_imm());

		if (o.op == shop_writem)
		{
			verify(o.rs1.is_reg());
			verify(o.rs2.is_reg());
		}

		if (o.op == shop_readm && o.rs1.is_imm())
		{
			verify(o.rs3.is_null());
			u32 size = o.flags & 0x7F;
			verify(size == 2 || size == 4);
		}

		// paired float loads: only the nvmem reg-base 32F/64F forms support rd2
		if (o.op == shop_readm && !o.rd2.is_null())
		{
			u32 size = o.flags & 0x7F;

			if (size == 8)
				verify(o.rd.is_r64f() && o.rd2.is_r64f() && o.rd._reg != o.rd2._reg);
			else
			{
				verify(o.rd.is_r32f() && o.rd2.is_r32f());
				verify(size == 4);
			}

			verify(o.rs1.is_r32i());
			verify(o.rs3.is_null());
		}

		if (o.op == shop_mov32 && o.rd.is_r32f() && o.rs1.is_imm())
			verify(o.rs1._imm == 0 || o.rs1._imm == 0x3F800000);

		if (o.op == shop_jdyn)
			verify(o.rs1.is_reg());
	}
}

// ---- canonical interpreter --------------------------------------------------

static u32 hash32(u32 x)
{
	x ^= x >> 16; x *= 0x7feb352d;
	x ^= x >> 15; x *= 0x846ca68b;
	x ^= x >> 16;
	return x;
}

enum { EV_WRITE = 1, EV_CHK = 2, EV_SQPREF = 3 };

struct Event
{
	u32 kind, a, b, c;
	bool operator==(const Event& e) const
	{
		return kind == e.kind && a == e.a && b == e.b && c == e.c;
	}
};

struct Interp
{
	u32 regs[sh4_reg_count] = {0};
	u32 bank[8] = {0};
	// copy-on-write overlay over mock_ram, keyed by physical offset
	std::unordered_map<u32, u8> memov;
	vector<Event> ev;
	bool has_jdyn = false;
	u32 jdyn_target = 0;

	u8 rd8(u32 addr)
	{
		verify(IsOnRam(addr));
		u32 o = addr & RAM_MASK;
		auto it = memov.find(o);
		return it != memov.end() ? it->second : mock_ram[o];
	}
	void wr8(u32 addr, u8 v)
	{
		verify(IsOnRam(addr));
		memov[addr & RAM_MASK] = v;
	}
	u32 rdmem(u32 addr, u32 size)
	{
		u32 v = 0;
		for (u32 i = 0; i < size; i++)
			v |= (u32)rd8(addr + i) << (8 * i);
		if (size == 1) v = (u32)(s32)(s8)v;
		if (size == 2) v = (u32)(s32)(s16)v;
		return v;
	}
	void wrmem(u32 addr, u32 size, u32 v)
	{
		for (u32 i = 0; i < size; i++)
			wr8(addr + i, (u8)(v >> (8 * i)));
		ev.push_back({EV_WRITE, addr, size, size == 4 ? v : (v & ((1u << (8 * size)) - 1))});
	}

	u32 rdp(const shil_param& p)
	{
		if (p.is_imm()) return p._imm;
		verify(p.is_reg());
		return regs[p._reg];
	}
	u32 rdp0(const shil_param& p) { return p.is_null() ? 0 : rdp(p); }
	void wrp(const shil_param& p, u32 v)
	{
		if (p.is_null()) return;
		verify(p.is_r32());
		regs[p._reg] = v;
	}

	void run(const vector<shil_opcode>& ops)
	{
		for (auto& o : ops)
			step(o);
	}

	void step(const shil_opcode& o)
	{
		switch (o.op)
		{
		case shop_mov32: wrp(o.rd, rdp(o.rs1)); break;

		case shop_add: wrp(o.rd, rdp(o.rs1) + rdp(o.rs2)); break;
		case shop_sub: wrp(o.rd, rdp(o.rs1) - rdp(o.rs2)); break;
		case shop_and: wrp(o.rd, rdp(o.rs1) & rdp(o.rs2)); break;
		case shop_or:  wrp(o.rd, rdp(o.rs1) | rdp(o.rs2)); break;
		case shop_xor: wrp(o.rd, rdp(o.rs1) ^ rdp(o.rs2)); break;

		case shop_not: wrp(o.rd, ~rdp(o.rs1)); break;
		case shop_neg: wrp(o.rd, 0 - rdp(o.rs1)); break;
		case shop_ext_s8:  wrp(o.rd, (u32)(s32)(s8)rdp(o.rs1)); break;
		case shop_ext_s16: wrp(o.rd, (u32)(s32)(s16)rdp(o.rs1)); break;

		case shop_swaplb:
		{
			u32 a = rdp(o.rs1);
			wrp(o.rd, (a & 0xFFFF0000) | ((a & 0xFF) << 8) | ((a >> 8) & 0xFF));
			break;
		}
		case shop_swap:
		{
			u32 a = rdp(o.rs1);
			wrp(o.rd, (a >> 24) | ((a >> 16) & 0xFF00) | ((a & 0xFF00) << 8) | (a << 24));
			break;
		}

		case shop_shl: wrp(o.rd, rdp(o.rs1) << (rdp(o.rs2) & 0x1F)); break;
		case shop_shr: wrp(o.rd, rdp(o.rs1) >> (rdp(o.rs2) & 0x1F)); break;
		case shop_sar: wrp(o.rd, (u32)((s32)rdp(o.rs1) >> (rdp(o.rs2) & 0x1F))); break;
		case shop_ror:
		{
			u32 a = rdp(o.rs1), k = rdp(o.rs2) & 0x1F;
			wrp(o.rd, k ? ((a >> k) | (a << (32 - k))) : a);
			break;
		}

		case shop_shld:
		{
			u32 a = rdp(o.rs1), b = rdp(o.rs2), r;
			if (!(b & 0x80000000))    r = a << (b & 0x1F);
			else if ((b & 0x1F) == 0) r = 0;
			else                      r = a >> ((~b & 0x1F) + 1);
			wrp(o.rd, r);
			break;
		}
		case shop_shad:
		{
			s32 a = (s32)rdp(o.rs1);
			u32 b = rdp(o.rs2), r;
			if (!(b & 0x80000000))    r = (u32)a << (b & 0x1F);
			else if ((b & 0x1F) == 0) r = (u32)(a >> 31);
			else                      r = (u32)(a >> ((~b & 0x1F) + 1));
			wrp(o.rd, r);
			break;
		}

		case shop_adc:
		{
			u64 res = (u64)rdp(o.rs1) + rdp(o.rs2) + rdp(o.rs3);
			wrp(o.rd, (u32)res);
			wrp(o.rd2, (u32)(res >> 32));
			break;
		}
		case shop_sbc:
		{
			u64 res = (u64)rdp(o.rs1) - rdp(o.rs2) - rdp(o.rs3);
			wrp(o.rd, (u32)res);
			wrp(o.rd2, (u32)(res >> 32) & 1);
			break;
		}
		case shop_rocl:
		{
			u32 a = rdp(o.rs1), C = rdp(o.rs2);
			wrp(o.rd, (a << 1) | C);
			wrp(o.rd2, a >> 31);
			break;
		}
		case shop_rocr:
		{
			u32 a = rdp(o.rs1), C = rdp(o.rs2);
			wrp(o.rd, (a >> 1) | (C << 31));
			wrp(o.rd2, a & 1);
			break;
		}

		case shop_mul_u16: wrp(o.rd, (u32)(u16)rdp(o.rs1) * (u32)(u16)rdp(o.rs2)); break;
		case shop_mul_s16: wrp(o.rd, (u32)((s32)(s16)rdp(o.rs1) * (s32)(s16)rdp(o.rs2))); break;
		case shop_mul_i32: wrp(o.rd, rdp(o.rs1) * rdp(o.rs2)); break;
		case shop_mul_u64:
		{
			u64 p = (u64)rdp(o.rs1) * rdp(o.rs2);
			wrp(o.rd, (u32)p);
			wrp(o.rd2, (u32)(p >> 32));
			break;
		}
		case shop_mul_s64:
		{
			u64 p = (u64)((s64)(s32)rdp(o.rs1) * (s64)(s32)rdp(o.rs2));
			wrp(o.rd, (u32)p);
			wrp(o.rd2, (u32)(p >> 32));
			break;
		}

		case shop_div32u:
		{
			u32 a = rdp(o.rs1), b = rdp(o.rs2), quo, rem;
			// division by zero is UB on the real thing; any consistent value works here
			if (b == 0) { quo = 0xFFFFFFFF; rem = a; }
			else        { quo = a / b; rem = a % b; }
			wrp(o.rd, quo);
			wrp(o.rd2, rem);
			break;
		}
		case shop_div32s:
		{
			s32 a = (s32)rdp(o.rs1), b = (s32)rdp(o.rs2);
			u32 quo, rem;
			if (b == 0)                            { quo = 0xFFFFFFFF; rem = (u32)a; }
			else if (a == (s32)0x80000000 && b == -1) { quo = 0x80000000; rem = 0; }
			else                                   { quo = (u32)(a / b); rem = (u32)(a % b); }
			wrp(o.rd, quo);
			wrp(o.rd2, rem);
			break;
		}
		case shop_div32p2:
		{
			u32 a = rdp(o.rs1), b = rdp(o.rs2);
			if (!rdp(o.rs3)) a -= b; // u32 wrap; matches the canonical s32 result bit-for-bit
			wrp(o.rd, a);
			break;
		}

		case shop_test:  wrp(o.rd, (rdp(o.rs1) & rdp(o.rs2)) == 0); break;
		case shop_seteq: wrp(o.rd, rdp(o.rs1) == rdp(o.rs2)); break;
		case shop_setge: wrp(o.rd, (s32)rdp(o.rs1) >= (s32)rdp(o.rs2)); break;
		case shop_setgt: wrp(o.rd, (s32)rdp(o.rs1) > (s32)rdp(o.rs2)); break;
		case shop_setae: wrp(o.rd, rdp(o.rs1) >= rdp(o.rs2)); break;
		case shop_setab: wrp(o.rd, rdp(o.rs1) > rdp(o.rs2)); break;
		case shop_setpeq:
		{
			u32 t = rdp(o.rs1) ^ rdp(o.rs2);
			wrp(o.rd, !((t & 0xFF000000) && (t & 0x00FF0000) && (t & 0x0000FF00) && (t & 0x000000FF)));
			break;
		}

		case shop_readm:
		{
			u32 addr = rdp(o.rs1) + rdp0(o.rs3);
			wrp(o.rd, rdmem(addr, o.flags & 0x7F));
			// paired f32 form: rd2 = [addr+4], read after rd like the backend
			if (!o.rd2.is_null())
				wrp(o.rd2, rdmem(addr + 4, 4));
			break;
		}
		case shop_writem:
			wrmem(rdp(o.rs1) + rdp0(o.rs3), o.flags & 0x7F, rdp(o.rs2));
			break;

		case shop_jdyn:
			has_jdyn = true;
			jdyn_target = rdp(o.rs1) + rdp0(o.rs2);
			break;
		case shop_jcond:
			regs[reg_pc_dyn] = rdp(o.rs1);
			break;

		case shop_ifb:
		{
			// checkpoint: at a fallback every guest register must be current
			for (u32 r = 0; r < 16; r++) ev.push_back({EV_CHK, r, regs[r], 0});
			ev.push_back({EV_CHK, reg_sr_T, regs[reg_sr_T], 0});
			ev.push_back({EV_CHK, reg_macl, regs[reg_macl], 0});
			ev.push_back({EV_CHK, reg_mach, regs[reg_mach], 0});
			// then clobber: the interpreted opcode may write anything.
			// (r7-r11 are kept intact: the generator uses them for addresses)
			u32 seed = o.rs2._imm;
			for (u32 r = 0; r < 16; r++)
				if (r < 7 || r > 11)
					regs[r] ^= hash32(seed + r);
			regs[reg_sr_T] = hash32(seed + 77) & 1;
			regs[reg_macl] ^= hash32(seed + 78);
			if (o.rs3.is_imm() && o.rs3._imm != 0 && IsOnRam(o.rs3._imm))
				wrmem(o.rs3._imm & ~3u, 4, hash32(seed + 99));
			break;
		}

		case shop_sync_sr:
		{
			// UpdateSR() may swap the banked registers
			for (u32 r = 0; r < 8; r++) ev.push_back({EV_CHK, r, regs[r], 1});
			for (u32 r = 0; r < 8; r++) std::swap(regs[r], bank[r]);
			break;
		}
		case shop_sync_fpscr:
			break; // fp banks only, no i32 effect

		case shop_pref:
		{
			u32 a = rdp(o.rs1);
			if ((a >> 26) == 0x38)
				ev.push_back({EV_SQPREF, a, 0, 0});
			break;
		}

		default:
			printf("interp: unhandled op %d\n", o.op);
			abort();
		}
	}
};

// compare an original run against the constprop'd block's run
static void compare_runs(const Interp& orig, const Interp& opt,
                         const RuntimeBlockInfo& optblk, BlockEndType orig_type)
{
	for (u32 r = 0; r < sh4_reg_count; r++)
	{
		if (r == reg_pc_dyn) continue; // covered by the jdyn/exit comparison
		if (orig.regs[r] != opt.regs[r])
		{
			printf("reg mismatch: r%u %08X != %08X\n", r, orig.regs[r], opt.regs[r]);
			abort();
		}
	}

	if (orig.ev.size() != opt.ev.size())
	{
		printf("event count mismatch: %zu != %zu\n", orig.ev.size(), opt.ev.size());
		abort();
	}
	for (size_t i = 0; i < orig.ev.size(); i++)
	{
		if (!(orig.ev[i] == opt.ev[i]))
		{
			printf("event %zu mismatch: {%u %08X %08X %08X} != {%u %08X %08X %08X}\n", i,
			       orig.ev[i].kind, orig.ev[i].a, orig.ev[i].b, orig.ev[i].c,
			       opt.ev[i].kind, opt.ev[i].a, opt.ev[i].b, opt.ev[i].c);
			abort();
		}
	}

	if (orig.has_jdyn)
	{
		if (opt.has_jdyn)
		{
			verify(optblk.BlockType == orig_type);
			verify(opt.jdyn_target == orig.jdyn_target);
		}
		else
		{
			// promoted to a static exit
			verify(orig_type == BET_DynamicJump || orig_type == BET_DynamicCall);
			verify(optblk.BlockType == (orig_type == BET_DynamicJump ? BET_StaticJump : BET_StaticCall));
			verify(optblk.BranchBlock == orig.jdyn_target);
		}
	}
	else
	{
		verify(!opt.has_jdyn);
		verify(optblk.BlockType == orig_type);
	}
}

// ---- directed tests ---------------------------------------------------------

static const u32 BLK = 0x8C010000; // page-aligned block address for tests
static u32 opt_run_count, bake_count, promote_count; // fuzz sanity counters

static RuntimeBlockInfo mkblk(u32 addr = BLK, u32 code_size = 64)
{
	RuntimeBlockInfo b;
	b.addr = addr;
	b.sh4_code_size = code_size;
	b.BlockType = BET_StaticJump;
	return b;
}

static void wr_ram32(u32 addr, u32 v) { memcpy(&mock_ram[addr & RAM_MASK], &v, 4); }

// literal on the block's own page, right after the "code"
static const u32 LIT = BLK + 64 + 4;
static const u32 LITVAL = 0x8C0F00C0;

static void t_bake_and_jdyn_promote()
{
	mock_reset();
	wr_ram32(LIT, LITVAL);

	RuntimeBlockInfo b = mkblk();
	b.BlockType = BET_DynamicJump;
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(LIT)));
	b.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r1), P(), 4));
	b.oplist.push_back(OP(shop_jdyn, R(reg_pc_dyn), R(reg_r0)));

	constprop(&b);
	check_arm32_forms(b.oplist);

	verify(b.BlockType == BET_StaticJump);
	verify(b.BranchBlock == LITVAL);
	verify(b.static_from_constprop); // must keep the backwards-only cycle check
	verify(count_op(b.oplist, shop_readm) == 0);
	verify(count_op(b.oplist, shop_jdyn) == 0);
	// end-of-block writebacks keep the guest state architecturally correct
	verify(find_mov_imm(b.oplist, reg_r1, LIT) >= 0);
	verify(find_mov_imm(b.oplist, reg_r0, LITVAL) >= 0);
	verify(mock_bad_reads == 0);
}

static void t_rd2_kill()
{
	// the old pass killed rd instead of rd2 -- a div32u writing rd2=r3 must
	// invalidate r3's tracked constant
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r3), I(5)));
	b.oplist.push_back(OP(shop_div32u, R(reg_r2), R(reg_r2), R(reg_r4), 0, P(), R(reg_r3)));
	b.oplist.push_back(OP(shop_add, R(reg_r5), R(reg_r3), I(1)));

	constprop(&b);
	check_arm32_forms(b.oplist);

	const shil_opcode* add = find_op(b.oplist, shop_add);
	verify(add != nullptr);              // not folded away
	verify(add->rs1.is_r32i() && add->rs1._reg == reg_r3);
	verify(find_mov_imm(b.oplist, reg_r5, 6) < 0); // no bogus fold of 5+1
}

static void t_no_bake_when_page_has_data()
{
	mock_reset();
	wr_ram32(LIT, LITVAL);
	mock_page_has_data[(BLK & RAM_MASK) / PAGE_SIZE] = true;

	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(LIT)));
	b.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r1), P(), 4));

	constprop(&b);
	check_arm32_forms(b.oplist);

	// address still becomes immediate, but the value must not be inlined
	const shil_opcode* rm = find_op(b.oplist, shop_readm);
	verify(rm != nullptr);
	verify(rm->rs1.is_imm() && rm->rs1._imm == LIT);
	verify(find_mov_imm(b.oplist, reg_r0, LITVAL) < 0);
}

static void t_no_bake_off_page()
{
	mock_reset();
	u32 far_lit = 0x8C200000;
	wr_ram32(far_lit, 0x12345678);

	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(far_lit)));
	b.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r1), P(), 4));

	constprop(&b);
	check_arm32_forms(b.oplist);

	const shil_opcode* rm = find_op(b.oplist, shop_readm);
	verify(rm != nullptr);
	verify(rm->rs1.is_imm() && rm->rs1._imm == far_lit);
	verify(find_mov_imm(b.oplist, reg_r0, 0x12345678) < 0);
}

static void t_no_bake_after_unknown_store()
{
	mock_reset();
	wr_ram32(LIT, LITVAL);
	wr_ram32(LIT + 4, 0x0BADF00D);

	RuntimeBlockInfo b = mkblk();
	// r8 holds an unknown address: this store may hit our own page
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(LIT)));
	b.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r1), P(), 4)); // before: baked
	b.oplist.push_back(OP(shop_writem, P(), R(reg_r8), R(reg_r2), 4));
	b.oplist.push_back(OP(shop_readm, R(reg_r3), R(reg_r1), P(), 4, I(4))); // after: not baked

	constprop(&b);
	check_arm32_forms(b.oplist);

	verify(count_op(b.oplist, shop_readm) == 1); // only the second survives
	const shil_opcode* rm = find_op(b.oplist, shop_readm);
	verify(rm->rs1.is_imm() && rm->rs1._imm == LIT + 4); // still promoted
	verify(find_mov_imm(b.oplist, reg_r3, 0x0BADF00D) < 0);
	verify(find_mov_imm(b.oplist, reg_r0, LITVAL) >= 0); // first one was baked
}

static void t_safe_store_keeps_baking()
{
	mock_reset();
	wr_ram32(LIT, LITVAL);

	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(LIT)));
	b.oplist.push_back(OP(shop_mov32, R(reg_r8), I(0x8C400000))); // provably off-page
	b.oplist.push_back(OP(shop_writem, P(), R(reg_r8), R(reg_r2), 4));
	b.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r1), P(), 4));

	constprop(&b);
	check_arm32_forms(b.oplist);

	verify(count_op(b.oplist, shop_readm) == 0); // still baked
	verify(find_mov_imm(b.oplist, reg_r0, LITVAL) >= 0);
	// and the store's address register was materialized before the store
	const shil_opcode* wm = find_op(b.oplist, shop_writem);
	verify(wm != nullptr && wm->rs1.is_r32i() && wm->rs1._reg == reg_r8);
	int mv = find_mov_imm(b.oplist, reg_r8, 0x8C400000);
	verify(mv >= 0 && &b.oplist[mv] < wm);
}

static void t_writem_forms()
{
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(0x8C300000)));
	b.oplist.push_back(OP(shop_mov32, R(reg_r0), I(8)));
	// const base, const reg offset, const data: everything is known, but
	// the store must keep register operands (backend requirement)
	b.oplist.push_back(OP(shop_mov32, R(reg_r2), I(0xCAFE0000)));
	b.oplist.push_back(OP(shop_writem, P(), R(reg_r1), R(reg_r2), 4, R(reg_r0)));

	constprop(&b);
	check_arm32_forms(b.oplist);

	const shil_opcode* wm = find_op(b.oplist, shop_writem);
	verify(wm != nullptr);
	verify(wm->rs1.is_r32i());
	verify(wm->rs2.is_r32i());
	verify(wm->rs3.is_imm() && wm->rs3._imm == 8); // reg offset folded to imm
	// base and data materialized before the store
	int m1 = find_mov_imm(b.oplist, reg_r1, 0x8C300000);
	int m2 = find_mov_imm(b.oplist, reg_r2, 0xCAFE0000);
	verify(m1 >= 0 && &b.oplist[m1] < wm);
	verify(m2 >= 0 && &b.oplist[m2] < wm);
}

static void t_shld_shad()
{
	struct Case { shilop in; s32 amount; shilop out; u32 imm; };
	static const Case cases[] = {
		{ shop_shld,   5, shop_shl,  5 },
		{ shop_shld,  -5, shop_shr,  5 },
		{ shop_shad,   9, shop_shl,  9 },
		{ shop_shad,  -9, shop_sar,  9 },
		{ shop_shad, -32, shop_sar, 31 },
	};
	for (auto& c : cases)
	{
		mock_reset();
		RuntimeBlockInfo b = mkblk();
		b.oplist.push_back(OP(shop_mov32, R(reg_r1), I((u32)c.amount)));
		b.oplist.push_back(OP(c.in, R(reg_r0), R(reg_r4), R(reg_r1))); // r4 unknown
		constprop(&b);
		check_arm32_forms(b.oplist);
		const shil_opcode* s = find_op(b.oplist, c.out);
		verify(s != nullptr);
		verify(s->rs2.is_imm() && s->rs2._imm == c.imm);
		verify(count_op(b.oplist, c.in) == 0);
	}

	// shld by -32 yields zero regardless of the unknown source
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I((u32)-32)));
	b.oplist.push_back(OP(shop_shld, R(reg_r0), R(reg_r4), R(reg_r1)));
	constprop(&b);
	check_arm32_forms(b.oplist);
	verify(count_op(b.oplist, shop_shld) == 0);
	verify(find_mov_imm(b.oplist, reg_r0, 0) >= 0);
}

static void t_sync_sr()
{
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(5)));   // banked
	b.oplist.push_back(OP(shop_mov32, R(reg_r12), I(7)));  // not banked
	b.oplist.push_back(OP(shop_sync_sr, P(), P()));
	b.oplist.push_back(OP(shop_add, R(reg_r2), R(reg_r1), I(1)));   // must not fold
	b.oplist.push_back(OP(shop_add, R(reg_r13), R(reg_r12), I(1))); // may fold

	constprop(&b);
	check_arm32_forms(b.oplist);

	const shil_opcode* sync = find_op(b.oplist, shop_sync_sr);
	verify(sync != nullptr);
	int m1 = find_mov_imm(b.oplist, reg_r1, 5);
	verify(m1 >= 0 && &b.oplist[m1] < sync); // r1 written back before the sync
	const shil_opcode* add = find_op(b.oplist, shop_add);
	verify(add != nullptr && add->rs1.is_r32i() && add->rs1._reg == reg_r1);
	verify(count_op(b.oplist, shop_add) == 1);        // r12+1 folded
	verify(find_mov_imm(b.oplist, reg_r13, 8) >= 0);
}

static void t_commutative_swap()
{
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r0), I(5)));
	b.oplist.push_back(OP(shop_add, R(reg_r1), R(reg_r0), R(reg_r2)));
	b.oplist.push_back(OP(shop_sub, R(reg_r3), R(reg_r0), R(reg_r2)));

	constprop(&b);
	check_arm32_forms(b.oplist);

	const shil_opcode* add = find_op(b.oplist, shop_add);
	verify(add != nullptr);
	verify(add->rs1.is_r32i() && add->rs1._reg == reg_r2);
	verify(add->rs2.is_imm() && add->rs2._imm == 5);

	// sub is not commutative: r0 must be materialized and stay a register
	const shil_opcode* sub = find_op(b.oplist, shop_sub);
	verify(sub != nullptr);
	verify(sub->rs1.is_r32i() && sub->rs1._reg == reg_r0);
	int mv = find_mov_imm(b.oplist, reg_r0, 5);
	verify(mv >= 0 && &b.oplist[mv] < sub);
}

static void t_writeback_dedup()
{
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r0), I(5)));
	b.oplist.push_back(OP(shop_writem, P(), R(reg_r8), R(reg_r0), 4));
	b.oplist.push_back(OP(shop_writem, P(), R(reg_r9), R(reg_r0), 4));

	constprop(&b);
	check_arm32_forms(b.oplist);

	int movs = 0;
	for (auto& o : b.oplist)
		if (o.op == shop_mov32 && o.rd.is_r32i() && o.rd._reg == reg_r0)
			movs++;
	verify(movs == 1);
}

static void t_mmio_promote()
{
	// promotion keeps the load at runtime, so it's fine for mmio too (the
	// backend calls the region handler with its registered context) -- but
	// the value must never be read at compile time, and 8-bit reads must
	// keep the register form (no 8-bit imm load on arm32)
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(0xA05F8000))); // pvr regs
	b.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r1), P(), 4));

	constprop(&b);
	check_arm32_forms(b.oplist);

	const shil_opcode* rm = find_op(b.oplist, shop_readm);
	verify(rm != nullptr && rm->rs1.is_imm() && rm->rs1._imm == 0xA05F8000);
	verify(mock_bad_reads == 0); // never read mmio at compile time!

	mock_reset();
	RuntimeBlockInfo b2 = mkblk();
	b2.oplist.push_back(OP(shop_mov32, R(reg_r1), I(0xA05F8000)));
	b2.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r1), P(), 1));

	constprop(&b2);
	check_arm32_forms(b2.oplist);

	rm = find_op(b2.oplist, shop_readm);
	verify(rm != nullptr && rm->rs1.is_r32i() && rm->rs1._reg == reg_r1);
	int mv = find_mov_imm(b2.oplist, reg_r1, 0xA05F8000);
	verify(mv >= 0 && &b2.oplist[mv] < rm);
	verify(mock_bad_reads == 0);
}

static void t_pref()
{
	mock_reset();
	wr_ram32(LIT, LITVAL);

	// provably not a store queue address: pref is a nop and is dropped
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(0x8C001000)));
	b.oplist.push_back(OP(shop_pref, P(), R(reg_r1)));
	constprop(&b);
	check_arm32_forms(b.oplist);
	verify(count_op(b.oplist, shop_pref) == 0);

	// store queue: kept, address materialized, and baking disabled after it
	mock_reset();
	wr_ram32(LIT, LITVAL);
	RuntimeBlockInfo b2 = mkblk();
	b2.oplist.push_back(OP(shop_mov32, R(reg_r1), I(0xE0000000)));
	b2.oplist.push_back(OP(shop_pref, P(), R(reg_r1)));
	b2.oplist.push_back(OP(shop_mov32, R(reg_r2), I(LIT)));
	b2.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r2), P(), 4));
	constprop(&b2);
	check_arm32_forms(b2.oplist);
	const shil_opcode* pf = find_op(b2.oplist, shop_pref);
	verify(pf != nullptr && pf->rs1.is_r32i() && pf->rs1._reg == reg_r1);
	int mv = find_mov_imm(b2.oplist, reg_r1, 0xE0000000);
	verify(mv >= 0 && &b2.oplist[mv] < pf);
	const shil_opcode* rm = find_op(b2.oplist, shop_readm);
	verify(rm != nullptr); // not baked
	verify(rm->rs1.is_imm() && rm->rs1._imm == LIT); // but still promoted
}

static void t_readm_sizes()
{
	// 64-bit read: no imm form on arm32, base register must survive
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(0x8C300000)));
	b.oplist.push_back(OP(shop_readm, D(reg_fr_0), R(reg_r1), P(), 8));
	constprop(&b);
	check_arm32_forms(b.oplist);
	const shil_opcode* rm = find_op(b.oplist, shop_readm);
	verify(rm != nullptr && rm->rs1.is_r32i() && rm->rs1._reg == reg_r1);
	int mv = find_mov_imm(b.oplist, reg_r1, 0x8C300000);
	verify(mv >= 0 && &b.oplist[mv] < rm);

	// 8-bit ram read off-page: neither baked nor promoted (backend limitation)
	mock_reset();
	RuntimeBlockInfo b2 = mkblk();
	b2.oplist.push_back(OP(shop_mov32, R(reg_r1), I(0x8C300000)));
	b2.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r1), P(), 1));
	constprop(&b2);
	check_arm32_forms(b2.oplist);
	rm = find_op(b2.oplist, shop_readm);
	verify(rm != nullptr && rm->rs1.is_r32i());

	// 8-bit read from the block's own page: baked, sign-extended
	mock_reset();
	mock_ram[LIT & RAM_MASK] = 0x80;
	RuntimeBlockInfo b3 = mkblk();
	b3.oplist.push_back(OP(shop_mov32, R(reg_r1), I(LIT)));
	b3.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r1), P(), 1));
	constprop(&b3);
	check_arm32_forms(b3.oplist);
	verify(count_op(b3.oplist, shop_readm) == 0);
	verify(find_mov_imm(b3.oplist, reg_r0, 0xFFFFFF80) >= 0);
}

static void t_cmp_fold()
{
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r0), I(5)));
	b.oplist.push_back(OP(shop_test, R(reg_sr_T), R(reg_r0), I(4)));
	constprop(&b);
	check_arm32_forms(b.oplist);
	verify(count_op(b.oplist, shop_test) == 0);
	verify(find_mov_imm(b.oplist, reg_sr_T, 0) >= 0); // 5&4 != 0 -> T=0
}

static void t_jdyn_offset()
{
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.BlockType = BET_DynamicCall;
	b.oplist.push_back(OP(shop_mov32, R(reg_r0), I(0x8C0100F0)));
	b.oplist.push_back(OP(shop_jdyn, R(reg_pc_dyn), R(reg_r0), I(8)));
	constprop(&b);
	check_arm32_forms(b.oplist);
	verify(b.BlockType == BET_StaticCall);
	verify(b.BranchBlock == 0x8C0100F8);
	verify(b.static_from_constprop);
	verify(count_op(b.oplist, shop_jdyn) == 0);
}

static void t_f32_mov()
{
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r0), I(0x3F800000)));
	b.oplist.push_back(OP(shop_mov32, F(reg_fr_0), R(reg_r0)));
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(0x40490FDB)));
	b.oplist.push_back(OP(shop_mov32, F(reg_fr_1), R(reg_r1)));
	constprop(&b);
	check_arm32_forms(b.oplist);
	for (auto& o : b.oplist)
	{
		if (o.op != shop_mov32 || !o.rd.is_r32f()) continue;
		if (o.rd._reg == reg_fr_0)
			verify(o.rs1.is_imm() && o.rs1._imm == 0x3F800000);
		if (o.rd._reg == reg_fr_1)
			verify(o.rs1.is_r32i() && o.rs1._reg == reg_r1); // arbitrary float: keep the reg
	}
}

static void t_shift_chains()
{
	// shll16; shll8; shll2; shll -> one shl #27
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	static const u32 amounts[] = {16, 8, 2, 1};
	for (u32 a : amounts)
		b.oplist.push_back(OP(shop_shl, R(reg_r4), R(reg_r4), I(a)));
	constprop(&b);
	check_arm32_forms(b.oplist);
	verify(count_op(b.oplist, shop_shl) == 1);
	verify(find_op(b.oplist, shop_shl)->rs2._imm == 27);

	// shll16 twice: everything shifted out, result is a constant zero
	mock_reset();
	RuntimeBlockInfo b2 = mkblk();
	b2.oplist.push_back(OP(shop_shl, R(reg_r4), R(reg_r4), I(16)));
	b2.oplist.push_back(OP(shop_shl, R(reg_r4), R(reg_r4), I(16)));
	constprop(&b2);
	check_arm32_forms(b2.oplist);
	verify(count_op(b2.oplist, shop_shl) == 0);
	verify(find_mov_imm(b2.oplist, reg_r4, 0) >= 0);

	// arithmetic shifts saturate at 31 (sign fill)
	mock_reset();
	RuntimeBlockInfo b3 = mkblk();
	b3.oplist.push_back(OP(shop_sar, R(reg_r4), R(reg_r4), I(20)));
	b3.oplist.push_back(OP(shop_sar, R(reg_r4), R(reg_r4), I(20)));
	constprop(&b3);
	check_arm32_forms(b3.oplist);
	verify(count_op(b3.oplist, shop_sar) == 1);
	verify(find_op(b3.oplist, shop_sar)->rs2._imm == 31);

	// a full rotation disappears entirely
	mock_reset();
	RuntimeBlockInfo b4 = mkblk();
	b4.oplist.push_back(OP(shop_ror, R(reg_r4), R(reg_r4), I(16)));
	b4.oplist.push_back(OP(shop_ror, R(reg_r4), R(reg_r4), I(16)));
	constprop(&b4);
	check_arm32_forms(b4.oplist);
	verify(count_op(b4.oplist, shop_ror) == 0);

	// an op in between reads the intermediate value: no merge
	mock_reset();
	RuntimeBlockInfo b5 = mkblk();
	b5.oplist.push_back(OP(shop_shl, R(reg_r4), R(reg_r4), I(16)));
	b5.oplist.push_back(OP(shop_add, R(reg_r5), R(reg_r4), I(1)));
	b5.oplist.push_back(OP(shop_shl, R(reg_r4), R(reg_r4), I(8)));
	constprop(&b5);
	check_arm32_forms(b5.oplist);
	verify(count_op(b5.oplist, shop_shl) == 2);

	// mixed shift kinds never merge (shl16;shr16 is a zero-extend!)
	mock_reset();
	RuntimeBlockInfo b6 = mkblk();
	b6.oplist.push_back(OP(shop_shl, R(reg_r4), R(reg_r4), I(16)));
	b6.oplist.push_back(OP(shop_shr, R(reg_r4), R(reg_r4), I(16)));
	constprop(&b6);
	check_arm32_forms(b6.oplist);
	verify(count_op(b6.oplist, shop_shl) == 1 && count_op(b6.oplist, shop_shr) == 1);
}

static void t_dse()
{
	// overwritten pure values die; the final write survives (live-out)
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_add, R(reg_r0), R(reg_r1), R(reg_r2)));
	b.oplist.push_back(OP(shop_add, R(reg_r0), R(reg_r3), R(reg_r4)));
	b.oplist.push_back(OP(shop_test, R(reg_sr_T), R(reg_r1), R(reg_r2)));
	b.oplist.push_back(OP(shop_test, R(reg_sr_T), R(reg_r3), R(reg_r4)));
	dead_value_elim(&b);
	verify(b.oplist.size() == 2);
	verify(b.oplist[0].rs1._reg == reg_r3);
	verify(b.oplist[1].rs1._reg == reg_r3);

	// a readm is never removed, but its write still kills the value before it
	mock_reset();
	RuntimeBlockInfo b2 = mkblk();
	b2.oplist.push_back(OP(shop_mov32, R(reg_r0), R(reg_r5)));
	b2.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r8), P(), 4));
	dead_value_elim(&b2);
	verify(b2.oplist.size() == 1);
	verify(b2.oplist[0].op == shop_readm);

	// a barrier may read anything: both writes stay
	mock_reset();
	RuntimeBlockInfo b3 = mkblk();
	b3.oplist.push_back(OP(shop_add, R(reg_r0), R(reg_r1), R(reg_r2)));
	b3.oplist.push_back(OP(shop_ifb, P(), I(0), I(0x8C000000), 0, I(0)));
	b3.oplist.push_back(OP(shop_add, R(reg_r0), R(reg_r3), R(reg_r4)));
	dead_value_elim(&b3);
	verify(count_op(b3.oplist, shop_add) == 2);

	// two-dest op dies only when both results are dead
	mock_reset();
	RuntimeBlockInfo b4 = mkblk();
	b4.oplist.push_back(OP(shop_mul_u64, R(reg_macl), R(reg_r1), R(reg_r2), 0, P(), R(reg_mach)));
	b4.oplist.push_back(OP(shop_mov32, R(reg_macl), R(reg_r3)));
	dead_value_elim(&b4);
	verify(count_op(b4.oplist, shop_mul_u64) == 1); // mach still live

	mock_reset();
	RuntimeBlockInfo b5 = mkblk();
	b5.oplist.push_back(OP(shop_mul_u64, R(reg_macl), R(reg_r1), R(reg_r2), 0, P(), R(reg_mach)));
	b5.oplist.push_back(OP(shop_mov32, R(reg_macl), R(reg_r3)));
	b5.oplist.push_back(OP(shop_mov32, R(reg_mach), R(reg_r4)));
	dead_value_elim(&b5);
	verify(count_op(b5.oplist, shop_mul_u64) == 0); // both dead now

	// a dead value chain collapses: the consumer dies first, then its feeder
	mock_reset();
	RuntimeBlockInfo b6 = mkblk();
	b6.oplist.push_back(OP(shop_add, R(reg_r0), R(reg_r1), R(reg_r2)));
	b6.oplist.push_back(OP(shop_shl, R(reg_r0), R(reg_r0), I(4)));
	b6.oplist.push_back(OP(shop_mov32, R(reg_r0), R(reg_r3)));
	dead_value_elim(&b6);
	verify(b6.oplist.size() == 1);
	verify(b6.oplist[0].op == shop_mov32);
}

static shil_opcode mk_ftrv(Sh4RegType fv)
{
	return OP(shop_ftrv, V(fv, FMT_V4), V(fv, FMT_V4), V(reg_xf_0, FMT_V16));
}

static void t_ftrv_known_w()
{
	// fldi1 into w right before the transform: flag + load removed
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, F(reg_fr_3), I(0x3F800000)));
	b.oplist.push_back(mk_ftrv(reg_fr_0));
	ftrv_known_w(&b);
	verify(count_op(b.oplist, shop_mov32) == 0);
	verify(find_op(b.oplist, shop_ftrv)->flags == FTRV_W_ONE);

	// fldi0, with unrelated integer ops in between: still caught
	mock_reset();
	RuntimeBlockInfo b2 = mkblk();
	b2.oplist.push_back(OP(shop_mov32, F(reg_fr_3), I(0)));
	b2.oplist.push_back(OP(shop_add, R(reg_r0), R(reg_r1), I(4)));
	b2.oplist.push_back(mk_ftrv(reg_fr_0));
	ftrv_known_w(&b2);
	verify(count_op(b2.oplist, shop_mov32) == 0);
	verify(find_op(b2.oplist, shop_ftrv)->flags == FTRV_W_ZERO);

	// a reader in between: the load must survive, no flag
	mock_reset();
	RuntimeBlockInfo b3 = mkblk();
	b3.oplist.push_back(OP(shop_mov32, F(reg_fr_3), I(0x3F800000)));
	b3.oplist.push_back(OP(shop_fadd, F(reg_fr_5), F(reg_fr_3), F(reg_fr_6)));
	b3.oplist.push_back(mk_ftrv(reg_fr_0));
	ftrv_known_w(&b3);
	verify(count_op(b3.oplist, shop_mov32) == 1);
	verify(find_op(b3.oplist, shop_ftrv)->flags == 0);

	// non-constant w write in between: no flag
	mock_reset();
	RuntimeBlockInfo b4 = mkblk();
	b4.oplist.push_back(OP(shop_mov32, F(reg_fr_3), I(0x3F800000)));
	b4.oplist.push_back(OP(shop_fadd, F(reg_fr_3), F(reg_fr_5), F(reg_fr_6)));
	b4.oplist.push_back(mk_ftrv(reg_fr_0));
	ftrv_known_w(&b4);
	verify(find_op(b4.oplist, shop_ftrv)->flags == 0);

	// arbitrary float constant: not 0/1, no flag
	mock_reset();
	RuntimeBlockInfo b5 = mkblk();
	b5.oplist.push_back(OP(shop_mov32, F(reg_fr_3), I(0x40490FDB)));
	b5.oplist.push_back(mk_ftrv(reg_fr_0));
	ftrv_known_w(&b5);
	verify(count_op(b5.oplist, shop_mov32) == 1);
	verify(find_op(b5.oplist, shop_ftrv)->flags == 0);

	// fp barrier in between: conservative, no flag
	mock_reset();
	RuntimeBlockInfo b6 = mkblk();
	b6.oplist.push_back(OP(shop_mov32, F(reg_fr_3), I(0x3F800000)));
	b6.oplist.push_back(OP(shop_sync_fpscr, P(), P()));
	b6.oplist.push_back(mk_ftrv(reg_fr_0));
	ftrv_known_w(&b6);
	verify(count_op(b6.oplist, shop_mov32) == 1);
	verify(find_op(b6.oplist, shop_ftrv)->flags == 0);

	// back-to-back transforms of the same vector: only the first sees the
	// constant; the second reads the transformed w
	mock_reset();
	RuntimeBlockInfo b7 = mkblk();
	b7.oplist.push_back(OP(shop_mov32, F(reg_fr_3), I(0x3F800000)));
	b7.oplist.push_back(mk_ftrv(reg_fr_0));
	b7.oplist.push_back(mk_ftrv(reg_fr_0));
	ftrv_known_w(&b7);
	verify(count_op(b7.oplist, shop_mov32) == 0);
	verify(b7.oplist[0].flags == FTRV_W_ONE);
	verify(b7.oplist[1].flags == 0);

	// through constprop: fldi via a known-const gpr becomes an imm first
	mock_reset();
	RuntimeBlockInfo b8 = mkblk();
	b8.oplist.push_back(OP(shop_mov32, R(reg_r0), I(0x3F800000)));
	b8.oplist.push_back(OP(shop_mov32, F(reg_fr_3), R(reg_r0)));
	b8.oplist.push_back(mk_ftrv(reg_fr_0));
	constprop(&b8);
	ftrv_known_w(&b8);
	verify(find_op(b8.oplist, shop_ftrv)->flags == FTRV_W_ONE);
}

static void t_readm_pair_fusion()
{
	// fmov.s @Rn+ x3 (a vertex fetch): first two loads pair up, their +4s
	// combine, the odd load stays single
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	static const Sh4RegType frs[] = {reg_fr_0, reg_fr_1, reg_fr_2};
	for (auto fr : frs)
	{
		b.oplist.push_back(OP(shop_readm, F(fr), R(reg_r8), P(), 4));
		b.oplist.push_back(OP(shop_add, R(reg_r8), R(reg_r8), I(4)));
	}
	fuse_readm_pairs(&b);
	check_arm32_forms(b.oplist);

	verify(count_op(b.oplist, shop_readm) == 2);
	verify(b.oplist[0].op == shop_readm);
	verify(b.oplist[0].rd._reg == reg_fr_0 && b.oplist[0].rd2._reg == reg_fr_1);
	verify(b.oplist[1].op == shop_add && b.oplist[1].rs2._imm == 8);
	verify(b.oplist[2].op == shop_readm && b.oplist[2].rd2.is_null());
	verify(b.oplist[3].op == shop_add && b.oplist[3].rs2._imm == 4);

	// four loads: two pairs, two combined adds
	mock_reset();
	RuntimeBlockInfo b2 = mkblk();
	static const Sh4RegType frs4[] = {reg_fr_0, reg_fr_1, reg_fr_2, reg_fr_3};
	for (auto fr : frs4)
	{
		b2.oplist.push_back(OP(shop_readm, F(fr), R(reg_r8), P(), 4));
		b2.oplist.push_back(OP(shop_add, R(reg_r8), R(reg_r8), I(4)));
	}
	fuse_readm_pairs(&b2);
	check_arm32_forms(b2.oplist);
	verify(count_op(b2.oplist, shop_readm) == 2);
	verify(count_op(b2.oplist, shop_add) == 2);

	// guards: different base, wrong add amount, i32 load, op in between
	mock_reset();
	RuntimeBlockInfo b3 = mkblk();
	b3.oplist.push_back(OP(shop_readm, F(reg_fr_0), R(reg_r8), P(), 4));
	b3.oplist.push_back(OP(shop_add, R(reg_r8), R(reg_r8), I(4)));
	b3.oplist.push_back(OP(shop_readm, F(reg_fr_1), R(reg_r9), P(), 4)); // other base
	b3.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r8), P(), 4));  // i32
	b3.oplist.push_back(OP(shop_add, R(reg_r8), R(reg_r8), I(8)));      // not +4
	b3.oplist.push_back(OP(shop_readm, F(reg_fr_2), R(reg_r8), P(), 4));
	fuse_readm_pairs(&b3);
	check_arm32_forms(b3.oplist);
	for (auto& o : b3.oplist)
		verify(!(o.op == shop_readm) || o.rd2.is_null());

	// fmov.d @Rn+ x2 (a matrix/vertex fetch) fuses the same way, on +8
	mock_reset();
	RuntimeBlockInfo b4 = mkblk();
	static const Sh4RegType drs[] = {reg_fr_0, reg_fr_2, reg_fr_4};
	for (auto dr : drs)
	{
		b4.oplist.push_back(OP(shop_readm, D(dr), R(reg_r8), P(), 8));
		b4.oplist.push_back(OP(shop_add, R(reg_r8), R(reg_r8), I(8)));
	}
	fuse_readm_pairs(&b4);
	check_arm32_forms(b4.oplist);

	verify(count_op(b4.oplist, shop_readm) == 2);
	verify(b4.oplist[0].rd._reg == reg_fr_0 && b4.oplist[0].rd2._reg == reg_fr_2);
	verify(b4.oplist[1].op == shop_add && b4.oplist[1].rs2._imm == 16);
	verify(b4.oplist[2].op == shop_readm && b4.oplist[2].rd2.is_null());

	// guards: a +4 between doubles, and a pair of different sizes
	mock_reset();
	RuntimeBlockInfo b5 = mkblk();
	b5.oplist.push_back(OP(shop_readm, D(reg_fr_0), R(reg_r8), P(), 8));
	b5.oplist.push_back(OP(shop_add, R(reg_r8), R(reg_r8), I(4)));   // not +8
	b5.oplist.push_back(OP(shop_readm, D(reg_fr_2), R(reg_r8), P(), 8));
	b5.oplist.push_back(OP(shop_add, R(reg_r8), R(reg_r8), I(8)));
	b5.oplist.push_back(OP(shop_readm, F(reg_fr_4), R(reg_r8), P(), 4)); // size mismatch
	fuse_readm_pairs(&b5);
	check_arm32_forms(b5.oplist);
	for (auto& o : b5.oplist)
		verify(!(o.op == shop_readm) || o.rd2.is_null());

	// the same double twice is left alone
	mock_reset();
	RuntimeBlockInfo b6 = mkblk();
	b6.oplist.push_back(OP(shop_readm, D(reg_fr_0), R(reg_r8), P(), 8));
	b6.oplist.push_back(OP(shop_add, R(reg_r8), R(reg_r8), I(8)));
	b6.oplist.push_back(OP(shop_readm, D(reg_fr_0), R(reg_r8), P(), 8));
	fuse_readm_pairs(&b6);
	check_arm32_forms(b6.oplist);
	verify(count_op(b6.oplist, shop_readm) == 2);
}

// writes 4 bytes at a time through `base`, then prefs it -- the store queue shape
static void mk_sq_writes(RuntimeBlockInfo* b, Sh4RegType base, int n)
{
	for (int i = 0; i < n; i++)
		b->oplist.push_back(OP(shop_writem, P(), R(base), R(reg_r1), 4));
}

static int sq_marked_writes(const RuntimeBlockInfo& b)
{
	int n = 0;
	for (auto& o : b.oplist)
		if (o.op == shop_writem && o.flags2 == 0x1337)
			n++;
	return n;
}

static void t_sq_pref()
{
	// the store queue shape: 32 bytes through r4, then pref @r4. (The scan
	// stops before op 0, so the block starts with the address setup anyway.)
	mock_reset();
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r4), I(0xE0000000)));
	mk_sq_writes(&b, reg_r4, 8);
	b.oplist.push_back(OP(shop_pref, P(), R(reg_r4)));
	sq_pref(&b);
	verify(b.oplist.back().flags == 0x1337);
	verify(sq_marked_writes(b) == 8);

	// the same, but something loads through r4: the store queue is write only,
	// so this is ordinary memory and the writes must stay ordinary writes
	mock_reset();
	RuntimeBlockInfo b2 = mkblk();
	b2.oplist.push_back(OP(shop_mov32, R(reg_r4), I(0xE0000000)));
	mk_sq_writes(&b2, reg_r4, 8);
	b2.oplist.push_back(OP(shop_readm, R(reg_r2), R(reg_r4), P(), 4));
	b2.oplist.push_back(OP(shop_pref, P(), R(reg_r4)));
	sq_pref(&b2);
	verify(b2.oplist.back().flags != 0x1337);
	verify(sq_marked_writes(b2) == 0);

	// a read of any size counts, and one before the writes counts too
	for (u32 sz = 1; sz <= 8; sz *= 2)
	{
		mock_reset();
		RuntimeBlockInfo b3 = mkblk();
		b3.oplist.push_back(OP(shop_mov32, R(reg_r4), I(0xE0000000)));
		b3.oplist.push_back(OP(shop_readm, R(reg_r2), R(reg_r4), P(), sz));
		mk_sq_writes(&b3, reg_r4, 8);
		b3.oplist.push_back(OP(shop_pref, P(), R(reg_r4)));
		sq_pref(&b3);
		verify(b3.oplist.back().flags != 0x1337);
		verify(sq_marked_writes(b3) == 0);
	}

	// a read through some other register leaves the match alone
	mock_reset();
	RuntimeBlockInfo b4 = mkblk();
	b4.oplist.push_back(OP(shop_mov32, R(reg_r4), I(0xE0000000)));
	mk_sq_writes(&b4, reg_r4, 8);
	b4.oplist.push_back(OP(shop_readm, R(reg_r2), R(reg_r5), P(), 4));
	b4.oplist.push_back(OP(shop_pref, P(), R(reg_r4)));
	sq_pref(&b4);
	verify(b4.oplist.back().flags == 0x1337);
	verify(sq_marked_writes(b4) == 8);

	// too little written to look like a store queue fill
	mock_reset();
	RuntimeBlockInfo b5 = mkblk();
	b5.oplist.push_back(OP(shop_mov32, R(reg_r4), I(0xE0000000)));
	mk_sq_writes(&b5, reg_r4, 1);
	b5.oplist.push_back(OP(shop_pref, P(), R(reg_r4)));
	sq_pref(&b5);
	verify(b5.oplist.back().flags != 0x1337);
	verify(sq_marked_writes(b5) == 0);
}

static void t_imm_literal_bake()
{
	// pc-relative literals arrive from the decoder with an immediate base;
	// they must bake exactly like const-register bases
	mock_reset();
	wr_ram32(LIT, LITVAL);
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_readm, R(reg_r0), I(LIT), P(), 4));
	constprop(&b);
	check_arm32_forms(b.oplist);
	verify(count_op(b.oplist, shop_readm) == 0);
	verify(find_mov_imm(b.oplist, reg_r0, LITVAL) >= 0);
	verify(mock_bad_reads == 0);
}

static void t_rom_bake()
{
	// the boot rom is immutable: reads bake regardless of page locks,
	// block location or preceding stores
	mock_reset();
	u32 rom_lit = 0x80000400; // p1-mapped boot rom
	u32 val = 0x12345678;
	memcpy(&mock_rom[0x400], &val, 4);

	RuntimeBlockInfo b = mkblk();
	mock_page_has_data[(BLK & RAM_MASK) / PAGE_SIZE] = true; // ram baking off
	b.oplist.push_back(OP(shop_writem, P(), R(reg_r8), R(reg_r2), 4)); // unknown store
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(rom_lit)));
	b.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r1), P(), 4));
	b.oplist.push_back(OP(shop_readm, R(reg_r3), I(rom_lit), P(), 4)); // decoder form
	constprop(&b);
	check_arm32_forms(b.oplist);
	verify(count_op(b.oplist, shop_readm) == 0);
	verify(find_mov_imm(b.oplist, reg_r0, val) >= 0);
	verify(find_mov_imm(b.oplist, reg_r3, val) >= 0);
	verify(mock_bad_reads == 0);
}

static void t_baking_disabled()
{
	// safemode: pure constprop still runs, memory is never read at compile time
	mock_reset();
	wr_ram32(LIT, LITVAL);
	RuntimeBlockInfo b = mkblk();
	b.oplist.push_back(OP(shop_mov32, R(reg_r1), I(LIT)));
	b.oplist.push_back(OP(shop_readm, R(reg_r0), R(reg_r1), P(), 4));
	constprop(&b, false);
	check_arm32_forms(b.oplist);
	const shil_opcode* rm = find_op(b.oplist, shop_readm);
	verify(rm != nullptr);
	verify(rm->rs1.is_imm() && rm->rs1._imm == LIT); // promotion is still fine
	verify(find_mov_imm(b.oplist, reg_r0, LITVAL) < 0);
}

// ---- fuzzer -----------------------------------------------------------------

struct Fuzz
{
	std::mt19937 rng;
	explicit Fuzz(u32 seed) : rng(seed) {}
	u32 rnd(u32 n) { return rng() % n; }
	u32 r32() { return rng(); }

	// register pools: r7 is a small preset offset, r8-r11 hold only addresses
	Sh4RegType gp() { static const Sh4RegType p[] = {reg_r0,reg_r1,reg_r2,reg_r3,reg_r4,reg_r5,reg_r6,reg_r12,reg_r13,reg_r14,reg_r15}; return p[rnd(11)]; }
	Sh4RegType areg() { static const Sh4RegType p[] = {reg_r8,reg_r9,reg_r10,reg_r11}; return p[rnd(4)]; }
	Sh4RegType anyint() { return rnd(3) ? gp() : (rnd(2) ? areg() : reg_r7); }

	void run_one(bool allow_baking)
	{
		mock_reset();

		RuntimeBlockInfo b;
		b.addr = 0x8C000000 + (rnd(RAM_SIZE - 4 * PAGE_SIZE) & ~1u);
		b.sh4_code_size = (4 + rnd(180)) & ~1u;
		b.BlockType = BET_StaticJump;

		// literal candidates: some on the block's pages, some far away
		vector<u32> lits;
		for (int i = 0; i < 4; i++)
			lits.push_back((b.addr + rnd(b.sh4_code_size + 96)) & ~3u);
		for (int i = 0; i < 2; i++)
			lits.push_back(0x8C000000 + (rnd(RAM_SIZE - 256) & ~3u));
		for (u32 l : lits)
			wr_ram32(l, r32());

		// sometimes the block's page already holds data: baking must not happen
		if (rnd(4) == 0)
			mock_page_has_data[(b.addr & RAM_MASK) / PAGE_SIZE] = true;

		u32 nops = 5 + rnd(35);
		for (u32 i = 0; i < nops; i++)
			gen_op(b, lits);

		// dynamic exit (with a delay-slot op after it, like the decoder emits)
		BlockEndType end_type = BET_StaticJump;
		if (rnd(3) == 0)
		{
			end_type = rnd(2) ? BET_DynamicJump : BET_DynamicCall;
			b.BlockType = end_type;
			Sh4RegType t = gp();
			if (rnd(2))
				b.oplist.push_back(OP(shop_mov32, R(t), I(0x8C000000 + (r32() & RAM_MASK & ~1u))));
			b.oplist.push_back(OP(shop_jdyn, R(reg_pc_dyn), R(t), rnd(2) ? I(4 * rnd(8)) : P()));
			if (rnd(2))
				gen_op(b, lits);
		}

		// keep pristine copies, then optimize
		vector<shil_opcode> orig_ops = b.oplist;
		size_t orig_size = orig_ops.size();

		constprop(&b, allow_baking);
		fuse_readm_pairs(&b);
		dead_value_elim(&b);

		verify(mock_bad_reads == 0);
		check_arm32_forms(b.oplist);

		if (b.oplist.size() < orig_size) opt_run_count++;
		if (b.BlockType != end_type) promote_count++;

		// mutate ram the block can't rely on (anything off its own pages)
		u32 bf = (b.addr & RAM_MASK) / PAGE_SIZE;
		u32 bl = ((b.addr + b.sh4_code_size - 1) & RAM_MASK) / PAGE_SIZE;
		for (int i = 0; i < 8; i++)
		{
			u32 off = r32() & RAM_MASK;
			u32 pg = off / PAGE_SIZE;
			if (pg >= bf && pg <= bl)
				continue;
			mock_ram[off] = (u8)r32();
		}

		// same initial state for both runs
		Interp init;
		for (u32 r = 0; r < sh4_reg_count; r++)
			init.regs[r] = rnd(2) ? r32() : rnd(64);
		for (auto a : {reg_r8, reg_r9, reg_r10, reg_r11})
			init.regs[a] = lits[rnd(lits.size())];
		init.regs[reg_r7] = 4 * rnd(16);
		init.regs[reg_sr_T] = rnd(2);
		for (u32 r = 0; r < 8; r++)
			init.bank[r] = r32();
		// r7 is used as a memory offset; keep its banked twin plausible too
		init.bank[7] = 4 * rnd(16);

		Interp a = init, o = init;
		a.run(orig_ops);
		o.run(b.oplist);
		compare_runs(a, o, b, end_type);
	}

	void gen_op(RuntimeBlockInfo& b, const vector<u32>& lits)
	{
		u32 lit = lits[rnd(lits.size())];
		switch (rnd(22))
		{
		case 20:
		{
			// in-place shift chain on one register, like sh4 wide shifts
			static const shilop sh[] = {shop_shl, shop_shr, shop_sar, shop_ror};
			shilop s = sh[rnd(4)];
			Sh4RegType r = gp();
			u32 n = 2 + rnd(3);
			for (u32 i = 0; i < n; i++)
				b.oplist.push_back(OP(s, R(r), R(r), I(1 + rnd(31))));
			break;
		}
		case 21:
		{
			// fmov.s @Rn+ chain: f32 loads with +4 bumps, fusion fodder
			Sh4RegType base = areg();
			if (rnd(2))
				b.oplist.push_back(OP(shop_mov32, R(base), I(lit)));
			u32 n = 1 + rnd(4);
			for (u32 i = 0; i < n; i++)
			{
				b.oplist.push_back(OP(shop_readm, F((Sh4RegType)(reg_fr_0 + rnd(8))), R(base), P(), 4));
				if (i + 1 < n || rnd(2))
					b.oplist.push_back(OP(shop_add, R(base), R(base), I(4)));
			}
			break;
		}
		case 0: // constants flowing in
		case 1:
			b.oplist.push_back(OP(shop_mov32, R(gp()), rnd(2) ? I(rnd(2) ? rnd(64) : r32()) : I(lit)));
			break;
		case 2: // address registers only ever get valid addresses
			b.oplist.push_back(OP(shop_mov32, R(areg()), I(lit)));
			break;
		case 3:
			b.oplist.push_back(OP(shop_mov32, R(gp()), R(anyint())));
			break;
		case 4:
		case 5:
		{
			static const shilop alu[] = {shop_add, shop_sub, shop_and, shop_or, shop_xor};
			b.oplist.push_back(OP(alu[rnd(5)], R(gp()), R(anyint()),
			                      rnd(2) ? R(anyint()) : I(r32())));
			break;
		}
		case 6:
		{
			static const shilop sh[] = {shop_shl, shop_shr, shop_sar, shop_ror};
			b.oplist.push_back(OP(sh[rnd(4)], R(gp()), R(anyint()), I(rnd(31) + 1)));
			break;
		}
		case 7:
			b.oplist.push_back(OP(rnd(2) ? shop_shld : shop_shad, R(gp()), R(anyint()), R(anyint())));
			break;
		case 8:
		{
			static const shilop un[] = {shop_not, shop_neg, shop_ext_s8, shop_ext_s16, shop_swaplb, shop_swap};
			b.oplist.push_back(OP(un[rnd(6)], R(gp()), R(anyint())));
			break;
		}
		case 9:
		{
			static const shilop ml[] = {shop_mul_i32, shop_mul_u16, shop_mul_s16};
			b.oplist.push_back(OP(ml[rnd(3)], R(reg_macl), R(anyint()), R(anyint())));
			break;
		}
		case 10:
			b.oplist.push_back(OP(rnd(2) ? shop_mul_u64 : shop_mul_s64, R(reg_macl),
			                      R(anyint()), R(anyint()), 0, P(), R(reg_mach)));
			break;
		case 11:
		{
			static const shilop cm[] = {shop_test, shop_seteq, shop_setge, shop_setgt, shop_setae, shop_setab};
			b.oplist.push_back(OP(cm[rnd(6)], R(reg_sr_T), R(anyint()),
			                      rnd(2) ? R(anyint()) : I(rnd(2) ? 0 : r32())));
			break;
		}
		case 12:
			b.oplist.push_back(OP(shop_setpeq, R(reg_sr_T), R(anyint()), R(anyint())));
			break;
		case 13:
		{
			if (rnd(2))
				b.oplist.push_back(OP(rnd(2) ? shop_adc : shop_sbc, R(gp()), R(anyint()),
				                      R(anyint()), 0, R(reg_sr_T), R(reg_sr_T)));
			else
				b.oplist.push_back(OP(rnd(2) ? shop_rocl : shop_rocr, R(gp()), R(anyint()),
				                      R(reg_sr_T), 0, P(), R(reg_sr_T)));
			break;
		}
		case 14:
		{
			Sh4RegType d1 = gp(), d2 = gp();
			if (d1 == d2) d2 = reg_r15 == d2 ? reg_r14 : reg_r15;
			b.oplist.push_back(OP(rnd(2) ? shop_div32u : shop_div32s, R(d1), R(d1),
			                      R(anyint()), 0, P(), R(d2)));
			break;
		}
		case 15:
			b.oplist.push_back(OP(shop_div32p2, R(gp()), R(anyint()), R(anyint()), 0, R(reg_sr_T)));
			break;
		case 16:
		case 17:
		{
			// memory op; base is an address reg, offset imm/none/r7
			static const u32 sizes[] = {1, 2, 4};
			u32 size = sizes[rnd(3)];
			shil_param rs3 = P();
			u32 k = rnd(3);
			if (k == 1) rs3 = I(4 * rnd(16));
			if (k == 2) rs3 = R(reg_r7);
			Sh4RegType base = areg();
			if (rnd(2)) // often set the base right here so it's a known const
				b.oplist.push_back(OP(shop_mov32, R(base), I(lit)));
			if (rnd(2))
				b.oplist.push_back(OP(shop_readm, R(gp()), R(base), P(), size, rs3));
			else
				b.oplist.push_back(OP(shop_writem, P(), R(base), R(gp()), size, rs3));
			break;
		}
		case 18:
			b.oplist.push_back(OP(shop_jcond, R(reg_pc_dyn), R(reg_sr_T)));
			break;
		case 19:
			if (rnd(3) == 0)
				b.oplist.push_back(OP(shop_ifb, P(), I(rnd(2)), I(r32()),
				                      0, I(rnd(2) ? lit : 0)));
			else if (rnd(2))
				b.oplist.push_back(OP(shop_sync_sr, P(), P()));
			else
				b.oplist.push_back(OP(shop_sync_fpscr, P(), P()));
			break;
		}
	}
};

int main(int argc, char** argv)
{
	u32 seed = argc > 1 ? (u32)strtoul(argv[1], nullptr, 0) : 1;
	u32 iters = argc > 2 ? (u32)strtoul(argv[2], nullptr, 0) : 5000;

	constprop_verbose = false; // the fuzzer would print millions of lines
	constprop_assume_stores_safe = false; // tests verify the safe behavior

	t_bake_and_jdyn_promote();
	t_rd2_kill();
	t_no_bake_when_page_has_data();
	t_no_bake_off_page();
	t_no_bake_after_unknown_store();
	t_safe_store_keeps_baking();
	t_writem_forms();
	t_shld_shad();
	t_sync_sr();
	t_commutative_swap();
	t_writeback_dedup();
	t_mmio_promote();
	t_pref();
	t_readm_sizes();
	t_cmp_fold();
	t_jdyn_offset();
	t_f32_mov();
	t_shift_chains();
	t_dse();
	t_ftrv_known_w();
	t_readm_pair_fusion();
	t_sq_pref();
	t_imm_literal_bake();
	t_rom_bake();
	t_baking_disabled();
	printf("directed tests: ok\n");

	Fuzz f(seed);
	for (u32 i = 0; i < iters; i++)
		f.run_one(i % 4 != 0); // every 4th block runs with baking disabled

	// the pass must actually be doing something, not just passing vacuously
	verify(opt_run_count > iters / 8);
	verify(promote_count > 0);

	printf("fuzz: %u blocks ok (%u optimized, %u static-promoted)\n",
	       iters, opt_run_count, promote_count);
	return 0;
}
