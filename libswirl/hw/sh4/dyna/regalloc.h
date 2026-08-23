/*
	This file is part of libswirl
*/
#include "license/bsd"


#pragma once

#include <types.h>
#include "shil.h"

#include <set>
#include <deque>
#include <algorithm>

static inline bool operator < (const shil_param &lhs, const shil_param &rhs)
{
	u32 rhsv=rhs.type + rhs._reg*32;
	u32 lhsv=lhs.type + lhs._reg*32;

	return rhsv < lhsv;
}

template<typename nreg_t,typename nregf_t,bool explode_spans=true>
struct RegAlloc
{
	struct RegSpan;
	RegSpan* spans[sh4_reg_count];

	enum AccessMode
	{
		AM_NONE,
		AM_READ,
		AM_WRITE,
		AM_READWRITE,
		//a vector operand reads this reg from the context at this op: any
		//dirty value is committed here (writeback at OpBegin), but the host
		//register still holds it, so the span stays live across the op
		AM_FLUSH
	};

	struct RegAccess
	{
		u32 pos;
		AccessMode am;
	};

	struct RegSpan
	{
		u32 start; //load before start
		u32 end;   //write after end

		u32 regstart;
		bool fpr;

		bool preload;
		bool writeback;

		nreg_t nreg;
		nregf_t nregf;

		bool aliased;
		bool freed_early;
		bool dirty;		//emission state: host reg holds a value ctx doesn't

		vector<RegAccess> accesses;

		RegSpan(const shil_param& prm,int pos, AccessMode mode)
		{
			start=pos;
			end=pos;
			writeback=preload=false;
			regstart=prm._reg;
			fpr=prm.is_r32f();
			verify(prm.count()==1);

			aliased=false;
			freed_early=false;
			dirty=false;

			if (mode&AM_WRITE)
				writeback=true;
			if (mode&AM_READ)
				preload=true;

			nreg=(nreg_t)-1;
			nregf=(nregf_t)-1;
			RegAccess ra={static_cast<u32>(pos),mode};
			accesses.push_back(ra);
		}

		void Flush()
		{
			//nothing required ?
		}

		void Kill()
		{
			writeback=false;
		}

		void Access(int pos,AccessMode mode)
		{
			end=pos;
			if (mode&AM_WRITE)
				writeback=true;
			RegAccess ra={static_cast<u32>(pos),mode};
			accesses.push_back(ra);
		}

		int pacc(int pos)
		{
			if (!contains(pos))
				return -1;

			for (int i=(int)accesses.size()-1;i>=0;i--)
			{
				if (accesses[i].pos<(u32)pos)
					return accesses[i].pos;
			}

			return -1;
		}

		int nacc(int pos)
		{
			if (!contains(pos))
				return -1;

			for (size_t i=0;i<accesses.size();i++)
			{
				if (accesses[i].pos>(u32)pos)
					return accesses[i].pos;
			}

			return -1;
		}

		//the next access the op body needs this register for: a flush reads
		//the operand from the context, so it doesn't count
		int nacc_use(int pos)
		{
			if (!contains(pos))
				return -1;

			for (size_t i=0;i<accesses.size();i++)
			{
				if (accesses[i].pos>(u32)pos && accesses[i].am!=AM_FLUSH)
					return accesses[i].pos;
			}

			return -1;
		}

		int nacc_w(int pos)
		{
			if (!contains(pos))
				return -1;

			for (size_t i=0;i<accesses.size();i++)
			{
				if (accesses[i].pos>(u32)pos && accesses[i].am&AM_WRITE)
					return accesses[i].pos;
			}

			return -1;
		}

		void trim_access()
		{
			for (size_t i=0;i<accesses.size();i++)
			{
				if (!contains(accesses[i].pos))
					accesses.erase(accesses.begin()+i--);
			}
		}

		bool cacc(int pos)
		{
			return cacc_am(pos)!=AM_NONE;
		}

		AccessMode cacc_am(int pos)
		{
			if (!contains(pos)) return AM_NONE;

			for (size_t i=0; i<accesses.size(); i++)
			{
				if (accesses[i].pos==pos)
					return accesses[i].am;
			}

			return AM_NONE;
		}

		bool NeedsWB()
		{
			for (size_t i=0; i<accesses.size(); i++)
			{
				if (accesses[i].am&AM_WRITE)
					return true;
			}
			return false;
		}

		bool NeedsPL()
		{
			if (accesses[0].am&AM_READ)
				return true;

			//a span(-half) that begins at a vector-read flush point holds no
			//value in its register yet; if anything is accessed after it, the
			//register must be loaded. The context is current at a flush by
			//construction, so preloading there is always sound.
			if (accesses[0].am==AM_FLUSH && accesses.size()>1)
				return true;

			return false;
		}

		bool begining(int pos)
		{
			return  pos==start;
		}

		bool ending(int pos)
		{
			return pos==end;
		}


		int contains(int pos)
		{
			return start<=(u32)pos && end>=(u32)pos;
		}
	};


	vector<RegSpan*> all_spans;
	u32 spills;

	//Optional optimisations, off by default. A backend may only enable these
	//if, for every op, its emitter reads all register-allocated sources before
	//writing any register-allocated dest (single 3-operand host instructions
	//have this for free; multi-instruction sequences must be audited).
	bool opt_alias_mov=false;   //coalesce reg-reg movs: alias the dst span onto the src host reg
	bool opt_reuse_dead=false;  //spans starting at an op may take a host reg that dies at that op

	//Elements of an exploded fpu operand do best in consecutive, in-order host
	//registers, for a backend that can move a register pair in one instruction
	//(on arm32 s2k/s2k+1 are the halves of dk): the two context accesses at the
	//span ends fold into one, and the operand itself can be loaded or stored as
	//a pair. Best effort -- elements fall back to one-at-a-time allocation when
	//no aligned run is free.
	bool opt_pair_fpu_spans=false;

	//The block branches back to its own start. On that path the backend skips
	//the end writebacks and the entry preloads, so values stay in host regs
	//across the back edge; spans are stretched over the whole block to make
	//that worth something. The backend must gate this on a block that never
	//moves the register file through the context behind a span's back (no
	//interpreter fallbacks, no vector operand at op 0) and must commit the
	//spans CarriedNeedsFlush() names before it branches back.
	bool block_loops=false;

