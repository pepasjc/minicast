/*
	This file is part of libswirl
*/
#include "license/bsd"


/*
	Some WIP optimisation stuff and maby helper functions for shil
*/

#include <sstream>

#include "types.h"
#include "shil.h"
#include "decoder.h"
#include "hw/sh4/sh4_mem.h"
#include "blockmanager.h"


#define printf_sqwm(...) 

u32 RegisterWrite[sh4_reg_count];
u32 RegisterRead[sh4_reg_count];

void RegReadInfo(shil_param p,size_t ord)
{
	if (p.is_reg())
	{
		for (u32 i=0; i<p.count(); i++)
			RegisterRead[p._reg+i]=(u32)ord;
	}
}
void RegWriteInfo(shil_opcode* ops, shil_param p,size_t ord)
{
	if (p.is_reg())
	{
		for (u32 i=0; i<p.count(); i++)
		{
			if (RegisterWrite[p._reg+i]>=RegisterRead[p._reg+i] && RegisterWrite[p._reg+i]!=0xFFFFFFFF)	//if last read was before last write, and there was a last write
			{
				printf("DEAD OPCODE %d %zd!\n",RegisterWrite[p._reg+i],ord);
				ops[RegisterWrite[p._reg+i]].Flow=1; //the last write was unused
			}
			RegisterWrite[p._reg+i]=(u32)ord;
		}
	}
}
u32 fallback_blocks;
u32 total_blocks;
u32 REMOVED_OPS;

bool isdst(shil_opcode* op,Sh4RegType rd)
{
	return (op->rd.is_r32() && op->rd._reg==rd) || (op->rd2.is_r32() && op->rd2._reg==rd);
}

//really hacky ~
//Obsolete: constprop() subsumes this (address promotion for any base reg, not just r0).
//Kept around for reference only; no callers.
void PromoteConstAddress(RuntimeBlockInfo* blk)
{
	bool is_const=false;
	u32 value=0;

	total_blocks++;
	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];
		if (is_const && op->op==shop_readm && op->rs1.is_reg() && op->rs1._reg==reg_r0)
		{
			u32 val=value;
			if (op->rs3.is_imm())
			{
				val+=op->rs3._imm;
				op->rs3=shil_param();
			}
			op->rs1=shil_param(FMT_IMM,val);
		}

		if (op->op==shop_mov32 && op->rs1.is_imm() && isdst(op,reg_r0) )
		{
			is_const=true;
			value=op->rs1._imm;
		}
		else if (is_const && (isdst(op,reg_r0) || op->op==shop_ifb || op->op==shop_sync_sr) )
			is_const=false;
	}
}

//The store queue is write only: whatever the writes before the pref look like,
//a register that anything in the block loads through is not pointing at one,
//and marking its writes would send them to the sq buffer instead of memory.
bool sq_reg_is_read(RuntimeBlockInfo* blk, Sh4RegType rt)
{
	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];

		if (op->op==shop_readm && op->rs1.is_reg() && op->rs1._reg==rt)
			return true;
	}

	return false;
}

bool sq_pref(RuntimeBlockInfo* blk, int i, Sh4RegType rt, bool mark)
{
	u32 data=0;
	u32 unit=0;
	for (int c=i-1;c>0;c--)
	{
		if (blk->oplist[c].op==shop_writem && blk->oplist[c].rs1._reg==rt)
		{
			if (blk->oplist[c].rs2.is_r32i() ||  blk->oplist[c].rs2.is_r32f() || blk->oplist[c].rs2.is_r64f() || blk->oplist[c].rs2.is_r32fv())
			{
				data+=blk->oplist[c].flags;
				unit=blk->oplist[c].flags;
				if (mark)
					blk->oplist[c].flags2=0x1337;
			}
			else
				break;
		}

		//add rt <= rt, imm with imm == the store size is the post-increment between
		//SQ stores (fmov @Rn; add #sz,Rn idiom) - base stays in the line, keep scanning
		if (blk->oplist[c].op==shop_add && blk->oplist[c].rd.is_reg() && blk->oplist[c].rd._reg==rt &&
			blk->oplist[c].rs1.is_reg() && blk->oplist[c].rs1._reg==rt &&
			blk->oplist[c].rs2.is_imm() && unit!=0 && blk->oplist[c].rs2._imm==unit)
		{
		}
		else if (blk->oplist[c].op==shop_pref || (blk->oplist[c].rd.is_reg() && blk->oplist[c].rd._reg==rt && blk->oplist[c].op!= shop_sub))
		{
			break;
		}

		if (data==32)
			break;
	}

	if (mark) return true;

	if (data>=8)
	{
		blk->oplist[i].flags =0x1337;
		sq_pref(blk,i,rt,true);
		printf_sqwm("SQW-WM match %d !\n",data);
		return true;
	}
	else if (data)
	{
		printf_sqwm("SQW-WM FAIL %d !\n",data);
		return false;
	}
	return false;
}

bool sq_pref(RuntimeBlockInfo* blk)
{
	bool optimized = false;
	for (int i=0;i<blk->oplist.size();i++)
	{
		blk->oplist[i].flags2=0;
		if (blk->oplist[i].op==shop_pref && !sq_reg_is_read(blk,blk->oplist[i].rs1._reg)) {
			if (sq_pref(blk,i,blk->oplist[i].rs1._reg,false)) {
				optimized = true;
			}
		}
	}

	if (optimized) {
		// auto f = stdout;
		// fprintf(f,"block: %p\n",blk);
		// fprintf(f,"addr: %08X\n",blk->addr);
		// fprintf(f,"il_opcodes: %zd\n",blk->oplist.size());


		// for (u32 j=0;j<blk->oplist.size();j++)
		// {
		// 	shil_opcode* op=&blk->oplist[j];
		// 	fprintf(f,"//il:%d:%d:%s\n",op->guest_offs,op->host_offs,op->dissasm().c_str());
		// }
		return true;
	}
	return false;
}

bool has_ftrv(RuntimeBlockInfo* blk)
{
	for (int i=0;i<blk->oplist.size();i++)
	{
		if (blk->oplist[i].op==shop_ftrv) {
			return true;
		}
	}
	return false;
}

//Read Groups
void rdgrp(RuntimeBlockInfo* blk)
{
	size_t started=-1;
	Sh4RegType reg=NoReg;
	Sh4RegType regd=NoReg;
	u32 stride=0;
	bool pend_add=false;
	u32 rdc=0;
	u32 addv=0;

	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];
		op->Flow=0;

		if (started<0)
		{
			if (op->op==shop_readm &&  op->rd.type>=FMT_F32 && op->rs1.is_reg() && op->rs3.is_null())
			{
				started=i;
				stride=op->rd.count();
				reg=op->rs1._reg;
				regd=op->rd._reg;
				pend_add=true;
				rdc=1;
				addv=0;
			}
		}
		else
		{
			if (!pend_add && op->op==shop_readm && op->rd._reg==(regd+stride) && op->rs1.is_reg() && op->rs1._reg==reg  && op->rs3.is_null())
			{
				regd=(Sh4RegType)(regd+stride);
				pend_add=true;
				rdc++;
			}
			else if (pend_add && op->op==shop_add && op->rd._reg==op->rs1._reg && op->rs1.is_reg() && op->rs2.is_imm() && op->rs2._imm==(stride*4))
			{
				pend_add=false;
				addv+=op->rs2._imm;
			}
			else
			{
				u32 byts=rdc*stride*4;
				if (rdc!=1 && (byts==8 || byts==12 || byts==16 || byts==32 || byts==64))
				{
					verify(addv==byts || (pend_add && (addv+stride*4==byts)));

					blk->oplist[started].rd.type=byts==8?FMT_V2:byts==12?FMT_V3:
						byts==16?FMT_V4:byts==32?FMT_V8:FMT_V16;
					blk->oplist[started].flags=byts|0x80;
					if (stride==8)
						blk->oplist[started].flags|=0x100;
					blk->oplist[started].Flow=(rdc-1)*2 - (pend_add?1:0);
					blk->oplist[started+1].rs2._imm=addv;

					printf("Read Combination %d %d!\n",rdc,addv);
				}
				else if (rdc!=1)
				{
					printf("Read Combination failed %d %d %d\n",rdc,rdc*stride*4,addv);
				}
				started=-1;
			}
		}
	}

	
	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];
		if (op->Flow)
		{
			blk->oplist.erase(blk->oplist.begin()+i+2,blk->oplist.begin()+i+2+op->Flow);
		}
	}
}
//Write Groups
void wtgrp(RuntimeBlockInfo* blk)
{
	size_t started=-1;
	Sh4RegType reg=NoReg;
	Sh4RegType regd=NoReg;
	u32 stride=0;
	bool pend_add=false;
	u32 rdc=0;
	u32 addv=0;

	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];
		op->Flow=0;

		if (started<0)
		{
			if (op->op==shop_writem &&  op->rs2.type>=FMT_F32 && op->rs1.is_reg() && op->rs3.is_null())
			{
				started=i;
				stride=op->rd.count();
				reg=op->rs1._reg;
				regd=op->rs2._reg;
				pend_add=true;
				rdc=1;
				addv=0;
			}
		}
		else
		{
			if (!pend_add && op->op==shop_writem && op->rs2._reg==(regd-stride) && op->rs1.is_reg() && op->rs1._reg==reg  && op->rs3.is_null())
			{
				regd=(Sh4RegType)(regd-stride);
				pend_add=true;
				rdc++;
			}
			else if (pend_add && op->op==shop_sub && op->rd._reg==op->rs1._reg && op->rs1.is_reg() && op->rs1._reg==reg && op->rs2.is_imm() && op->rs2._imm==(stride*4))
			{
				pend_add=false;
				addv+=op->rs2._imm;
			}
			else
			{
				u32 byts=rdc*stride*4;
				u32 mask=byts/4;
				if (mask==3) mask=4;
				mask--;

				if (rdc!=1 /*&& (!(regd&mask))*/ && (byts==8 || byts==12 || byts==16 || byts==32 || byts==64))
				{
					verify(addv==byts || (pend_add && (addv+stride*4==byts)));

					blk->oplist[started].rs2.type=byts==8?FMT_V2:byts==12?FMT_V3:
						byts==16?FMT_V4:byts==32?FMT_V8:FMT_V16;
					blk->oplist[started].rs2._reg=regd;
					blk->oplist[started].rs3._imm=-(rdc-1)*stride*4;
					blk->oplist[started].rs3.type=FMT_IMM;
					blk->oplist[started].flags=byts|0x80;
					if (stride==8)
						blk->oplist[started].flags|=0x100;
					blk->oplist[started].Flow=(rdc-1)*2 - (pend_add?1:0);
					blk->oplist[started+1].rs2._imm=addv;

					printf("Write Combination %d %d!\n",rdc,addv);
				}
				else if (rdc!=1)
				{
					printf("Write Combination failed fr%d,%d, %d %d %d\n",regd,mask,rdc,rdc*stride*4,addv);
				}
				i=started;
				started=-1;
			}
		}
	}

	
	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];
		if (op->Flow)
		{
			blk->oplist.erase(blk->oplist.begin()+i+2,blk->oplist.begin()+i+2+op->Flow);
		}
	}
}

