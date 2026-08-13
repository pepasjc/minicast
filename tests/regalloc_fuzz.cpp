// Fuzz harness for regalloc.h: generates random shil-like blocks, runs
// DoAlloc, then simulates execution. The simulation follows the backend
// contract (preloads at OpBegin, op body reads all mapped sources before
// writing any mapped dest, writebacks at OpEnd) and checks every source
// read and the final context state against an architectural model.
#include "mockshil.h"
#include "regalloc_under_test.h"

#include <cstring>

struct TestAlloc : RegAlloc<int, int, false>
{
	u64 Rg[16];
	u64 Rf[16];
	u64 ctx[sh4_reg_count];

	TestAlloc()
	{
		memset(spans, 0, sizeof(spans));
		memset(Rg, 0, sizeof(Rg));
		memset(Rf, 0, sizeof(Rf));
		spills = 0;
		opt_pair_fpu_spans = true;
	}

	virtual void Preload(u32 reg, int nreg) { Rg[nreg] = ctx[reg]; }
	virtual void Writeback(u32 reg, int nreg) { ctx[reg] = Rg[nreg]; }
	virtual void Preload_FPU(u32 reg, int nreg) { Rf[nreg] = ctx[reg]; }
	virtual void Writeback_FPU(u32 reg, int nreg) { ctx[reg] = Rf[nreg]; }

	// mirror the arm32 backend: ftrv/fipr fv operands become 4 allocatable
	// f32 spans, and the f64 pair of a mov64/readm/writem/fsca becomes 2;
	// every other multi-reg param stays a memory operand
	virtual bool ExplodeVec(shil_opcode* op, const shil_param& prm)
	{
		if ((op->op == shop_ftrv || op->op == shop_fipr) && prm.count() == 4)
			return true;

		return prm.is_r64f() &&
			(op->op == shop_mov64 || op->op == shop_readm ||
			 op->op == shop_writem || op->op == shop_fsca);
	}
};

static u64 rng_state;
static u64 rnd()
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}
static u32 rnd_below(u32 n) { return (u32)(rnd() % n); }

static const Sh4RegType gpr_pool[] = { reg_r0, reg_r1, reg_r2, reg_r3, reg_r4, reg_r5, reg_r6, reg_r7 };
static const Sh4RegType fpr_pool[] = { reg_fr_0, reg_fr_1, reg_fr_2, reg_fr_3, reg_fr_4, reg_fr_5, reg_fr_6, reg_fr_7,
                                       reg_fr_8, reg_fr_9, reg_fr_10, reg_fr_11, reg_fr_12, reg_fr_13, reg_fr_14, reg_fr_15 };

static shil_param mkreg(Sh4RegType r, bool fpr)
{
	shil_param p;
	p.type = fpr ? FMT_F32 : FMT_I32;
	p._reg = r;
	return p;
}

static shil_param mkimm(u32 v)
{
	shil_param p;
	p.type = FMT_IMM;
	p._imm = v;
	return p;
}

static shil_param mkvec(Sh4RegType r, u32 fmt)
{
	shil_param p;
	p.type = fmt;
	p._reg = r;
	return p;
}

// ops also reach into the xf bank now and then: those spans are what the
// non-exploded xmtrx operand of an ftrv flushes (AM_FLUSH)
static Sh4RegType rnd_fpr()
{
	u32 i = rnd_below(16);
	return (Sh4RegType)((rnd_below(4) == 0 ? reg_xf_0 : reg_fr_0) + i);
}

// even-aligned base of a dr/xd pair
static Sh4RegType rnd_fpr_pair()
{
	u32 i = rnd_below(8) * 2;
	return (Sh4RegType)((rnd_below(4) == 0 ? reg_xf_0 : reg_fr_0) + i);
}

// pick `n` distinct regs from a pool (params of one op must fit in the host pool)
static void pick_distinct(const Sh4RegType* pool, u32 pool_sz, u32 n, Sh4RegType* out)
{
	verify(n <= pool_sz);
	for (u32 i = 0; i < n; i++)
	{
		bool dup;
		do
		{
			out[i] = pool[rnd_below(pool_sz)];
			dup = false;
			for (u32 j = 0; j < i; j++)
				if (out[j] == out[i]) dup = true;
		} while (dup);
	}
}