	//--- overlay: cross-block resident registers, managed by the region
	//compiler (CompileLoop) above the span machinery. Overlaid registers are
	//invisible to the span builder (InsertRegs skips their accesses) and the
	//alloc queries resolve them first; the region compiler emits their
	//preloads/writebacks itself (region preamble, shared exit epilogue, and
	//spill/restore brackets around ifb/canonical ops that reach the context
	//behind the allocator's back). The table is region-constant for now; the
	//design allows per-label tables plus edge moves later.
	bool overlay_active=false;
	u32 overlay_gpr[sh4_reg_count];
	u32 overlay_fpu[sh4_reg_count];

	void OverlayClear()
	{
		overlay_active=false;
		for (int i=0;i<sh4_reg_count;i++)
			overlay_gpr[i]=overlay_fpu[i]=0xFFFFFFFF;
	}

	void OverlaySetg(Sh4RegType reg, nreg_t nreg)
	{
		overlay_gpr[reg]=(u32)nreg;
		overlay_active=true;
	}

	void OverlaySetf(Sh4RegType reg, nregf_t nreg)
	{
		overlay_fpu[reg]=(u32)nreg;
		overlay_active=true;
	}

	bool IsOverlaidg(Sh4RegType reg) { return overlay_active && overlay_gpr[reg]!=0xFFFFFFFF; }
	bool IsOverlaidf(Sh4RegType reg) { return overlay_active && overlay_fpu[reg]!=0xFFFFFFFF; }

	u32 current_opid;
	u32 block_last;		//index of the block's last op
	u32 preload_fpu,preload_gpr;
	u32 writeback_fpu,writeback_gpr;

	bool IsAllocAny(Sh4RegType reg)
	{
		return IsAllocg(reg) || IsAllocf(reg);
	}

	bool IsAllocAny(const shil_param& prm)
	{
		if (prm.is_reg())
		{
			bool rv=IsAllocAny(prm._reg);
			if (prm.count()!=1)
			{
				for (u32 i=1;i<prm.count();i++)
					verify(IsAllocAny((Sh4RegType) (prm._reg+i))==rv);
			}
			return rv;
		}
		else
		{
			return false;
		}
	}

	bool IsAllocg(Sh4RegType reg)
	{
		if (IsOverlaidg(reg))
			return true;

		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			if (all_spans[sid]->regstart==(u32)reg && all_spans[sid]->contains(current_opid))
				return !all_spans[sid]->fpr;
		}