bool ReadsPhy(shil_opcode* op, u32 phy)
{
	return true;
}

bool WritesPhy(shil_opcode* op, u32 phy)
{
	return true;
}

void rw_related(RuntimeBlockInfo* blk)
{
	u32 reg[sh4_reg_count]={0};

	u32 total=0;
	u32 memtotal=0;
	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];
		op->Flow=0;


		if (op->op==shop_ifb || op->op==shop_sync_sr)
		{
			memset(reg,0,sizeof(reg));
		}
		if ( (op->op==shop_add || op->op==shop_sub) )
		{
			if (reg[op->rd._reg])
			{
				if (op->rs1.is_r32i() && op->rs1._reg==op->rd._reg && op->rs2.is_imm_s16())
				{
					//nothing !
				}
				else
					reg[op->rd._reg]=0;
			}
		}
		else 
		{

			if (op->op==shop_readm || op->op==shop_writem)
				if (op->rs1.is_r32i())
					memtotal++;

			if (op->op==shop_readm || op->op==shop_writem)
			{
				if (op->rs1.is_r32i())
				{
					if (op->rs3.is_imm_s16() || op->rs3.is_null())
					{
						reg[op->rs1._reg]++;
						if (reg[op->rs1._reg]>1)
							total++;
					}
					//else
						//reg[op->rs1._reg]=0;
				}
			}

			if (op->rd.is_reg() && reg[op->rd._reg]) 
				reg[op->rd._reg]=0;
			if (op->rd2.is_reg() && reg[op->rd2._reg]) 
				reg[op->rd2._reg]=0;
		}

	}

	if (memtotal)
	{
		//u32 lookups=memtotal-total;

		//printf("rw_related total: %d/%d -- %.2f:1\n",total,memtotal,memtotal/(float)lookups);
	}
	else
	{
		//printf("rw_related total: none\n");
	}

	blk->memops=memtotal;
	blk->linkedmemops=total;
}


//-----------------------------------------------------------------------------
// Constant propagation
//
// Modeled on nullDC's shil_optimise_pass_ce_main (shil_ce.cpp): a forward walk
// over the block that tracks known-constant values of r0..r15 and rebuilds the
// opcode stream as it goes.  An opcode whose result is fully known is dropped
// and its destination just becomes a tracked constant; when a later opcode
// needs the real register (or the block ends) the constant is materialized
// ("written back") as a mov32 right before it.  Sources are rewritten to
// immediate form only where the arm32 backend accepts immediates:
//   - rs2 of add/sub/and/or/xor/shl/shr/sar/ror/test/set*/jdyn
//   - rs1 of readm (rs3 folded in; 16/32-bit only)
//   - rs3 of readm/writem
//   - rs1 of mov32 (f32 destinations only for 0/0x3F800000)
// Notably NOT: shld/shad rs2 (converted to shl/shr/sar instead), writem
// rs1/rs2, and rs1 of everything else.
//
// Memory reads are folded to compile-time constants ("baked") only when every
// page of [addr, addr+size) belongs to the block's own locked code pages: any
// write there faults and discards this block (bm_LockedWrite), which is the
// same guarantee the block's own code relies on.  Baking stops at the first
// opcode that may store to those pages (writem with unknown or overlapping
// target, ifb, pref) -- past that point this very block could have modified
// the data mid-execution, like nullDC's is_writem_safe/shil_ce_is_locked.
//-----------------------------------------------------------------------------

//chatty by default; the host tests set this to false
bool constprop_verbose=false;

//TEST KNOB: when true, stores to unknown addresses don't stop baking.
//UNSAFE in general -- a store through this very block's literal pool would
//leave already-baked values stale for the rest of the execution.  Defaulted
//on for the current experiment; flip back to false after.
bool constprop_assume_stores_safe=true;

