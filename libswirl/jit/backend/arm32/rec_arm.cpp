/*
	This file is part of libswirl
*/
#include "license/bsd"


#include <unistd.h>
#include <sys/mman.h>
#include "types.h"

#if FEAT_SHREC == DYNAREC_JIT
#include "hw/sh4/sh4_opcode_list.h"

#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/sh4_mem.h"

#include "ngen_arm.h"

/*

	ARM ABI
		r0~r1: scratch, params, return
		r2~r3: scratch, params
		8 regs, v6 is platform dependent
			r4~r11
		r12 is "The Intra-Procedure-call scratch register"
		r13 stack
		r14 link
		r15 pc

		Registers f0-s15 (d0-d7, q0-q3) do not need to be preserved (and can be used for passing arguments or returning results in standard procedure-call variants).
		Registers s16-s31 (d8-d15, q4-q7) must be preserved across subroutine calls;
		Registers d16-d31 (q8-q15), if present, do not need to be preserved.

	Block linking
	Reg alloc
		r0~r4: scratch
		r5,r6,r7,r10,r11: allocated
		r8: sh4 cntx
		r9: cycle counter

	Callstack cache

	fpu reg alloc
	d8:d15, single storage

*/

struct DynaRBI: RuntimeBlockInfo
{
	virtual u32 Relink();
	virtual void Relocate(void* dst)
	{

	}
};


#ifdef _ANDROID
#include <sys/syscall.h>  // for cache flushing.
#endif

#if HOST_OS == OS_DARWIN
#include <libkern/OSCacheControl.h>
void CacheFlush(void* code, void* pEnd)
{
    sys_dcache_flush(code, (u8*)pEnd - (u8*)code + 1);
    sys_icache_invalidate(code, (u8*)pEnd - (u8*)code + 1);
}
#elif !defined(ARMCC)
void CacheFlush(void* code, void* pEnd)
{
#if !defined(_ANDROID) && HOST_OS!=OS_DARWIN
	__builtin___clear_cache((char*)code, (char*)pEnd);
#else
	void* start=code;
	size_t size=(u8*)pEnd-(u8*)start+4;

  // Ideally, we would call
  //   syscall(__ARM_NR_cacheflush, start,
  //           reinterpret_cast<intptr_t>(start) + size, 0);
  // however, syscall(int, ...) is not supported on all platforms, especially
  // not when using EABI, so we call the __ARM_NR_cacheflush syscall directly.

  register uint32_t beg asm("a1") = reinterpret_cast<uint32_t>(start);
  register uint32_t end asm("a2") = reinterpret_cast<uint32_t>(start) + size;
  register uint32_t flg asm("a3") = 0;

  #ifdef __ARM_EABI__
    #if defined (__arm__) && !defined(__thumb__)
      // __arm__ may be defined in thumb mode.
      register uint32_t scno asm("r7") = __ARM_NR_cacheflush;
      asm volatile(
          "svc 0x0"
          : "=r" (beg)
          : "0" (beg), "r" (end), "r" (flg), "r" (scno));
    #else
      // r7 is reserved by the EABI in thumb mode.
      asm volatile(
      "@   Enter ARM Mode  \n\t"
          "adr r3, 1f      \n\t"
          "bx  r3          \n\t"
          ".ALIGN 4        \n\t"
          ".ARM            \n"
      "1:  push {r7}       \n\t"
          "mov r7, %4      \n\t"
          "svc 0x0         \n\t"
          "pop {r7}        \n\t"
      "@   Enter THUMB Mode\n\t"
          "adr r3, 2f+1    \n\t"
          "bx  r3          \n\t"
          ".THUMB          \n"
      "2:                  \n\t"
          : "=r" (beg)
          : "0" (beg), "r" (end), "r" (flg), "r" (__ARM_NR_cacheflush)
          : "r3");
    #endif
  #else
    #if defined (__arm__) && !defined(__thumb__)
      // __arm__ may be defined in thumb mode.
      asm volatile(
          "svc %1"
          : "=r" (beg)
          : "i" (__ARM_NR_cacheflush), "0" (beg), "r" (end), "r" (flg));
    #else
      // Do not use the value of __ARM_NR_cacheflush in the inline assembly
      // below, because the thumb mode value would be used, which would be
      // wrong, since we switch to ARM mode before executing the svc instruction
      asm volatile(
      "@   Enter ARM Mode  \n\t"
          "adr r3, 1f      \n\t"
          "bx  r3          \n\t"
          ".ALIGN 4        \n\t"
          ".ARM            \n"
      "1:  svc 0x9f0002    \n"
      "@   Enter THUMB Mode\n\t"
          "adr r3, 2f+1    \n\t"
          "bx  r3          \n\t"
          ".THUMB          \n"
      "2:                  \n\t"
          : "=r" (beg)
          : "0" (beg), "r" (end), "r" (flg)
          : "r3");
    #endif
  #endif
	#if 0
		const int syscall = 0xf0002;
		__asm __volatile (
			"mov     r0, %0\n"
			"mov     r1, %1\n"
			"mov     r7, %2\n"
			"mov     r2, #0x0\n"
			"svc     0x00000000\n"
			:
			:   "r" (code), "r" (pEnd), "r" (syscall)
			:   "r0", "r1", "r7"
			);
	#endif
#endif
}
#else
asm void CacheFlush(void* code, void* pEnd)
{
	ARM
	push {r7}
	//add r1, r1, r0
	mov r7, #0xf0000
	add r7, r7, #0x2
	mov r2, #0x0
	svc #0x0
	pop {r7}
	bx lr
}
#endif

#define _DEVEL 1

#include "jit/emitter/arm32/arm_emitter.h"
using namespace ARM;


// These have to be declared somewhere or linker dies
u8* ARM::emit_opt=0;
eReg ARM::reg_addr;
eReg ARM::reg_dst;
s32 ARM::imma;


typedef ConditionCode eCC;

#define EmitAPI		\
	inline static void

#define lr_r14 r14
#define rfp_r9 r9


typedef void FPBinOP        (eFSReg Sd, eFSReg Sn, eFSReg Sm, ConditionCode CC);
typedef void FPUnOP         (eFSReg Sd, eFSReg Sm,            ConditionCode CC);
typedef void BinaryOP       (eReg Rd, eReg Rn, eReg Rm,       ConditionCode CC);
typedef void BinaryOPImm    (eReg Rd, eReg Rn, s32 sImm8,     ConditionCode CC);
typedef void UnaryOP        (eReg Rd, eReg Rs);

// you pick reg, loads Base with reg addr, no reg. mapping yet !
void LoadSh4Reg_mem(eReg Rt, u32 Sh4_Reg, eCC CC=CC_AL)
{
	const u32 shRegOffs = (u8*)GetRegPtr(Sh4_Reg)-sh4_dyna_rcb ;

	LDR(Rt, r8, shRegOffs, Offset, CC);
}


// you pick regs, loads Base with reg addr, no reg. mapping yet !
// data should already exist for Rt !
void StoreSh4Reg_mem(eReg Rt,u32 Sh4_Reg, eCC CC=CC_AL)
{
	const u32 shRegOffs = (u8*)GetRegPtr(Sh4_Reg)-sh4_dyna_rcb ;

	STR(Rt, r8, shRegOffs, Offset, CC);
}


EmitAPI LoadSh4Reg_mem(eReg Rt, shil_param Sh4_Reg, eCC CC=CC_AL)
{
	if (!Sh4_Reg.is_r32())
		printf("REG FAIL: %d\n",Sh4_Reg._reg);
	verify(Sh4_Reg.is_r32());
	LoadSh4Reg_mem(Rt,Sh4_Reg._reg,CC);
}

EmitAPI StoreSh4Reg_mem(eReg Rt, shil_param Sh4_Reg, eCC CC=CC_AL)
{
	verify(Sh4_Reg.is_r32());
	StoreSh4Reg_mem(Rt,Sh4_Reg._reg,CC);
}


EmitAPI LoadSh4Reg64(eReg Rt, shil_param Sh4_Reg, eCC CC=CC_AL)
{
	verify(Sh4_Reg.is_r64());
	//possibly use ldm/ldrd ?
	LoadSh4Reg_mem(Rt,Sh4_Reg._reg,CC);
	LoadSh4Reg_mem((eReg)(Rt+1),Sh4_Reg._reg+1,CC);
}

EmitAPI StoreSh4Reg64(eReg Rt, shil_param Sh4_Reg, eCC CC=CC_AL)
{
	verify(Sh4_Reg.is_r64());
	//possibly use stm/strd ?
	StoreSh4Reg_mem(Rt,Sh4_Reg._reg,CC);
	StoreSh4Reg_mem((eReg)(Rt+1),Sh4_Reg._reg+1,CC);
}


#include "hw/sh4/dyna/regalloc.h"

#if HOST_OS == OS_DARWIN
eReg alloc_regs[]={r5,r6,r7,r10,(eReg)-1};
#else
eReg alloc_regs[]={r5,r6,r7,r10,r11,(eReg)-1};
#endif
eFSReg alloc_fpu[]={f16,f17,f18,f19,f20,f21,f22,f23,
					f24,f25,f26,f27,f28,f29,f30,f31,(eFSReg)-1};

//blocks with ftrv/fipr reserve f28..f31 as backend scratch ("fpu temps"):
//accumulators for the vector ops, and a scratch bank that lets the other
//emitters stay out of s0-s15, where the xmtrx may be cached
eFSReg alloc_fpu_temps[]={f16,f17,f18,f19,f20,f21,f22,f23,
					f24,f25,f26,f27,(eFSReg)-1};

bool fpu_temps_avail;	//f28-f31 are backend scratch for this block
bool xmtrx_resident;	//s0-s15 currently hold xf0-xf15

struct arm_reg_alloc: RegAlloc<eReg,eFSReg,false>
{
	arm_reg_alloc()
	{
		//every emitter in this backend reads its mapped sources before writing
		//any mapped dest, so dests may share a host reg with a dying source
		opt_alias_mov=true;
		opt_reuse_dead=true;

		//s2k/s2k+1 are the halves of dk: exploded f64/fv operands in an
		//in-order pair move to and from the context in one 64 bit access
		opt_pair_fpu_spans=true;
	}

	//Preload_FPU/Writeback_FPU queue up while an op's preamble/postamble is
	//emitted, so an aligned sh4 pair sitting in an in-order host pair can go
	//as a single vldr.64/vstr.64 instead of two 32 bit ones
	struct CtxMove
	{
		u32 reg;
		eFSReg nreg;
	};

	vector<CtxMove> pend_ld,pend_st;
	bool batching=false;

	static bool pairable(const CtxMove& lo, const CtxMove& hi)
	{
		//consecutive fpu regs starting on an even one (so they are contiguous
		//in the context and inside the same bank), in an even/odd host pair
		return hi.reg==lo.reg+1 && ((lo.reg-reg_fr_0)&1)==0 &&
			(lo.nreg&1)==0 && hi.nreg==(eFSReg)(lo.nreg+1);
	}

	void EmitMoves(vector<CtxMove>& l, bool store)
	{
		std::sort(l.begin(),l.end(),[](const CtxMove& a,const CtxMove& b){ return a.reg<b.reg; });

		for (size_t i=0;i<l.size();)
		{
			const s32 ofs=((u8*)GetRegPtr(l[i].reg)-sh4_dyna_rcb)/4;

			if (i+1<l.size() && pairable(l[i],l[i+1]))
			{
				eFDReg d=(eFDReg)(l[i].nreg/2);

				if (store)
					VSTR(d,r8,ofs);
				else
					VLDR(d,r8,ofs);

				i+=2;
			}
			else
			{
				if (store)
					VSTR(l[i].nreg,r8,ofs);
				else
					VLDR(l[i].nreg,r8,ofs);

				i++;
			}
		}

		l.clear();
	}

	void OpBegin(shil_opcode* op,int opid)
	{
		batching=true;
		RegAlloc<eReg,eFSReg,false>::OpBegin(op,opid);
		batching=false;

		FlushOpBegin();
	}

	void OpBeginCarried(shil_opcode* op,int opid)
	{
		batching=true;
		RegAlloc<eReg,eFSReg,false>::OpBeginCarried(op,opid);
		batching=false;

		FlushOpBegin();
	}

	void OpBeginTransient(shil_opcode* op,int opid)
	{
		batching=true;
		RegAlloc<eReg,eFSReg,false>::OpBeginTransient(op,opid);
		batching=false;

		FlushOpBegin();
	}

	void FlushOpBegin()
	{
		//the flushes first: the op body reads those operands from the context
		EmitMoves(pend_st,true);
		EmitMoves(pend_ld,false);
	}

	void OpEnd(shil_opcode* op)
	{
		batching=true;
		RegAlloc<eReg,eFSReg,false>::OpEnd(op);
		batching=false;

		EmitMoves(pend_st,true);
	}