		return false;
	}

	bool IsAllocg(const shil_param& prm)
	{
		if (prm.is_reg())
		{
			verify(prm.count()==1);
			return IsAllocg(prm._reg);
		}
		else
		{
			return false;
		}
	}

	bool IsAllocf(Sh4RegType reg)
	{
		if (IsOverlaidf(reg))
			return true;

		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			if (all_spans[sid]->regstart==(u32)reg && all_spans[sid]->contains(current_opid))
				return all_spans[sid]->fpr;
		}

		return false;
	}

	bool IsAllocf(const shil_param& prm, u32 i)
	{
		verify(prm.count()>i);

		return IsAllocf((Sh4RegType)(prm._reg+i));
	}

	bool IsAllocf(const shil_param& prm)
	{
		if (prm.is_reg())
		{
			verify(prm.count()==1);
			return IsAllocf(prm._reg);
		}
		else
			return false;
	}

	nreg_t mapg(Sh4RegType reg)
	{
		verify(IsAllocg(reg));

		if (IsOverlaidg(reg))
			return (nreg_t)overlay_gpr[reg];

		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			if (all_spans[sid]->regstart==(u32)reg && all_spans[sid]->contains(current_opid))
			{
				verify(!all_spans[sid]->fpr);
				verify(all_spans[sid]->nreg!=-1);
				return all_spans[sid]->nreg;
			}
		}

		die("map must return value\n");
		return (nreg_t)-1;
	}

	nreg_t mapg(const shil_param& prm)
	{
		verify(IsAllocg(prm));

		if (prm.is_reg())
		{
			verify(prm.count()==1);
			return mapg(prm._reg);
		}
		else
		{
			die("map must return value\n");
			return (nreg_t)-1;
		}
	}

	nregf_t mapf(Sh4RegType reg)
	{
		verify(IsAllocf(reg));

		if (IsOverlaidf(reg))
			return (nregf_t)overlay_fpu[reg];

		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			if (all_spans[sid]->regstart==(u32)reg && all_spans[sid]->contains(current_opid))
			{
				verify(all_spans[sid]->fpr);
				verify(all_spans[sid]->nregf!=-1);
				return all_spans[sid]->nregf;
			}
		}

		die("map must return value\n");
		return (nregf_t)-1;
	}

	nregf_t mapf(const shil_param& prm)
	{
		verify(IsAllocf(prm));

		if (prm.is_reg())
		{
			verify(prm.count()==1);
			return mapf(prm._reg);
		}
		else
		{
			die("map must return value\n");
			return (nregf_t)-1;
		}
	}

	nregf_t mapfv(const shil_param& prm,u32 i)
	{
		verify(IsAllocf(prm,i));

		if (prm.is_reg())
		{
			return mapf((Sh4RegType)(prm._reg+i));
		}
		else
		{
			die("map must return value\n");
			return (nregf_t)-1;
		}
	}
	

	bool IsFlushOp(RuntimeBlockInfo* block, int opid)
	{
		verify(opid>=0 && opid<block->oplist.size());
		shil_opcode* op=&block->oplist[opid];

		return op->op == shop_sync_fpscr || op->op == shop_sync_sr || op->op == shop_ifb;
	}

	bool IsRegWallOp(RuntimeBlockInfo* block, int opid, bool is_fpr)
	{
		if (IsFlushOp(block,opid))
			return true;

		shil_opcode* op=&block->oplist[opid];

		return is_fpr && (op->rd.count()>=2 || op->rd2.count()>=2 || op->rs1.count()>=2 ||  op->rs2.count()>=2 || op->rs3.count()>=2 );
	}

	//opt-in per-op explode of multi-reg fpu operands (f64 pairs, 4-wide
	//vectors) into individually allocatable f32 spans. The backend must then
	//access the elements via mapfv() in that op's emitter -- see the
	//ExplodeVec() overrides.
	virtual bool ExplodeVec(shil_opcode* op, const shil_param& prm)
	{
		return false;
	}

	void InsertRegs(set<shil_param>& l, const shil_param& regs, shil_opcode* op)
	{
		bool explode;
		if (regs.count()==2)
			explode = explode_spans || (regs.is_reg() && ExplodeVec(op,regs));
		else if (regs.count()==4)
			explode = regs.is_reg() && ExplodeVec(op,regs);
		else
			explode = false;

		if (!explode)
		{
			//overlaid registers are invisible to the span machinery; the
			//region compiler owns their residency. (multi-reg params that do
			//not explode access the context directly - the region compiler
			//never overlays their elements, see the census in CompileLoop)
			if (overlay_active && regs.is_reg() && regs.count()==1)
			{
				if (regs.is_r32i() ? IsOverlaidg(regs._reg) : IsOverlaidf(regs._reg))
					return;
			}

			l.insert(regs);
		}
		else
		{
			verify(regs.type==FMT_V4 || regs.type==FMT_V2 || regs.type==FMT_F64);

			for (u32 i=0; i<regs.count(); i++)
			{
				shil_param p=regs;
				p.type=FMT_F32;
				p._reg=(Sh4RegType)(p._reg+i);

				if (overlay_active && IsOverlaidf(p._reg))
					continue;

				l.insert(p);
			}
		}
	}

	//A span dying at opid may return its host reg to the pool before the op's
	//dests are allocated, so a dest can take it over. Only spans that are clean
	//(no writeback: OpEnd runs after the op body, and would store the new
	//owner's value) and whose final access is a plain read can donate — a span
	//ending on a write is this op's dest and is written by the op body itself.
	//The reg must also be exclusively owned: an alias partner still live at
	//this op may be read, or written back at OpEnd, from the shared reg.
	bool CanFreeEarly(RegSpan* spn, int opid)
	{
		//a whole-block span in a looping block holds its value in that register
		//across the back edge (and its preload sits in the skipped preamble),
		//so it is not free to hand it over at the last op
		if (block_loops && spn->start==0 && spn->end==block_last)
			return false;

		if (!(spn->ending(opid) && !spn->begining(opid) && !spn->aliased
			&& !spn->writeback && spn->cacc_am(opid)==AM_READ))
			return false;

		if (spn->fpr)
			return SpanNRegfIntr(opid,spn->nregf)==1;
		else
			return SpanNRegIntr(opid,spn->nreg)==1;
	}

	//takes n consecutive host fpu regs, base aligned to 2, out of the free list.
	//Regs in `avoid` are off limits: they were handed back by a span dying at
	//this very op, and a preload (which runs at OpBegin, before the op body)
	//would clobber a value that body still has to read.
	bool TakeFpuRun(deque<nregf_t>& regsf,const vector<nregf_t>& avoid,u32 n,nregf_t* out)
	{
		for (size_t i=0;i<regsf.size();i++)
		{
			u32 base=(u32)regsf[i];

			if ((base&1)!=0)
				continue;

			u32 have=0;

			for (u32 k=0;k<n;k++)
			{
				nregf_t want=(nregf_t)(base+k);

				if (std::find(avoid.begin(),avoid.end(),want)!=avoid.end())
					break;

				if (std::find(regsf.begin(),regsf.end(),want)==regsf.end())
					break;

				have++;
			}

			if (have!=n)
				continue;

			for (u32 k=0;k<n;k++)
			{
				out[k]=(nregf_t)(base+k);
				regsf.erase(std::find(regsf.begin(),regsf.end(),out[k]));
			}

			return true;
		}

		return false;
	}

	//Gives the elements of this op's exploded fpu operands consecutive, in-order
	//host registers when a free run is available -- see opt_pair_fpu_spans. Only
	//operands whose elements all start here can be placed; anything already live
	//keeps the registers it has, and a failed placement just falls through to
	//the plain one-at-a-time allocation.
	void AllocFpuRuns(shil_opcode* op,u32 opid,deque<nregf_t>& regsf,const vector<nregf_t>& freed_now)
	{
		const shil_param* prms[5]={&op->rd,&op->rd2,&op->rs1,&op->rs2,&op->rs3};

		for (int i=0;i<5;i++)
		{
			const shil_param& prm=*prms[i];

			if (!prm.is_reg() || prm.count()<2 || prm.count()>4 || !ExplodeVec(op,prm))
				continue;

			RegSpan* el[4];
			bool placeable=true;
			bool preload=false;

			//overlaid elements have no spans (the region compiler owns their
			//residency), so an operand touching any cannot take a host run
			for (u32 e=0;e<prm.count();e++)
				if (IsOverlaidf((Sh4RegType)(prm._reg+e)))
					placeable=false;

			for (u32 e=0;e<prm.count() && placeable;e++)
			{
				el[e]=FindSpan((Sh4RegType)(prm._reg+e),opid);

				if (!el[e]->begining((int)opid) || el[e]->nregf!=(nregf_t)-1)
					placeable=false;
				else
					preload|=el[e]->preload;
			}

			if (!placeable)
				continue;

			static const vector<nregf_t> none;
			nregf_t nr[4];

			if (!TakeFpuRun(regsf,preload?freed_now:none,prm.count(),nr))
				continue;

			for (u32 e=0;e<prm.count();e++)
				el[e]->nregf=nr[e];
		}
	}

	//does any op name this register at a point none of its spans covers? A
	//vector operand or an interpreter fallback moves the value through the
	//context there, so the register cannot be held across it
	bool RegTouchedOutsideSpans(RuntimeBlockInfo* block, u32 reg)
	{
		for (size_t opid=0;opid<block->oplist.size();opid++)
		{
			bool covered=false;

			for (u32 sid=0;sid<all_spans.size();sid++)
			{
				if (all_spans[sid]->regstart==reg && all_spans[sid]->contains((int)opid))
					covered=true;
			}

			if (covered)
				continue;

			shil_opcode* op=&block->oplist[opid];
			const shil_param* p[5]={&op->rd,&op->rd2,&op->rs1,&op->rs2,&op->rs3};

			for (int i=0;i<5;i++)
			{
				if (p[i]->is_reg() && reg>=(u32)p[i]->_reg &&
					reg<(u32)p[i]->_reg+p[i]->count())
					return true;
			}
		}

		return false;
	}

	//most spans of one class alive at any single op
	u32 PeakLive(RuntimeBlockInfo* block, bool fpr)
	{
		u32 peak=0;

		for (size_t opid=0;opid<block->oplist.size();opid++)
		{
			u32 cc=0;

			for (u32 sid=0;sid<all_spans.size();sid++)
			{
				if (all_spans[sid]->fpr==fpr && all_spans[sid]->contains((int)opid))
					cc++;
			}

			if (cc>peak)
				peak=cc;
		}

		return peak;
	}

	//peak liveness of one class, counting `reg` as live at every op
	u32 PeakLiveHeld(RuntimeBlockInfo* block, bool fpr, u32 reg)
	{
		u32 peak=0;

		for (size_t opid=0;opid<block->oplist.size();opid++)
		{
			u32 cc=1;	//the register we want to hold all the way through

			for (u32 sid=0;sid<all_spans.size();sid++)
			{
				RegSpan* spn=all_spans[sid];

				if (spn->fpr==fpr && spn->regstart!=reg && spn->contains((int)opid))
					cc++;
			}

			if (cc>peak)
				peak=cc;
		}

		return peak;
	}

	//A looping block wants its values to live in host registers for the whole
	//block, so the back edge has nothing to write back and nothing to reload.
	//Every register whose value never goes through the context behind a span's
	//back gets one span over the whole block -- including registers that have
	//several spans today, which is what a loop's rotating value looks like: read
	//near the top, written near the bottom, and handed between the two through
	//memory. Joining those is the whole point.
	//
	//What limits it is the host register file: a held register is live at every
	//op, and one that had to be split again would be no use here (a span cannot
	//be cut at op 0, so overshooting is not recoverable either). Registers are
	//taken most-context-traffic-first so the ones that save the most win the
	//space.
	void ExtendLoopSpans(RuntimeBlockInfo* block, u32 max_g, u32 max_f)
	{
		for (size_t opid=0;opid<block->oplist.size();opid++)
		{
			//these move the whole register file through the context
			if (IsFlushOp(block,(int)opid))
				return;
		}

		u32 last=(u32)block->oplist.size()-1;

		//one candidate per register, worth the context accesses it would save
		vector<u32> regs,saving;

		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			RegSpan* spn=all_spans[sid];
			size_t at=0;

			while (at<regs.size() && regs[at]!=spn->regstart)
				at++;

			if (at==regs.size())
			{
				regs.push_back(spn->regstart);
				saving.push_back(0);
			}

			saving[at]+=spn->preload+spn->writeback;
		}

		vector<u32> order;

		for (u32 i=0;i<regs.size();i++)
			order.push_back(i);

		std::stable_sort(order.begin(),order.end(),[&saving](u32 a,u32 b)
		{
			return saving[a]>saving[b];
		});

		for (u32 oi=0;oi<order.size();oi++)
		{
			u32 reg=regs[order[oi]];

			vector<RegSpan*> grp;

			for (u32 sid=0;sid<all_spans.size();sid++)
			{
				if (all_spans[sid]->regstart==reg)
					grp.push_back(all_spans[sid]);
			}

			if (grp.empty())
				continue;

			if (grp.size()==1 && grp[0]->start==0 && grp[0]->end==last)
				continue;

			if (RegTouchedOutsideSpans(block,reg))
				continue;

			if (PeakLiveHeld(block,grp[0]->fpr,reg)>(grp[0]->fpr?max_f:max_g))
				continue;

			//join them: the accesses of all of them, over the whole block
			std::stable_sort(grp.begin(),grp.end(),[](RegSpan* a,RegSpan* b)
			{
				return a->start<b->start;
			});

			RegSpan* keep=grp[0];

			for (u32 i=1;i<grp.size();i++)
			{
				keep->accesses.insert(keep->accesses.end(),
					grp[i]->accesses.begin(),grp[i]->accesses.end());

				for (u32 sid=0;sid<all_spans.size();sid++)
				{
					if (all_spans[sid]==grp[i])
					{
						all_spans.erase(all_spans.begin()+sid);
						break;
					}
				}

				delete grp[i];
			}

			std::stable_sort(keep->accesses.begin(),keep->accesses.end(),
				[](const RegAccess& a,const RegAccess& b){ return a.pos<b.pos; });

			keep->start=0;
			keep->end=last;
			keep->aliased=false;
			keep->preload=keep->NeedsPL();
			keep->writeback=keep->NeedsWB();
		}
	}

	//Only a span that owns its register for the whole block can ride the back
	//edge in it. Anything shorter either hands the register to another span
	//before the loop comes round, or has its value read back out of the context
	//inside the block (another span's preload, an operand that goes through
	//memory) -- and its own preload, if it has one, sits below the loop header
	//and would reload a context this path never wrote. A whole-block span has
	//none of that: nothing else can hold its register, and an operand reading
	//it from the context lands on the span as a flush, which a looping block
	//commits unconditionally.
	bool CarriedNeedsFlush(RegSpan* spn)
	{
		return spn->writeback && !(spn->start==0 && spn->end==block_last);
	}

	//Commits the values ending here that a back edge cannot carry in registers
	void FlushCarried(int opid)
	{
		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			RegSpan* spn=all_spans[sid];

			if (!spn->ending(opid) || !CarriedNeedsFlush(spn))
				continue;

			if (spn->fpr)
			{
				writeback_fpu++;
				Writeback_FPU(spn->regstart,spn->nregf);
			}
			else
			{
				writeback_gpr++;
				Writeback(spn->regstart,spn->nreg);
			}
		}
	}

	//Commits every value that is live here, whether or not this pass has seen
	//it written: on a back edge the registers carry the previous iteration's
	//writes. Used where the block can be abandoned with values still in host
	//registers -- the interrupt path takes the block's start pc and re-enters,
	//so the context has to be whole.
	void FlushLive(int opid)
	{
		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			RegSpan* spn=all_spans[sid];

			if (!spn->contains(opid) || !spn->writeback)
				continue;

			if (spn->fpr)
			{
				writeback_fpu++;
				Writeback_FPU(spn->regstart,spn->nregf);
			}
			else
			{
				writeback_gpr++;
				Writeback(spn->regstart,spn->nreg);
			}
		}
	}

	RegSpan* FindSpan(Sh4RegType reg, u32 opid)
	{
		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			if (all_spans[sid]->regstart==(u32)reg && all_spans[sid]->contains(opid))
			{
				return all_spans[sid];
			}
		}
		die("Failed to find span");
		return NULL;
	}

	void flush_span(u32 sid)
	{
		if (spans[sid]) 
		{
			spans[sid]->Flush();
			spans[sid]=0;
		}
	}
	void DoAlloc(RuntimeBlockInfo* block,const nreg_t* nregs_avail,const nregf_t* nregsf_avail)
	{
		Cleanup();
		block_last=(u32)block->oplist.size()-1;
		shil_opcode* op;
		for (size_t opid=0;opid<block->oplist.size();opid++)
		{
			op=&block->oplist[opid];

			/*
			if (op->op!=shop_readm && op->op!=shop_writem && ( op->rd.is_vector() ||op->rs1.is_vector()))
			{
			//	__asm int 3;
			}*/

			//if (op->op != shop_readm && op->op != shop_writem && op->op != shop_jcond && op->op != shop_jdyn && op->op != shop_mov32 && op->op != shop_mov64 && (op->op != shop_add || block->addr<=0x8c0DA0BA))
			if (IsFlushOp(block,(int)opid))
			{
				bool fp=false,gpr_b=false,all=false;

					#define GetN(str) ((str>>8) & 0xf)
#define GetM(str) ((str>>4) & 0xf)
#define Mask_n_m 0xF00F
#define Mask_n_m_imm4 0xF000
#define Mask_n 0xF0FF
#define Mask_none 0xFFFF
#define Mask_imm8 0xFF00
#define Mask_imm12 0xF000
#define Mask_n_imm8 0xF000
#define Mask_n_ml3bit 0xF08F
#define Mask_nh3bit 0xF1FF
#define Mask_nh2bit 0xF3FF

				if (op->op==shop_ifb)
				{
					flush_span(reg_r0);
					flush_span(reg_sr_T);
					flush_span(reg_sr_status);
					flush_span(reg_fpscr);

					for (int i=reg_gbr;i<=reg_fpul;i++)
						flush_span(i);

					switch(OpDesc[op->rs3._imm]->mask)
					{
					case Mask_imm8:
						break;



					case Mask_n_m:
					case Mask_n_m_imm4:
					case Mask_n_ml3bit:
						flush_span(GetN(op->rs3._imm));
						flush_span(GetM(op->rs3._imm));
						break;

					default:
						all=true;
						break;
					}

					if (op->rs3._imm>=0xF000)
						fp=true;

				}
				else if(op->op==shop_sync_sr)
				{
					gpr_b=true;
				}
				else if (op->op==shop_sync_fpscr)
				{
					fp=true;
				}

				if (all)
				{
					//flush
					for (int sid=0;sid<sh4_reg_count;sid++)
					{
						if (sid<reg_fr_0 || sid>reg_xf_15)
							flush_span(sid);
					}
				}

				if (fp)
				{
					//flush
					flush_span(reg_fpscr);
					flush_span(reg_old_fpscr);

					for (int sid=0;sid<16;sid++)
					{
						flush_span(reg_fr_0+sid);
						flush_span(reg_xf_0+sid);
					}
				}

				if (gpr_b)
				{
					//flush
					flush_span(reg_sr_status);
					flush_span(reg_old_sr_status);

					for (int sid=0;sid<8;sid++)
					{
						flush_span(reg_r0+sid);
						flush_span(reg_r0_Bank+sid);
					}
				}

			}
			else
			{
				set<shil_param> reg_wt;
				set<shil_param> reg_rd;

				//insert regs into sets ..
				InsertRegs(reg_wt,op->rd,op);

				InsertRegs(reg_wt,op->rd2,op);

				InsertRegs(reg_rd,op->rs1,op);

				InsertRegs(reg_rd,op->rs2,op);

				InsertRegs(reg_rd,op->rs3,op);

				set<shil_param>::iterator iter=reg_wt.begin();
				while( iter != reg_wt.end() ) 
				{
					if (reg_rd.count(*iter))
					{
						reg_rd.erase(*iter);
						{
							if ((*iter).is_reg())
							{
								//r~w
								if ((*iter).is_r32())
								{
									if (spans[(*iter)._reg]==0)
									{
										spans[(*iter)._reg] = new RegSpan((*iter),(int)opid,AM_READWRITE);
										all_spans.push_back(spans[(*iter)._reg]);
									}
									else
									{
										spans[(*iter)._reg]->Access((int)opid,AM_READWRITE);
									}
								}
								else
								{
									for (u32 i=0; i<(*iter).count(); i++)
									{
										if (spans[(*iter)._reg+i]!=0)
											spans[(*iter)._reg+i]->Flush();

										spans[(*iter)._reg+i]=0;
									}
								}
							}
						}
					}
					else
					{
						if ((*iter).is_reg())
						{
							for (u32 i=0; i<(*iter).count(); i++)
							{
								if (spans[(*iter)._reg+i]!=0)
								{
									//hack//
									//this is a bug on the current reg alloc code, affects fipr.
									//generally, vector registers aren't treated correctly
									//as groups of phy registers. I really need a better model
									//to accommodate for that on the reg alloc side of things ..
									if ((*iter).count()==1 && iter->_reg==op->rs1._reg+3)
										spans[(*iter)._reg+i]->Flush();
									else
										spans[(*iter)._reg+i]->Kill();
								}
								spans[(*iter)._reg+i]=0;
							}

							//w
							if ((*iter).is_r32())
							{
								if (spans[(*iter)._reg]!=0)
									spans[(*iter)._reg]->Kill();

								spans[(*iter)._reg]= new RegSpan((*iter),(int)opid,AM_WRITE);
								all_spans.push_back(spans[(*iter)._reg]);
							}
						}
					}
					++iter;
				}

				iter=reg_rd.begin();
				while( iter != reg_rd.end() )
				{
					//r
					if ((*iter).is_reg())
					{
						if ((*iter).is_r32())
						{
							if (spans[(*iter)._reg]==0)
							{
								spans[(*iter)._reg] = new RegSpan((*iter),(int)opid,AM_READ);
								all_spans.push_back(spans[(*iter)._reg]);
							}
							else
							{
								spans[(*iter)._reg]->Access((int)opid,AM_READ);
							}
						}
						else
						{
							//vector operand: the op reads these regs from the
							//context, so any dirty span commits here (AM_FLUSH
							//writes back at OpBegin) -- but the host register
							//still holds the value, so the span stays live and
							//later scalar reads skip the re-preload
							for (u32 i=0; i<(*iter).count(); i++)
							{
								if (spans[(*iter)._reg+i]!=0)
									spans[(*iter)._reg+i]->Access((int)opid,AM_FLUSH);
							}
						}
					}
					++iter;
				}
			
				/*
				for (int sid=0;sid<sh4_reg_count;sid++)
				{
					if (spans[sid]) 
					{
						spans[sid]->Flush();
						spans[sid]=0;
					}
				}
				*/
			}

		}

		// create register lists
		deque<nreg_t> regs;
		deque<nregf_t> regsf;

		const nreg_t* nregs=nregs_avail;

		while (*nregs!=-1)
			regs.push_back(*nregs++);

		u32 reg_cc_max_g=(u32)regs.size();

		const nregf_t* nregsf=nregsf_avail;

		while (*nregsf!=-1)
			regsf.push_back(*nregsf++);

		u32 reg_cc_max_f=(u32)regsf.size();

		if (block_loops)
			ExtendLoopSpans(block,reg_cc_max_g,reg_cc_max_f);

		//Trim span count to max reg count
		for (size_t opid=0;opid<block->oplist.size();opid++)
		{
			u32 cc_g=0;
			u32 cc_f=0;

			for (u32 sid=0;sid<all_spans.size();sid++)
			{
				if (all_spans[sid]->contains((int)opid))
				{
					if (all_spans[sid]->fpr)
						cc_f++;
					else
						cc_g++;
				}
			}

			SplitSpans(cc_g,reg_cc_max_g,false,(u32)opid);

			SplitSpans(cc_f,reg_cc_max_f,true,(u32)opid);

			if (false)
			{
				printf("After reduction ..\n");
				for (u32 sid=0;sid<all_spans.size();sid++)
				{
					RegSpan* spn=all_spans[sid];

					if (spn->contains(opid))
						printf("\t[%c]span: %d (r%d), [%d:%d],n: %d, p: %d\n",spn->cacc(opid)?'x':' ',sid,all_spans[sid]->regstart,all_spans[sid]->start,all_spans[sid]->end,all_spans[sid]->nacc(opid),all_spans[sid]->pacc(opid));
				}
			}
		}

		//Allocate the registers to the spans !
		for (size_t opid=0;opid<block->oplist.size();opid++)
		{
			RegSpan* alias_src=0;
			RegSpan* alias_dst=0;

			if (opt_alias_mov && block->oplist[opid].op==shop_mov32 &&
				(
				(block->oplist[opid].rd.is_r32i() && block->oplist[opid].rs1.is_r32i() ) ||
				(block->oplist[opid].rd.is_r32f() && block->oplist[opid].rs1.is_r32f() )
				) &&
				//overlaid regs have no spans and never alias
				!IsOverlaidg(block->oplist[opid].rd._reg)  && !IsOverlaidf(block->oplist[opid].rd._reg) &&
				!IsOverlaidg(block->oplist[opid].rs1._reg) && !IsOverlaidf(block->oplist[opid].rs1._reg))
			{
				RegSpan* x=FindSpan(block->oplist[opid].rs1._reg,(u32)opid);
				RegSpan* d=FindSpan(block->oplist[opid].rd._reg,(u32)opid);

				//The shared reg holds both values until one of them dies, so
				//neither may be written while the other is still live: src not
				//before dst's end (dst's writeback runs at OpEnd of that op,
				//and would store src's new value), and dst not until strictly
				//after src's end (src's writeback runs at OpEnd of its last op,
				//after that op's body has already stored dst into the reg).
				int swa=x->nacc_w((int)opid);

				//a dst stretched over a whole looping block does not begin here
				//any more, and the value it carries predates the mov
				if (x!=d && !x->aliased && (swa==-1 || swa>(int)d->end) &&
					d->begining((int)opid) && !d->preload)
				{
					int nwa=d->nacc_w((int)opid);

					if (nwa==-1 || nwa>(int)x->end)
					{
						verify(d->fpr==x->fpr);

						//exactly one of the pair returns the shared reg to the pool
						if (d->end>=x->end)
							x->aliased=true;
						else
							d->aliased=true;

						alias_src=x;
						alias_dst=d;
						//printf("rALIA %d from %d\n",d->regstart,x->regstart);
					}
				}
			}

			vector<nregf_t> freed_now;

			if (opt_reuse_dead)
			{
				//return regs dying at this op to the back of the pool before
				//allocating, so the op's dests (allocated from the back) take
				//them over first
				for (u32 sid=0;sid<all_spans.size();sid++)
				{
					RegSpan* spn=all_spans[sid];

					if (CanFreeEarly(spn,(int)opid))
					{
						spn->freed_early=true;

						if (spn->fpr)
						{
							regsf.push_back(spn->nregf);
							freed_now.push_back(spn->nregf);
						}
						else
							regs.push_back(spn->nreg);
					}
				}
			}

			if (opt_pair_fpu_spans)
				AllocFpuRuns(&block->oplist[opid],(u32)opid,regsf,freed_now);

			for (u32 sid=0;sid<all_spans.size();sid++)
			{
				RegSpan* spn=all_spans[sid];

				//AllocFpuRuns may have served this one already
				if (spn->fpr && spn->nregf!=(nregf_t)-1)
					continue;

				if (spn->begining((int)opid) && spn!=alias_dst)
				{
					//preloads run at OpBegin, before this op's body reads the
					//dying regs freed above: serve them from the front so the
					//just-died regs only go to write-first (dest) spans
					bool from_front=opt_reuse_dead && spn->preload;

					if (spn->fpr)
					{
						verify(regsf.size()>0);
						if (from_front)
						{
							spn->nregf=regsf.front();
							regsf.pop_front();
						}
						else
						{
							spn->nregf=regsf.back();
							regsf.pop_back();
						}
					}
					else
					{
						verify(regs.size()>0);
						if (from_front)
						{
							spn->nreg=regs.front();
							regs.pop_front();
						}
						else
						{
							spn->nreg=regs.back();
							regs.pop_back();
						}

						//printf("rALOC %d from %d\n",spn->regstart,spn->nreg);
					}
				}
			}

			if (alias_dst)
			{
				//src is allocated by now even if its own span starts at this op
				alias_dst->nreg=alias_src->nreg;
				alias_dst->nregf=alias_src->nregf;
			}

			for (u32 sid=0;sid<all_spans.size();sid++)
			{
				RegSpan* spn=all_spans[sid];

				if ( spn->ending((int)opid) && !spn->aliased && !spn->freed_early)
				{
					if (spn->fpr)
					{
						verify(regsf.size()<reg_cc_max_f);
						regsf.push_front(spn->nregf);
					}
					else
					{
						verify(regs.size()<reg_cc_max_g);
						regs.push_front(spn->nreg);
						//printf("rFREE %d from %d\n",spn->regstart,spn->nreg);
					}
				}
			}
		}

		verify(regs.size()==reg_cc_max_g);
		verify(regsf.size()==reg_cc_max_f);

	//Pipeline optimisations

#ifdef REGALLOC_PIPELINE
	//move spans earlier for preloading !
#if 1
		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			RegSpan* spn=all_spans[sid];
			
			if (spn->start==0) continue; //can't move anyway ..

			//try to move beginnings backwards
			u32 opid;
			for (opid=spn->start;opid-->0;)
			{
				if (!SpanRegIntr(opid,spn->regstart) &&  ( spn->fpr ? !SpanNRegfIntr(opid,spn->nregf) : !SpanNRegIntr(opid,spn->nreg)) && !IsRegWallOp(block,opid,spn->fpr))
					continue;
				else
					break;
			}
			opid++;

			//
			//this can cause slowdowns on aggregation of load/stores
			//we need some slack mechanism to distribute it !
			//<< this will for now blindly avoid >2preload/op
			//it still slows it down so disabled

			if (spn->start>opid)
			{
				int opid_found=-1;
				int opid_plc=60;

				int slack=spn->start-opid;

				while (spn->start>opid)
				{
					if (SpanPLD(opid)<opid_plc)
					{
						opid_found=opid;
						opid_plc=SpanPLD(opid);
					}
					if (opid_found!=-1 && (spn->start-opid)<=1 && opid_plc<2)
						break;
					
					opid++;
				}

				bool do_move= false;
				
				if (opid_found!=-1)
				{
					do_move=true;
					do_move &= opid_plc<3;
				}

				if ( do_move)
				{
					printf("Span PLD is movable by %d, moved by %d w/ %d!!\n",slack,spn->start-opid_found,opid_plc);
					spn->start=opid_found;
				}
				else
				{
					printf("Span PLD is movable by %d but  %d -> not moved :(\n",slack,opid_plc);
				}
			}
		}