//logs one transform and counts it for the per-block summary
#define cplog(fmt, ...) do { changes++; if (constprop_verbose) \
	printf("cprop %08X+%X: " fmt "\n", blk->addr, cur_guest_offs, ##__VA_ARGS__); } while (0)

struct constprop_pass
{
	RuntimeBlockInfo* blk;
	vector<shil_opcode> out;
	u32 changes;

	u32  val[16];     //known value, if konst
	bool konst[16];
	u32  rb_val[16];  //value the register really holds in the emitted stream
	bool rb[16];      //rb_val is valid

	bool locked;      //no possibly-self-modifying store seen yet
	bool baking_ok;   //block runs from ram and its pages will be locked
	bool allow_baking;

	u16 cur_guest_offs;

	static bool tracked(const shil_param& p) { return p.is_r32i() && p._reg<16; }
	bool is_const(const shil_param& p) { return tracked(p) && konst[p._reg]; }
	u32 cval(const shil_param& p) { return val[p._reg]; }

	static shil_param mk_immp(u32 v) { shil_param p; p.type=FMT_IMM; p._imm=v; return p; }
	static shil_param mk_regp(u32 r) { shil_param p; p.type=FMT_I32; p._reg=(Sh4RegType)r; return p; }

	void kill(const shil_param& p) { if (tracked(p)) { konst[p._reg]=false; rb[p._reg]=false; } }

	//rb/rb_val deliberately survive a value change: if the register already
	//holds the new value in the emitted stream the next writeback is free
	void set_const(const shil_param& p, u32 v) { konst[p._reg]=true; val[p._reg]=v; }

	void push(const shil_opcode& o) { out.push_back(o); }

	void emit_mov32(const shil_param& rd, u32 v)
	{
		shil_opcode o{};
		o.op=shop_mov32;
		o.rd=rd;
		o.rs1=mk_immp(v);
		o.guest_offs=cur_guest_offs;
		push(o);
	}

	void writeback(u32 r)
	{
		if (!konst[r]) return;
		if (rb[r] && rb_val[r]==val[r]) return;
		emit_mov32(mk_regp(r),val[r]);
		rb[r]=true;
		rb_val[r]=val[r];
	}
	void writeback(const shil_param& p) { if (tracked(p)) writeback(p._reg); }
	void writeback_range(u32 first, u32 last) { for (u32 r=first;r<=last;r++) writeback(r); }
	void kill_range(u32 first, u32 last) { for (u32 r=first;r<=last;r++) { konst[r]=false; rb[r]=false; } }

	//default handling: sources must hold their real values, dests lose any const
	void fallback(shil_opcode& o)
	{
		writeback(o.rs1);
		writeback(o.rs2);
		writeback(o.rs3);
		push(o);
		kill(o.rd);
		kill(o.rd2);
	}

	//result fully known: track it if we can, else emit it as a mov32
	void set_or_mov32(const shil_param& rd, u32 v)
	{
		if (tracked(rd))
		{
			set_const(rd,v);
			return;
		}
		verify(rd.is_r32i());
		emit_mov32(rd,v);
	}

	//does [addr,addr+size) live entirely on the block's own (lockable) pages?
	bool on_block_pages(u32 addr, u32 size)
	{
		if (!IsOnRam(addr)) return false;
		u32 pf=(addr&RAM_MASK)/PAGE_SIZE;
		u32 pl=((addr+size-1)&RAM_MASK)/PAGE_SIZE;
		u32 bf=(blk->addr&RAM_MASK)/PAGE_SIZE;
		u32 bl=((blk->addr+blk->sh4_code_size-1)&RAM_MASK)/PAGE_SIZE;
		return pf>=bf && pl<=bl;
	}

	bool write_may_hit_block_pages(u32 addr, u32 size)
	{
		if (!IsOnRam(addr)) return false;
		u32 pf=(addr&RAM_MASK)/PAGE_SIZE;
		u32 pl=((addr+size-1)&RAM_MASK)/PAGE_SIZE;
		u32 bf=(blk->addr&RAM_MASK)/PAGE_SIZE;
		u32 bl=((blk->addr+blk->sh4_code_size-1)&RAM_MASK)/PAGE_SIZE;
		return !(pl<bf || pf>bl);
	}

	//dc boot rom: 2MB at physical 0, immutable
	static bool in_boot_rom(u32 addr, u32 size)
	{
		if (((addr>>29)&7)==7) return false;	//p4 / store queues
		return (addr&0x1FFFFFFF)+size <= 0x00200000;
	}

	void step(shil_opcode o)
	{
		switch(o.op)
		{
		case shop_mov32:
		{
			bool known=false, from_reg=false;
			u32 v=0;
			if (o.rs1.is_imm())          { known=true; v=o.rs1._imm; }
			else if (is_const(o.rs1))    { known=true; from_reg=true; v=cval(o.rs1); }

			if (known)
			{
				if (tracked(o.rd))
				{
					if (from_reg)
						cplog("mov32: r%d = r%d = %08X",o.rd._reg,o.rs1._reg,v);
					set_const(o.rd,v);
					return;
				}
				if (o.rd.is_r32i())
				{
					if (from_reg)
						cplog("mov32: reg%d = #%08X",o.rd._reg,v);
					o.rs1=mk_immp(v); push(o); return;
				}
				//f32 dest: arm32 can only load these two immediates
				if (o.rd.is_r32f() && (v==0 || v==0x3F800000))
				{
					if (from_reg)
						cplog("mov32: f%d = #%08X",o.rd._reg-reg_fr_0,v);
					o.rs1=mk_immp(v); push(o); return;
				}
			}
			fallback(o);
			return;
		}

		case shop_add: case shop_sub: case shop_and: case shop_or: case shop_xor:
		{
			if (is_const(o.rs2))
			{
				cplog("op%d: rs2 r%d -> #%08X",o.op,o.rs2._reg,cval(o.rs2));
				o.rs2=mk_immp(cval(o.rs2));
			}
			if (is_const(o.rs1) && o.rs2.is_imm() && o.rd.is_r32i())
			{
				u32 a=cval(o.rs1), b=o.rs2._imm, r;
				switch(o.op)
				{
				case shop_add: r=a+b; break;
				case shop_sub: r=a-b; break;
				case shop_and: r=a&b; break;
				case shop_or:  r=a|b; break;
				default:       r=a^b; break;
				}
				cplog("op%d folded: reg%d = %08X",o.op,o.rd._reg,r);
				set_or_mov32(o.rd,r);
				return;
			}
			if (is_const(o.rs1) && o.rs2.is_r32i() && o.op!=shop_sub)
			{
				//commutative: move the constant into the imm slot
				u32 a=cval(o.rs1);
				cplog("op%d: commuted, r%d -> #%08X",o.op,o.rs1._reg,a);
				o.rs1=o.rs2;
				o.rs2=mk_immp(a);
			}
			fallback(o);
			return;
		}

		case shop_shld: case shop_shad:
		{
			if (!is_const(o.rs2) || !o.rd.is_r32i()) { fallback(o); return; }
			//dynamic shifts don't take an immediate on arm32; rewrite them
			//to the fixed-shift ops, which do (semantics from shil_canonical)
			{
				s32 sh=(s32)cval(o.rs2);
				bool arith = o.op==shop_shad;
				if (sh>=0)
				{
					o.op=shop_shl;
					o.rs2=mk_immp(sh&0x1F);
				}
				else if ((sh&0x1F)==0)
				{
					if (!arith)
					{
						cplog("shld by %d: reg%d = 0",sh,o.rd._reg);
						set_or_mov32(o.rd,0);
						return;
					}
					o.op=shop_sar;
					o.rs2=mk_immp(31);
				}
				else
				{
					o.op=arith?shop_sar:shop_shr;
					o.rs2=mk_immp((-sh)&0x1F);
				}
				cplog("shld/shad by %d -> op%d #%d",sh,o.op,o.rs2._imm);
			}
			//fallthrough: maybe rs1 is known too
		}
		case shop_shl: case shop_shr: case shop_sar: case shop_ror:
		{
			if (is_const(o.rs2))
			{
				cplog("op%d: rs2 r%d -> #%08X",o.op,o.rs2._reg,cval(o.rs2));
				o.rs2=mk_immp(cval(o.rs2));
			}
			if (is_const(o.rs1) && o.rs2.is_imm() && o.rd.is_r32i())
			{
				u32 a=cval(o.rs1), k=o.rs2._imm&0x1F, r;
				switch(o.op)
				{
				case shop_shl: r=a<<k; break;
				case shop_shr: r=a>>k; break;
				case shop_sar: r=(u32)((s32)a>>k); break;
				default:       r=k?((a>>k)|(a<<(32-k))):a; break;
				}
				cplog("op%d folded: reg%d = %08X",o.op,o.rd._reg,r);
				set_or_mov32(o.rd,r);
				return;
			}

			//merge shift chains: sh4 spells "shl #27" as shll16/shll8/
			//shll2/shll.  Safe only when this op immediately follows the
			//other half in the emitted stream and overwrites its dest, so
			//the intermediate value is provably unobservable.
			if (o.rs2.is_imm() && o.rd.is_r32i() &&
			    o.rs1.is_r32i() && o.rs1._reg==o.rd._reg &&
			    !out.empty())
			{
				shil_opcode& p=out.back();
				if (p.op==o.op && p.rs2.is_imm() &&
				    p.rd.is_r32i() && p.rd._reg==o.rd._reg)
				{
					u32 total=p.rs2._imm+o.rs2._imm;

					if (o.op==shop_ror)
					{
						total&=31;
						if (total==0)
						{
							//full rotation: the pair reduces to a move
							cplog("ror chain: full rotation eliminated");
							shil_opcode src=p;
							out.pop_back();
							if (!(src.rs1.is_r32i() && src.rs1._reg==o.rd._reg))
							{
								shil_opcode m{};
								m.op=shop_mov32;
								m.rd=o.rd;
								m.rs1=src.rs1;
								m.guest_offs=cur_guest_offs;
								step(m);
							}
							return;
						}
					}
					else if (o.op==shop_sar)
					{
						//sign fill saturates
						if (total>31) total=31;
					}
					else if (total>=32)
					{
						//shl/shr: everything shifted out, result is zero
						cplog("op%d chain: reg%d = 0",o.op,o.rd._reg);
						out.pop_back();
						set_or_mov32(o.rd,0);
						return;
					}

					cplog("op%d chain merged: #%d + #%d -> #%d",o.op,p.rs2._imm,o.rs2._imm,total);
					p.rs2._imm=total;
					return;
				}
			}

			fallback(o);
			return;
		}

		case shop_not: case shop_neg: case shop_ext_s8: case shop_ext_s16:
		case shop_swaplb: case shop_swap:
		{
			if (is_const(o.rs1) && o.rd.is_r32i())
			{
				u32 a=cval(o.rs1), r;
				switch(o.op)
				{
				case shop_not:     r=~a; break;
				case shop_neg:     r=0-a; break;
				case shop_ext_s8:  r=(u32)(s32)(s8)a; break;
				case shop_ext_s16: r=(u32)(s32)(s16)a; break;
				case shop_swaplb:  r=(a&0xFFFF0000)|((a&0xFF)<<8)|((a>>8)&0xFF); break;
				default:           r=(a>>24)|((a>>16)&0xFF00)|((a&0xFF00)<<8)|(a<<24); break;
				}
				cplog("op%d folded: reg%d = %08X",o.op,o.rd._reg,r);
				set_or_mov32(o.rd,r);
				return;
			}
			fallback(o);
			return;
		}

		case shop_mul_u16: case shop_mul_s16: case shop_mul_i32:
		{
			if (is_const(o.rs1) && is_const(o.rs2) && o.rd.is_r32i())
			{
				u32 a=cval(o.rs1), b=cval(o.rs2), r;
				switch(o.op)
				{
				case shop_mul_u16: r=(u32)(u16)a*(u32)(u16)b; break;
				case shop_mul_s16: r=(u32)((s32)(s16)a*(s32)(s16)b); break;
				default:           r=a*b; break;
				}
				cplog("op%d folded: reg%d = %08X",o.op,o.rd._reg,r);
				set_or_mov32(o.rd,r);
				return;
			}
			fallback(o);
			return;
		}

		case shop_test: case shop_seteq: case shop_setge: case shop_setgt:
		case shop_setae: case shop_setab:
		{
			if (is_const(o.rs2))
			{
				cplog("op%d: rs2 r%d -> #%08X",o.op,o.rs2._reg,cval(o.rs2));
				o.rs2=mk_immp(cval(o.rs2));
			}
			if (is_const(o.rs1) && o.rs2.is_imm() && o.rd.is_r32i())
			{
				u32 a=cval(o.rs1), b=o.rs2._imm, r;
				switch(o.op)
				{
				case shop_test:  r=(a&b)==0; break;
				case shop_seteq: r=a==b; break;
				case shop_setge: r=(s32)a>=(s32)b; break;
				case shop_setgt: r=(s32)a>(s32)b; break;
				case shop_setae: r=a>=b; break;
				default:         r=a>b; break;
				}
				cplog("op%d folded: T = %d",o.op,r);
				set_or_mov32(o.rd,r);
				return;
			}
			fallback(o);
			return;
		}

		case shop_setpeq:
		{
			//no immediate form on the backend; fold only if fully known
			if (is_const(o.rs1) && is_const(o.rs2) && o.rd.is_r32i())
			{
				u32 t=cval(o.rs1)^cval(o.rs2);
				u32 r=!((t&0xFF000000)&&(t&0x00FF0000)&&(t&0x0000FF00)&&(t&0x000000FF));
				cplog("setpeq folded: T = %d",r);
				set_or_mov32(o.rd,r);
				return;
			}
			fallback(o);
			return;
		}

		case shop_readm:
		{
			if (is_const(o.rs3))
			{
				cplog("readm: rs3 r%d -> #%08X",o.rs3._reg,cval(o.rs3));
				o.rs3=mk_immp(cval(o.rs3));
			}
			u32 size=o.flags&0x7F;

			//the address is known when the decoder already emitted an
			//immediate base (pc-relative literals) or the base register is
			//a tracked constant
			bool addr_known=false, base_reg_const=false;
			u32 addr=0;
			if (o.rs3.is_null() || o.rs3.is_imm())
			{
				u32 offs=o.rs3.is_imm()?o.rs3._imm:0;
				if (o.rs1.is_imm())
				{
					addr_known=true;
					addr=o.rs1._imm+offs;
				}
				else if (is_const(o.rs1))
				{
					addr_known=true;
					base_reg_const=true;
					addr=cval(o.rs1)+offs;
				}
			}

			if (addr_known)
			{
				//ram reads bake only while a write to them still discards
				//this block; the boot rom can't change, so it always bakes
				bool can_bake = (baking_ok && locked && on_block_pages(addr,size))
				             || in_boot_rom(addr,size);

				if (allow_baking && can_bake && (size==1||size==2||size==4))
				{
					u32 data = size==1 ? (u32)(s32)(s8)ReadMem8(addr)
					         : size==2 ? (u32)(s32)(s16)ReadMem16(addr)
					         :           ReadMem32(addr);
					if (tracked(o.rd))
					{
						cplog("readm baked: [%08X] sz%d = %08X -> r%d",addr,size,data,o.rd._reg);
						set_const(o.rd,data);
						return;
					}
					if (o.rd.is_r32i())
					{
						cplog("readm baked: [%08X] sz%d = %08X -> reg%d",addr,size,data,o.rd._reg);
						emit_mov32(o.rd,data);
						return;
					}
					if (o.rd.is_r32f() && (data==0||data==0x3F800000))
					{
						cplog("readm baked: [%08X] sz%d = %08X -> f%d",addr,size,data,o.rd._reg-reg_fr_0);
						emit_mov32(o.rd,data);
						return;
					}
					//other destinations: keep the load, imm address form below
				}

				//address-imm form: 16/32-bit only.  arm32 has no 64-bit
				//imm loads, and no 8-bit ones for direct-mapped regions
				//(which we can't tell apart from handler regions here).
				if (base_reg_const && (size==2 || size==4) && o.rd.is_r32())
				{
					cplog("readm promoted: [%08X] sz%d",addr,size);
					o.rs1=mk_immp(addr);
					o.rs3=shil_param();
					push(o);
					kill(o.rd);
					return;
				}

				//already-imm base with an imm offset: fold to the canonical
				//form the backend expects (imm rs1, null rs3)
				if (o.rs1.is_imm() && o.rs3.is_imm())
				{
					o.rs1=mk_immp(addr);
					o.rs3=shil_param();
				}
			}
			else if (is_const(o.rs1) && o.rs3.is_r32i())
			{
				//const base + reg offset: swap them, base becomes the imm offset
				u32 base=cval(o.rs1);
				cplog("readm: const base r%d = %08X -> imm offset",o.rs1._reg,base);
				o.rs1=o.rs3;
				o.rs3=mk_immp(base);
			}
			fallback(o);
			return;
		}

		case shop_writem:
		{
			if (is_const(o.rs3))
			{
				cplog("writem: rs3 r%d -> #%08X",o.rs3._reg,cval(o.rs3));
				o.rs3=mk_immp(cval(o.rs3));
			}

			if (locked)
			{
				if (is_const(o.rs1) && (o.rs3.is_null() || o.rs3.is_imm()))
				{
					u32 addr=cval(o.rs1)+(o.rs3.is_imm()?o.rs3._imm:0);
					if (write_may_hit_block_pages(addr,o.flags&0x7F))
					{
						cplog("store may hit block pages [%08X]: baking off",addr);
						locked=false;
					}
				}
				else if (!constprop_assume_stores_safe)
				{
					cplog("store to unknown address: baking off");
					locked=false; //unknown target: assume the worst
				}
			}
			fallback(o); //address and data always stay in registers on arm32
			return;
		}

		case shop_jdyn:
		{
			if (is_const(o.rs2))
			{
				cplog("jdyn: rs2 r%d -> #%08X",o.rs2._reg,cval(o.rs2));
				o.rs2=mk_immp(cval(o.rs2));
			}
			if (is_const(o.rs1))
			{
				u32 target=cval(o.rs1)+(o.rs2.is_imm()?o.rs2._imm:0);
				if (blk->BlockType==BET_DynamicJump || blk->BlockType==BET_DynamicCall)
				{
					blk->BranchBlock=target;
					blk->BlockType = blk->BlockType==BET_DynamicJump ? BET_StaticJump : BET_StaticCall;
					//stands in for an indirect jump: the backwards-only
					//cycle check must not treat it as provably forward
					blk->static_from_constprop=true;
					cplog("jdyn -> static %s %08X",blk->BlockType==BET_StaticJump?"jump":"call",target);
					return; //dropped; pc_dyn no longer needed
				}
			}
			fallback(o);
			return;
		}

		case shop_ifb:
		{
			if (locked)
				cplog("ifb: baking off");
			writeback_range(0,15);
			push(o);
			kill_range(0,15);
			if (!constprop_assume_stores_safe) {
				locked=false; //the interpreter can write anywhere
			}
			return;
		}

		case shop_sync_sr:
		{
			//UpdateSR() may swap the r0-r7 banks; r8-r15 are unaffected
			writeback_range(0,7);
			push(o);
			kill_range(0,7);
			return;
		}

		case shop_pref:
		{
			if (is_const(o.rs1) && (cval(o.rs1)>>26)!=0x38)
			{
				cplog("pref dropped: r%d = %08X is not sq",o.rs1._reg,cval(o.rs1));
				return; //provably not a store-queue address: pref is a nop
			}
			if (!constprop_assume_stores_safe) {
				if (locked)
					cplog("pref: baking off");
				locked=false; //an SQ flush stores to ram we can't see
			}
			fallback(o);
			return;
		}

		default:
			fallback(o);
			return;
		}
	}
};

void constprop(RuntimeBlockInfo* blk, bool allow_memory_baking=true)
{
	constprop_pass cp{};

	cp.blk=blk;
	cp.allow_baking=allow_memory_baking;
	cp.locked=true;
	cp.baking_ok = IsOnRam(blk->addr) && !bm_RamPageHasData(blk->addr,blk->sh4_code_size);
	cp.out.reserve(blk->oplist.size());

	for (size_t i=0;i<blk->oplist.size();i++)
	{
		cp.cur_guest_offs=blk->oplist[i].guest_offs;
		cp.step(blk->oplist[i]);
	}

	cp.writeback_range(0,15);

	if (constprop_verbose && cp.changes)
		printf("cprop %08X: %u changes, %u -> %u ops\n",
			blk->addr,cp.changes,(u32)blk->oplist.size(),(u32)cp.out.size());

	blk->oplist.swap(cp.out);

	rw_related(blk);
}

//-----------------------------------------------------------------------------
// fmov.s / fmov.d @Rn+ pair fusion
//
// The decoder spells a two-float fetch as
//     readm frA, [rn] (f32);  add rn, rn, #4;  readm frB, [rn] (f32)
// Fuse it into one readm with rd2 set -- the backend reads [addr] into rd and
// [addr+size] into rd2 off a single address computation -- followed by the add.
// When the second load had its own increment the two adds become adjacent and
// are combined. Doubles (fmov.d, +8) fuse the same way.
//
// Only the nvmem fastpath emits the paired form, so this never runs without
// nvmem. A paired load that faults is still rewritable: the backend keeps the
// two loads adjacent, and ngen_Rewrite moves the second one out to a fixup,
// because the handler call it installs clobbers the base they share.
//-----------------------------------------------------------------------------
void fuse_readm_pairs(RuntimeBlockInfo* blk)
{
	if (!_nvmem_enabled())
		return;

	for (size_t i=0; i+2<blk->oplist.size(); i++)
	{
		shil_opcode& r1=blk->oplist[i];
		shil_opcode& ad=blk->oplist[i+1];
		shil_opcode& r2=blk->oplist[i+2];

		if (r1.op!=shop_readm || ad.op!=shop_add || r2.op!=shop_readm)
			continue;

		u32 sz=r1.flags&0x7F;

		if ((sz!=4 && sz!=8) || (r2.flags&0x7F)!=sz)
			continue;

		bool f64=sz==8;

		if (f64)
		{
			if (!r1.rd.is_r64f() || !r2.rd.is_r64f())
				continue;
			//a pair that loads the same regs twice is not worth the bother
			if (r1.rd._reg==r2.rd._reg)
				continue;
		}
		else
		{
			if (!r1.rd.is_r32f() || !r2.rd.is_r32f())
				continue;
		}

		if (!r1.rd2.is_null() || !r2.rd2.is_null())
			continue;
		if (!r1.rs1.is_r32i() || !r1.rs3.is_null() || !r2.rs3.is_null())
			continue;
		if (!r2.rs1.is_r32i() || r2.rs1._reg!=r1.rs1._reg)
			continue;
		//the middle op must be exactly rn += size
		if (!(ad.rd.is_r32i() && ad.rd._reg==r1.rs1._reg &&
		      ad.rs1.is_r32i() && ad.rs1._reg==r1.rs1._reg &&
		      ad.rs2.is_imm() && ad.rs2._imm==sz))
			continue;

		if (constprop_verbose)
			printf("fusem %08X+%X: readm %s pair f%d,f%d @ r%d\n",blk->addr,r1.guest_offs,
				f64?"f64":"f32",r1.rd._reg-reg_fr_0,r2.rd._reg-reg_fr_0,r1.rs1._reg);

		r1.rd2=r2.rd;
		blk->oplist.erase(blk->oplist.begin()+i+2);

		//the pair's two +4s are adjacent now; combine them
		if (i+2<blk->oplist.size())
		{
			shil_opcode& a1=blk->oplist[i+1];
			shil_opcode& a2=blk->oplist[i+2];

			if (a2.op==shop_add &&
			    a2.rd.is_r32i() && a2.rd._reg==a1.rd._reg &&
			    a2.rs1.is_r32i() && a2.rs1._reg==a1.rd._reg &&
			    a2.rs2.is_imm())
			{
				a1.rs2._imm+=a2.rs2._imm;
				blk->oplist.erase(blk->oplist.begin()+i+2);
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Dead value elimination
//
// Backward liveness over the block: an op with no side effects whose every
// written register is overwritten later in the block before any read is dead.
// Everything is live at block end -- the next block may read any register.
// This generalizes srt_waw() from sr_T to the whole register file.
//-----------------------------------------------------------------------------

//ops that only compute register values -- removable when their outputs die.
//Everything else (memory, branches, calls, frswap, debug) stays.
static bool dse_pure(shilop op)
{
	switch(op)
	{
	case shop_mov32: case shop_mov64:
	case shop_and: case shop_or: case shop_xor: case shop_not:
	case shop_add: case shop_sub: case shop_neg:
	case shop_shl: case shop_shr: case shop_sar: case shop_ror:
	case shop_adc: case shop_sbc: case shop_rocl: case shop_rocr:
	case shop_swaplb: case shop_swap: case shop_shld: case shop_shad:
	case shop_ext_s8: case shop_ext_s16:
	case shop_mul_u16: case shop_mul_s16: case shop_mul_i32:
	case shop_mul_u64: case shop_mul_s64:
	case shop_div32u: case shop_div32s: case shop_div32p2:
	case shop_cvt_f2i_t: case shop_cvt_i2f_n: case shop_cvt_i2f_z:
	case shop_test: case shop_seteq: case shop_setge: case shop_setgt:
	case shop_setae: case shop_setab: case shop_setpeq:
	case shop_fadd: case shop_fsub: case shop_fmul: case shop_fdiv:
	case shop_fabs: case shop_fneg: case shop_fsqrt: case shop_fmac:
	case shop_fsrra: case shop_fipr: case shop_ftrv: case shop_fsca:
	case shop_fseteq: case shop_fsetgt:
		return true;
	default:
		return false;
	}
}

void dead_value_elim(RuntimeBlockInfo* blk)
{
	bool live[sh4_reg_count];
	memset(live,1,sizeof(live));

	for (int i=(int)blk->oplist.size();i-->0;)
	{
		shil_opcode& o=blk->oplist[i];

		//conservative barriers: may read and write any register
		if (o.op==shop_ifb || o.op==shop_sync_sr || o.op==shop_sync_fpscr)
		{
			memset(live,1,sizeof(live));
			continue;
		}

		const shil_param* wr[2]={&o.rd,&o.rd2};
		const shil_param* rs[3]={&o.rs1,&o.rs2,&o.rs3};

		if (dse_pure(o.op))
		{
			bool has_dest=false, any_live=false;
			for (int p=0;p<2;p++)
			{
				if (!wr[p]->is_reg()) continue;
				has_dest=true;
				for (u32 c=0;c<wr[p]->count();c++)
					any_live|=live[wr[p]->_reg+c];
			}

			if (has_dest && !any_live)
			{
				if (constprop_verbose)
					printf("dse %08X+%X: op%d (reg%d) is dead\n",
						blk->addr,o.guest_offs,o.op,o.rd._reg);
				blk->oplist.erase(blk->oplist.begin()+i);
				continue;
			}
		}

		//the op stays: its writes kill, then its reads revive
		for (int p=0;p<2;p++)
			if (wr[p]->is_reg())
				for (u32 c=0;c<wr[p]->count();c++)
					live[wr[p]->_reg+c]=false;

		for (int p=0;p<3;p++)
			if (rs[p]->is_reg())
				for (u32 c=0;c<rs[p]->count();c++)
					live[rs[p]->_reg+c]=true;
	}
}

//-----------------------------------------------------------------------------
// ftrv known-w detection
//
// Geometry code loads the transformed vector's w with fldi0/fldi1 right
// before the ftrv (point vs direction transform), and the ftrv overwrites
// the whole vector, so that constant has exactly one consumer. Flag the
// ftrv (FTRV_W_ZERO/ONE) and drop the load; the emitter substitutes the
// constant, skipping the fourth MAC stage entirely for w=0 and turning it
// into a plain add for w=1.
//
// The flag means the w register's content is UNDEFINED at the ftrv --
// only backends that honor it may run this pass.
//-----------------------------------------------------------------------------

static bool param_covers(const shil_param& p, u32 reg)
{
	return p.is_reg() && reg>=p._reg && reg<p._reg+p.count();
}

void ftrv_known_w(RuntimeBlockInfo* blk)
{
	for (size_t i=0;i<blk->oplist.size();i++)
	{
		{
			shil_opcode& tr=blk->oplist[i];

			if (tr.op!=shop_ftrv || tr.flags!=0)
				continue;
			//the mov is dead only because the ftrv overwrites its vector
			if (!tr.rd.is_reg() || !tr.rs1.is_reg() || tr.rd._reg!=tr.rs1._reg)
				continue;
		}

		u32 wreg=blk->oplist[i].rs1._reg+3;
		size_t mov=(size_t)-1;
		u32 w_imm=0;

		for (size_t j=i;j-->0;)
		{
			shil_opcode& o=blk->oplist[j];

			//fp state can change wholesale under these
			if (o.op==shop_ifb || o.op==shop_sync_fpscr || o.op==shop_frswap)
				break;

			//another reader: the load must stay
			if (param_covers(o.rs1,wreg) || param_covers(o.rs2,wreg) || param_covers(o.rs3,wreg))
				break;

			if (param_covers(o.rd,wreg) || param_covers(o.rd2,wreg))
			{
				if (o.op==shop_mov32 && o.rd.is_r32f() && o.rd._reg==wreg &&
				    o.rs1.is_imm() && (o.rs1._imm==0 || o.rs1._imm==0x3F800000))
				{
					mov=j;
					w_imm=o.rs1._imm;
				}
				break; //whatever wrote w, the search ends here
			}
		}

		if (mov==(size_t)-1)
			continue;

		blk->oplist[i].flags |= w_imm==0 ? FTRV_W_ZERO : FTRV_W_ONE;

		if (constprop_verbose)
			printf("ftrvw %08X+%X: w = %s, load dropped\n",blk->addr,
				blk->oplist[i].guest_offs, w_imm==0 ? "0" : "1");

		blk->oplist.erase(blk->oplist.begin()+mov);
		i--; //the ftrv shifted down by one
	}
}

//read_v4m3z1
void read_v4m3z1(RuntimeBlockInfo* blk)
{
	
	int state=0;
	int st_sta=0;
	Sh4RegType reg_a=NoReg;
	Sh4RegType reg_fb=NoReg;

	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];

		bool a=false,b=false;
		if ((i+6)>blk->oplist.size())
			break;

		if (state==0 && op->op==shop_readm && op->rd.is_r32f() && op->rs1.is_r32i() && op->rs3.is_null())
		{
			if (op->rd._reg==reg_fr_0 || op->rd._reg==reg_fr_4 || op->rd._reg==reg_fr_8 || op->rd._reg==reg_fr_12)
			{
				reg_a=op->rs1._reg;
				reg_fb=op->rd._reg;
				st_sta=(int)i;
				goto _next_st;
			}
			goto _fail;
		}
		else if (state < 8 && state & 1 && op->op==shop_add && op->rd._reg==reg_a && op->rs1.is_reg() && op->rs1._reg==reg_a && op->rs2.is_imm() && op->rs2._imm==4)
		{
			if (state==7)
			{
				u32 start=st_sta;
				
				for (int j=0;j<6;j++)
				{
					blk->oplist.erase(blk->oplist.begin()+start);
				}

				i=start+1;
				op=&blk->oplist[start+0];
				op->op=shop_readm;
				op->flags=0x440;
				op->rd=shil_param(reg_fb==reg_fr_0?regv_fv_0:
								  reg_fb==reg_fr_4?regv_fv_4:
								  reg_fb==reg_fr_8?regv_fv_8:
								  reg_fb==reg_fr_12?regv_fv_12:reg_sr_T);
				op->rd2=shil_param();

				op->rs1=shil_param(reg_a);
				op->rs2=shil_param();
				op->rs3=shil_param();

				op=&blk->oplist[start+1];
				op->op=shop_add;
				op->flags=0;
				op->rd=shil_param(reg_a);
				op->rd2=shil_param();

				op->rs1=shil_param(reg_a);
				op->rs2=shil_param(FMT_IMM,16);
				op->rs3=shil_param();

				goto _end;
			}
			else
				goto _next_st;
		}
		else if (state >1 && 
			op->op==shop_readm && op->rd.is_r32f() && op->rd._reg==(reg_fb+state/2) && op->rs1.is_r32i() && op->rs1._reg==reg_a && op->rs3.is_null())
		{
			goto _next_st;
		}
		else if ((a=(op->op==shop_mov32 && op->rd._reg==(reg_fb+3) && op->rs1.is_imm() && (op->rs1._imm==0x3f800000 /*|| op->rs1._imm==0*/))) ||
			    (b=(i>7 && op[-7].op==shop_mov32 && op[-7].rd._reg==(reg_fb+3) && op[-7].rs1.is_imm() && (op[-7].rs1._imm==0x3f800000 /*|| op[-7].rs1._imm==0*/))) )
		{
			if (state==6)
			{
				if (b)
					st_sta--;
				if (a)
					printf("NOT B\b");
				u32 start=st_sta;
								
				for (int j=0;j<5;j++)
				{
					blk->oplist.erase(blk->oplist.begin()+start);
				}
				
				i=start+1;
				op=&blk->oplist[start+0];
				op->op=shop_readm;
				op->flags=0x431;
				op->rd=shil_param(reg_fb==reg_fr_0?regv_fv_0:
								  reg_fb==reg_fr_4?regv_fv_4:
								  reg_fb==reg_fr_8?regv_fv_8:
								  reg_fb==reg_fr_12?regv_fv_12:reg_sr_T);
				op->rd2=shil_param();

				op->rs1=shil_param(reg_a);
				op->rs2=shil_param();
				op->rs3=shil_param();

				op=&blk->oplist[start+1];
				op->op=shop_add;
				op->flags=0;
				op->rd=shil_param(reg_a);
				op->rd2=shil_param();

				op->rs1=shil_param(reg_a);
				op->rs2=shil_param(FMT_IMM,12);
				op->rs3=shil_param();

				goto _end;
			}
			else
				goto _fail;
		}
		else
			goto _fail;


		die("wth");

_next_st:
		state ++;
		continue;

_fail:
		if (state)
			i=st_sta;
_end:
		state=0;

	}

}

//dejcond
void dejcond(RuntimeBlockInfo* blk)
{
	if (!blk->has_jcond) return;

	bool found=false;
	u32 jcondp=0;

	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];

		if (found)
		{
			if ((op->rd.is_reg() && op->rd._reg==reg_sr_T) ||  op->op==shop_ifb)
			{
				found=false;
			}
		}

		if (op->op==shop_jcond)
		{
			found=true;
			jcondp=(u32)i;
		}
	}

	if (found)
	{
		blk->has_jcond=false;
		blk->oplist.erase(blk->oplist.begin()+jcondp);
	}
}

//detect bswaps and talk about them
void enswap(RuntimeBlockInfo* blk)
{
	Sh4RegType r=NoReg;
	int state=0;
	
	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];

		op->Flow=0;

		if (state==0 && op->op==shop_swaplb)
		{
			if (op->rd._reg==op->rs1._reg)
			{
				state=1;
				r=op->rd._reg;
				op->Flow=1;
				continue;
			}
			else
			{
				printf("bswap -- wrong regs\n");
			}
		}

		if (state==1 && op->op==shop_ror && op->rs2.is_imm() && op->rs2._imm==16 && 
			op->rs1._reg==r)
		{
			if (op->rd._reg==r)
			{
				state=2;
				op->Flow=1;
				continue;
			}
			else
			{
				printf("bswap -- wrong regs\n");
			}
		}

		if (state==2 && op->op==shop_swaplb && op->rs1._reg==r)
		{
			if (op->rd._reg!=r)
			{
				printf("oops?\n");
			}
			else
			{
				printf("SWAPM!\n");
			}
			op->Flow=1;
			state=0;
		}

	}
}