	void FlushCarried(int opid)
	{
		batching=true;
		RegAlloc<eReg,eFSReg,false>::FlushCarried(opid);
		batching=false;

		EmitMoves(pend_st,true);
	}

	void FlushLive(int opid)
	{
		batching=true;
		RegAlloc<eReg,eFSReg,false>::FlushLive(opid);
		batching=false;

		EmitMoves(pend_st,true);
	}

	//ftrv/fipr consume their fv operands element-wise via mapfv(); their
	//emitters require this (no memory fallback), see shop_ftrv/shop_fipr
	virtual bool ExplodeVec(shil_opcode* op, const shil_param& prm)
	{
		if ((op->op==shop_ftrv || op->op==shop_fipr) && prm.count()==4)
			return true;

		//f64 pairs that are pure register traffic on the fpu side: the halves
		//become two allocated singles, so fmov DRm,DRn is (at most) two vmovs
		//and the 64-bit memops/fsca move their value between the d0/d14
		//staging register and the mapped halves instead of the context. The
		//emitters keep their memory paths, so this stays a pure optimisation.
		//(the only f64 param of a memop is its value: rd for readm, rs2 for
		//writem -- the addresses are i32/imm)
		if (prm.is_r64f() &&
			(op->op==shop_mov64 || op->op==shop_readm || op->op==shop_writem ||
			 op->op==shop_fsca))
			return true;

		return false;
	}

	virtual eFSReg FpuMap(u32 reg)
	{
		if (reg>=reg_fr_0 && reg<=reg_fr_15)
		{
			return alloc_fpu[reg-reg_fr_0];
		}
		else
			return (eFSReg)-1;
	}

	virtual void Preload(u32 reg,eReg nreg)
	{
		verify(reg!=reg_pc_dyn);
		LoadSh4Reg_mem(nreg,reg);
	}
	virtual void Writeback(u32 reg,eReg nreg)
	{
		if (reg==reg_pc_dyn)
			;//MOV(r4,nreg);
		else
			StoreSh4Reg_mem(nreg,reg);
	}

	virtual void Preload_FPU(u32 reg,eFSReg nreg)
	{
		CtxMove m={reg,nreg};

		if (batching)
		{
			pend_ld.push_back(m);
			return;
		}

		const s32 shRegOffs = (u8*)GetRegPtr(reg)-sh4_dyna_rcb ;

		VLDR((nreg),r8,shRegOffs/4);
	}
	virtual void Writeback_FPU(u32 reg,eFSReg nreg)
	{
		CtxMove m={reg,nreg};

		if (batching)
		{
			pend_st.push_back(m);
			return;
		}

		const s32 shRegOffs = (u8*)GetRegPtr(reg)-sh4_dyna_rcb ;

		VSTR((nreg),r8,shRegOffs/4);
	}
	/*
	eFSReg fd0_to_fs(eFDReg fd0)
	{
		eFSReg rv=(eFSReg)(fd0*2);
		verify(rv<32);
		return rv;
	}
	*/
	eFSReg mapfs(const shil_param& prm)
	{
		return mapf(prm);
	}

};


arm_reg_alloc reg;


u32 blockno=0;

#include <map>

map<shilop,ConditionCode> ccmap;
map<shilop,ConditionCode> ccnmap;

//a branch to the instruction right after itself is a fallthrough: emit a NOP
//instead, saving the redirect when the target block sits physically next in
//the code buffer. The slot stays 4 bytes, so a later Relink() can still turn
//it back into a real branch (or an unlink stub call).
static void JUMP_or_fallthrough(u32 target)
{
	if (target == (u32)EMIT_GET_PTR() + 4)
		NOP();
	else
		JUMP(target);
}

//A block whose taken edge is its own start is a loop. Everything the ops need
//can stay in host registers across the back edge, so the loop path skips both
//the end writebacks and the entry preloads -- see emit_self_loop_tail.
//
//Gated on the block never moving the register file through the context behind
//a span's back: interpreter fallbacks and the sync ops do that wholesale, and
//a vector operand at op 0 would need its commit inside the skipped preamble.
static bool block_self_loops(RuntimeBlockInfo* block)
{
	if (!settings.dynarec.opt_self_loops || block->oplist.empty())
		return false;

	if (block->BlockType!=BET_Cond_0 && block->BlockType!=BET_Cond_1)
		return false;

	if (block->BranchBlock!=block->addr)
		return false;

	for (size_t i=0;i<block->oplist.size();i++)
	{
		shil_opcode* op=&block->oplist[i];

		if (op->op==shop_ifb || op->op==shop_sync_sr || op->op==shop_sync_fpscr)
			return false;

		if (i!=0)
			continue;

		//A vector operand reads its registers from the context, so a span
		//holding one of them commits it first -- and at op 0 that commit lands
		//in the preamble, which later iterations skip. Nothing precedes op 0,
		//so a span can only exist there if this same op names the register
		//somewhere it can be allocated: that is what rules the block out, not
		//the vector operand by itself (an ftrv reading an unallocated xmtrx is
		//fine, and that is what a loop full of them looks like).
		const shil_param* rd[3]={&op->rs1,&op->rs2,&op->rs3};
		const shil_param* all[5]={&op->rd,&op->rd2,&op->rs1,&op->rs2,&op->rs3};

		for (int p=0;p<3;p++)
		{
			if (!rd[p]->is_reg() || rd[p]->count()<=1 || reg.ExplodeVec(op,*rd[p]))
				continue;

			for (u32 e=0;e<rd[p]->count();e++)
			{
				u32 ra=(u32)rd[p]->_reg+e;

				for (int q=0;q<5;q++)
				{
					const shil_param* o=all[q];

					if (o==rd[p] || !o->is_reg())
						continue;

					if (o->count()!=1 && !reg.ExplodeVec(op,*o))
						continue;	//not allocatable either, no span

					if (ra>=(u32)o->_reg && ra<(u32)o->_reg+o->count())
						return false;
				}
			}
		}
	}

	return true;
}

//The condition under which the block takes its branch -- for a self loop, the
//condition to go around again. Mirrors DynaRBI::Relink's selection, except
//sr_T may still be in a host register here: the writebacks come after.
static ConditionCode self_loop_cc(RuntimeBlockInfo* block)
{
	shil_opcode* last=&block->oplist[block->oplist.size()-1];

	if (!block->has_jcond && last->rd._reg==reg_sr_T && ccmap.count(last->op))
		return (block->BlockType&1)==1?ccmap[last->op]:ccnmap[last->op];

	if (block->has_jcond)
		CMP(r4,(block->BlockType&1));
	else if (reg.IsAllocg(reg_sr_T))
		CMP(reg.mapg(reg_sr_T),(block->BlockType&1));
	else
	{
		LoadSh4Reg_mem(r0,reg_sr_T);
		CMP(r0,(block->BlockType&1));
	}

	return CC_EQ;
}

//Emitted after the last op's body, before its writebacks:
//
//    <condition>
//    b   exit, !cc          ; leaving the loop: the writebacks and the relink
//    <commit what cannot be carried>
//    subs r9, r9, #cycles   ; one poll per iteration, like a block entry
//    bgt loop_header        ; the whole point: no writebacks, no preloads
//    <commit everything live>
//    bl  intc_sched         ; may not return -- it takes the interrupt at this
//    b   loop_header        ; block's start pc, which is where the loop is
//  exit:
//
//The context only has to be whole where the block can be abandoned, so the
//commits sit on the two cold paths. Doing this at the back edge rather than at
//loop_header is what makes them safe: by here every span has been written at
//least once, while at the top of the first iteration a write-first span still
//holds whatever the last block left in the register.
static void emit_self_loop_tail(RuntimeBlockInfo* block, u32 loop_header, u32 cyc)
{
	int last=(int)block->oplist.size()-1;

	ConditionCode cc=self_loop_cc(block);

	u32* exit_slot=(u32*)EMIT_GET_PTR();
	NOP();	//the exit branch, once its target is known

	reg.FlushCarried(last);

#if HOST_OS == OS_DARWIN
	SUB(r11,r11,cyc,true,CC_AL);
#else
	SUB(rfp_r9,rfp_r9,cyc,true,CC_AL);
#endif
	JUMP(loop_header,CC_GT);

	reg.FlushLive(last);
	CALL((u32)intc_sched);
	JUMP(loop_header);

	u32 exit=(u32)EMIT_GET_PTR();

	verify(emit_ptr==0);
	emit_ptr=exit_slot;
	JUMP(exit,(ConditionCode)(cc^1));
	verify(emit_ptr==exit_slot+1);
	emit_ptr=0;
}

u32 DynaRBI::Relink()
{
	verify(emit_ptr==0);
	u8* code_start=(u8*)code+relink_offset;
	emit_ptr=(u32*)code_start;

	switch(BlockType)
	{
	case BET_Cond_0:
	case BET_Cond_1:
	{
		//quick opt here:
		//peek into reg alloc, store actuall sr_T register to relink_data
		bool last_op_sets_flags=!has_jcond && oplist.size() > 0 && 
			oplist[oplist.size()-1].rd._reg==reg_sr_T && ccmap.count(oplist[oplist.size()-1].op);

		ConditionCode CC=CC_EQ;

		if (last_op_sets_flags)
		{
			shilop op=oplist[oplist.size()-1].op;

			verify(ccmap.count(op)>0);

			if ((BlockType&1)==1)
				CC=ccmap[op];
			else
				CC=ccnmap[op];
		}
		else
		{
			if (!has_jcond)
			{
				printf("SLOW COND PATH %d\n",oplist[oplist.size()-1].op);
				LoadSh4Reg_mem(r4,reg_sr_T);
			}

			CMP(r4,(BlockType&1));
		}

		if (pBranchBlock)
			JUMP((u32)pBranchBlock->code,CC);
		else
			CALL((u32)ngen_LinkBlock_cond_Branch_stub,CC);

		if (pNextBlock)
			JUMP_or_fallthrough((u32)pNextBlock->code);
		else
			CALL((u32)ngen_LinkBlock_cond_Next_stub);
		break;
	}


	case BET_DynamicRet:
	case BET_DynamicCall:
	case BET_DynamicJump:
    {
#ifdef CALLSTACK
#error offset broken
		SUB(r2, r8, -FPCB_OFFSET);
#if RAM_SIZE_MAX == 33554432
		UBFX(r1, r4, 1, 24);
#else
		UBFX(r1, r4, 1, 23);
#endif

		if (BlockType==BET_DynamicRet)
		{
			LDR(r14,r2,r1,Offset,true,S_LSL,2);
			BX(R14);    //BX LR (ret hint)
		}
		else if (BlockType==BET_DynamicCall)
		{
			LDR(r0,r2,r1,Offset,true,S_LSL,2);
			BLX(r0);    //BLX r0 (call hint)
		}
		else
		{
			LDR(r15,r2,r1,Offset,true,S_LSL,2);
		}
#else
		if (relink_data==0)
		{
#if 1
			//this is faster
			//why ? (Icache ?)
			SUB(r2, r8, -FPCB_OFFSET);
#if RAM_SIZE_MAX == 33554432
			UBFX(r1, r4, 1, 24);
#else
			UBFX(r1, r4, 1, 23);
#endif
			LDR(r15,r2,r1,Offset,true,S_LSL,2);
			
#else
			if (pBranchBlock)
			{
				MOV32(r1,pBranchBlock->addr);           //2
				CMP(r4,r1);                             //1
				JUMP((unat)pBranchBlock->code,CC_EQ);   //1
				CALL((unat)ngen_LinkBlock_Generic_stub);//1
			}
			else
			{
				SUB(r2, r8, -FPCB_OFFSET);

#if RAM_SIZE_MAX == 33554432
				UBFX(r1, r4, 1, 24);
#else
				UBFX(r1, r4, 1, 23);
#endif
				NOP();NOP();                            //2
				LDR(r15,r2,r1,Offset,true,S_LSL,2);     //1
			}
#endif
		}
		else
		{
			verify(pBranchBlock==0);
			SUB(r2, r8, -FPCB_OFFSET);

#if RAM_SIZE_MAX == 33554432
			UBFX(r1, r4, 1, 24);
#else
			UBFX(r1, r4, 1, 23);
#endif
			LDR(r15,r2,r1,Offset,true,S_LSL,2);
		}
#endif
		break;
	}

	case BET_StaticCall:
	case BET_StaticJump:
	{
		if (pBranchBlock==0)
			CALL((u32)ngen_LinkBlock_Generic_stub);
		else
		{
#ifdef CALLSTACK
			if (BlockType==BET_StaticCall)
				CALL((u32)pBranchBlock->code);
			else
#endif
				JUMP_or_fallthrough((u32)pBranchBlock->code);
		}
		break;
	}

	case BET_StaticIntr:
	case BET_DynamicIntr:
		{
			if (BlockType==BET_StaticIntr)
			{
				MOV32(r4,NextBlock);
			}
			//else -> already in r4 djump !

			StoreSh4Reg_mem(r4,reg_nextpc);

			CALL((u32)UpdateINTC);
			LoadSh4Reg_mem(r4,reg_nextpc);
			JUMP((u32)no_update);
			break;
		}

	default:
		printf("Error, Relink() Block Type: %X\n", BlockType);
		verify(false);
		break;
	}

	CacheFlush(code_start,emit_ptr);

	u32 sz=(u8*)emit_ptr-code_start;

	emit_ptr=0;
	return sz;
}