static void gen_block(RuntimeBlockInfo* blk, u32 ngpr, u32 nfpr)
{
	// mostly real-block sized, sometimes long (sprite loops unroll far)
	u32 nops = rnd_below(4) ? 1 + rnd_below(40) : 1 + rnd_below(100);

	for (u32 i = 0; i < nops; i++)
	{
		shil_opcode op;
		Sh4RegType r[5];
		u32 kind = rnd_below(17);

		if (kind == 10 && nfpr >= 4) // exploded vector op, like ftrv
		{
			// rd/rs1 in-place like the real thing (any FVn); rs2 is a memory operand
			Sh4RegType base = (Sh4RegType)(reg_fr_0 + rnd_below(4) * 4);
			op.op = shop_ftrv;
			op.rd = mkvec(base, FMT_V4);
			op.rs1 = mkvec(base, FMT_V4);
			op.rs2 = mkvec(reg_xf_0, FMT_V16);
			blk->oplist.push_back(op);
			continue;
		}
		if (kind == 11 && nfpr >= 4) // fipr FVm,FVn: rd = rs1[3] (DM_fiprOp), any bases
		{
			u32 n = rnd_below(4) * 4;
			u32 m = nfpr >= 8 ? rnd_below(4) * 4 : n;
			op.op = shop_fipr;
			op.rs1 = mkvec((Sh4RegType)(reg_fr_0 + n), FMT_V4);
			op.rs2 = mkvec((Sh4RegType)(reg_fr_0 + m), FMT_V4);
			op.rd = mkreg((Sh4RegType)(reg_fr_0 + n + 3), true);
			blk->oplist.push_back(op);
			continue;
		}
		if (kind == 12 && nfpr >= 4) // exploded f64 pair copy: up to 4 live halves
		{
			op.op = shop_mov64;
			op.rd = mkvec(rnd_fpr_pair(), FMT_F64);
			op.rs1 = mkvec(rnd_fpr_pair(), FMT_F64);
			blk->oplist.push_back(op);
			continue;
		}
		if (kind == 13 && nfpr >= 2) // fsca: exploded F64 pair write from a gpr source
		{
			op.op = shop_fsca;
			op.rd = mkvec(rnd_fpr_pair(), FMT_F64);
			op.rs1 = mkreg(gpr_pool[rnd_below(8)], false);
			blk->oplist.push_back(op);
			continue;
		}
		if (kind == 14 && nfpr >= 2) // 64 bit fpu read: exploded pair dest, gpr address
		{
			op.op = shop_readm;
			op.rd = mkvec(rnd_fpr_pair(), FMT_F64);
			op.rs1 = mkreg(gpr_pool[rnd_below(8)], false);

			// fused form (fuse_readm_pairs): a second double off the same address
			if (nfpr >= 4 && rnd_below(2) == 0)
			{
				Sh4RegType d2 = rnd_fpr_pair();

				if (d2 != op.rd._reg)
					op.rd2 = mkvec(d2, FMT_F64);
			}

			blk->oplist.push_back(op);
			continue;
		}
		if (kind == 15 && nfpr >= 2) // 64 bit fpu write: exploded pair source, gpr address
		{
			op.op = shop_writem;
			op.rs1 = mkreg(gpr_pool[rnd_below(8)], false);
			op.rs2 = mkvec(rnd_fpr_pair(), FMT_F64);
			blk->oplist.push_back(op);
			continue;
		}
		// frswap swaps both banks wholesale: every fpu span has to flush and
		// die there. Kept rare -- it wipes the fpu state the other ops build up
		if (kind == 16 && rnd_below(4) == 0)
		{
			op.op = shop_frswap;
			op.rd = mkvec(reg_xf_0, FMT_V16);
			op.rd2 = mkvec(reg_fr_0, FMT_V16);
			op.rs1 = mkvec(reg_fr_0, FMT_V16);
			op.rs2 = mkvec(reg_xf_0, FMT_V16);
			blk->oplist.push_back(op);
			continue;
		}
		if (kind >= 10)
			kind = rnd_below(10);

		if (kind < 3) // mov32 reg-reg (i-i, f-f, or cross-file)
		{
			u32 cls = rnd_below(4);
			bool df = cls & 1, sf = cls & 2;
			if ((df || sf) && nfpr < 1) { df = sf = false; }
			op.op = shop_mov32;
			Sh4RegType dr = df ? rnd_fpr() : gpr_pool[rnd_below(8)];
			Sh4RegType sr = sf ? rnd_fpr() : gpr_pool[rnd_below(8)];
			op.rd = mkreg(dr, df);
			op.rs1 = mkreg(sr, sf);
		}
		else if (kind == 3) // mov32 imm
		{
			op.op = shop_mov32;
			op.rd = mkreg(gpr_pool[rnd_below(8)], false);
			op.rs1 = mkimm((u32)rnd());
		}
		else if (kind == 4 && nfpr >= 3) // fpu binary
		{
			op.op = shop_fadd;
			pick_distinct(fpr_pool, 16, 3, r);
			op.rd = mkreg(r[0], true);
			op.rs1 = mkreg(r[1], true);
			op.rs2 = mkreg(r[2], true);
		}
		else if (kind < 8 || ngpr < 5) // gpr binary: rd,rs1,rs2 (rs2 may be imm, rd may equal a source)
		{
			u32 need = ngpr < 3 ? ngpr : 3;
			op.op = shop_add;
			pick_distinct(gpr_pool, 8, need, r);
			op.rd = mkreg(r[0], false);
			op.rs1 = mkreg(r[rnd_below(need)], false);
			if (rnd_below(4) == 0)
				op.rs2 = mkimm((u32)rnd());
			else
				op.rs2 = mkreg(r[rnd_below(need)], false);
		}
		else // wide op: rd, rd2, rs1, rs2, rs3 (like sbc)
		{
			op.op = shop_sbc;
			pick_distinct(gpr_pool, 8, 5, r);
			op.rd = mkreg(r[0], false);
			op.rd2 = mkreg(r[1], false);
			op.rs1 = mkreg(r[2], false);
			op.rs2 = mkreg(r[3], false);
			op.rs3 = mkreg(r[4], false);
		}

		blk->oplist.push_back(op);
	}
}

