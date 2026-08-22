/*
	This file is part of libswirl
*/
#include "license/bsd"


#include "pvr_yuv.h"
#include "pvr_regs.h"
#include "hw/holly/holly_intc.h"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

//YUV converter code :)
//inits the YUV converter
alignas(32) u32 YUV_tempdata[512 / 4];//512 bytes; aligned so the NEON path's hinted loads hold for the reassembly case too

u32 YUV_dest = 0;

u32 YUV_blockcount;

u32 YUV_x_curr;
u32 YUV_y_curr;

u32 YUV_x_size;
u32 YUV_y_size;

static u32 YUV_index = 0;

static ASIC* asic;

void YUV_init(ASIC* asic)
{
	::asic = asic;
	YUV_x_curr = 0;
	YUV_y_curr = 0;

	YUV_dest = TA_YUV_TEX_BASE & VRAM_MASK;//TODO : add the masking needed
	TA_YUV_TEX_CNT = 0;
	YUV_blockcount = (TA_YUV_TEX_CTRL.yuv_u_size + 1) * (TA_YUV_TEX_CTRL.yuv_v_size + 1);

	if (TA_YUV_TEX_CTRL.yuv_tex != 0)
	{
		die("YUV: Not supported configuration\n");
		YUV_x_size = 16;
		YUV_y_size = 16;
	}
	else // yesh!!!
	{
		YUV_x_size = (TA_YUV_TEX_CTRL.yuv_u_size + 1) * 16;
		YUV_y_size = (TA_YUV_TEX_CTRL.yuv_v_size + 1) * 16;
	}
	YUV_index = 0;
}


INLINE u8 GetY420(int x, int y, u8* base)
{
	//u32 base=0;
	if (x > 7)
	{
		x -= 8;
		base += 64;
	}

	if (y > 7)
	{
		y -= 8;
		base += 128;
	}

	return base[x + y * 8];
}

INLINE u8 GetUV420(int x, int y, u8* base)
{
	int realx = x >> 1;
	int realy = y >> 1;

	return base[realx + realy * 8];
}

void YUV_Block8x8(u8* inuv, u8* iny, u8* out)
{
	u8* line_out_0 = out + 0;
	u8* line_out_1 = out + YUV_x_size * 2;

	for (int y = 0; y < 8; y += 2)
	{
		for (int x = 0; x < 8; x += 2)
		{
			u8 u = inuv[0];
			u8 v = inuv[64];

			line_out_0[0] = u;
			line_out_0[1] = iny[0];
			line_out_0[2] = v;
			line_out_0[3] = iny[1];

			line_out_1[0] = u;
			line_out_1[1] = iny[8 + 0];
			line_out_1[2] = v;
			line_out_1[3] = iny[8 + 1];

			inuv += 1;
			iny += 2;

			line_out_0 += 4;
			line_out_1 += 4;
		}
		iny += 8;
		inuv += 4;

		line_out_0 += YUV_x_size * 4 - 8 * 2;
		line_out_1 += YUV_x_size * 4 - 8 * 2;
	}
}