void ngen_Unary(shil_opcode* op, UnaryOP unop)
{
	unop(reg.mapg(op->rd),reg.mapg(op->rs1));
}

void ngen_Binary(shil_opcode* op, BinaryOP dtop,BinaryOPImm dtopimm, bool has_imm=true)
{
	verify(reg.mapg(op->rd)!=r0 && reg.mapg(op->rs1)!=r0);
	
	eReg rs2=r0;
	if (has_imm && op->rs2.is_imm())    // **FIXME** ???
	{
		if (is_i8r4(op->rs2._imm))
		{
			dtopimm(reg.mapg(op->rd), reg.mapg(op->rs1), op->rs2._imm, CC_AL);
			return;
		}
		else
		{
			MOV32(rs2,(u32)op->rs2._imm);
		}
	}
	else if (op->rs2.is_r32i())  // **FIXME** Correct?
	{
		rs2=reg.mapg(op->rs2);
	}
	else
	{
		printf("ngen_Bin ??? %d \n",op->rs2.type);
		verify(false);
	}

	dtop(reg.mapg(op->rd),reg.mapg(op->rs1), rs2, CC_AL);
}

void ngen_fp_bin(shil_opcode* op, const FPBinOP fpop)
{
	verify(op->rs1.is_r32f());
	verify(op->rs2.is_r32f());

	fpop(reg.mapfs(op->rd),reg.mapfs(op->rs1),reg.mapfs(op->rs2),CC_AL);
}
void ngen_fp_una(shil_opcode* op, const FPUnOP fpop)
{
	verify(op->rd.is_r32f());
	verify(op->rs1.is_r32f());

	fpop(reg.mapfs(op->rd),reg.mapfs(op->rs1),CC_AL);
}

struct CC_PS
{
	CanonicalParamType type;
	shil_param* par;
};
vector<CC_PS> CC_pars;

void* _vmem_read_const(u32 addr,bool& ismem,u32 sz,void** ctx_out);
void* _vmem_page_info(u32 addr,bool& ismem,u32 sz,u32& page_sz, bool rw);


enum mem_op_type
{
	SZ_8,
	SZ_16,
	SZ_32I,
	SZ_32F,
	SZ_64F,
};

mem_op_type memop_type(shil_opcode* op)
{

	int Lsz=-1;
	int sz=op->flags&0x7f;

	bool fp32=op->rs2.is_r32f() || op->rd.is_r32f();

	if (sz==1) Lsz=SZ_8;
	if (sz==2) Lsz=SZ_16;
	if (sz==4 && !fp32) Lsz=SZ_32I;
	if (sz==4 && fp32) Lsz=SZ_32F;
	if (sz==8) Lsz=SZ_64F;

	verify(Lsz!=-1);
	
	return (mem_op_type)Lsz;
}

u32 memop_bytes(mem_op_type tp)
{
	const u32 rv[] = { 1,2,4,4,8};

	return rv[tp];
}

/*
	8/16/32 I R/W B
	ubfx r0,raddr,..
	ldr(sh/sb)/str(h/b) rd/s,[r0+r8]

	32/64 F R/W B
	ubfx r0,raddr,..
	add r0,r0,r8
	vldr/vstr rd/s,[r0] {32 or 64 bit forms}


	32 I / 32/64 F W SQ
	ubfx r0,raddr,..
	add r0,r0,r8
	str/vstr/vstr.d rs,[r0-offs]

	8/16/32 I R/W M
	mov r0,raddr
	call MEMHANDLER<rd/s>

	32/64 F R M
	mov r0,raddr
	vmov r1,rs // vmov.d r3:r2,rs
	call MEMHANDER<r1> // call MEMHANDLER64

	32/64 F W M
	mov r0,raddr
	call MEMHANDER<r1> // call MEMHANDLER64
	vmov rd,r0 // vmov.d rd,r3:r2
*/
unat _mem_hndl_SQ32[14];
unat _mem_hndl[2][3][14];
unat _mem_func[2][5]=
{
	{0,0,0,(unat)_vmem_WriteMem32,(unat)_vmem_WriteMem64},
	{0,0,0,(unat)_vmem_ReadMem32,(unat)_vmem_ReadMem64},
};

struct 
{
	u32 mask;
	u32 key;
	bool read;
	mem_op_type optp;
	u32 offs;
}
op_table[]=
{
	//LDRSB
	{0x0E500FF0,0x001000D0,true,SZ_8,1},
	//LDRSH
	{0x0E500FF0,0x001000F0,true,SZ_16,1},
	//LDR
	{0x0E500010,0x06100000,true,SZ_32I,1},
	//VLDR.32
	{0x0F300F00,0x0D100A00,true,SZ_32F,2},
	//VLDR.64
	{0x0F300F00,0x0D100B00,true,SZ_64F,2},

	//
	//STRB
	{0x0FF00010,0x07C00000,false,SZ_8,1},
	//STRH
	{0x0FF00FF0,0x018000B0,false,SZ_16,1},
	//STR
	{0x0E500010,0x06000000,false,SZ_32I,1},
	//VSTR.32
	{0x0F300F00,0x0D000A00,false,SZ_32F,2},
	//VSTR.64
	{0x0F300F00,0x0D000B00,false,SZ_64F,2},
	
	{0,0},
};

union arm_mem_op
{
	struct
	{
		u32 Ra:4;
		u32 pad0:8;
		u32 Rt:4;
		u32 Rn:4;
		u32 pad1:2;
		u32 D:1;
		u32 pad3:1;
		u32 pad4:4;
		u32 cond:4;
	};

	u32 full;
};

void vmem_slowpath(eReg raddr, eReg rt, eFSReg ft, eFDReg fd, mem_op_type optp, bool read)
{
	if (raddr != r0)
		MOV(r0, (eReg)raddr);

	if (!read)
	{
		if (optp <= SZ_32I) MOV(r1, rt);
		else if (optp == SZ_32F) VMOV(r1, ft);
		else if (optp == SZ_64F) VMOV(r2, r3, fd);
	}

	//the value rides in fd across the call: either a staging register or the
	//pair the operand is allocated in, which is always inside the callee-saved
	//d8-d15 bank (see param_dreg)
	verify(optp != SZ_64F || fd == d0 || (fd >= d8 && fd <= d15));

	u32 funct = 0;

	if (optp <= SZ_32I)
		funct = _mem_hndl[read][optp][raddr];
	else
		funct = _mem_func[read][optp];

	verify(funct != 0);
	CALL(funct);

	if (read)
	{
		if (optp <= SZ_32I) MOV(rt, r0);
		else if (optp == SZ_32F) VMOV(ft, r0);
		else if (optp == SZ_64F) VMOV(fd, r0, r1);
	}
}


extern u8* virt_ram_base;

EAPI NEG(eReg Rd,eReg Rs)
{
	RSB(Rd,Rs,0);
}

EAPI NEG(eReg Rd,eReg Rs, bool S, ConditionCode cond = CC_AL)
{
	RSB(Rd,Rs,0, S, cond);
}

eReg GenMemAddr(shil_opcode* op,eReg raddr=r0)
{
	if (op->rs3.is_imm())
	{
		if(is_i8r4(op->rs3._imm))
		{
			ADD(raddr,reg.mapg(op->rs1),op->rs3._imm);
		}
		else 
		{
			MOV32(r1,op->rs3._imm);
			ADD(raddr,reg.mapg(op->rs1),r1);
		}
	}
	else if (op->rs3.is_r32i())
	{
		ADD(raddr,reg.mapg(op->rs1),reg.mapg(op->rs3));
	}
	else if (!op->rs3.is_null())
	{
		printf("rs3: %08X\n",op->rs3.type);
		die("invalid rs3");
	}
	else
	{
		raddr = reg.mapg(op->rs1);
	}

	return raddr;
}

//The d register an exploded f64 param lives in, or -1 when its halves didn't
//land in an in-order even/odd pair (see opt_pair_fpu_spans). Such a pair can
//serve as a memop's value register directly: the fpu pool is s16-s31, which is
//exactly d8-d15, the bank AAPCS preserves across calls -- so it survives both
//the slowpath call and the handler call ngen_Rewrite may patch the fastpath
//into (the handler moves the value through whatever d register it finds in the
//faulting instruction).
static eFDReg param_dreg(const shil_param& prm)
{
	if (!prm.is_r64f() || !reg.IsAllocf(prm,0) || !reg.IsAllocf(prm,1))
		return (eFDReg)-1;

	eFSReg h0=reg.mapfv(prm,0),h1=reg.mapfv(prm,1);

	if ((h0&1)!=0 || h1!=(eFSReg)(h0+1))
		return (eFDReg)-1;

	return (eFDReg)(h0/2);
}

//...otherwise the value moves through the d0/d14 staging register and these
//replace the context store/load that used to bracket the memop with vmovs
//into/out of the halves the allocator gave the f64 param. Unallocated halves
//keep using the context.
static void d64_to_param(eFDReg d64, const shil_param& prm)
{
	verify(prm.is_r64f());

	if (!reg.IsAllocf(prm,0) && !reg.IsAllocf(prm,1))
	{
		VSTR(d64,r8,prm.reg_nofs()/4);
		return;
	}

	for (u32 i=0;i<2;i++)
	{
		eFSReg half=(eFSReg)(d64*2+i);

		if (reg.IsAllocf(prm,i))
			VMOV(reg.mapfv(prm,i),half);
		else
			VSTR(half,r8,prm.reg_nofs()/4+i);
	}
}

//loads the 64 bit pair at [base+ofs] (word offset) into an f64 param. Unlike
//the memops, this one can pick its own instructions: nothing rewrites it, so
//the pair goes straight into the mapped halves. One vldr.64 when the allocator
//happened to hand out an even/odd pair, two vldr.32 otherwise -- either way no
//vmovs, and the d64 staging register is only needed if nothing is mapped.
static void load64_to_param(const shil_param& prm, eReg base, s32 ofs, eFDReg d64)
{
	verify(prm.is_r64f());

	if (reg.IsAllocf(prm,0) && reg.IsAllocf(prm,1))
	{
		eFDReg pair=param_dreg(prm);
		eFSReg h0=reg.mapfv(prm,0),h1=reg.mapfv(prm,1);

		if (pair!=(eFDReg)-1)
			VLDR(pair,base,ofs);
		else
		{
			VLDR(h0,base,ofs);
			VLDR(h1,base,ofs+1);
		}
		return;
	}

	if (!reg.IsAllocf(prm,0) && !reg.IsAllocf(prm,1))
	{
		VLDR(d64,base,ofs);
		VSTR(d64,r8,prm.reg_nofs()/4);
		return;
	}

	for (u32 i=0;i<2;i++)
	{
		if (reg.IsAllocf(prm,i))
			VLDR(reg.mapfv(prm,i),base,ofs+i);
		else
		{
			eFSReg t=fpu_temps_avail?f28:f0;
			VLDR(t,base,ofs+i);
			VSTR(t,r8,prm.reg_nofs()/4+i);
		}
	}
}

static void param_to_d64(eFDReg d64, const shil_param& prm)
{
	verify(prm.is_r64f());

	if (!reg.IsAllocf(prm,0) && !reg.IsAllocf(prm,1))
	{
		VLDR(d64,r8,prm.reg_nofs()/4);
		return;
	}

	for (u32 i=0;i<2;i++)
	{
		eFSReg half=(eFSReg)(d64*2+i);

		if (reg.IsAllocf(prm,i))
			VMOV(half,reg.mapfv(prm,i));
		else
			VLDR(half,r8,prm.reg_nofs()/4+i);
	}
}