static void dump_block(RuntimeBlockInfo* blk)
{
	static const char* fmt_names[] = { "null", "imm", "i32", "f32", "f64", "v2", "v4", "v16" };
	for (size_t i = 0; i < blk->oplist.size(); i++)
	{
		shil_opcode* op = &blk->oplist[i];
		printf("%3zu: op=%d", i, op->op);
		shil_param* ps[] = { &op->rd, &op->rd2, &op->rs1, &op->rs2, &op->rs3 };
		const char* pn[] = { "rd", "rd2", "rs1", "rs2", "rs3" };
		for (int j = 0; j < 5; j++)
		{
			if (ps[j]->is_reg())
				printf(" %s=r%d(%s)", pn[j], ps[j]->_reg, fmt_names[ps[j]->type]);
			else if (ps[j]->is_imm())
				printf(" %s=#%08x", pn[j], ps[j]->_imm);
		}
		printf("\n");
	}
}

static u64 g_elided, g_movs, g_spills, g_fail_ctx, g_vecops, g_vecruns;
static u64 g_loops, g_carried_wb, g_carried_pl;
static RuntimeBlockInfo* g_cur_blk;
static u64 g_cur_seed;
static TestAlloc* g_cur_alloc;

static void dump_spans(TestAlloc* a)
{
	for (size_t i = 0; i < a->all_spans.size(); i++)
	{
		TestAlloc::RegSpan* s = a->all_spans[i];
		printf("span %2zu: r%-2d [%2d..%2d] %s nreg=%d nregf=%d pl=%d wb=%d aliased=%d\n",
			i, s->regstart, s->start, s->end, s->fpr ? "fpr" : "gpr",
			(int)s->nreg, (int)s->nregf, s->preload, s->writeback, s->aliased);
	}
}