//enjcond
//this is a normally slower
//however, cause of reg alloc stuff in arm, this
//speeds up access to SR_T (pc_dyn is stored in reg, not mem)
//This is temporary til that limitation is fixed on the reg alloc logic
void enjcond(RuntimeBlockInfo* blk)
{
	if (!blk->has_jcond && (blk->BlockType==BET_Cond_0||blk->BlockType==BET_Cond_1))
	{
		shil_opcode jcnd;

		jcnd.op=shop_jcond;
		jcnd.rs1=shil_param(reg_sr_T);
		jcnd.rd=shil_param(reg_pc_dyn);
		jcnd.flags=0;
		blk->oplist.push_back(jcnd);
		blk->has_jcond=true;
	}
}


//"links" consts to each other
void constlink(RuntimeBlockInfo* blk)
{
	Sh4RegType def=NoReg;
	s32 val=0;

	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];

		if (op->op!=shop_mov32)
			def=NoReg;
		else
		{

			if (def!=NoReg && op->rs1.is_imm() && op->rs1._imm==val)
			{
				op->rs1=shil_param(def);
			}
			else if (def==NoReg && op->rs1.is_imm() && op->rs1._imm==0)
			{
				//def=op->rd._reg;
				val=op->rs1._imm;
			}
		}
	}
}