void ngen_compile_opcode(RuntimeBlockInfo* block, shil_opcode* op, bool staging, bool optimise)
{
	switch(op->op)
	{
		case shop_readm:
		{
			mem_op_type optp=memop_type(op);
			if (op->rs1.is_imm())
			{
				bool isram=false;
				void* handler_ctx=nullptr;
				void* ptr=_vmem_read_const(op->rs1._imm,isram,memop_bytes(optp),&handler_ctx);
				
				verify(optp!=SZ_64F);

				if (isram)
				{
					switch(optp)
					{
					case SZ_8:
						{
							verify(false);
							MOV32(r0,(u32)ptr);
							LDRB(reg.mapg(op->rd),r0);
							SXTB(reg.mapg(op->rd),reg.mapg(op->rd));
						} 
						break;

					case SZ_16:
						{
							LoadImmBase16(reg.mapg(op->rd),(u32)ptr,true);    // true for sx
						} 
						break;

					case SZ_32I:
						{
							verify(reg.IsAllocg(op->rd));
							{
								if (optimise && staging && !is_s8(*(u32*)ptr) && abs((int)op->rs1._imm-(int)block->addr)<=1024)
								{
									op->flags|=0x40000000;

									MOV32(r0,(u32)ptr);
									LDR(reg.mapg(op->rd),r0);
									MOV32(r1,*(u32*)ptr);
									CMP(reg.mapg(op->rd),r1);
									//JUMP((unat)EMIT_GET_PTR()+24,CC_EQ);
									MOV32(r1,(u32)&op->flags);
									MOV32(r2,~0x40000000);
									LDR(r3,r1);
									AND(r3,r3,r2,CC_NE);
									STR(r3,r1);
								}
								else if (optimise && !staging && op->flags & 0x40000000)
								{
									MOV32(reg.mapg(op->rd),*(u32*)ptr);
								}
								else
								{
									MOV32(r0,(u32)ptr);
									LDR(reg.mapg(op->rd),r0);
								}
							}
						}
						break;

					case SZ_32F:
						{
							verify(reg.IsAllocf(op->rd));
							MOV32(r0,(u32)ptr);
							VLDR(reg.mapfs(op->rd),r0,0);

						}
						break;

					case SZ_64F:
						{
							verify(false);
							MOV32(r0,(u32)ptr);
							LDR(r1,r0,4);
							LDR(r0,r0,0);
						}
						break;
					}
				} 
				else
				{
					//handlers take (ctx, addr); ctx is whatever the region
					//was registered with (mmr, vram, ...), NOT sh4_cpu
					MOV32(r0, (uintptr_t)handler_ctx);
					MOV32(r1,op->rs1._imm);

					switch(optp)
					{
					case SZ_8: CALL((u32)ptr); SXTB(r0,r0); break;

					case SZ_16: CALL((u32)ptr); SXTH(r0,r0); break;

					case SZ_32I:
					case SZ_32F: CALL((u32)ptr); break;

					case SZ_64F:
						die("SZ_64F not supported");
						break;
					}

					if (reg.IsAllocg(op->rd))
						MOV(reg.mapg(op->rd),r0);
					else
						VMOV(reg.mapfs(op->rd),r0);
				}
			}
			else
			{
				eReg raddr=GenMemAddr(op);

				if (_nvmem_enabled()) {
					BIC(r1,raddr,0xE0000000);

					switch(optp)
					{
					case SZ_8:	
						LDRSB(reg.mapg(op->rd),r1,r8,true); 
						break;

					case SZ_16: 
						LDRSH(reg.mapg(op->rd),r1,r8,true); 
						break;

					case SZ_32I: 
						LDR(reg.mapg(op->rd),r1,r8,Offset,true); 
						break;

					case SZ_32F:
						ADD(r1,r1,r8);	//3 opcodes, there's no [REG+REG] VLDR
						VLDR(reg.mapf(op->rd),r1,0);
						//paired form (fuse_readm_pairs): rd2 = [addr+4],
						//one address computation for both halves
						if (op->rd2.is_r32f())
							VLDR(reg.mapf(op->rd2),r1,1);
						break;

					case SZ_64F:
					{
						eFDReg pair=param_dreg(op->rd);
						eFDReg pair2=op->rd2.is_r64f()?param_dreg(op->rd2):(eFDReg)-1;

						//Paired form (fuse_readm_pairs): rd2 = [addr+8] off the
						//same address computation. The two vldrs have to stay
						//adjacent -- that shape is what ngen_Rewrite looks for
						//when it has to move the second one off a base its
						//handler call clobbered -- so there is no room for
						//staging vmovs between them, and the shared form is
						//only used when both doubles got a register pair.
						bool fused=pair!=(eFDReg)-1 && pair2!=(eFDReg)-1;

						ADD(r1,r1,r8);	//3 opcodes, there's no [REG+REG] VLDR

						if (fused)
						{
							VLDR(pair,r1,0);
							VLDR(pair2,r1,2);
							break;
						}

						//loads straight into the mapped pair when there is one,
						//otherwise stages in d64; either way this vldr is the
						//op ngen_Rewrite patches
						eFDReg d64=pair!=(eFDReg)-1?pair:(fpu_temps_avail?d14:d0);

						VLDR(d64,r1,0);

						if (pair==(eFDReg)-1)
							d64_to_param(d64,op->rd);

						//the second double gets an address computation of its
						//own then. It has to start from addr+8 in a register:
						//a rewrite reads the guest address out of whatever the
						//masking op sources, and rs3 is null on a fused op, so
						//raddr is one of the callee-saved pool regs and still
						//holds the base even if the load above became a call.
						if (op->rd2.is_r64f())
						{
							ADD(r0,raddr,8);
							BIC(r1,r0,0xE0000000);
							ADD(r1,r1,r8);

							eFDReg d64b=pair2!=(eFDReg)-1?pair2:(fpu_temps_avail?d14:d0);

							VLDR(d64b,r1,0);

							if (pair2==(eFDReg)-1)
								d64_to_param(d64b,op->rd2);
						}
						break;
					}
					}
				} else {
					eFDReg d64=fpu_temps_avail?d14:d0;
					switch(optp)
					{
					case SZ_8:
						vmem_slowpath(raddr, reg.mapg(op->rd), f0, d64, optp, true);
						break;

					case SZ_16:
						vmem_slowpath(raddr, reg.mapg(op->rd), f0, d64, optp, true);
						break;

					case SZ_32I:
						vmem_slowpath(raddr, reg.mapg(op->rd), f0, d64, optp, true);
						break;

					case SZ_32F:
						vmem_slowpath(raddr, r0, reg.mapf(op->rd), d64, optp, true);
						break;

					case SZ_64F:
					{
						//pairs are only fused on the nvmem fastpath
						verify(op->rd2.is_null());

						eFDReg pair=param_dreg(op->rd);

						vmem_slowpath(raddr, r0, f0, pair!=(eFDReg)-1?pair:d64, optp, true);

						if (pair==(eFDReg)-1)
							d64_to_param(d64,op->rd);
						break;
					}
					}
				}
			}
		}
		break;


		case shop_writem:
		{
			mem_op_type optp=memop_type(op);
			eFDReg d64=fpu_temps_avail?d14:d0;

			eReg raddr=GenMemAddr(op);

			//stores straight out of the mapped pair when there is one, else the
			//value is staged in d64; either way the store below is the op
			//ngen_Rewrite patches
			if (optp == SZ_64F)
			{
				eFDReg pair=param_dreg(op->rs2);

				if (pair!=(eFDReg)-1)
					d64=pair;
				else
					param_to_d64(d64,op->rs2);
			}

			if (_nvmem_enabled()) {
				BIC(r1,raddr,0xE0000000);
				//UBFX(r1,raddr,0,29);
				//SUB(r1,raddr,raddr);
				
				s32 sq_offs=rcb_noffs(sq_both);
				switch(optp)
				{
				case SZ_8:
					STRB(reg.mapg(op->rs2),r1,r8,Offset,true);
					break;

				case SZ_16:
					STRH(reg.mapg(op->rs2),r1,r8,true);
					break;

				case SZ_32I:
					if (op->flags2!=0x1337)
						STR(reg.mapg(op->rs2),r1,r8,Offset,true); 
					else
					{
						emit_Skip(-4);
						AND(r1,raddr,0x3F);
						ADD(r1,r1,r8);
						STR(reg.mapg(op->rs2),r1,sq_offs);
					}
					break;

				case SZ_32F:
					if (op->flags2!=0x1337)
					{
						ADD(r1,r1,r8);	//3 opcodes: there's no [REG+REG] VLDR, also required for SQ
						VSTR(reg.mapf(op->rs2),r1,0);
					}
					else
					{
						emit_Skip(-4);
						AND(r1,raddr,0x3F);
						ADD(r1,r1,r8);
						VSTR(reg.mapf(op->rs2),r1,sq_offs/4);
					}
					break;

				case SZ_64F:
					if (op->flags2!=0x1337)
					{
						ADD(r1,r1,r8);	//3 opcodes: there's no [REG+REG] VLDR, also required for SQ
						VSTR(d64,r1,0);
					}
					else
					{
						emit_Skip(-4);
						AND(r1,raddr,0x3F);
						ADD(r1,r1,r8);
						VSTR(d64,r1,sq_offs/4);
					}
					break;
				}
			} else {
				switch(optp)
				{
				case SZ_8:
					vmem_slowpath(raddr, reg.mapg(op->rs2), f0, d64, optp, false);
					break;

				case SZ_16:
					vmem_slowpath(raddr, reg.mapg(op->rs2), f0, d64, optp, false);
					break;

				case SZ_32I:
					vmem_slowpath(raddr, reg.mapg(op->rs2), f0, d64, optp, false);
					break;

				case SZ_32F:
					vmem_slowpath(raddr, r0, reg.mapf(op->rs2), d64, optp, false);
					break;

				case SZ_64F:
					vmem_slowpath(raddr, r0, f0, d64, optp, false);
					break;
				}
			}
		}
		break;

		//dynamic jump, r+imm32.This will be at the end of the block, but doesn't -have- to be the last opcode
		case shop_jdyn:
		{
			//ReadReg rs1(r4,op->rs1);
			reg.writeback_gpr--;
			verify(op->rd.is_reg() && op->rd._reg==reg_pc_dyn);
			if (op->rs2.is_imm())
			{
				MOV32(r2, (u32)op->rs2._imm);
				ADD(r4,reg.mapg(op->rs1),r2);
			}
			else //if (r4!=rs1.reg)
			{
				MOV(r4, reg.mapg(op->rs1));
			}
			break;
		}

		case shop_mov32:
			{
				verify(op->rd.is_r32());

				if (op->rs1.is_imm())
				{
					if (op->rd.is_r32i())
					{
						MOV32(reg.mapg(op->rd),op->rs1._imm);
					}
					else
					{
						verify(op->rs1._imm==0 || op->rs1._imm==0x3F800000);
						if (op->rs1._imm==0)
						{
							//VEOR(reg.mapf(op->rd),reg.mapf(op->rd),reg.mapf(op->rd));
							//hum, vmov can't do 0, but can do all kind of weird small consts ... really useful ...
							//simd is slow on a9
#if 0
							MOVW(r0,0);
							VMOV(reg.mapfs(op->rd),r0);
#else
							//1-1=0 !
							//should be slightly faster ...
							//we could get rid of the imm mov, if not for infs & co ..
							VMOV(reg.mapfs(op->rd),fpu_imm_1);
							VSUB_VFP(reg.mapfs(op->rd),reg.mapfs(op->rd),reg.mapfs(op->rd));
#endif
						}
						else
							VMOV(reg.mapfs(op->rd),fpu_imm_1);
					}
				}
				else if (op->rs1.is_r32())
				{
					u32 type=0;

					if (reg.IsAllocf(op->rd))
						type|=1;
					
					if (reg.IsAllocf(op->rs1))
						type|=2;

					switch(type)
					{
					case 0: //reg=reg
						if (reg.mapg(op->rd)!=reg.mapg(op->rs1))
							MOV(reg.mapg(op->rd),reg.mapg(op->rs1));
						break;

					case 1: //vfp=reg
						VMOV(reg.mapfs(op->rd),reg.mapg(op->rs1));
						break;

					case 2: //reg=vfp
						VMOV(reg.mapg(op->rd),reg.mapfs(op->rs1));
						break;

					case 3: //vfp=vfp
						if (reg.mapfs(op->rd)!=reg.mapfs(op->rs1))
							VMOV(reg.mapfs(op->rd),reg.mapfs(op->rs1));
						break;
					}
				}
				else
				{
					die("Invalid mov32 size");
				}
				
			}
			break;
			
		case shop_mov64:
		{
			verify(op->rs1.is_r64() && op->rd.is_r64());

			//both halves are register allocated (ExplodeVec), so this is a
			//pair of vmovs; the memory paths are only here for the (currently
			//unreachable) case of a half that didn't get a register
			eFSReg src[2],dst[2];
			bool src_a[2],dst_a[2];

			for (u32 i=0;i<2;i++)
			{
				src_a[i]=reg.IsAllocf(op->rs1,i);
				dst_a[i]=reg.IsAllocf(op->rd,i);

				if (src_a[i]) src[i]=reg.mapfv(op->rs1,i);
				if (dst_a[i]) dst[i]=reg.mapfv(op->rd,i);
			}

			if (!src_a[0] && !src_a[1] && !dst_a[0] && !dst_a[1])
			{
				//nothing mapped: the old ctx-to-ctx copy, in one 64 bit pair
				eFDReg t=fpu_temps_avail?d14:d0;
				VLDR(t,r8,op->rs1.reg_nofs()/4);
				VSTR(t,r8,op->rd.reg_nofs()/4);
				break;
			}

			//a dest may have taken over the host reg of a source that dies
			//here (opt_reuse_dead), so a half must not be written while the
			//other half still has to read that register
			u32 order[2]={0,1};

			if (dst_a[0] && src_a[1] && dst[0]==src[1])
			{
				if (dst_a[1] && src_a[0] && dst[1]==src[0])
				{
					//the pair is swapped: break the cycle through a scratch
					eFSReg t=fpu_temps_avail?f28:f0;
					VMOV(t,src[1]);
					src[1]=t;
				}
				else
				{
					order[0]=1;
					order[1]=0;
				}
			}

			for (u32 k=0;k<2;k++)
			{
				u32 i=order[k];

				if (dst_a[i] && src_a[i])
				{
					if (dst[i]!=src[i])
						VMOV(dst[i],src[i]);
				}
				else if (dst_a[i])
					VLDR(dst[i],r8,op->rs1.reg_nofs()/4+i);
				else if (src_a[i])
					VSTR(src[i],r8,op->rd.reg_nofs()/4+i);
				else
				{
					eFSReg t=fpu_temps_avail?f28:f0;
					VLDR(t,r8,op->rs1.reg_nofs()/4+i);
					VSTR(t,r8,op->rd.reg_nofs()/4+i);
				}
			}
			break;
		}

		case shop_jcond:
		{
			verify(op->rd.is_reg() && op->rd._reg==reg_pc_dyn);
			//ReadReg rs1(r4,op->rs1);

			//if (r4!=rs1.reg)
				MOV(r4,reg.mapg(op->rs1));
			break;
		}

		case shop_ifb:
		{
			if (op->rs1._imm) 
			{
				MOV32(r1,op->rs2._imm);
				StoreSh4Reg_mem(r1,reg_nextpc);
				//StoreImms(r3,r2,(u32)&next_pc,(u32)op->rs2._imm);
			}

			MOV32(r0, op->rs3._imm);
			CALL((u32)(OpPtr[op->rs3._imm]));
			break;
		}

//#define CANONICALTEST
#ifndef CANONICALTEST
		case shop_neg: ngen_Unary(op,NEG);     break;
		case shop_not: ngen_Unary(op,NOT);     break;


		case shop_shl: ngen_Binary(op,LSL,LSL); break;
		case shop_shr: ngen_Binary(op,LSR,LSR); break;
		case shop_sar: ngen_Binary(op,ASR,ASR); break;

		case shop_and:  ngen_Binary(op,AND,AND);    break;
		case shop_or:   ngen_Binary(op,ORR,ORR);    break;
		case shop_xor:	ngen_Binary(op,EOR,EOR);    break;

		case shop_add:	ngen_Binary(op,ADD,ADD);    break;
		case shop_sub:	ngen_Binary(op,SUB,SUB);    break;
		case shop_ror:	ngen_Binary(op,ROR,ROR);    break;

			
		case shop_adc:
		{
			//RSBS(reg.map(op.rs3),reg.map(op.rs3),0);
			//ADCS(reg.map(op.rs1),reg.map(op.rs2),reg.map(op.rs3));
			//ADC(reg.map(op.rs3),reg.map(op.rs3),reg.map(op.rs3),LSL,31);

			//ADD(r0,reg.map(op.rs1),

#if 0
			MOVW(r1,0);
			ADD(r0,reg.mapg(op->rs1),reg.mapg(op->rs2),true);
			ADC(r1,r1,0);
			ADD(reg.mapg(op->rd),r0,reg.mapg(op->rs3),true);
			ADC(reg.mapg(op->rd2),r1,0);
#else

			LSR(r0,reg.mapg(op->rs3),1,true); //C=rs3, r0=0
			ADC(reg.mapg(op->rd),reg.mapg(op->rs1),reg.mapg(op->rs2),true); //(C,rd)=rs1+rs2+rs3(C)
			ADC(reg.mapg(op->rd2),r0,0);	//rd2=C, (or MOVCS rd2, 1)
#endif
		}
		break;

		case shop_rocr:
			{

				if (reg.mapg(op->rd2)!=reg.mapg(op->rs1)) {
					LSR(reg.mapg(op->rd2),reg.mapg(op->rs2),1,true); //C=rs2, rd2=0
					AND(reg.mapg(op->rd2),reg.mapg(op->rs1),1);      //get new carry
				} else {
					LSR(r0,reg.mapg(op->rs2),1,true); //C=rs2
					AND(r0, reg.mapg(op->rs1),1);     //get new carry
				}
				RRX(reg.mapg(op->rd),reg.mapg(op->rs1));         //RRX w/ carry :)
				if (reg.mapg(op->rd2)==reg.mapg(op->rs1))
					MOV(reg.mapg(op->rd2), r0);
				
			}
			break;
			
		case shop_rocl:
			{
				//ADD(reg.mapg(op->rd),reg.mapg(op->rs2),reg.mapg(op->rs1),1,true); //(C,rd)= rs1<<1 + (|) rs2
				ORR(reg.mapg(op->rd),reg.mapg(op->rs2),reg.mapg(op->rs1),true, S_LSL, 1); //(C,rd)= rs1<<1 + (|) rs2
				MOVW(reg.mapg(op->rd2),0);                      //clear rd2 (for ADC/MOVCS)
				ADC(reg.mapg(op->rd2),reg.mapg(op->rd2),0);     //rd2=C (or MOVCS rd2, 1)
			}
			break;
			
		case shop_sbc:
			//printf("sbc: r%d r%d r%d r%d r%d\n",reg.mapg(op->rd),reg.mapg(op->rd2),reg.mapg(op->rs1),reg.mapg(op->rs2), reg.mapg(op->rs3));
			{
				eReg rd2=reg.mapg(op->rd2);
				if (rd2==reg.mapg(op->rs1) || rd2==reg.mapg(op->rs2))
				{
					//rd2 shares a host reg with a source: prep the carry in r0
					//and write rd2 only after SBC has consumed rs1/rs2
					EOR(r0,reg.mapg(op->rs3),1);
					LSR(r0,r0,1,true); //C=rs3
					SBC(reg.mapg(op->rd), reg.mapg(op->rs1), reg.mapg(op->rs2), true);
					MOVW(rd2,0);
					MOV(rd2, 1, CC_CC);
				}
				else
				{
					EOR(rd2,reg.mapg(op->rs3),1);
					LSR(rd2,rd2,1,true); //C=rs3, rd2=0
					SBC(reg.mapg(op->rd), reg.mapg(op->rs1), reg.mapg(op->rs2), true);
					MOV(rd2, 1, CC_CC);
				}
			}
			break;
		
		case shop_shld:
			//printf("shld: r%d r%d r%d\n",reg.mapg(op->rd),reg.mapg(op->rs1),reg.mapg(op->rs2));
			{
				verify(!op->rs2.is_imm());
				AND(r0, reg.mapg(op->rs2), 0x8000001F, true);
				RSB(r0, r0, 0x80000020, CC_MI);
				LSR(reg.mapg(op->rd), reg.mapg(op->rs1), r0, CC_MI);
				LSL(reg.mapg(op->rd), reg.mapg(op->rs1), r0, CC_PL);
				//MOV(reg.mapg(op->rd), reg.mapg(op->rs1), S_LSL, r0, CC_PL);
				//MOV(reg.mapg(op->rd), reg.mapg(op->rs1), S_LSR, r0, CC_MI);
			}		
			break;

		case shop_shad:
			//printf("shad: r%d r%d r%d\n",reg.mapg(op->rd),reg.mapg(op->rs1),reg.mapg(op->rs2));
			{
				verify(!op->rs2.is_imm());
				AND(r0, reg.mapg(op->rs2), 0x8000001F, true);
				RSB(r0, r0, 0x80000020, CC_MI);
				ASR(reg.mapg(op->rd), reg.mapg(op->rs1), r0, CC_MI);
				LSL(reg.mapg(op->rd), reg.mapg(op->rs1), r0, CC_PL);
				//MOV(reg.mapg(op->rd), reg.mapg(op->rs1), S_LSL, r0, CC_PL);
				//MOV(reg.mapg(op->rd), reg.mapg(op->rs1), S_ASR, r0, CC_MI);
			}		
			break;

		case shop_sync_sr:
		{
			//must flush: SRS, SRT, r0-r7, r0b-r7b
			CALL((u32)UpdateSR);
			break;
		}

		case shop_div32p2:
		{
			//rd=rs3==0 ? rs1-rs2 : rs1, with all sources read before rd is
			//written so rd may share a host reg with any of them
			CMP(reg.mapg(op->rs3),0);
			SUB(reg.mapg(op->rd),reg.mapg(op->rs1),reg.mapg(op->rs2),CC_EQ);
			if (reg.mapg(op->rs1)!=reg.mapg(op->rd))
				MOV(reg.mapg(op->rd),reg.mapg(op->rs1),CC_NE);
		}
		break;

		case shop_test:
		case shop_seteq:
		case shop_setge:
		case shop_setgt:
		case shop_setae:
		case shop_setab:
		{
			eReg rs2=r0;
			bool is_imm=false;

			if (op->rs2.is_imm())
			{
				if (!is_i8r4(op->rs2._imm))
					MOV32(rs2,(u32)op->rs2._imm);
				else
					is_imm=true;
			}
			else if (op->rs2.is_r32i())
			{
				rs2=reg.mapg(op->rs2);
			}
			else
			{
				printf("ngen_Bin ??? %d \n",op->rs2.type);
				verify(false);
			}

			if (op->op==shop_test)
			{
				if (is_imm)
					TST(reg.mapg(op->rs1),op->rs2._imm);
				else
					TST(reg.mapg(op->rs1),rs2);
			}
			else
			{
				if (is_imm)
					CMP(reg.mapg(op->rs1),op->rs2._imm);
				else
					CMP(reg.mapg(op->rs1),rs2);
			}


			eCC opcls2[]={CC_EQ,CC_EQ,CC_GE,CC_GT,CC_HS,CC_HI };

			//rd is written after the compare (MOVW leaves flags alone), so it
			//may share a host reg with rs1/rs2
			MOVW(reg.mapg(op->rd),0);
		    MOVW(reg.mapg(op->rd),1,opcls2[op->op-shop_test]);
		    break;
        }
		
		case shop_setpeq:
			{
				EOR(r1, reg.mapg(op->rs1), reg.mapg(op->rs2));
				MOVW(reg.mapg(op->rd), 0);
				
				TST(r1, 0xFF000000);
				TST(r1, 0x00FF0000, CC_NE);
				TST(r1, 0x0000FF00, CC_NE);
				TST(r1, 0x000000FF, CC_NE);
				MOVW(reg.mapg(op->rd), 1, CC_EQ);
			}
			break;
		
		//UXTH for zero extention and/or more mul forms (for 16 and 64 bits)

		case shop_mul_u16:
			{
				UXTH(r1, reg.mapg(op->rs1));
				UXTH(r2, reg.mapg(op->rs2));
				MUL(reg.mapg(op->rd),r1,r2);
			}
			break;
		case shop_mul_s16:
			{
				SXTH(r1, reg.mapg(op->rs1));
				SXTH(r2, reg.mapg(op->rs2));
				MUL(reg.mapg(op->rd),r1,r2);
			}
			break;
		case shop_mul_i32:
			{
				//x86_opcode_class opdt[]={op_movzx16to32,op_movsx16to32,op_mov32,op_mov32,op_mov32};
				//x86_opcode_class opmt[]={op_mul32,op_mul32,op_mul32,op_mul32,op_imul32};
				//only the top 32 bits are different on signed vs unsigned

				MUL(reg.mapg(op->rd),reg.mapg(op->rs1),reg.mapg(op->rs2));
			}
			break;
		case shop_mul_u64:
			{
				UMULL(reg.mapg(op->rd2), reg.mapg(op->rd), reg.mapg(op->rs1), reg.mapg(op->rs2));
			}
			break;
		case shop_mul_s64:
			{
				SMULL(reg.mapg(op->rd2), reg.mapg(op->rd), reg.mapg(op->rs1), reg.mapg(op->rs2));
			}
			break;

/*		case shop_div32u:
			// Doesn't work
			// algo from new arm dynarec from mupen64plus
			//printf("div32u: r%d r%d r%d r%d\n",reg.mapg(op->rd2),reg.mapg(op->rd),reg.mapg(op->rs1),reg.mapg(op->rs2));
			{
				// remainder = r0, quotient = r1, HOST_TEMPREG = r2, copy de rs1 = r3, copy de rs2 = r4
				MOV(r3, reg.mapg(op->rs1));
				MOV(r4, reg.mapg(op->rs2), true);
				MOV(r0, reg.mapg(op->rs1));	// dividend = d1 , divisor = d2
				MVN(r1, 0);
				B(10*4-8, CC_EQ);
				CLZ(r2, r4);
				MOV(r1, 1<<31);
				LSL(r4, r4, r2);
				LSR(r1, r1, r2);
				CMP(r0, r4);
				SUB(r0, r0, r4, CC_CS);
				ADC(r1, r1, r1, true);
				MOV(r4, r4, S_LSR, 1, CC_CC);
				B(-4*4-8, CC_CC);
				MOV(reg.mapg(op->rd), r1);
				MOV(reg.mapg(op->rd2), r0);
			}
			break;*/
/*		case shop_div32s:
			//printf("div32s r%d, r%d, r%d, r%d\n", reg.mapg(op->rd2),reg.mapg(op->rd),reg.mapg(op->rs1),reg.mapg(op->rs2));
			// algo from dynarec from pcsxrearmed
			// remainder = r0, quotient = r1, HOST_TEMPREG = r2, copy de rs1 = r3, copy de rs2 = r4
			{
				MOV(r3, reg.mapg(op->rs1));
				MOV(r4, reg.mapg(op->rs2));
				MOV(r0, reg.mapg(op->rs1), true);
				MVN(r1, 0);
				RSB(r1, r1, 0, CC_MI); // .. quotient and ..
				RSB(r0, r0, 0, CC_MI); // .. remainder for div0 case (will be negated back after jump)
				MOV(r2, reg.mapg(op->rs2), true);
				B(14*4-8, CC_EQ); // Division by zero
				RSB(r2, r2, 0, true, CC_MI);
				CLZ(r1, r2);
				LSL(r2, r2, r1);
				ORR(r1, r1, 1<<31);
				LSR(r1, r1, r1);
				CMP(r0, r2);
				SUB(r0, r0, r2, CC_CS);
				ADC(r1, r1, r1, true);
				MOV(r2, r2, S_LSR, 1);
				B(-4*4-8, CC_CC); // -4
				TEQ(r3, r4, S_LSL, CC_AL);
				RSB(r1, r1, 0, CC_MI);
				TST(r3, r3);
				RSB(r0, r0, 0, CC_MI);
				MOV(reg.mapg(op->rd2), r0);
				MOV(reg.mapg(op->rd), r1);
			}
			break;*/
	
		case shop_pref:
			{
				if (op->flags != 0x1337)
				{
					LSR(r1,reg.mapg(op->rs1),26);
					MOV(r0,reg.mapg(op->rs1));
					CMP(r1,0x38);
				}
				else
					MOV(r0,reg.mapg(op->rs1));

				auto sh4mmr = sh4_cpu->sh4mmr.get();

				if (CCN_MMUCR.AT)
				{
					CALL((unat)&do_sqw_mmu,op->flags==0x1337?CC_AL:CC_EQ);
				}
				else
				{	
					LDR(r2,r8,rcb_noffs(&do_sqw_nommu));
					SUB(r1,r8,-rcb_noffs(sq_both));
					BLX(r2,op->flags==0x1337?CC_AL:CC_EQ);
				}
			}
			break;

/*
		case shop_cvt_i2f_z:
		case shop_cvt_i2f_n:
			
			break;
*/

			//shop_cvt_f2i_t
			//shop_cvt_i2f_n
			//shop_cvt_i2f_z
		case shop_ext_s8:
		case shop_ext_s16:
			{
				verify(op->rd.is_r32i());
				verify(op->rs1.is_r32i());

				(op->op==shop_ext_s8?SXTB:SXTH)(reg.mapg(op->rd),reg.mapg(op->rs1),CC_AL);
			}
			break;
			

				//fpu
		case shop_fadd:
		case shop_fsub:
		case shop_fmul:
		case shop_fdiv:
		{
			const FPBinOP* opcds[]= { VADD_VFP,VSUB_VFP,VMUL_VFP,VDIV_VFP };
			ngen_fp_bin(op, opcds[op->op-shop_fadd]);
		}
		break;

		case shop_fabs:
		case shop_fneg:
		{
			const FPUnOP* opcds[]= { VABS_VFP,VNEG_VFP};
			ngen_fp_una(op, opcds[op->op-shop_fabs]);
		}
		break;

		case shop_fsqrt:
		{
			ngen_fp_una(op, VSQRT_F32);
		}
		break;

		
		case shop_fmac:
			{
				verify(reg.mapfs(op->rd)==reg.mapfs(op->rs1));
				VMLA_VFP(reg.mapfs(op->rs1),reg.mapfs(op->rs2),reg.mapfs(op->rs3));
			}
			break;

		case shop_fsrra:
			{
				//scratch out of s0-s15 when the xmtrx may be cached there
				eFSReg t0=fpu_temps_avail?f28:f0;
				eFSReg t1=fpu_temps_avail?f29:f1;

				VMOV(t1,fpu_imm_1);
				VSQRT_VFP(t0,reg.mapfs(op->rs1));

				VDIV_VFP(reg.mapfs(op->rd),t1,t0);
			}
			break;

		case shop_fsetgt:
		case shop_fseteq:
		{
			//

#if 1
			{
				//this is aparently much faster (tested on A9)
				MOVW(reg.mapg(op->rd),0);
				VCMP_F32(reg.mapfs(op->rs1),reg.mapfs(op->rs2));

				VMRS(R15);
				if (op->op==shop_fsetgt)
				{
					MOVW(reg.mapg(op->rd),1,CC_GT);
				}
				else
				{
					MOVW(reg.mapg(op->rd),1,CC_EQ);
				}
			}
#else
			{
				if (op->op==shop_fsetgt)
					VCGT_F32(d0,reg.mapf(op->rs1),reg.mapf(op->rs2));
				else
					VCEQ_F32(d0,reg.mapf(op->rs1),reg.mapf(op->rs2));

				VMOV(r0,f0);

				AND(reg.mapg(op->rd),r0,1);
			}
#endif
		}
		break;
			

		case shop_fsca:
			{
				//r1: base ptr
				MOVW(r1,((unat)sin_table)&0xFFFF);
				UXTH(r0,reg.mapg(op->rs1));
				MOVT(r1,((u32)sin_table)>>16);
				
				/*
					LDRD(r0,r1,r0,lsl,3);
					VMOV.64
					or
					ADD(r0,r1,r0,LSL,3);
					VLDR(d0,r0);
				*/

				//LSL(r0,r0,3);
				//ADD(r0,r1,r0); //EMITTER: Todo, add with shifted !
				ADD(r0,r1,r0, S_LSL, 3);

				load64_to_param(op->rd,r0,0,fpu_temps_avail?d14:d0);
			}
			break;

		case shop_fipr:
			{
				//operands are exploded to allocated singles (ExplodeVec);
				//accumulate in a temp: rd aliases rs1[3]
				verify(fpu_temps_avail);

				eFSReg a[4],b[4];
				for (int i=0;i<4;i++)
				{
					a[i]=reg.mapfv(op->rs1,i);
					b[i]=reg.mapfv(op->rs2,i);
				}

				VMUL_VFP(f28,a[0],b[0]);
				VMLA_VFP(f28,a[1],b[1]);
				VMLA_VFP(f28,a[2],b[2]);
				VMLA_VFP(f28,a[3],b[3]);
				VMOV(reg.mapfs(op->rd),f28);
			}
			break;

		case shop_ftrv:
			{
				//in/out vector elements are register allocated (ExplodeVec);
				//the matrix lives in s0-s15 and stays resident until an op
				//that calls out / may fault / writes the back bank kills it.
				//Scalar VFP on purpose: NEON timed slower on the A9 here.
				verify(fpu_temps_avail);

				bool load_mtx=!xmtrx_resident;
				if (load_mtx)
				{
					SUB(r2,r8,op->rs2.reg_aofs());
					VLDM(d0,r2,2,1);	//column 0
				}
				xmtrx_resident=true;

				//in-place transform: the outputs alias the inputs
				verify(op->rd._reg==op->rs1._reg);

				eFSReg v[4];
				for (int i=0;i<4;i++)
					v[i]=reg.mapfv(op->rs1,i);

				bool w0=(op->flags & FTRV_W_ZERO)!=0;
				bool w1=(op->flags & FTRV_W_ONE)!=0;

				//accumulate straight into the mapped outputs; the chains then
				//END in their destination registers, with no result movs on
				//the tail. Stage 0 overwrites the input registers, so v1..v3
				//are copied aside first (head copies, hidden under the column
				//load); v0 is fully consumed by stage 0 itself when its own
				//register is written last, and a flagged w never reads v3.
				eFSReg mul[4];
				mul[0]=v[0];
				for (int j=1;j<4;j++)
				{
					if (j==3 && (w0 || w1))
						break;
					mul[j]=(eFSReg)(f28+j);
					VMOV(mul[j],v[j]);
				}

				//(all four columns still load when the matrix isn't resident,
				//so xmtrx_resident stays truthful for the next ftrv)
				for (int j=0;j<4;j++)
				{
					//prefetch the next column: the load retires in the LS
					//pipe underneath this stage's MACs
					if (load_mtx && j<3)
						VLDM((eFDReg)(d0+2*(j+1)),r2,2,j<2?1:0);

					//known w=0: the fourth column contributes nothing
					if (j==3 && w0)
						continue;

					if (j==0)
					{
						//all chains read v0; its own register goes last
						VMUL_VFP(v[1],(eFSReg)(f0+1),mul[0]);
						VMUL_VFP(v[2],(eFSReg)(f0+2),mul[0]);
						VMUL_VFP(v[3],(eFSReg)(f0+3),mul[0]);
						VMUL_VFP(v[0],(eFSReg)(f0+0),mul[0]);
					}
					else if (j==3 && w1)
					{
						//known w=1: plain add, and vadd retires ~4 cycles
						//sooner than a dependent vmla at the chain tail
						for (int i=0;i<4;i++)
							VADD_VFP(v[i],v[i],(eFSReg)(f0+12+i));
					}
					else
					{
						for (int i=0;i<4;i++)
							VMLA_VFP(v[i],(eFSReg)(f0+4*j+i),mul[j]);
					}
				}
			}
			break;


			case shop_frswap:
			{
				reg.preload_fpu+=16;
				reg.writeback_fpu+=16;

				verify(op->rd._reg==op->rs2._reg);
				verify(op->rd2._reg==op->rs1._reg);

				verify(op->rs1.count()==16 && op->rs2.count()==16);
				verify(op->rd2.count()==16 && op->rd.count()==16);

				SUB(r0,r8,op->rs1.reg_aofs());
				SUB(r1,r8,op->rd.reg_aofs());
				//Assumes no FPU reg alloc here
				//frswap touches all FPU regs, so all spans should be clear here ..
				VLDM(d0,r1,8);
				VLDM(d8,r0,8);
				VSTM(d0,r0,8);
				VSTM(d8,r1,8);
			}
			break;

			
			case shop_cvt_f2i_t:
			{
				eFSReg t=fpu_temps_avail?f28:f0;
				VCVT_to_S32_VFP(t,reg.mapf(op->rs1));
				VMOV(reg.mapg(op->rd),t);
				break;
			}

			case shop_cvt_i2f_n:	// may be some difference should be made ?
			case shop_cvt_i2f_z:
			{
				eFSReg t=fpu_temps_avail?f28:f0;
				VMOV(t, reg.mapg(op->rs1));
				VCVT_from_S32_VFP(reg.mapfs(op->rd),t);
				break;
			}
#endif

		default:
			//printf("CFB %d\n",op->op);
			shil_chf[op->op](op);
			break;

__default:
			printf("@@\tError, Default case (0x%X) in ngen_CompileBlock!\n", op->op);
			verify(false);
			break;
		}
}