INLINE void YUV_Block384(u8* in, u8* out)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	//in: 64 bytes U (8x8), 64 bytes V (8x8), 4x64 bytes Y (8x8 blocks: TL TR BL BR)
	//out: 16 lines of 16 UYVY pixels (32 bytes), line stride YUV_x_size*2
	//a line is vzip(vzip(u_row,v_row), y_row): u0 y0 v0 y1 u1 y2 v1 y3 ...
	//in is >=8-byte aligned (SH4 DMA source is 32-byte aligned, YUV_tempdata is alignas(32));
	//out and the stride are 32-byte multiples (x_size is a multiple of 16) - the
	//assume_aligned's below turn the vld1/vst1 into hinted [:64]/[:128] forms
	u8* u_row = in;
	u8* v_row = in + 64;
	u32 stride = YUV_x_size * 2;

	for (int half = 0; half < 2; half++)
	{
		const u8* yl = in + 128 + half * 128;	//left 8x8 Y block of this half
		const u8* yr = yl + 64;					//right 8x8 Y block

		for (int r = 0; r < 4; r++)				//4 UV rows per half, 2 lines each
		{
			u_row = (u8*)__builtin_assume_aligned(u_row, 8);
			v_row = (u8*)__builtin_assume_aligned(v_row, 8);
			uint8x8x2_t uvz = vzip_u8(vld1_u8(u_row), vld1_u8(v_row));
			uint8x16_t uv = vcombine_u8(uvz.val[0], uvz.val[1]);
			u_row += 8;
			v_row += 8;

			for (int l = 0; l < 2; l++)
			{
				yl = (const u8*)__builtin_assume_aligned(yl, 8);
				yr = (const u8*)__builtin_assume_aligned(yr, 8);
				uint8x16_t y = vcombine_u8(vld1_u8(yl), vld1_u8(yr));
				yl += 8;
				yr += 8;

				uint8x16x2_t o = vzipq_u8(uv, y);
				out = (u8*)__builtin_assume_aligned(out, 32);
				vst1q_u8(out, o.val[0]);
				vst1q_u8(out + 16, o.val[1]);
				out += stride;
			}
		}
	}
#else
	u8* inuv = in;
	u8* iny = in + 128;
	u8* p_out = out;

	YUV_Block8x8(inuv + 0, iny + 0, p_out);                    //(0,0)
	YUV_Block8x8(inuv + 4, iny + 64, p_out + 8 * 2);                 //(8,0)
	YUV_Block8x8(inuv + 32, iny + 128, p_out + YUV_x_size * 8 * 2);     //(0,8)
	YUV_Block8x8(inuv + 36, iny + 192, p_out + YUV_x_size * 8 * 2 + 8 * 2); //(8,8)
#endif
}

INLINE void YUV_ConvertMacroBlock(u8* vram, u8* datap)
{
	//do shit
	TA_YUV_TEX_CNT++;

	YUV_Block384((u8*)datap, vram + YUV_dest);

	YUV_dest += 32;

	YUV_x_curr += 16;
	if (YUV_x_curr == YUV_x_size)
	{
		YUV_dest += 15 * YUV_x_size * 2;
		YUV_x_curr = 0;
		YUV_y_curr += 16;
		if (YUV_y_curr == YUV_y_size)
		{
			YUV_y_curr = 0;
		}
	}

	if (YUV_blockcount == TA_YUV_TEX_CNT)
	{
		YUV_init(asic);

		asic->RaiseInterrupt(holly_YUV_DMA);
	}
}

void YUV_data(u32* data, u32 count, u8* vram)
{
	if (YUV_blockcount == 0)
	{
		die("YUV_data : YUV decoder not inited , *WATCH*\n");
		//wtf ? not inited
		YUV_init(asic);
	}

	u32 block_size = TA_YUV_TEX_CTRL.yuv_form == 0 ? 384 : 512;

	verify(block_size == 384); //no support for 512

	count *= 32;

	while (count > 0)
	{
		if (YUV_index + count >= block_size)
		{
			//more or exactly one block remaining
			u32 dr = block_size - YUV_index;				//remaining bytes til block end
			if (YUV_index == 0)
			{
				// Avoid copy
				YUV_ConvertMacroBlock(vram, (u8*)data);				//convert block
			}
			else
			{
				memcpy(&YUV_tempdata[YUV_index >> 2], data, dr);//copy em
				YUV_ConvertMacroBlock(vram, (u8*)&YUV_tempdata[0]);	//convert block
				YUV_index = 0;
			}
			data += dr >> 2;									//count em
			count -= dr;
		}
		else
		{	//less that a whole block remaining
			memcpy(&YUV_tempdata[YUV_index >> 2], data, count);	//append it
			YUV_index += count;
			count = 0;
		}
	}

	verify(count == 0);
}