void srt_waw(RuntimeBlockInfo* blk)
{
	bool found=false;
	u32 srtw=0;

	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];

		if (found)
		{
			if ((op->rs1.is_reg() && op->rs1._reg==reg_sr_T)
				|| (op->rs2.is_reg() && op->rs2._reg==reg_sr_T)
				|| (op->rs3.is_reg() && op->rs3._reg==reg_sr_T)
				|| op->op==shop_ifb)
			{
				found=false;
			}
		}

		if (op->rd.is_reg() && op->rd._reg==reg_sr_T && op->rd2.is_null())
		{
			if (found)
			{
				blk->oplist.erase(blk->oplist.begin()+srtw);
				i--;
			}

			found=true;
			srtw=(u32)i;
		}
	}

}

/*
	The statically-knowable successors of a block: BET_Cond_0/1 have both a
	branch and a fallthrough target, BET_StaticJump only a branch target.
	Anything else (dynamic jumps, rets, calls, intr) ends the walk on that
	path - its successors are not knowable here.

	The p-pointers are filled lazily at link time and severed on invalidation,
	so an unlinked edge is resolved through bm_GetBlock() by address. Returns
	false when a static successor exists but has no compiled block yet - the
	graph is incomplete there and the caller should retry later.
*/
static bool anchor_succs(RuntimeBlockInfo* rbi, RuntimeBlockInfo* succ[2])
{
	succ[0] = succ[1] = nullptr;

	if (rbi->BlockType != BET_Cond_0 && rbi->BlockType != BET_Cond_1 && rbi->BlockType != BET_StaticJump)
		return true;

	bool complete = true;

	if (rbi->BranchBlock != 0xFFFFFFFF)
	{
		succ[0] = rbi->pBranchBlock ? rbi->pBranchBlock : bm_GetBlock(rbi->BranchBlock);
		if (!succ[0])
			complete = false;
	}

	if (rbi->BlockType != BET_StaticJump && rbi->NextBlock != 0xFFFFFFFF)
	{
		succ[1] = rbi->pNextBlock ? rbi->pNextBlock : bm_GetBlock(rbi->NextBlock);
		if (!succ[1])
			complete = false;
	}

	return complete;
}