//Does compiling this opcode invalidate the xmtrx cached in s0-s15?
//Whitelist: opcodes whose emitters neither call out nor touch s0-s15.
//Any memory access that could ever fault must kill: ngen_Rewrite may turn
//its fastpath into a handler call at any time, and handlers clobber the
//caller-saved vfp bank.
static bool op_kills_xmtrx(shil_opcode* op)
{
	//a write into the back bank changes the matrix itself
	const shil_param* wr[2]={&op->rd,&op->rd2};
	for (int i=0;i<2;i++)
	{
		if (wr[i]->is_reg() &&
		    wr[i]->_reg<=reg_xf_15 &&
		    wr[i]->_reg+wr[i]->count()-1>=reg_xf_0)
			return true;
	}

	switch(op->op)
	{
	case shop_mov32: case shop_mov64:
	case shop_jdyn: case shop_jcond:
	case shop_add: case shop_sub: case shop_and: case shop_or: case shop_xor:
	case shop_not: case shop_neg: case shop_shl: case shop_shr: case shop_sar:
	case shop_ror: case shop_rocl: case shop_rocr: case shop_adc: case shop_sbc:
	case shop_shld: case shop_shad: case shop_ext_s8: case shop_ext_s16:
	case shop_mul_u16: case shop_mul_s16: case shop_mul_i32:
	case shop_mul_u64: case shop_mul_s64:
	case shop_div32p2:
	case shop_test: case shop_seteq: case shop_setge: case shop_setgt:
	case shop_setae: case shop_setab: case shop_setpeq:
	case shop_fadd: case shop_fsub: case shop_fmul: case shop_fdiv:
	case shop_fabs: case shop_fneg: case shop_fsqrt: case shop_fmac:
	case shop_fseteq: case shop_fsetgt:
	case shop_fsrra: case shop_cvt_f2i_t: case shop_cvt_i2f_n: case shop_cvt_i2f_z:
	case shop_fsca:		//table lookup via the d14 scratch, no call
	case shop_fipr:		//accumulates in the f28+ temps
	case shop_ftrv:		//keeps the matrix resident
		return false;

	case shop_readm:
		//imm reads from ram are plain host loads and can never fault;
		//imm mmio reads call the handler, reg-address reads may be
		//rewritten into one later
		if (op->rs1.is_imm())
		{
			bool isram=false;
			_vmem_read_const(op->rs1._imm,isram,memop_bytes(memop_type(op)));
			return !isram;
		}
		return true;

	case shop_writem:
		//sq-marked stores hit the context inline and can never fault
		return op->flags2!=0x1337;

	default:
		//pref/ifb/sync_sr/sync_fpscr/frswap/fsca/canonical fallbacks:
		//they call out or clobber the caller-saved vfp bank
		return true;
	}
}