#define CHECK(x, ...) do { if (!(x)) { \
	printf("CHECK failed %s:%d: " #x "\n", __FILE__, __LINE__); \
	printf(__VA_ARGS__); \
	printf("seed=%llu\n", (unsigned long long)g_cur_seed); \
	dump_block(g_cur_blk); \
	dump_spans(g_cur_alloc); \
	exit(1); } } while (0)

// mirrors the arm32 gate (block_self_loops): the preamble is skipped on the
// back edge, so nothing may hand a register over through the context there
static bool can_loop(RuntimeBlockInfo* blk, TestAlloc* alloc)
{
	if (blk->oplist.empty())
		return false;

	shil_opcode* op = &blk->oplist[0];
	shil_param* rd[3] = { &op->rs1, &op->rs2, &op->rs3 };
	shil_param* all[5] = { &op->rd, &op->rd2, &op->rs1, &op->rs2, &op->rs3 };

	// a vector operand at op 0 only rules the block out if one of its registers
	// can also hold a span there -- see block_self_loops
	for (int p = 0; p < 3; p++)
	{
		if (!rd[p]->is_reg() || rd[p]->count() <= 1 || alloc->ExplodeVec(op, *rd[p]))
			continue;

		for (u32 e = 0; e < rd[p]->count(); e++)
		{
			u32 r = (u32)rd[p]->_reg + e;

			for (int q = 0; q < 5; q++)
			{
				shil_param* o = all[q];

				if (o == rd[p] || !o->is_reg())
					continue;

				if (o->count() != 1 && !alloc->ExplodeVec(op, *o))
					continue;

				if (r >= (u32)o->_reg && r < (u32)o->_reg + o->count())
					return false;
			}
		}
	}

	return true;
}