/*
	Multiblock-aware SQ store/pref marking, run over a detected loop's member
	set before CompileLoop. The per-block matcher (sq_pref above) needs the
	stores and the pref in one block and counts bytes; real loops split them
	across members (the pref rides a branch delay slot in the successor
	block) and overwrite bytes (pointer-as-PCW tricks make the naive byte sum
	36 != 32), so it misses. Region-level the confidence test is stronger and
	simpler, per base register used by a pref:
	  - write-only within the region: nothing reads through the base (a
	    store queue cannot be read back)
	  - the base is only ever redefined by add/sub immediate (address
	    arithmetic walking the SQ lines)
	  - only plain 32/64-bit stores through it (the sq_both fast path exists
	    only for those)
	Every writem through such a base and every pref of it get marked; the
	region emission then uses the inline sq_both forms - no runtime checks,
	no per-block slowpath calls, no first-touch fault rewrites.

	Same trust model as the per-block matcher, so the same gate:
	opt_sqw_on_pref ("on write before pref, assume SQW"). Marks live in the
	member oplists but only the region emission consumes them - the
	fragments' code is already emitted, and a fragment recompile rebuilds
	its oplist from scratch.
*/
void sq_pref_multiblock(std::map<u32, RuntimeBlockInfo*>& blocks)
{
	if (!settings.dynarec.opt_sqw_on_pref)
		return;

	//candidate bases: registers pref'd anywhere in the region
	std::set<u32> cand;
	for (auto& kv : blocks)
		for (auto& op : kv.second->oplist)
			if (op.op==shop_pref && op.rs1.is_r32i())
				cand.insert((u32)op.rs1._reg);

	for (auto rt : cand)
	{
		bool ok=true;
		u32 stores=0;

		for (auto& kv : blocks)
		{
			for (auto& op : kv.second->oplist)
			{
				if (op.op==shop_readm && op.rs1.is_reg() && (u32)op.rs1._reg==rt)
				{
					ok=false;	//read through the base: not a store queue
					break;
				}

				if (op.op==shop_writem && op.rs1.is_reg() && (u32)op.rs1._reg==rt)
				{
					u32 sz=op.flags&0x7F;
					bool grouped=(op.flags&0x80)!=0;
					bool val_ok=op.rs2.is_r32i()||op.rs2.is_r32f()||op.rs2.is_r64f()||op.rs2.is_r32fv();

					if (grouped || (sz!=4 && sz!=8) || !val_ok)
					{
						ok=false;	//no sq_both fast path for this store form
						break;
					}
					stores++;
					continue;
				}

				//any other redefinition of the base makes it unknowable
				const shil_param* wr[2]={&op.rd,&op.rd2};
				for (int w=0;w<2 && ok;w++)
				{
					if (!wr[w]->is_reg())
						continue;
					if (rt<(u32)wr[w]->_reg || rt>=(u32)wr[w]->_reg+wr[w]->count())
						continue;

					bool arith=(op.op==shop_add || op.op==shop_sub) &&
						op.rs1.is_reg() && (u32)op.rs1._reg==rt && op.rs2.is_imm();
					if (!arith)
						ok=false;
				}
				if (!ok)
					break;
			}
			if (!ok)
				break;
		}

		if (!ok || !stores)
			continue;

		u32 marked=0;
		for (auto& kv : blocks)
		{
			for (auto& op : kv.second->oplist)
			{
				if (op.op==shop_writem && op.rs1.is_reg() && (u32)op.rs1._reg==rt)
				{
					op.flags2=0x1337;
					marked++;
				}
				if (op.op==shop_pref && op.rs1.is_r32i() && (u32)op.rs1._reg==rt)
					op.flags=0x1337;
			}
		}

		printf_sqwm("sq_pref_multiblock: base guest#%u, %u stores marked SQ\n", rt, marked);
	}
}