struct Arm32NGenBackend: NGenBackend
{

	bool Init()
	{
		printf("Initializing the ARM32 dynarec\n");
	    verify(FPCB_OFFSET == -0x2100000 || FPCB_OFFSET == -0x4100000);
	    verify(rcb_noffs(p_sh4rcb->fpcb) == FPCB_OFFSET);
	   	
	    for (int s=0;s<6;s++)
		{
			void* fn=s==0?(void*)_vmem_ReadMem8SX32:
					 s==1?(void*)_vmem_ReadMem16SX32:
					 s==2?(void*)_vmem_ReadMem32:
					 s==3?(void*)_vmem_WriteMem8:
					 s==4?(void*)_vmem_WriteMem16:
					 s==5?(void*)_vmem_WriteMem32:
					 0;

			bool read=s<=2;

			//r0 to r13
			for (int i=0;i<=13;i++)
			{
				if (i==1 || i ==2 || i == 3 || i == 4 || i==12 || i==13)
					continue;

				unat v;
				if (i==0)
					v=(unat)fn;
				else
				{
					v=(unat)EMIT_GET_PTR();
					MOV(r0,(eReg)(i));
					JUMP((u32)fn);
				}

				_mem_hndl[read][s%3][i]=v;
			}
		}

		for (int i=0;i<=13;i++)
		{
			if (i==1 || i ==2 || i == 3 || i == 4 || i==12 || i==13)
				continue;

			_mem_hndl_SQ32[i]=(unat)EMIT_GET_PTR();

			//UBFX(r3,(eReg)i,0,6);
			AND(r3,(eReg)i,0x3F);
			LSR(r2,(eReg)i,26);
			MOV(r0,(eReg)i);
			ADD(r3,r3,r8);
			CMP(r2,0x38);
			JUMP((unat)&WriteMem32,CC_NE);
			STR(r1,r3,rcb_noffs(sq_both));
			BX(LR);
		}

		printf("readm helpers: up to %08X\n",EMIT_GET_PTR());
		emit_SetBaseAddr();


		ccmap[shop_test]=CC_EQ;
		ccnmap[shop_test]=CC_NE;

		ccmap[shop_seteq]=CC_EQ;
		ccnmap[shop_seteq]=CC_NE;
		
		
		ccmap[shop_setge]=CC_GE;
		ccnmap[shop_setge]=CC_LT;
		
		ccmap[shop_setgt]=CC_GT;
		ccnmap[shop_setgt]=CC_LE;

		ccmap[shop_setae]=CC_HS;
		ccnmap[shop_setae]=CC_LO;

		ccmap[shop_setab]=CC_HI;
		ccnmap[shop_setab]=CC_LS;

		//ccmap[shop_fseteq]=CC_EQ;
		//ccmap[shop_fsetgt]=CC_GT;

		this->Mainloop = &ngen_mainloop;
		this->FailedToFindBlock = &ngen_FailedToFindBlock_;

		return true;
	}