#endif

	//move spans later for post-saving !
#if 1
		u32 blk_last=block->oplist.size()-1;

		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			RegSpan* spn=all_spans[sid];
			
			if (spn->end==blk_last) continue; //can't move anyway ..

			//try to move beginnings backwards
			u32 opid;
			for (opid=spn->end+1;opid<=blk_last;opid++)
			{
				if (!SpanRegIntr(opid,spn->regstart) &&  ( spn->fpr ? !SpanNRegfIntr(opid,spn->nregf) : !SpanNRegIntr(opid,spn->nreg)) && !IsRegWallOp(block,opid,spn->fpr))
					continue;
				else
					break;
			}
			opid--;

			//
			//this can cause slowdowns on aggregation of load/stores
			//we need some slack mechanism to distribute it !
			//<< this will for now blindly avoid >1preload/op
			//it still slows it down so disabled

			if (spn->end<opid)
			{
				int opid_found=-1;
				int opid_plc=60;

				int slack=opid-spn->end;

				while (spn->end<opid)
				{
					if (SpanWB(opid)<opid_plc)
					{
						opid_found=opid;
						opid_plc=SpanWB(opid);
					}

					if (opid_found!=-1 && (opid-spn->end)<=1 && opid_plc<2)
						break;
					
					opid--;
				}

				bool do_move= false;

				if (opid_found!=-1)
				{
					do_move=true;
					do_move &= opid_plc<3;
				}

				if ( do_move)
				{
					printf("Span WB is movable by %d, moved by %d w/ %d!!\n",slack,opid_found-spn->end,opid_plc);
					spn->end=opid_found;
				}
				else
				{
					printf("Span WB is movable by %d but  %d -> not moved :(\n",slack,opid_plc);
				}
			}
		}