static void run_block(TestAlloc* alloc, RuntimeBlockInfo* blk, u32 ngpr, u32 nfpr)
{
	int gprs[17], fprs[17];
	for (u32 i = 0; i < ngpr; i++) gprs[i] = i;
	gprs[ngpr] = -1;
	for (u32 i = 0; i < nfpr; i++) fprs[i] = i;
	fprs[nfpr] = -1;

	g_cur_alloc = alloc;

	// a block that branches back to its own start: run it several times with
	// the loop path in between (no end writebacks, no entry preloads)
	bool loops = can_loop(blk, alloc) && rnd_below(3) == 0;
	u32 iters = loops ? 2 + rnd_below(3) : 1;

	alloc->block_loops = loops;
	alloc->DoAlloc(blk, gprs, fprs);

	// context traffic the back edge skips, per iteration
	if (loops)
	{
		g_loops++;
		for (size_t s = 0; s < alloc->all_spans.size(); s++)
		{
			TestAlloc::RegSpan* spn = alloc->all_spans[s];

			if (alloc->CarriedNeedsFlush(spn))
				continue;

			if (spn->writeback && spn->end + 1 == blk->oplist.size())
				g_carried_wb++;
			if (spn->preload && spn->start == 0)
				g_carried_pl++;
		}
	}
	g_spills += alloc->spills;
	alloc->spills = 0;

	// how often an exploded operand ended up in consecutive, in-order host regs
	// (an even base): that's what lets the backend fold its context accesses
	for (size_t opid = 0; opid < blk->oplist.size(); opid++)
	{
		shil_opcode* op = &blk->oplist[opid];
		shil_param* ps[] = { &op->rd, &op->rd2, &op->rs1, &op->rs2, &op->rs3 };

		for (int j = 0; j < 5; j++)
		{
			if (!ps[j]->is_reg() || ps[j]->count() < 2 || !alloc->ExplodeVec(op, *ps[j]))
				continue;

			alloc->current_opid = (u32)opid;

			int base = alloc->mapf((Sh4RegType)ps[j]->_reg);
			bool run = (base & 1) == 0;

			for (u32 e = 1; e < ps[j]->count(); e++)
				run &= alloc->mapf((Sh4RegType)(ps[j]->_reg + e)) == base + (int)e;

			g_vecops++;
			if (run)
				g_vecruns++;
		}
	}

	u64 arch[sh4_reg_count];
	u64 next_val = 0x1000000;
	for (int i = 0; i < sh4_reg_count; i++)
		arch[i] = alloc->ctx[i] = next_val++;

	for (u32 iter = 0; iter < iters; iter++)
	{
	// the hardware runs one emitted copy of the block, so every iteration has
	// to see the state this pass had at the top of the first one
	for (size_t s = 0; s < alloc->all_spans.size(); s++)
		alloc->all_spans[s]->dirty = false;

	for (size_t opid = 0; opid < blk->oplist.size(); opid++)
	{
		shil_opcode* op = &blk->oplist[opid];

		// only the carried half of op 0's preloads sits above the loop header
		if (opid == 0 && loops)
		{
			if (iter == 0)
				alloc->OpBeginCarried(op, 0);

			alloc->OpBeginTransient(op, 0);
		}
		else
			alloc->OpBegin(op, (int)opid);

		// read all mapped sources first (the backend contract)
		shil_param* srcs[] = { &op->rs1, &op->rs2, &op->rs3 };
		for (int s = 0; s < 3; s++)
		{
			shil_param* p = srcs[s];
			if (p->is_r32())
			{
				u64 v = p->is_r32f() ? alloc->Rf[alloc->mapf(p->_reg)] : alloc->Rg[alloc->mapg(p->_reg)];
				CHECK(v == arch[p->_reg], "op %zu reads r%d: host has %llx, arch has %llx\n",
					opid, p->_reg, (unsigned long long)v, (unsigned long long)arch[p->_reg]);
			}
			else if (p->is_reg() && p->count() >= 2)
			{
				if (alloc->ExplodeVec(op, *p))
				{
					// exploded: every element must be mapped and current
					for (u32 i = 0; i < p->count(); i++)
					{
						u64 v = alloc->Rf[alloc->mapf((Sh4RegType)(p->_reg + i))];
						CHECK(v == arch[p->_reg + i], "op %zu reads vec elem r%d: host has %llx, arch has %llx\n",
							opid, p->_reg + i, (unsigned long long)v, (unsigned long long)arch[p->_reg + i]);
					}
				}
				else
				{
					// memory operand: dirty spans over it must have been
					// written back before the op body runs
					for (u32 i = 0; i < p->count(); i++)
						CHECK(alloc->ctx[p->_reg + i] == arch[p->_reg + i],
							"op %zu reads mem-vec elem r%d: ctx has %llx, arch has %llx\n",
							opid, p->_reg + i, (unsigned long long)alloc->ctx[p->_reg + i],
							(unsigned long long)arch[p->_reg + i]);
				}
			}
		}

		// then write dests
		if (op->op == shop_mov32)
		{
			u64 v = op->rs1.is_imm() ? op->rs1._imm : arch[op->rs1._reg];
			arch[op->rd._reg] = v;
			if (op->rs1.is_r32() && op->rs1.is_r32f() == op->rd.is_r32f())
			{
				// same-class movs are elided by the backend when src/dst share a host reg
				bool fpr = op->rd.is_r32f();
				int dn = fpr ? alloc->mapf(op->rd._reg) : alloc->mapg(op->rd._reg);
				int sn = fpr ? alloc->mapf(op->rs1._reg) : alloc->mapg(op->rs1._reg);
				g_movs++;
				if (dn == sn)
					g_elided++;
				else if (fpr)
					alloc->Rf[dn] = alloc->Rf[sn];
				else
					alloc->Rg[dn] = alloc->Rg[sn];
			}
			else if (op->rd.is_r32f())
				alloc->Rf[alloc->mapf(op->rd._reg)] = v;
			else
				alloc->Rg[alloc->mapg(op->rd._reg)] = v;
		}
		else
		{
			shil_param* dsts[] = { &op->rd, &op->rd2 };
			for (int d = 0; d < 2; d++)
			{
				shil_param* p = dsts[d];
				if (p->is_r32())
				{
					u64 v = next_val++;
					arch[p->_reg] = v;
					if (p->is_r32f())
						alloc->Rf[alloc->mapf(p->_reg)] = v;
					else
						alloc->Rg[alloc->mapg(p->_reg)] = v;
				}
				else if (p->is_reg() && p->count() >= 2)
				{
					if (alloc->ExplodeVec(op, *p))
					{
						// exploded: the op body writes the mapped elements
						for (u32 i = 0; i < p->count(); i++)
						{
							u64 v = next_val++;
							arch[p->_reg + i] = v;
							alloc->Rf[alloc->mapf((Sh4RegType)(p->_reg + i))] = v;
						}
					}
					else
					{
						// memory operand: the op body writes guest memory;
						// overlapping spans must be dead (killed), so a later
						// bogus writeback would corrupt this and be caught
						for (u32 i = 0; i < p->count(); i++)
						{
							u64 v = next_val++;
							arch[p->_reg + i] = v;
							alloc->ctx[p->_reg + i] = v;
						}
					}
				}
			}
		}

		// on the back edge the values stay in their registers, except the ones
		// the next iteration would read back out of the context
		if (opid + 1 == blk->oplist.size() && iter + 1 < iters)
			alloc->OpEndLoop(op);
		else
			alloc->OpEnd(op);
	}
	}

	for (int i = 0; i < sh4_reg_count; i++)
		CHECK(alloc->ctx[i] == arch[i], "final state of r%d: ctx has %llx, arch has %llx\n",
			i, (unsigned long long)alloc->ctx[i], (unsigned long long)arch[i]);
}