	void GetFeatures(ngen_features* dst)
	{
		dst->InterpreterFallback=false;
		dst->OnlyDynamicEnds=false;
	}

	RuntimeBlockInfo* AllocateBlock()
	{
		return new DynaRBI();
	};


	void Compile(RuntimeBlockInfo* block, SmcCheckEnum smc_checks, bool reset, bool staging,bool optimise)
	{
		//printf("Compile: %08X, %d, %d\n",block->addr,staging,optimise);
		block->code=(DynarecCodeEntryPtr)EMIT_GET_PTR();

		//StoreImms(r0,r1,(u32)&last_run_block,(u32)code); //useful when code jumps to random locations ...
		++blockno;

		if (settings.profile.run_counts)
		{
			MOV32(r1,(u32)&block->runs);
			LDR(r0,r1);
			ADD(r0,r0,1);
			STR(r0,r1);
		}

		//blocks with vector fpu ops reserve f28-f31 as backend scratch and
		//may keep the xmtrx cached in s0-s15 across opcodes
		fpu_temps_avail=false;
		for (size_t i=0;i<block->oplist.size();i++)
		{
			if (block->oplist[i].op==shop_ftrv || block->oplist[i].op==shop_fipr)
			{
				fpu_temps_avail=true;
				break;
			}
		}
		xmtrx_resident=false;

		//a staging block is counted down per entry to decide when to recompile
		//it optimised, so it must keep going around through the dispatcher
		bool self_loop=!staging && block_self_loops(block);

		//reg alloc
		reg.block_loops=self_loop;
		reg.DoAlloc(block,alloc_regs,fpu_temps_avail?alloc_fpu_temps:alloc_fpu);

		u8* blk_start=(u8*)EMIT_GET_PTR();

		if (staging)
		{
			MOV32(r0,(u32)&block->staging_runs);
			LDR(r1,r0);
			SUB(r1,r1,1);
			STR(r1,r0);
		}
		//pre-load the first reg alloc operations, for better efficiency ..
		block->host_preload0=(u8*)EMIT_GET_PTR()-blk_start;
		if (self_loop)
			reg.OpBeginCarried(&block->oplist[0],0);	//the rest below the loop header
		else
			reg.OpBegin(&block->oplist[0],0);
		block->host_preload0_end=(u8*)EMIT_GET_PTR()-blk_start;

		//scheduler
		switch (smc_checks) {
			case NoCheck:
				break;

			case FastCheck: {
				MOV32(r0,block->addr);
				u32* ptr=(u32*)GetMemPtr(block->addr,4);
				if (ptr != NULL)
				{
					MOV32(r2,(u32)ptr);
					LDR(r2,r2,0);
					MOV32(r1,*ptr);
					CMP(r1,r2);
					JUMP((u32)ngen_blockcheckfail, CC_NE);
				}
			}
			break;

			case FullCheck: {
				s32 sz = block->sh4_code_size;
				u32 addr = block->addr;
				MOV32(r0,addr);

				while (sz > 0)
				{
					if (sz > 2)
					{
						u32* ptr=(u32*)GetMemPtr(addr,4);
						if (ptr != NULL)
						{
							MOV32(r2,(u32)ptr);
							LDR(r2,r2,0);
							MOV32(r1,*ptr);
							CMP(r1,r2);

							JUMP((u32)ngen_blockcheckfail, CC_NE);
						}
						addr += 4;
						sz -= 4;
					}
					else
					{
						u16* ptr = (u16 *)GetMemPtr(addr, 2);
						if (ptr != NULL)
						{
							MOV32(r2, (u32)ptr);
							LDRH(r2, r2, 0, AL);
							MOVW(r1, *ptr, AL);
							CMP(r1, r2);

							JUMP((u32)ngen_blockcheckfail, CC_NE);
						}
						addr += 2;
						sz -= 2;
					}
				}
			}
			break;

			default: {
				die("unhandled smc_checks");
			}
		}

		u32 cyc=block->guest_cycles;
		if (!is_i8r4(cyc))
		{
			cyc&=~3;
		}

		//only provably-backward static branches poll for interrupts (subs +
		//blle intc_sched); everything else -- forward statics AND dynamic
		//ends -- just pays the decrement with a plain sub.  Constprop-promoted
		//static ends have a known target and are judged like any other static.
		bool cycle_check=true;
		if (settings.dynarec.opt_cyclecheck_backwards_only)
		{
			switch(block->BlockType)
			{
			case BET_StaticJump:
			case BET_Cond_0:
			case BET_Cond_1:	//NextBlock is always forward
				cycle_check = block->BranchBlock<=block->addr;
				break;

			default:
				cycle_check = true;	//dynamic ends/ calls: always do the calls
				break;
			}
		}

	#if HOST_OS == OS_DARWIN
		SUB(r11,r11,cyc,cycle_check,CC_AL);
	#else
		SUB(rfp_r9,rfp_r9,cyc,cycle_check,CC_AL);
	#endif
		if (cycle_check)
			CALL((u32)intc_sched, CC_LE);

		//The matrix is another thing a loop need not redo: load it above the
		//loop header and the ftrv emitters skip their column loads entirely.
		//Only when nothing in the block can clobber s0-s15 though -- and that
		//includes every memop, because ngen_Rewrite may turn its fastpath into
		//a handler call at any time, and the handler owns the caller-saved vfp
		//bank the matrix lives in.
		if (self_loop && fpu_temps_avail)
		{
			shil_opcode* ftrv=0;
			bool kills=false;

			for (size_t i=0;i<block->oplist.size();i++)
			{
				if (block->oplist[i].op==shop_ftrv && !ftrv)
					ftrv=&block->oplist[i];

				kills|=op_kills_xmtrx(&block->oplist[i]);
			}

			if (ftrv && !kills)
			{
				SUB(r2,r8,ftrv->rs2.reg_aofs());
				VLDM(d0,r2,8);
				xmtrx_resident=true;
			}
		}

		//everything above runs once per entry from the outside; the back edge
		//comes back here with the carried registers still loaded
		u32 loop_header=(u32)EMIT_GET_PTR();

		if (self_loop)
			reg.OpBeginTransient(&block->oplist[0],0);

		//compile the block's opcodes
		shil_opcode* op;
		for (size_t i=0;i<block->oplist.size();i++)
		{
			op=&block->oplist[i];

			op->host_offs=(u8*)EMIT_GET_PTR()-blk_start;

			if (i!=0)
				reg.OpBegin(op,i);

			op->host_body=(u8*)EMIT_GET_PTR()-blk_start;

			ngen_compile_opcode(block,op,staging,optimise);

			op->host_body_end=(u8*)EMIT_GET_PTR()-blk_start;

			if (xmtrx_resident && op_kills_xmtrx(op))
				xmtrx_resident=false;

			//the loop path branches off before the writebacks
			if (self_loop && i+1==block->oplist.size())
				emit_self_loop_tail(block,loop_header,cyc);

			reg.OpEnd(op);
		}

		/*

		extern u32 ralst[4];

		MOV32(r0,(u32)&ralst[0]);
		
		LDR(r1,r0,0);
		ADD(r1,r1,reg.preload_gpr);
		STR(r1,r0,0);

		LDR(r1,r0,4);
		ADD(r1,r1,reg.preload_fpu);
		STR(r1,r0,4);

		LDR(r1,r0,8);
		ADD(r1,r1,reg.writeback_gpr);
		STR(r1,r0,8);

		LDR(r1,r0,12);
		ADD(r1,r1,reg.writeback_fpu);
		STR(r1,r0,12);
		*/

		/*
			//try to early-lookup the blocks -- to avoid rewrites in case they exist ...
			//this isn't enabled for now, as I'm not quite solid on the state of block referrals ..

			block->pBranchBlock=bm_GetBlock(block->BranchBlock);
			block->pNextBlock=bm_GetBlock(block->NextBlock);
			if (block->pNextBlock) block->pNextBlock->AddRef(block);
			if (block->pBranchBlock) block->pBranchBlock->AddRef(block);
		*/

		
		//Relink written bytes must be added to the count !

		block->relink_offset=(u8*)EMIT_GET_PTR()-(u8*)block->code;
		block->relink_data=0;

		emit_Skip(block->Relink());
		u8* pEnd = (u8*)EMIT_GET_PTR();

		// Clear the area we've written to for cache
		CacheFlush((void*)block->code, pEnd);

		//blk_start might not be the same, due to profiling counters ..
		block->host_opcodes=(pEnd-blk_start)/4;

		//host code size needs to cover the entire range of the block
		block->host_code_size=(pEnd-(u8*)block->code);

		void emit_WriteCodeCache();
	//	emit_WriteCodeCache();
	}

	void OnResetBlocks()
	{
		printf("@@\tngen_ResetBlocks()\n");
	}

	
	//A fused float load pair (fuse_readm_pairs) shares one address computation:
	//   bic r1,raddr,#e0000000 / add r1,r1,r8 / vldr a,[r1] / vldr b,[r1,#size]
	//Is this instruction the [r1,#size] half of such a pair?
	static bool is_pair_second(const arm_mem_op* op, mem_op_type optp)
	{
		const u32 key=optp==SZ_64F?0x0D100B00u:0x0D100A00u;

		//same size, base r1, positive immediate of exactly one element
		return (op->full&0x0F300F00)==key && op->Rn==1 &&
			(op->full&(1<<23))!=0 && (op->full&0xFF)==memop_bytes(optp)/4;
	}