#endif

#endif
		//printf("%d spills\n",spills);
	}


	//A span can be cut at an op that doesn't use its host register: one it
	//doesn't access at all, or a flush -- there the operand is read from the
	//context, so ending the span at its previous access (which writes back any
	//dirty value) and re-loading it at the next one is transparent. Cutting
	//needs a previous access to end on.
	bool CanSplit(RegSpan* spn, u32 opid)
	{
		AccessMode am=spn->cacc_am(opid);

		return (am==AM_NONE || am==AM_FLUSH) && spn->pacc(opid)!=-1;
	}

	void SplitSpans(u32 cc,u32 reg_cc_max ,bool fpr,u32 opid)
	{
		bool was_large=false;	//this control prints

		while (cc>reg_cc_max)
		{
			if (was_large)
				printf("Opcode: %d, %d active spans\n",opid,cc);

			RegSpan* last_pacc=0;
			RegSpan* last_nacc=0;

			for (u32 sid=0;sid<all_spans.size();sid++)
			{
				RegSpan* spn=all_spans[sid];

				if (spn->contains(opid) && fpr==spn->fpr)
				{
					if (CanSplit(spn,opid))
					{
						//a span with no further use is the cheapest to cut: it
						//just ends early, no reload
						if (!last_nacc || spn->nacc_use(opid)==-1 ||
							(last_nacc->nacc_use(opid)!=-1 && spn->nacc_use(opid)>last_nacc->nacc_use(opid)))
							last_nacc=spn;

						if (!last_pacc || spn->pacc(opid)<last_pacc->pacc(opid))
							last_pacc=spn;
					}
					if (was_large)
						printf("\t[%c]span: %d (r%d), [%d:%d],n: %d, p: %d\n",spn->cacc(opid)?'x':' ',sid,all_spans[sid]->regstart,all_spans[sid]->start,all_spans[sid]->end,all_spans[sid]->nacc(opid),all_spans[sid]->pacc(opid));
				}
			}

			//nothing left to cut: this op alone needs more registers than the
			//host has
			verify(last_nacc!=0);

			//printf("Last pacc: %d, vlen %d | reg r%d\n",last_pacc->nacc(opid)-opid,last_pacc->nacc(opid)-last_pacc->pacc(opid),last_pacc->regstart);
			//printf("Last nacc: %d, vlen %d | reg r%d\n",last_nacc->nacc(opid)-opid,last_nacc->nacc(opid)-last_nacc->pacc(opid),last_nacc->regstart);

			//the tail picks up at the next access that needs a register; any
			//flush in between is served from the context
			int tail=last_nacc->nacc_use(opid);

			//the tail becomes a span of its own, re-loaded at its first access
			//(the context is current: the head writes back at its own end)
			if (tail!=-1)
			{
				RegSpan* spn= new RegSpan(*last_nacc);
				spn->start=tail;

				spn->trim_access();

				spn->preload=spn->NeedsPL();
				spn->writeback=spn->NeedsWB();

				//add it to the span list !
				all_spans.push_back(spn);
				spills++;
			}

			last_nacc->end=last_nacc->pacc(opid);

			//trim the access arrays as required ..
			last_nacc->trim_access();

			last_nacc->writeback=last_nacc->NeedsWB();

			cc--;
		}
	}

	int SpanRegIntr(int opid,u32 reg)
	{
		verify(reg!=-1);
		int cc=0;
		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			if (all_spans[sid]->regstart==reg && all_spans[sid]->contains(opid))
				cc++;
		}
		return cc;
	}

	int SpanNRegIntr(int opid,nreg_t nreg)
	{
		verify(nreg!=-1);
		int cc=0;
		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			if (all_spans[sid]->nreg==nreg && all_spans[sid]->contains(opid))
				cc++;
		}
		return cc;
	}

	int SpanNRegfIntr(int opid,nregf_t nregf)
	{
		verify(nregf!=-1);
		int cc=0;
		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			if (all_spans[sid]->nregf==nregf && all_spans[sid]->contains(opid))
				cc++;
		}
		return cc;
	}

	int SpanPLD(int opid)
	{
		int rv=0;

		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			RegSpan* spn=all_spans[sid];

			if (spn->preload && spn->begining(opid))
				rv++;
		}

		return rv;
	}

	int SpanWB(int opid)
	{
		int rv=0;

		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			RegSpan* spn=all_spans[sid];

			if (spn->writeback && spn->ending(opid))
				rv++;
		}

		return rv;
	}

	void OpBegin(shil_opcode* op,int opid)
	{
		OpBeginImpl(op,opid,0);
	}

	//A looping block splits op 0's preloads in two. The ones whose span lasts
	//the whole block go in the preamble, above the loop header: the back edge
	//keeps those values in their registers. The rest die inside the block, so
	//their register is handed to someone else before the back edge and they
	//have to be loaded again every iteration -- those go below the header.
	void OpBeginCarried(shil_opcode* op,int opid)
	{
		OpBeginImpl(op,opid,1);
	}

	void OpBeginTransient(shil_opcode* op,int opid)
	{
		OpBeginImpl(op,opid,2);
	}

	void OpBeginImpl(shil_opcode* op,int opid,int part)
	{
		current_opid=opid;

		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			RegSpan* spn=all_spans[sid];

			if (part!=0)
			{
				bool carried=spn->end==block_last;

				if ((part==1)!=carried)
					continue;
			}

			if (spn->begining(current_opid) && spn->preload)
			{
				if (spn->fpr)
				{
					//printf("Op %d: Preloading f%d to %d\n",current_opid,spn->regstart,spn->nregf);
					preload_fpu++;
					Preload_FPU(spn->regstart,spn->nregf);
				}
				else
				{
					//printf("Op %d: Preloading r%d to %d\n",current_opid,spn->regstart,spn->nreg);
					preload_gpr++;
					Preload(spn->regstart,spn->nreg);
				}
			}
			//in a looping block the register may carry a write from a previous
			//iteration, which this pass has not seen yet: commit unconditionally
			else if (spn->cacc_am(current_opid)==AM_FLUSH &&
				(spn->dirty || (block_loops && spn->NeedsWB())))
			{
				//a vector operand reads this reg from the context in the op
				//body: commit the dirty value now, span stays live
				if (spn->fpr)
				{
					writeback_fpu++;
					Writeback_FPU(spn->regstart,spn->nregf);
				}
				else
				{
					writeback_gpr++;
					Writeback(spn->regstart,spn->nreg);
				}
				spn->dirty=false;
			}
		}
	}

	void OpEnd(shil_opcode* op)
	{
		OpEndImpl(op,false);
	}

	//The loop-path variant, for the last op of a block that branches back to
	//its own start: the ending spans keep their values in registers instead of
	//writing them back, except the ones the next iteration would re-read from
	//the context.
	void OpEndLoop(shil_opcode* op)
	{
		OpEndImpl(op,true);
	}

	void OpEndImpl(shil_opcode* op, bool loop)
	{
		for (u32 sid=0;sid<all_spans.size();sid++)
		{
			RegSpan* spn=all_spans[sid];

			//the op body just ran: writes leave the host reg ahead of ctx
			if (spn->cacc_am(current_opid)&AM_WRITE)
				spn->dirty=true;

			bool wb=spn->ending(current_opid) && spn->writeback && spn->dirty;

			//a carried value may have been written on an earlier iteration, so
			//dirty says nothing here; what matters is whether it has to be in
			//the context for the next one
			if (loop && spn->ending(current_opid))
				wb=CarriedNeedsFlush(spn);

			//skip the end writeback when an AM_FLUSH already committed the
			//final value (dirty==false); Kill() still suppresses via writeback
			if (wb)
			{
				if (spn->fpr)
				{
					//printf("Op %d: Writing back f%d from %d\n",current_opid,spn->regstart,spn->nregf);
					writeback_fpu++;
					Writeback_FPU(spn->regstart,spn->nregf);
				}
				else
				{
					//printf("Op %d: Writing back r%d from %d\n",current_opid,spn->regstart,spn->nreg);
					writeback_gpr++;
					Writeback(spn->regstart,spn->nreg);
				}
			}
		}
	}

	void Cleanup()
	{
		writeback_gpr=writeback_fpu=0;
		preload_gpr=preload_fpu=0;

		for (int sid=0;sid<sh4_reg_count;sid++)
		{
			if (spans[sid]) 
				spans[sid]=0;
		}

		for (size_t sid=0;sid<all_spans.size();sid++)
			delete all_spans[sid];

		all_spans.clear();
	}

	virtual nregf_t FpuMap(u32 reg) { return (nregf_t)-1; }

	virtual void Preload(u32 reg,nreg_t nreg)=0;
	virtual void Writeback(u32 reg,nreg_t nreg)=0;

	virtual void Preload_FPU(u32 reg,nregf_t nreg)=0;
	virtual void Writeback_FPU(u32 reg,nregf_t nreg)=0;
};