/*
	Region-growth probe: does any static path from 'start' lead to an edge
	targeting 'header'? An installed region's exit target that can reach the
	header again is a loop path that only got compiled after the region was
	built (games routinely leave loop arms unexecuted until much later) - the
	region should be discarded and regrown to include it.

	Tests edge ADDRESSES against the header rather than blocks: the block at
	the header is the opaque region itself, so the walk can never pass through
	it - an edge pointing at it is what closes the cycle.
*/
bool anchor_reaches_header(u32 start, u32 header)
{
	RuntimeBlockInfo* first=bm_GetBlock(start);
	if (!first)
		return false;

	std::set<RuntimeBlockInfo*> seen;
	std::set<RuntimeBlockInfo*> todo;
	todo.insert(first);

	for (int i=0;i<32 && todo.size();i++)
	{
		auto rbi=*todo.begin();
		todo.erase(todo.begin());
		seen.insert(rbi);

		if (rbi->BlockType==BET_Cond_0 || rbi->BlockType==BET_Cond_1 || rbi->BlockType==BET_StaticJump)
		{
			if (rbi->BranchBlock==header)
				return true;
			if (rbi->BlockType!=BET_StaticJump && rbi->NextBlock==header)
				return true;
		}

		RuntimeBlockInfo* succ[2];
		anchor_succs(rbi,succ);

		for (auto s : succ)
			if (s && !seen.count(s))
				todo.insert(s);
	}

	return false;
}