	//...and is it the [r1] half, with its sibling right behind it?
	static bool is_pair_first(const arm_mem_op* op, mem_op_type optp)
	{
		const u32 key=optp==SZ_64F?0x0D100B00u:0x0D100A00u;

		return (op->full&0x0F300F00)==key && op->Rn==1 && (op->full&0xFF)==0 &&
			is_pair_second(op+1,optp);
	}

	//The handler call clobbers r1, so the sibling load cannot stay where it is.
	//It moves to a fixup at the end of the code cache, which recomputes the
	//address from raddr (callee saved, and the fusion pass keeps the guest's
	//post-increment after the pair, so it still holds the base) and calls the
	//handler too. Its slot in the block becomes a branch here.
	static void RewritePairSecond(arm_mem_op* sibling, eReg raddr, mem_op_type optp)
	{
		eFSReg ft=(eFSReg)(sibling->Rt*2 + sibling->D);
		eFDReg fd=(eFDReg)(sibling->D*16 + sibling->Rt);
		u32 back=(u32)(sibling+1);

		verify(emit_ptr==0);
		verify(emit_FreeSpace()>64);

		u32 fixup=(u32)EMIT_GET_PTR();

		ADD(r0,raddr,(s32)memop_bytes(optp));
		CALL(_mem_func[1][optp]);

		if (optp==SZ_32F)
			VMOV(ft,r0);
		else
			VMOV(fd,r0,r1);

		JUMP(back);

		CacheFlush((void*)fixup,EMIT_GET_PTR());

		//the branch has to fit the one slot the sibling occupies
		snat lit=(snat)fixup-((snat)sibling+8);
		verify(lit>=-33554432 && lit<=33554428);

		emit_ptr=(u32*)sibling;
		JUMP(fixup);
		verify(emit_ptr==(u32*)(sibling+1));
		emit_ptr=0;

		CacheFlush((void*)sibling,(void*)(sibling+1));
	}

	bool Rewrite(rei_host_context_t* ctx)
	{
		arm_mem_op* ptr=(arm_mem_op*)ctx->pc;
		auto regs = ctx->regs;

		verify(sizeof(*ptr)==4);

		mem_op_type optp;
		u32 read=0;
		s32 offs=-1;

		u32 fop=ptr[0].full;

		for (int i=0;op_table[i].mask;i++)
		{
			if ((fop&op_table[i].mask)==op_table[i].key)
			{
				optp=op_table[i].optp;
				read=op_table[i].read;
				offs=op_table[i].offs;
			}
		}

		if (offs==-1)
		{
			printf("%08X : invalid size\n",ptr[0]);
			die("can't decode opcode\n");
		}

		//the second half of a fused pair has no address setup of its own: the
		//first half keeps its fastpath, this one moves to a fixup
		if (read && (optp==SZ_32F || optp==SZ_64F) && is_pair_second(ptr,optp) &&
			is_pair_first(ptr-1,optp))
		{
			arm_mem_op* setup=ptr-1-offs;
			eReg raddr;

			if ((setup[0].full & 0x0FE00070)==0x07E00050)
				raddr=(eReg)(setup[0].Ra);
			else if ((setup[0].full & 0x0FE00000)==0x03C00000)
				raddr=(eReg)(setup[0].Rn);
			else
			{
				printf("fail raddr (pair) %08X:(\n",setup[0].full);
				die("Invalid opcode: vmem fixup\n");
			}

			RewritePairSecond(ptr,raddr,optp);

			//re-run the slot, it is a branch to the fixup now
			return true;
		}

		//a fused pair's first half: rewriting it clobbers the shared base, so
		//the sibling has to be moved out of the way as well (below)
		arm_mem_op* pair_sibling=read && (optp==SZ_32F || optp==SZ_64F) &&
			is_pair_first(ptr,optp) ? ptr+1 : 0;

		ptr -= offs;

		eReg raddr,rt;
		eFSReg ft;
		eFDReg fd;

		//Get used regs from opcodes ..
		
		if ((ptr[0].full & 0x0FE00070)==0x07E00050)
		{
			//from ubfx !
			raddr=(eReg)(ptr[0].Ra);
		}
		else if ((ptr[0].full & 0x0FE00000)==0x03C00000)
		{
			raddr=(eReg)(ptr[0].Rn);
		}
		else
		{
			printf("fail raddr %08X {@%08X}:(\n",ptr[0].full,regs[1]);
			die("Invalid opcode: vmem fixup\n");
		}
		//from mem op
		rt=(eReg)(ptr[offs].Rt);
		ft=(eFSReg)(ptr[offs].Rt*2 + ptr[offs].D);
		fd=(eFDReg)(ptr[offs].D*16 + ptr[offs].Rt);

		//get some other relevant data
		u32 sh4_addr=regs[raddr];
		
		u8* sh4_ctr=(u8*)regs[8];
		bool is_sq=(sh4_addr>>26)==0x38;

		verify(emit_ptr==0);
		emit_ptr=(u32*)ptr;


		/*
				mov r0,raddr

			8/16/32I:
				call _mem_hdlp[read][optp][rt]
			32F/64F:
		32F,r:	vmov r1,ft
		64F,r:	vmov [r3:r2],fd
				call _mem_hdlp[read][optp][0]
		32F,w:	vmov ft,r0
		64F,w:	vmov fd,[r1:r0]
		*/
		
		
		//printf("Failed %08X:%08X (%d,%d,%d,r%d, r%d,f%d,d%d) code %08X, addr %08X, native %08X (%08X), fixing via %s\n",ptr->full,fop,optp,read,offs,raddr,rt,ft,fd,ptr,sh4_addr,fault_addr,fault_offs,is_sq?"SQ":"MR");

		//fault offset must always be the addr from ubfx (sanity check)

		// TODO: Reimplement this sanity check, it needs the fault mem address
		
		#if 0 && TODO
			u32 fault_offs=fault_addr-regs[8];
			verify((fault_offs==0) || fault_offs==(0x1FFFFFFF&sh4_addr));
		#endif

		if (settings.dynarec.opt_sqw_rewrite && is_sq) //THPS2 uses cross area SZ_32F so this is disabled for now
		{
			//SQ !
			s32 sq_offs=sq_both-sh4_ctr;
			verify(sq_offs==rcb_noffs(sq_both));
			
			verify(!read && optp>=SZ_32I);

			if (optp==SZ_32I)
			{
				MOV(r1,rt);

				CALL((unat)_mem_hndl_SQ32[raddr]);
			}
			else
			{
				//UBFX(r1,raddr,0,6);
				AND(r1,raddr,0x3F);
				ADD(r1,r1,r8);

				if (optp==SZ_32I) STR(rt,r1,sq_offs); // cross writes are possible, so this can't be assumed
				else if (optp==SZ_32F) VSTR(ft,r1,sq_offs/4);
				else if (optp==SZ_64F) VSTR(fd,r1,sq_offs/4);
			}
		}
		else
		{
			//Fallback to function !

			if (offs==2)
			{
				if (raddr!=r0)
					MOV(r0,(eReg)raddr);
				else
					NOP();
			}

			if (!read)
			{
				if (optp<=SZ_32I) MOV(r1,rt);
				else if (optp==SZ_32F) VMOV(r1,ft);
				else if (optp==SZ_64F) VMOV(r2,r3,fd);
			}

			//staging register, or the callee-saved pair the operand lives in
			verify(optp!=SZ_64F || fd==d0 || (fd>=d8 && fd<=d15));

			u32 funct=0;

			if (offs==1)
				funct=_mem_hndl[read][optp][raddr];
			else if (offs==2)
				funct=_mem_func[read][optp];

			verify(funct!=0);
			CALL(funct);

			if (read)
			{
				if (optp<=SZ_32I) MOV(rt,r0);
				else if (optp==SZ_32F) VMOV(ft,r0);
				else if (optp==SZ_64F) VMOV(fd,r0,r1);
			}
		}


		CacheFlush((void*)ptr, (void*)emit_ptr);

		//the call above just clobbered the base the sibling load reads through
		if (pair_sibling)
			verify(emit_ptr==(u32*)pair_sibling);

		emit_ptr=0;

		if (pair_sibling)
			RewritePairSecond(pair_sibling,raddr,optp);

		ctx->pc = (unat)ptr;

		return true;
	}

	void CC_Start(shil_opcode* op) 
	{ 
		CC_pars.clear();
	}
	void CC_Param(shil_opcode* op,shil_param* par,CanonicalParamType tp) 
	{ 
		switch(tp)
		{
			case CPT_f32rv:
			#ifdef ARM_HARDFP
				{
					if (reg.IsAllocg(*par))
					{
						//printf("MOV(reg.map(*par),r0); %d\n",reg.map(*par));
						VMOV(reg.mapg(*par),f0);
					}
					else if (reg.IsAllocf(*par))
					{
						//VMOV(reg.mapf(*par),0,r0); %d\n",reg.map(*par));
						VMOV(reg.mapfs(*par),f0);
					}
				}
				break;
			#endif

			case CPT_u32rv:
			case CPT_u64rvL:
				{
					if (reg.IsAllocg(*par))
					{
						//printf("MOV(reg.map(*par),r0); %d\n",reg.map(*par));
						MOV(reg.mapg(*par),r0);
					}
					else if (reg.IsAllocf(*par))
					{
						//VMOV(reg.mapf(*par),0,r0); %d\n",reg.map(*par));
						VMOV(reg.mapfs(*par),r0);
					}
				}
				break;

			case CPT_u64rvH:
				{
					verify(reg.IsAllocg(*par));

					MOV(reg.mapg(*par),r1);
				}
				break;

			case CPT_u32:
			case CPT_ptr:
			case CPT_f32:
				{
					CC_PS t={tp,par};
					CC_pars.push_back(t);
				}
				break;

			default:
				die("invalid tp");
		}
	}

	void CC_Call(shil_opcode* op,void* function) 
	{
		u32 rd=r0;
		u32 fd=f0;

		for (int i=CC_pars.size();i-->0;)
		{
			if (CC_pars[i].type==CPT_ptr)
			{
				MOV32((eReg)rd, (u32)CC_pars[i].par->reg_ptr());
			}
			else
			{
				if (CC_pars[i].par->is_reg())
				{
					#ifdef ARM_HARDFP
					if (CC_pars[i].type == CPT_f32) 
					{
						if (reg.IsAllocg(*CC_pars[i].par))
						{
							//printf("MOV((eReg)rd,reg.map(*CC_pars[i].par)); %d %d\n",rd,reg.map(*CC_pars[i].par));
							VMOV((eFSReg)fd,reg.mapg(*CC_pars[i].par));
						}
						else if (reg.IsAllocf(*CC_pars[i].par))
						{
							//printf("LoadSh4Reg_mem((eReg)rd, *CC_pars[i].par); %d\n",rd);
							VMOV((eFSReg)fd,reg.mapfs(*CC_pars[i].par));
						}
						else
							die("Must not happen!\n");
						continue;
					}
					#endif

					if (reg.IsAllocg(*CC_pars[i].par))
					{
						//printf("MOV((eReg)rd,reg.map(*CC_pars[i].par)); %d %d\n",rd,reg.map(*CC_pars[i].par));
						MOV((eReg)rd,reg.mapg(*CC_pars[i].par));
					}
					else if (reg.IsAllocf(*CC_pars[i].par))
					{
						//printf("LoadSh4Reg_mem((eReg)rd, *CC_pars[i].par); %d\n",rd);
						VMOV((eReg)rd,reg.mapfs(*CC_pars[i].par));
					}
					else
						die("Must not happen!\n");
				}
				else
				{
					verify(CC_pars[i].type != CPT_f32);
					//printf("MOV32((eReg)rd, CC_pars[i].par->_imm); %d\n",rd);
					MOV32((eReg)rd, CC_pars[i].par->_imm);
				}
			}
			rd++;
		}
		//printf("used reg r0 to r%d, %d params, calling %08X\n",rd-1,CC_pars.size(),function);
		CALL((u32)function);
	}

	void CC_Finish(shil_opcode* op) 
	{ 
		CC_pars.clear(); 
	}
};

void CacheFlush()
{
	printf("Flushing cache from %08p to %08p\n", &CodeCache[0], &CodeCache[CODE_SIZE - 1]);
	//CacheFlush(&CodeCache[0], &CodeCache[CODE_SIZE - 1]);
}



static NGenBackend* create_ngen_arm32() {
	return new Arm32NGenBackend();
}


static auto rec_cpp = rdv_RegisterShilBackend(ngen_backend_t{"arm32", "Fast arm32 jit", &create_ngen_arm32 });

#endif