int main(int argc, char** argv)
{
	u64 base_seed = argc > 1 ? strtoull(argv[1], 0, 0) : 1;
	u32 iters = argc > 2 ? (u32)strtoul(argv[2], 0, 0) : 5000;

	// {5,12} mirrors the real arm32 temps-mode pools (r5/r6/r7/r10/r11, f16-f27)
	struct { u32 ngpr, nfpr; } pools[] = { {5, 4}, {3, 2}, {2, 2}, {6, 8}, {5, 12} };
	const u32 npools = sizeof(pools) / sizeof(pools[0]);

	for (int alias = 0; alias <= 1; alias++)
	for (int reuse = 0; reuse <= 1; reuse++)
	{
		g_elided = g_movs = g_spills = g_vecops = g_vecruns = 0;
		g_loops = g_carried_wb = g_carried_pl = 0;
		for (u32 pi = 0; pi < npools; pi++)
		{
			TestAlloc alloc;
			alloc.opt_alias_mov = alias != 0;
			alloc.opt_reuse_dead = reuse != 0;

			for (u32 it = 0; it < iters; it++)
			{
				g_cur_seed = base_seed + it + pi * 1000000ull + (alias * 2 + reuse) * 100000000ull;
				rng_state = g_cur_seed * 2654435761ull + 1;

				RuntimeBlockInfo blk;
				gen_block(&blk, pools[pi].ngpr, pools[pi].nfpr);
				g_cur_blk = &blk;

				run_block(&alloc, &blk, pools[pi].ngpr, pools[pi].nfpr);
			}
		}
		printf("alias=%d reuse=%d: OK  (movs %llu, elided %llu, spills %llu, vec operands %llu, in one run %llu)\n",
			alias, reuse, (unsigned long long)g_movs, (unsigned long long)g_elided,
			(unsigned long long)g_spills, (unsigned long long)g_vecops,
			(unsigned long long)g_vecruns);
		printf("            %llu looping blocks: %llu writebacks + %llu preloads carried per iteration\n",
			(unsigned long long)g_loops, (unsigned long long)g_carried_wb,
			(unsigned long long)g_carried_pl);
	}

	printf("all passed\n");
	return 0;
}