bool optimize_anchor(RuntimeBlockInfo* anchor_rbi, std::map<u32, RuntimeBlockInfo*>* loop_out) {
	// find loop

	std::set<RuntimeBlockInfo*> loop_rbis;
	std::set<RuntimeBlockInfo*> todo_rbis;

	todo_rbis.insert(anchor_rbi);

	bool partial = false;

	for (int i = 0; i < 32 && todo_rbis.size(); i++) {
		auto rbi = *todo_rbis.begin();
		todo_rbis.erase(todo_rbis.begin());
		loop_rbis.insert(rbi);

		RuntimeBlockInfo* succ[2];
		if (!anchor_succs(rbi, succ))
			partial = true;		//edge to a not-yet-compiled block

		for (auto s : succ)
			if (s && !loop_rbis.count(s))
				todo_rbis.insert(s);
	}

	if (!todo_rbis.empty())
		partial = true;			//32-block budget ran out before the walk settled

	// Find if looping here (anchor block -> via branches can loop back to anchor block)
	// The anchor is on a loop iff it reaches itself via >=1 static edge.
	// Everything explored is reachable FROM the anchor; compute which blocks
	// reach it BACK (reverse fixpoint over the small explored set) - that
	// intersection is exactly the cycle.
	std::set<RuntimeBlockInfo*> reaches_anchor;

	for (bool changed = true; changed; )
	{
		changed = false;
		for (auto rbi : loop_rbis)
		{
			if (reaches_anchor.count(rbi))
				continue;

			RuntimeBlockInfo* succ[2];
			anchor_succs(rbi, succ);

			for (auto s : succ)
			{
				if (s && (s == anchor_rbi || reaches_anchor.count(s)))
				{
					reaches_anchor.insert(rbi);
					changed = true;
					break;
				}
			}
		}
	}

	if (reaches_anchor.count(anchor_rbi))
	{
		// if looping, find loop header: loops are laid out contiguously, so
		// the header (entry) is the lowest guest address on the cycle
		u32 header = 0xFFFFFFFF;
		u32 members = 0;

		for (auto rbi : loop_rbis)
		{
			if (rbi != anchor_rbi && !reaches_anchor.count(rbi))
				continue;		//explored but off-cycle (side path / exit)

			if (rbi->addr < header)
				header = rbi->addr;
			members++;

			if (loop_out)
				(*loop_out)[rbi->addr] = rbi;
		}

		// printf("FOUND loop: anchor %08X header %08X, %u cycle blocks / %zu explored%s\n",
		// 	anchor_rbi->addr, header, members, loop_rbis.size(),
		// 	partial ? " (graph partial)" : "");

		// std::map<u32, RuntimeBlockInfo*> loop_print;
		// for (auto rbi : loop_rbis) {
		// 	loop_print.emplace(rbi->addr, rbi);
		// }

		// for (auto addr_rbi: loop_print) {
		// 	auto blk = addr_rbi.second;
		// 	auto f = stdout;
		// 	fprintf(f,"addr: %08X\n",blk->addr);
		// 	for (u32 j=0;j<blk->oplist.size();j++)
		// 	{
		// 		shil_opcode* op=&blk->oplist[j];
		// 		fprintf(f," %s (%x)\n",op->dissasm().c_str(), op->flags2);
		// 	}

		// 	fprintf(f,"branch addr: %08X\n",blk->BranchBlock);
		// 	fprintf(f,"next addr: %08X\n",blk->NextBlock);
		// }
		return true;
	}

	// if not looping, if partial return false (the loop may just not be
	// compiled/linked yet - retry later), else return true to not further
	// optimize the block
	return !partial;
}

typedef set<RuntimeBlockInfo*> bm_List;

extern bm_List anchor_blocks;

//Simplistic Write after Write without read pass to remove (a few) dead opcodes
//Seems to be working
void AnalyseBlock(RuntimeBlockInfo* blk)
{
    /*
	u32 st[sh4_reg_count]={0};

	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];
		
		if (op->rs1.is_reg() && st[op->rs1._reg]==0)
			st[op->rs1._reg]=1;

		if (op->rs2.is_reg() && st[op->rs2._reg]==0)
			st[op->rs2._reg]=1;

		if (op->rs3.is_reg() && st[op->rs3._reg]==0)
			st[op->rs3._reg]=1;

		if (op->rd.is_reg())
			st[op->rd._reg]|=2;

		if (op->rd2.is_reg())
			st[op->rd2._reg]|=2;
	}

	if (st[reg_sr_T]&1)
	{
		printf("BLOCK: %08X\n",blk->addr);

		puts("rin: ");

		for (int i=0;i<sh4_reg_count;i++)
		{
			if (st[i]&1)
				printf("%s ",name_reg(i).c_str());
		}

		puts("\nrout: ");

		for (int i=0;i<sh4_reg_count;i++)
		{
			if (st[i]&2)
				printf("%s ",name_reg(i).c_str());
		}

		puts("\nr-ns: ");

		for (int i=0;i<sh4_reg_count;i++)
		{
			if (st[i]==2)
				printf("%s ",name_reg(i).c_str());
		}


		puts("\n");
	}
	*/
	if (settings.dynarec.opt_constprop) {
		constprop(blk, settings.dynarec.opt_constprop >= 2);
	}
	if (settings.dynarec.opt_readm_pairs) {
		fuse_readm_pairs(blk);
	}
	dead_value_elim(blk);
#if HOST_CPU == CPU_ARM
	//deletes the w load: only run for backends whose ftrv honors the flag
	if (settings.dynarec.opt_fipr_w) {
		ftrv_known_w(blk);
	}
#endif

	bool anchor = false;
	if (settings.dynarec.opt_sqw_on_pref) {
		anchor |= sq_pref(blk);
	}

	anchor |= has_ftrv(blk);
	
	bool last_op_sets_flags=!blk->has_jcond && blk->oplist.size() > 0 && 
		blk->oplist[blk->oplist.size()-1].rd._reg==reg_sr_T;

	srt_waw(blk);
	constlink(blk);
	//dejcond(blk);
	if (last_op_sets_flags)
	{
		shilop op= blk->oplist[blk->oplist.size()-1].op;
		if (op == shop_test || op==shop_seteq || op==shop_setab || op==shop_setae
			|| op == shop_setge || op==shop_setgt)
			;
		else
			last_op_sets_flags=false;
	}
	if (!last_op_sets_flags)
		enjcond(blk);
	//read_v4m3z1(blk);
	//rw_related(blk);

	if (anchor && settings.dynarec.opt_anchored_loops) {
		anchor_blocks.insert(blk);
	}
	return;	//disbled to be on the safe side ..
    /*
	memset(RegisterWrite,-1,sizeof(RegisterWrite));
	memset(RegisterRead,-1,sizeof(RegisterRead));

	total_blocks++;
	for (size_t i=0;i<blk->oplist.size();i++)
	{
		shil_opcode* op=&blk->oplist[i];
		op->Flow=0;
		if (op->op==shop_ifb)
		{
			fallback_blocks++;
			return;
		}

		RegReadInfo(op->rs1,i);
		RegReadInfo(op->rs2,i);
		RegReadInfo(op->rs3,i);

		RegWriteInfo(&blk->oplist[0],op->rd,i);
		RegWriteInfo(&blk->oplist[0],op->rd2,i);
	}

	for (size_t i=0;i<blk->oplist.size();i++)
	{
		if (blk->oplist[i].Flow)
		{
			blk->oplist.erase(blk->oplist.begin()+i);
			REMOVED_OPS++;
			i--;
		}
	}

	int affregs=0;
	for (int i=0;i<16;i++)
	{
		if (RegisterWrite[i]!=0)
		{
			affregs++;
			//printf("r%02d:%02d ",i,RegisterWrite[i]);
		}
	}
	//printf("<> %d\n",affregs);

	//printf("%d FB, %d native, %.2f%% || %d removed ops!\n",fallback_blocks,total_blocks-fallback_blocks,fallback_blocks*100.f/total_blocks,REMOVED_OPS);
	//printf("\nBlock: %d affecter regs %d c\n",affregs,blk->guest_cycles);
    */
}

void UpdateFPSCR();
bool UpdateSR();
#include "hw/sh4/modules/ccn.h"
#include "ngen.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mmr.h"


#define SHIL_MODE 1
#include "shil_canonical.h"

#define SHIL_MODE 4
#include "shil_canonical.h"

//#define SHIL_MODE 2
//#include "shil_canonical.h"

#if FEAT_SHREC != DYNAREC_NONE
#define SHIL_MODE 3
#include "shil_canonical.h"
#endif

string name_reg(u32 reg)
{
	stringstream ss;

	if (reg>=reg_fr_0 && reg<=reg_xf_15)
		ss << "f" << (reg-16);
	else if (reg<=reg_r15)
		ss << "r" << reg;
	else if (reg == reg_sr_T)
		ss << "sr.T";
	else if (reg == reg_fpscr)
		ss << "fpscr";
	else if (reg == reg_sr_status)
		ss << "sr";
	else
		ss << "s" << reg;

	return ss.str();
}
string dissasm_param(const shil_param& prm, bool comma)
{
	stringstream ss;

	if (!prm.is_null() && comma)
			ss << ", ";

	if (prm.is_imm())
	{	
		if (prm.is_imm_s8())
			ss  << (s32)prm._imm ;
		else
			ss << "0x" << hex << prm._imm;
	}
	else if (prm.is_reg())
	{
		if (!prm.is_r32i())
			ss << "f" << (prm._reg-16);
		else if (prm._reg<=reg_r15)
			ss << "r" << prm._reg;
		else if (prm._reg == reg_sr_T)
			ss << "sr.T";
		else if (prm._reg == reg_fpscr)
			ss << "fpscr";
		else if (prm._reg == reg_sr_status)
			ss << "sr";
		else
			ss << "s" << prm._reg;
			

		if (prm.count()>1)
		{
			ss << "v" << prm.count();
		}
	}

	return ss.str();
}

string shil_opcode::dissasm()
{
	stringstream ss;
	ss << shilop_str[op] << " " << dissasm_param(rd,false) << dissasm_param(rd2,true) << " <= " << dissasm_param(rs1,false) << dissasm_param(rs2,true) << dissasm_param(rs3,true);
	return ss.str();
}

const char* shil_opcode_name(int op)
{
	return shilop_str[op];
}
