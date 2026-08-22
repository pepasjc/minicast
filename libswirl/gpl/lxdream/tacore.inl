/*
    Derived from lxdream, original copyright notice follows
*/
#include <license/gpl>

/**
 * $Id$
 *
 * PVR2 Tile Accelerator implementation
 *
 * Copyright (c) 2005 Nathan Keynes.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <string.h>
#include <stdint.h>
#include <limits.h>

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "tacore.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_sb_regs.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/ta_ring.h"
#include "hw/holly/holly_intc.h"

#include <pthread.h>

#define PVR2_RAM_SIZE VRAM_SIZE
#define PVR2_RAM_MASK VRAM_MASK

static u8* pvr2_main_ram_hidden;

/* Local inlinable copy of pvr_map32() (hw/pvr/pvr_mem.cpp). Every PVRRAM access
 * goes through this; keeping it inline in this TU turns each access into a few
 * bit ops instead of a cross-TU function call. Must stay bit-identical to the
 * canonical pvr_map32 - VRAM_BANK_BIT is private to pvr_mem.cpp, so it is
 * duplicated here.
 */
#define TACORE_VRAM_BANK_BIT 0x400000
static inline uint32_t pvr_map32_inl(uint32_t offset32) {
    //64b wide bus is achieved by interleaving the banks every 32 bits
    const uint32_t static_bits = (VRAM_MASK - (TACORE_VRAM_BANK_BIT * 2 - 1)) | 3;
    const uint32_t offset_bits = (TACORE_VRAM_BANK_BIT - 1) & ~3;

    uint32_t bank = (offset32 & TACORE_VRAM_BANK_BIT) / TACORE_VRAM_BANK_BIT;
    uint32_t rv = offset32 & static_bits;
    rv |= (offset32 & offset_bits) * 2;
    rv |= bank * 4;
    return rv;
}

#define PVRRAM(addr) (*(uint32_t *)(pvr2_main_ram_hidden + (pvr_map32_inl(addr))))

/*
#include "lxdream.h"
#include "pvr2/pvr2.h"
#include "pvr2/pvr2mmio.h"
#include "asic.h"
#include "dream.h"
*/

#define CLAMP(v, low, high) (v < low? low : (v > high? high: v))
#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)

#define TRUE true
#define FALSE false

#define SEGMENT_END         0x80000000
#define SEGMENT_ZCLEAR      0x40000000
#define SEGMENT_SORT_TRANS  0x20000000
#define SEGMENT_START       0x10000000
#define SEGMENT_X(c)        (((c) >> 2) & 0x3F)
#define SEGMENT_Y(c)        (((c) >> 8) & 0x3F)
#define NO_POINTER          0x80000000
#define IS_TILE_PTR(p)      ( ((p)&NO_POINTER) == 0 )
#define IS_LAST_SEGMENT(s)  (((s)->control) & SEGMENT_END)

struct tile_segment {
    uint32_t control;
    pvraddr_t opaque_ptr;
    pvraddr_t opaquemod_ptr;
    pvraddr_t trans_ptr;
    pvraddr_t transmod_ptr;
    pvraddr_t punchout_ptr;
};


struct tile_bounds {
    int32_t x1, y1, x2, y2;
};


#define STATE_IDLE                 0
#define STATE_IN_LIST              1
#define STATE_IN_POLYGON           2
#define STATE_EXPECT_POLY_BLOCK2   3
#define STATE_EXPECT_VERTEX_BLOCK2 4
#define STATE_ERROR                5
#define STATE_EXPECT_END_VERTEX_BLOCK2 7

#define TA_CMD(i) ( (i) >> 29 )
#define TA_CMD_END_LIST 0
#define TA_CMD_CLIP 1
#define TA_CMD_POLYGON_CONTEXT 4
#define TA_CMD_SPRITE_CONTEXT 5
#define TA_CMD_VERTEX 7

#define TA_LIST_NONE -1
#define TA_LIST_OPAQUE 0
#define TA_LIST_OPAQUE_MOD 1
#define TA_LIST_TRANS 2
#define TA_LIST_TRANS_MOD 3
#define TA_LIST_PUNCH_OUT 4
#define TA_IS_MODIFIER_LIST(list) (list == TA_LIST_OPAQUE_MOD || list == TA_LIST_TRANS_MOD)

#define TA_GROW_UP 0
#define TA_GROW_DOWN 1

#define TA_VERTEX_NONE                        -1
#define TA_VERTEX_PACKED                      0x00
#define TA_VERTEX_TEX_PACKED                  0x08
#define TA_VERTEX_TEX_SPEC_PACKED             0x0C
#define TA_VERTEX_TEX_UV16_PACKED             0x09
#define TA_VERTEX_TEX_UV16_SPEC_PACKED        0x0D
#define TA_VERTEX_FLOAT                       0x10
#define TA_VERTEX_TEX_FLOAT                   0x18
#define TA_VERTEX_TEX_SPEC_FLOAT              0x1C
#define TA_VERTEX_TEX_UV16_FLOAT              0x19
#define TA_VERTEX_TEX_UV16_SPEC_FLOAT         0x1D
#define TA_VERTEX_INTENSITY                   0x20
#define TA_VERTEX_TEX_INTENSITY               0x28
#define TA_VERTEX_TEX_SPEC_INTENSITY          0x2C
#define TA_VERTEX_TEX_UV16_INTENSITY          0x29
#define TA_VERTEX_TEX_UV16_SPEC_INTENSITY     0x2D
#define TA_VERTEX_PACKED_MOD                  0x40
#define TA_VERTEX_TEX_PACKED_MOD              0x48
#define TA_VERTEX_TEX_SPEC_PACKED_MOD         0x4C
#define TA_VERTEX_TEX_UV16_PACKED_MOD         0x49
#define TA_VERTEX_TEX_UV16_SPEC_PACKED_MOD    0x4D
#define TA_VERTEX_INTENSITY_MOD               0x60
#define TA_VERTEX_TEX_INTENSITY_MOD           0x68
#define TA_VERTEX_TEX_SPEC_INTENSITY_MOD      0x6C
#define TA_VERTEX_TEX_UV16_INTENSITY_MOD      0x69
#define TA_VERTEX_TEX_UV16_SPEC_INTENSITY_MOD 0x6D
#define TA_VERTEX_SPRITE                      0x80
#define TA_VERTEX_TEX_SPRITE                  0x88
#define TA_VERTEX_MOD_VOLUME                  0x81
#define TA_VERTEX_LISTLESS                    0xFF

#define TA_IS_NORMAL_POLY() (ta_status.current_vertex_type < TA_VERTEX_SPRITE)

static int strip_lengths[4] = {3,4,6,8}; /* in vertexes */
#define TA_POLYCMD_LISTTYPE(i) ( ((i) >> 24) & 0x0F )
#define TA_POLYCMD_USELENGTH(i) ( i & 0x00800000 )
#define TA_POLYCMD_LENGTH(i)  strip_lengths[((i >> 18) & 0x03)]
#define TA_POLYCMD_CLIP(i)  ((i>>16)&0x03)
#define TA_POLYCMD_CLIP_NONE 0
#define TA_POLYCMD_CLIP_INSIDE 2
#define TA_POLYCMD_CLIP_OUTSIDE 3
#define TA_POLYCMD_COLOURFMT(i)  (i & 0x00000030)
#define TA_POLYCMD_COLOURFMT_ARGB32 0x00000000
#define TA_POLYCMD_COLOURFMT_FLOAT 0x00000010
#define TA_POLYCMD_COLOURFMT_INTENSITY 0x00000020
#define TA_POLYCMD_COLOURFMT_LASTINT 0x00000030

#define TA_POLYCMD_MODIFIED 0x00000080
#define TA_POLYCMD_FULLMOD  0x00000040
#define TA_POLYCMD_TEXTURED 0x00000008
#define TA_POLYCMD_SPECULAR 0x00000004
#define TA_POLYCMD_SHADED 0x00000002
#define TA_POLYCMD_UV16 0x00000001

#define TA_POLYCMD_IS_SPECULAR(i) ((i & 0x0000000C)==0x0000000C) /* Only applies to textured polys */
#define TA_POLYCMD_IS_FULLMOD(i) ((i & 0x000000C0)==0x000000C0)


#define TA_IS_END_VERTEX(i) (i & 0x10000000)

/** Note these are not the IEEE 754 definitions - the TA treats NANs
 * as if they were INFs of the appropriate sign.
 */
#define TA_IS_INF(f) (((*((uint32_t *)&f)) & 0xFF800000) == 0x7F800000)
#define TA_IS_NINF(f) (((*((uint32_t *)&f)) & 0xFF800000) == 0xFF800000)

#define MIN3( x1, x2, x3 ) ( (x1)<(x2)? ((x1)<(x3)?(x1):(x3)) : ((x2)<(x3)?(x2):(x3)) )
#define MAX3( x1, x2, x3 ) ( (x1)>(x2)? ((x1)>(x3)?(x1):(x3)) : ((x2)>(x3)?(x2):(x3)) )

#define TILESLOT( x, y ) (ta_status.current_tile_matrix + (ta_status.current_tile_size * (y * ta_status.width+ x) << 2))

struct pvr2_ta_vertex {
    float x,y,z;
    uint32_t detail[8]; /* 0-8 detail words */
};

struct pvr2_ta_status {
    int32_t state;
    int32_t width, height; /* Tile resolution, ie 20x15 */
    int32_t tilelist_dir; /* Growth direction of the tilelist, 0 = up, 1 = down */
    /* (tilelist_size removed - devcast fix: ta_alloc_tilelist uses the
     * per-list current_tile_size instead of one init-captured size) */
    uint32_t tilelist_start; /* Initial address of the tilelist */
    uint32_t polybuf_start; /* Initial bank address of the polygon buffer (ie &0x00F00000) */
    int32_t current_vertex_type;
    uint32_t accept_vertexes; /* 0 = NO, 1 = YES */
    int32_t vertex_count; /* index of last start-vertex seen, or -1 if no vertexes 
     * are present
     */
    uint32_t max_vertex;     /* Maximum number of vertexes in the current polygon (3/4/6/8) */
    uint32_t current_list_type;
    uint32_t current_tile_matrix; /* Memory location of the first tile for the current list. */
    uint32_t current_tile_size; /* Size of the tile matrix space  in 32-bit words (0/8/16/32)*/
    /* Cached per-tile write cursor for the currently-active list. Each entry
     * points at the vram address of that tile's 0xF0000000 end marker, i.e. the
     * next slot to write. Reset in ta_init_list. Avoids re-walking the tile's
     * linked list on every ta_write_tile_entry. Sized for the max 64x64 tile grid.
     */
    uint32_t tile_cursor[64*64];
    /* vram address of the head of the block that tile_cursor currently points
     * into, so the block-relative offset of the cursor can be recovered without
     * walking. Reset alongside tile_cursor in ta_init_list.
     */
    uint32_t tile_block[64*64];
    /* Cached value of the entry immediately preceding the cursor (the triangle-
     * stacking merge candidate), so ta_write_tile_entry needn't re-read it from
     * vram. Only meaningful when the cursor's block-relative offset is >= 1;
     * kept in sync on every append/merge. Reset in ta_init_list.
     */
    uint32_t tile_lastval[64*64];
    uint32_t intensity1, intensity2;
    struct tile_bounds clip;
    int32_t clip_mode;
    /**
     * Current working object
     */
    int32_t poly_context_size;
    int32_t poly_vertex_size;
    int32_t poly_parity;
    uint32_t poly_context[5];
    uint32_t poly_pointer;
    struct tile_bounds last_triangle_bounds;
    struct pvr2_ta_vertex poly_vertex[8];
    uint32_t debug_output;

    bool modifier_last_volume;
    struct tile_bounds modifier_bounds;
};

static struct pvr2_ta_status ta_status;

static int tilematrix_sizes[4] = {0,8,16,32};

/**
 * Convenience union - ta data is either 32-bit integer or 32-bit float.
 */
union ta_data {
    unsigned int i;
    float f;
};


void lxd_ta_reset() {
    ta_status.state = STATE_ERROR; /* State not valid until initialized */
    ta_status.debug_output = 0;
}

void lxd_ta_init(u8* vram) {
    pvr2_main_ram_hidden = vram;

    ta_status.state = STATE_IDLE;
    ta_status.current_list_type = -1;
    ta_status.current_vertex_type = -1;
    ta_status.poly_parity = 0;
    ta_status.vertex_count = 0;
    ta_status.max_vertex = 3;
    ta_status.current_vertex_type = TA_VERTEX_LISTLESS;
    ta_status.poly_vertex_size = 0;
    ta_status.poly_context[1] = 0;
    ta_status.last_triangle_bounds.x1 = -1;
    ta_status.accept_vertexes = TRUE;
    ta_status.clip.x1 = 0;
    ta_status.clip.y1 = 0;
    ta_status.clip_mode = TA_POLYCMD_CLIP_NONE;

    uint32_t size =  TA_GLOB_TILE_CLIP.full;// MMIO_READ( PVR2, TA_TILESIZE );
    ta_status.width = (size & 0xFFFF) + 1;
    ta_status.height = (size >> 16) + 1;
    ta_status.clip.x2 = ta_status.width-1;
    ta_status.clip.y2 = ta_status.height-1;
    uint32_t control = TA_ALLOC_CTRL; //MMIO_READ( PVR2, TA_TILECFG );
    ta_status.tilelist_dir = (control >> 20) & 0x01;
    // NOTE: no global tilelist_size here (devcast fix): overflow OPB blocks
    // are sized PER LIST TYPE (current_tile_size, from TA_ALLOC_CTRL at each
    // list init) in ta_alloc_tilelist, and GROW_DOWN pre-decrements there -
    // the old init-time plistpos adjustment + fixed size corrupted overflow
    // lists whose alloc size differed from the init-captured one (MvC2
    // pre-intro: TR text-sprite OPB spills -> missing letters).
    TA_ISP_CURRENT = TA_ISP_BASE;
    //MMIO_WRITE( PVR2, TA_POLYPOS, MMIO_READ( PVR2, TA_POLYBASE ) );
    uint32_t plistpos = TA_NEXT_OPB_INIT >> 2; //MMIO_READ( PVR2, TA_LISTBASE )
    TA_NEXT_OPB = plistpos;
    //MMIO_WRITE( PVR2, TA_LISTPOS, plistpos );
    ta_status.tilelist_start = plistpos;
    ta_status.polybuf_start = TA_ISP_BASE & 0x00F00000; // MMIO_READ( PVR2, TA_POLYBASE )
}

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
/* Branchless in the NEON pipe: the scalar versions pay an inf-check vmov plus
 * two vcmpe+vmrs flag transfers per channel, each serializing the core
 * against the FPU on the A9.
 *  - 256*c-1 via vmla is chained mul+add (no NEON fma on the A9), bit-equal
 *    to the scalar expression; vcvt truncates toward zero like the scalar
 *    int conversion
 *  - +inf saturates to INT_MAX then clamps to 255 (the TA_IS_INF value);
 *    -inf / c<=0 land at -1 or below and clamp to 0
 *  - lanes are packed {b,g,r,a} so two narrows produce the ARGB word
 * (+NaN becomes 0 instead of the inf-macro's 255 - garbage input either way.)
 */
static uint32_t parse_float_colour( float a, float r, float g, float b ) {
    float32x4_t v = { b, g, r, a };
    int32x4_t t = vcvtq_s32_f32(vmlaq_n_f32(vdupq_n_f32(-1.0f), v, 256.0f));
    t = vmaxq_s32(t, vdupq_n_s32(0));
    t = vminq_s32(t, vdupq_n_s32(255));
    uint16x4_t n16 = vmovn_u32(vreinterpretq_u32_s32(t));
    uint8x8_t n8 = vmovn_u16(vcombine_u16(n16, n16));
    return vget_lane_u32(vreinterpret_u32_u8(n8), 0);
}

static uint32_t parse_intensity_colour( uint32_t base, float intensity )
{
    /* NEON vmax/vmin propagate NaN, and vcvt.u32 sends NaN to 0 - same
     * result as the scalar CLAMP + convert. */
    float32x2_t v = vmax_f32(vdup_n_f32(intensity), vdup_n_f32(0.0f));
    v = vmin_f32(v, vdup_n_f32(1.0f));
    unsigned int i = vget_lane_u32(vcvt_u32_f32(vmul_n_f32(v, 256.0f)), 0);

    return
    (((((base & 0xFF) * i) & 0xFF00) |
            (((base & 0xFF00) * i) & 0xFF0000) |
            (((base & 0xFF0000) * i) & 0xFF000000)) >> 8) |
            (base & 0xFF000000);
}
#else
static uint32_t parse_float_colour( float a, float r, float g, float b ) {
    int ai,ri,gi,bi;

    if( TA_IS_INF(a) ) {
        ai = 255;
    } else {
        ai = 256 * CLAMP(a,0.0f,1.0f) - 1;
        if( ai < 0 ) ai = 0;
    }
    if( TA_IS_INF(r) ) {
        ri = 255;
    } else {
        ri = 256 * CLAMP(r,0.0f,1.0f) - 1;
        if( ri < 0 ) ri = 0;
    }
    if( TA_IS_INF(g) ) {
        gi = 255;
    } else {
        gi = 256 * CLAMP(g,0.0f,1.0f) - 1;
        if( gi < 0 ) gi = 0;
    }
    if( TA_IS_INF(b) ) {
        bi = 255;
    } else {
        bi = 256 * CLAMP(b,0.0f,1.0f) - 1;
        if( bi < 0 ) bi = 0;
    }
    return (ai << 24) | (ri << 16) | (gi << 8) | bi;
}

static uint32_t parse_intensity_colour( uint32_t base, float intensity )
{
    unsigned int i = (unsigned int)(256 * CLAMP(intensity, 0.0f,1.0f));

    return
    (((((base & 0xFF) * i) & 0xFF00) |
            (((base & 0xFF00) * i) & 0xFF0000) |
            (((base & 0xFF0000) * i) & 0xFF000000)) >> 8) |
            (base & 0xFF000000);
}
#endif

/**
 * Initialize the specified TA list.
 */
static void ta_init_list( unsigned int listtype ) {
    int config = TA_ALLOC_CTRL;//MMIO_READ( PVR2, TA_TILECFG );
    int tile_matrix = TA_OL_BASE; //MMIO_READ( PVR2, TA_TILEBASE );
    int list_end = TA_OL_LIMIT;//MMIO_READ( PVR2, TA_LISTEND );

    ta_status.current_tile_matrix = tile_matrix;

    // /* If the list grows down, the end must be < tile matrix start. 
    //  * If it grows up, the end must be > tile matrix start.
    //  * Don't ask me why, it just does...
    //  */
    // if( ((ta_status.tilelist_dir == TA_GROW_DOWN && list_end <= tile_matrix) ||
    //         (ta_status.tilelist_dir == TA_GROW_UP && list_end >= tile_matrix )) &&
    //         listtype <= TA_LIST_PUNCH_OUT )
    // I don't think the hardware enforces this, it coud just have no "end" in practice.
    // Guilty Gear sets it up incorrectly and it seems to work on real hw
    // This needs further testing ~skmp

    {
        int i;
        for( i=0; i < listtype; i++ ) {
            int size = tilematrix_sizes[(config & 0x03)] << 2;
            ta_status.current_tile_matrix += ta_status.width * ta_status.height * size;
            config >>= 4;
        }
        ta_status.current_tile_size = tilematrix_sizes[(config & 0x03)];

        /* Initialize each tile to 0xF0000000, and point each tile's write
         * cursor at that marker (the next slot to write).
         */
        if( ta_status.current_tile_size != 0 ) {
            pvraddr_t p = (ta_status.current_tile_matrix);
            for( i=0; i< ta_status.width * ta_status.height; i++ ) {
                PVRRAM(p) = 0xF0000000;
                ta_status.tile_cursor[i] = p;
                ta_status.tile_block[i] = p;
                ta_status.tile_lastval[i] = 0; /* no preceding entry yet (offset 0) */
                p += ta_status.current_tile_size * 4;
            }
        }
    }
    // See commend above
    // else {
    //     ta_status.current_tile_size = 0;
    // }
    // 
    // if( tile_matrix == list_end ) {
    //     ta_status.current_tile_size = 0;
    // }

    ta_status.state = STATE_IN_LIST;
    ta_status.current_list_type = listtype;
    ta_status.last_triangle_bounds.x1 = -1;
}

/*
static int list_events[5] = {EVENT_PVR_OPAQUE_DONE, EVENT_PVR_OPAQUEMOD_DONE, 
        EVENT_PVR_TRANS_DONE, EVENT_PVR_TRANSMOD_DONE,
        EVENT_PVR_PUNCHOUT_DONE };
*/
static HollyInterruptID list_events[5] = {holly_OPAQUE, holly_OPAQUEMOD, 
        holly_TRANS, holly_TRANSMOD,
        holly_PUNCHTHRU };

static void ta_end_list() {
    if( ta_status.current_list_type != TA_LIST_NONE ) {
        //asic_event( list_events[ta_status.current_list_type] );
        if (settings.pvr.MultithreadedTA == TA_STTA)
        {
            asic_RaiseInterrupt(list_events[ta_status.current_list_type]);
        }
    }
    if (settings.pvr.MultithreadedTA != TA_STTA) {
        ta_eol_interrupt_mark++;
    }
    ta_status.current_list_type = TA_LIST_NONE;
    ta_status.current_vertex_type = TA_VERTEX_LISTLESS;
    ta_status.poly_vertex_size = 0;
    ta_status.poly_context[1] = 0;
    ta_status.state = STATE_IDLE;
}

static void ta_bad_input_error() {
    if (settings.pvr.MultithreadedTA != TA_STTA)
    {
        if (settings.debug.PrintTaErrors) {
            printf("TA error: holly_ILLEGAL_PARAM. (interrupt suppressed)\n");
        }
    }
    else
    {
        asic_RaiseInterrupt(holly_ILLEGAL_PARAM);
        if (settings.debug.PrintTaErrors) {
            printf("TA error: holly_ILLEGAL_PARAM. Interrupt raised\n");
        }
        //asic_event( EVENT_PVR_BAD_INPUT );
    }
}

/**
 * Write data out to the polygon buffer.
 * If the end-of-buffer is reached, asserts EVENT_PVR_PRIM_ALLOC_FAIL
 * @param data to be written
 * @param length Number of 32-bit words to write.
 * @return number of words actually written
 */
static int ta_write_polygon_buffer( uint32_t *data, int length )
{
    int rv;
    int posn = TA_ISP_CURRENT;// MMIO_READ( PVR2, TA_POLYPOS );
    int end = TA_ISP_LIMIT;// MMIO_READ( PVR2, TA_POLYEND );
    for( rv=0; rv < length; rv++ ) {
        if( posn == end ) {
            if (settings.pvr.MultithreadedTA != TA_STTA) {
                if (settings.debug.PrintTaErrors) {
                    printf("TA error: holly_PRIM_NOMEM. (interrupt suppressed)\n");
                }
            } else {
                asic_RaiseInterrupt(holly_PRIM_NOMEM);
                if (settings.debug.PrintTaErrors) {
                    printf("TA error: holly_PRIM_NOMEM. Interrupt Raised\n");
                }
                //asic_event( EVENT_PVR_PRIM_ALLOC_FAIL );
            }
            ////	    ta_status.state = STATE_ERROR;
            break;
        }
        if( posn < PVR2_RAM_SIZE ) {
            PVRRAM(posn) = *data++;
        }
        posn += 4;
    }

    TA_ISP_CURRENT = posn;
    //MMIO_WRITE( PVR2, TA_POLYPOS, posn );
    return rv;
}

#define TA_NO_ALLOC 0xFFFFFFFF

/**
 * Allocate a new tile list block from the grow space and update the
 * word at reference to be a link to the new block.
 */
static uint32_t ta_alloc_tilelist( uint32_t reference ) {
    uint32_t posn = TA_NEXT_OPB;//MMIO_READ( PVR2, TA_LISTPOS );
    uint32_t limit = TA_OL_LIMIT >> 2;//MMIO_READ( PVR2, TA_LISTEND ) >> 2;
    uint32_t newposn;
    if( ta_status.tilelist_dir == TA_GROW_DOWN ) {
        posn -= ta_status.current_tile_size;   // devcast: pre-decrement, PER-LIST size
        newposn = posn;
        if( posn == limit ) {
            PVRRAM(posn<<2) = 0xF0000000;
            PVRRAM(reference) = 0xE0000000 | (posn<<2);
            return TA_NO_ALLOC;
        } else if( posn < limit ) {
            PVRRAM(reference) = 0xE0000000 | (posn<<2);
            return TA_NO_ALLOC;
        } else if( newposn <= limit ) {
        } else if( newposn <= (limit + ta_status.current_tile_size) ) {
            if (settings.pvr.MultithreadedTA != TA_STTA) {
                if (settings.debug.PrintTaErrors) {
                    printf("TA error: holly_MATR_NOMEM. (interrupt suppressed)\n");
                }
            } else {
                asic_RaiseInterrupt(holly_MATR_NOMEM);
                if (settings.debug.PrintTaErrors) {
                    printf("TA error: holly_MATR_NOMEM. Interrupt raised\n");
                }
                //asic_event( EVENT_PVR_MATRIX_ALLOC_FAIL );
            }
            TA_NEXT_OPB = newposn;
            //MMIO_WRITE( PVR2, TA_LISTPOS, newposn );
        } else {
            TA_NEXT_OPB = newposn;
            //MMIO_WRITE( PVR2, TA_LISTPOS, newposn );
        }
        PVRRAM(reference) = 0xE0000000 | (posn<<2);
        return posn << 2;
    } else {
        newposn = posn + ta_status.current_tile_size;   // devcast: PER-LIST size
        if( posn == limit ) {
            PVRRAM(posn<<2) = 0xF0000000;
            PVRRAM(reference) = 0xE0000000 | (posn<<2);
            return TA_NO_ALLOC;
        } else if ( posn > limit ) {
            PVRRAM(reference) = 0xE0000000 | (posn<<2);
            return TA_NO_ALLOC;
        } else if( newposn >= limit ) {
        } else if( newposn >= (limit - ta_status.current_tile_size) ) {
            if (settings.pvr.MultithreadedTA != TA_STTA) {
                if (settings.debug.PrintTaErrors) {
                    printf("TA error: holly_MATR_NOMEM. (interrupt suppressed)\n");
                }
            } else {
                asic_RaiseInterrupt(holly_MATR_NOMEM);
                if (settings.debug.PrintTaErrors) {
                    printf("TA error: holly_MATR_NOMEM. Interrupt raised\n");
                }
                //asic_event( EVENT_PVR_MATRIX_ALLOC_FAIL );
            }
            TA_NEXT_OPB = newposn;
            //MMIO_WRITE( PVR2, TA_LISTPOS, newposn );
        } else {
            TA_NEXT_OPB = newposn;
            //MMIO_WRITE( PVR2, TA_LISTPOS, newposn );
        }	    
        PVRRAM(reference) = 0xE0000000 | (posn<<2);
        return posn << 2;
    }
}

/**
 * Write a tile entry out to the matrix.
 */
static void ta_write_tile_entry( int x, int y, uint32_t tile_entry ) {
    int tileidx = y * ta_status.width + x;
    uint32_t lasttri = 0;

    if( ta_status.clip_mode == TA_POLYCMD_CLIP_OUTSIDE &&
            x >= ta_status.clip.x1 && x <= ta_status.clip.x2 &&
            y >= ta_status.clip.y1 && y <= ta_status.clip.y2 ) {
        /* Tile clipped out */
        return;
    }

    if( (tile_entry & 0x80000000) &&
            ta_status.last_triangle_bounds.x1 != -1 &&
            ta_status.last_triangle_bounds.x1 <= x &&
            ta_status.last_triangle_bounds.x2 >= x &&
            ta_status.last_triangle_bounds.y1 <= y &&
            ta_status.last_triangle_bounds.y2 >= y ) {
        /* potential for triangle stacking */
        lasttri = tile_entry & 0xE1E00000;
    }

    /* cursor points at this tile's 0xF0000000 end marker (the next slot to
     * write); block is the head of the block the cursor lives in. The
     * block-relative offset recovers the geometry the original linear walk
     * derived from its loop index.
     */
    uint32_t cursor = ta_status.tile_cursor[tileidx];
    uint32_t block  = ta_status.tile_block[tileidx];
    uint32_t offset = (cursor - block) >> 2;

    /* Triangle stacking: try to merge into the entry immediately preceding the
     * marker, exactly as the linear walk did (only within a block, never across
     * the block head). The candidate's value is cached, so no vram read here.
     */
    if( lasttri != 0 && offset >= 1 ) {
        uint32_t value = ta_status.tile_lastval[tileidx];
        if( lasttri == (value & 0xE1E00000) ) {
            int count = (value & 0x1E000000) + 0x02000000;
            if( count < 0x20000000 ) {
                uint32_t merged = (value & 0xE1FFFFFF) | count;
                PVRRAM(cursor-4) = merged;
                ta_status.tile_lastval[tileidx] = merged;
                return;
            }
        }
    }

    /* If the marker sits at the last slot of the block (the link slot), this
     * block is full - allocate a new one and continue there.
     */
    if( offset >= ta_status.current_tile_size-1 ) {
        uint32_t newtile = ta_alloc_tilelist(cursor);
        if( newtile == TA_NO_ALLOC ) {
            /* Out of tile-list memory - drop the entry, leave cursor put. */
            return;
        }
        cursor = newtile;
        block = newtile;
        ta_status.tile_block[tileidx] = block;
    }

    /* Append: entry into the marker slot, new marker after it, advance cursor.
     * The just-written entry becomes the new stacking candidate.
     */
    PVRRAM(cursor) = tile_entry;
    PVRRAM(cursor+4) = 0xF0000000;
    ta_status.tile_cursor[tileidx] = cursor + 4;
    ta_status.tile_lastval[tileidx] = tile_entry;
}

/**
 * Write a completed polygon out to the memory buffers 
 * OPTIMIZEME: This is not terribly efficient at the moment.
 */
static void ta_commit_polygon( ) {
    int i, x, y;
    int* tx = (int*)alloca(ta_status.vertex_count * sizeof(int));
    int* ty = (int*)alloca(ta_status.vertex_count * sizeof(int));
    struct tile_bounds* triangle_bound = (struct tile_bounds*)alloca((ta_status.vertex_count - 2) * sizeof(tile_bounds));
    struct tile_bounds polygon_bound;
    uint32_t poly_context[5];

    memcpy( poly_context, ta_status.poly_context, ta_status.poly_context_size * 4 );

    /* Compute the tile coordinates for each vertex (need to be careful with
     * clamping here)
     */
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
    /* Branchless, entirely inside the NEON pipe: the scalar version's four
     * vcmpe+vmrs flag transfers per vertex serialize the core against the
     * FPU on the A9 and dominate this loop.
     *  - x/32.0f is exact (power of two), so (int)(x/32.0f) == ((int)x)>>5
     *  - vcvt saturates: +inf/huge -> INT_MAX, and INT_MAX>>5 == INT_MAX/32,
     *    exactly the explicit clamp
     *  - v < 0 (including -inf) forces -1 via the compare mask
     * (NaNs become 0 instead of the inf-macro clamps - nominal under
     * -ffast-math either way.)
     */
    for( i=0; i<ta_status.vertex_count; i++ ) {
        float32x2_t v = vld1_f32(&ta_status.poly_vertex[i].x);
        int32x2_t t = vshr_n_s32(vcvt_s32_f32(v), 5);
        t = vorr_s32(t, vreinterpret_s32_u32(vclt_f32(v, vdup_n_f32(0.0f))));
        vst1_lane_s32(&tx[i], t, 0);
        vst1_lane_s32(&ty[i], t, 1);
    }
#else
    for( i=0; i<ta_status.vertex_count; i++ ) {
        if( ta_status.poly_vertex[i].x < 0.0f || TA_IS_NINF(ta_status.poly_vertex[i].x) ) {
            tx[i] = -1;
        } else if( ta_status.poly_vertex[i].x > (float)INT_MAX || TA_IS_INF(ta_status.poly_vertex[i].x) ) {
            tx[i] = INT_MAX/32;
        } else {
            tx[i] = (int)(ta_status.poly_vertex[i].x / 32.0f);
        }
        if( ta_status.poly_vertex[i].y < 0.0f || TA_IS_NINF(ta_status.poly_vertex[i].y)) {
            ty[i] = -1;
        } else if( ta_status.poly_vertex[i].y > (float)INT_MAX || TA_IS_INF(ta_status.poly_vertex[i].y) ) {
            ty[i] = INT_MAX/32;
        } else {
            ty[i] = (int)(ta_status.poly_vertex[i].y / 32.0f);
        }
    }
#endif

    /* Compute bounding box for each triangle individually, as well
     * as the overall polygon.
     */

    triangle_bound[0].x1 = MIN3(tx[0],tx[1],tx[2]);
    triangle_bound[0].x2 = MAX3(tx[0],tx[1],tx[2]);
    triangle_bound[0].y1 = MIN3(ty[0],ty[1],ty[2]);
    triangle_bound[0].y2 = MAX3(ty[0],ty[1],ty[2]);
    polygon_bound.x1 = triangle_bound[0].x1;
    polygon_bound.y1 = triangle_bound[0].y1;
    polygon_bound.x2 = triangle_bound[0].x2;
    polygon_bound.y2 = triangle_bound[0].y2;

    for( i=1; i<ta_status.vertex_count-2; i++ ) {
        triangle_bound[i].x1 = MIN3(tx[i],tx[i+1],tx[i+2]);
        triangle_bound[i].x2 = MAX3(tx[i],tx[i+1],tx[i+2]);
        triangle_bound[i].y1 = MIN3(ty[i],ty[i+1],ty[i+2]);
        triangle_bound[i].y2 = MAX3(ty[i],ty[i+1],ty[i+2]);
        polygon_bound.x1 = MIN(polygon_bound.x1, triangle_bound[i].x1);
        polygon_bound.x2 = MAX(polygon_bound.x2, triangle_bound[i].x2);
        polygon_bound.y1 = MIN(polygon_bound.y1, triangle_bound[i].y1);
        polygon_bound.y2 = MAX(polygon_bound.y2, triangle_bound[i].y2);
    }

    /* Clamp the polygon bounds to the frustum */
    if( polygon_bound.x1 < 0 ) polygon_bound.x1 = 0;
    if( polygon_bound.x2 >= ta_status.width ) polygon_bound.x2 = ta_status.width-1;
    if( polygon_bound.y1 < 0 ) polygon_bound.y1 = 0;
    if( polygon_bound.y2 >= ta_status.height ) polygon_bound.y2 = ta_status.height-1;

    if (ta_status.current_vertex_type == TA_VERTEX_MOD_VOLUME) {
        ta_status.modifier_bounds.x1 = MIN(ta_status.modifier_bounds.x1, polygon_bound.x1);
        ta_status.modifier_bounds.x2 = MAX(ta_status.modifier_bounds.x2, polygon_bound.x2);
        ta_status.modifier_bounds.y1 = MIN(ta_status.modifier_bounds.y1, polygon_bound.y1);
        ta_status.modifier_bounds.y2 = MAX(ta_status.modifier_bounds.y2, polygon_bound.y2);

        if (ta_status.modifier_last_volume) {
            polygon_bound = ta_status.modifier_bounds;
        }
    }

    /* Set the "single tile" flag if it's entirely contained in 1 tile */
    if( polygon_bound.x1 == polygon_bound.x2 &&
            polygon_bound.y1 == polygon_bound.y2 ) {
        poly_context[0] |= 0x00200000;
    }

    /* If the polygon is entirely clipped, don't even write the polygon data */
    switch( ta_status.clip_mode ) {
    case TA_POLYCMD_CLIP_NONE:
        if( polygon_bound.x2 < 0 || polygon_bound.x1 >= ta_status.width ||
                polygon_bound.y2 < 0 || polygon_bound.y1 >= ta_status.height ) {
            return;
        }
        break;
    case TA_POLYCMD_CLIP_INSIDE:
        if( polygon_bound.x2 < ta_status.clip.x1 || polygon_bound.x1 > ta_status.clip.x2 ||
                polygon_bound.y2 < ta_status.clip.y1 || polygon_bound.y1 > ta_status.clip.y2 ) {
            return;
        } else {
            /* Clamp to clip bounds */
            if( polygon_bound.x1 < ta_status.clip.x1 ) polygon_bound.x1 = ta_status.clip.x1;
            if( polygon_bound.x2 > ta_status.clip.x2 ) polygon_bound.x2 = ta_status.clip.x2;
            if( polygon_bound.y1 < ta_status.clip.y1 ) polygon_bound.y1 = ta_status.clip.y1;
            if( polygon_bound.y2 > ta_status.clip.y2 ) polygon_bound.y2 = ta_status.clip.y2;
        }
        break;
    case TA_POLYCMD_CLIP_OUTSIDE:
        if( polygon_bound.x1 >= ta_status.clip.x1 && polygon_bound.x2 <= ta_status.clip.x2 &&
                polygon_bound.y1 >= ta_status.clip.y1 && polygon_bound.y2 <= ta_status.clip.y2 ) {
            return;
        }
        break;
    }

    /* Ok, we're good to go - write out the polygon first */
    uint32_t tile_entry = (TA_ISP_CURRENT/*MMIO_READ( PVR2, TA_POLYPOS )*/ - ta_status.polybuf_start) >> 2 | 
    ta_status.poly_pointer;

    int status = ta_write_polygon_buffer( poly_context, ta_status.poly_context_size );
    if( status == 0 ) {
        /* No memory available - abort */
        return;
    } else {
        for( i=0; i<ta_status.vertex_count && status != 0; i++ ) {
            status = ta_write_polygon_buffer( (uint32_t *)(&ta_status.poly_vertex[i]), 3 + ta_status.poly_vertex_size );
        }
    }

    if( ta_status.current_tile_size == 0 ) {
        /* No memory for tile entry, so don't write anything */
        return;
    }

    /* And now the tile entries. Triangles are different from everything else */
    if( ta_status.vertex_count == 3 ) {
        tile_entry |= 0x80000000;
        for( y=polygon_bound.y1; y<=polygon_bound.y2; y++ ) {
            for( x=polygon_bound.x1; x<=polygon_bound.x2; x++ ) {
                ta_write_tile_entry( x,y,tile_entry );
            }
        }
        ta_status.last_triangle_bounds.x1 = polygon_bound.x1;
        ta_status.last_triangle_bounds.y1 = polygon_bound.y1;
        ta_status.last_triangle_bounds.x2 = polygon_bound.x2;
        ta_status.last_triangle_bounds.y2 = polygon_bound.y2;
    } else if( ta_status.current_vertex_type == TA_VERTEX_SPRITE ||
            ta_status.current_vertex_type == TA_VERTEX_TEX_SPRITE ) {
        tile_entry |= 0xA0000000;
        for( y=polygon_bound.y1; y<=polygon_bound.y2; y++ ) {
            for( x=polygon_bound.x1; x<=polygon_bound.x2; x++ ) {
                ta_write_tile_entry( x,y,tile_entry );
            }
        }
        ta_status.last_triangle_bounds.x1 = polygon_bound.x1;
        ta_status.last_triangle_bounds.y1 = polygon_bound.y1;
        ta_status.last_triangle_bounds.x2 = polygon_bound.x2;
        ta_status.last_triangle_bounds.y2 = polygon_bound.y2;
    } else {
        for( y=polygon_bound.y1; y<=polygon_bound.y2; y++ ) {
            for( x=polygon_bound.x1; x<=polygon_bound.x2; x++ ) {
                uint32_t entry = tile_entry;
                for( i=0; i<ta_status.vertex_count-2; i++ ) {
                    if( triangle_bound[i].x1 <= x && triangle_bound[i].x2 >= x &&
                            triangle_bound[i].y1 <= y && triangle_bound[i].y2 >= y ) {
                        entry |= (0x40000000>>i);
                    }
                }
                ta_write_tile_entry( x, y, entry );
            }
        }
        ta_status.last_triangle_bounds.x1 = -1;
    }
}

/**
 * Variant of ta_split_polygon called when vertex_count == max_vertex, but 
 * the client hasn't sent the LAST VERTEX flag. Commit the poly as normal
 * first, then start a new poly with the first 2 vertexes taken from the 
 * current one.
 */
static void ta_split_polygon() {
    ta_commit_polygon();
    if( TA_IS_NORMAL_POLY() ) { 
        /* This only applies to ordinary polys - Sprites + modifier lists are
         * handled differently
         */
        if( ta_status.vertex_count == 3 ) {
            /* Triangles use an odd/even scheme */
            if( ta_status.poly_parity == 0 ) {
                memcpy( &ta_status.poly_vertex[0], &ta_status.poly_vertex[2], 
                        sizeof(struct pvr2_ta_vertex) );
                ta_status.poly_parity = 1;
            } else {
                memcpy( &ta_status.poly_vertex[1], &ta_status.poly_vertex[2],
                        sizeof(struct pvr2_ta_vertex) );
                ta_status.poly_parity = 0;
            }
        } else {
            /* Everything else just uses the last 2 vertexes in order */
            memcpy( &ta_status.poly_vertex[0], &ta_status.poly_vertex[ta_status.vertex_count-2], 
                    sizeof(struct pvr2_ta_vertex)*2 );
            ta_status.poly_parity = 0;
        }
        ta_status.vertex_count = 2;
    } else {
        ta_status.vertex_count = 0;
    }
}

/**
 * Parse the polygon context block and setup the internal state to receive
 * vertexes.
 * @param data 32 bytes of parameter data.
 */
static void ta_parse_polygon_context( union ta_data *data ) {
    int colourfmt = TA_POLYCMD_COLOURFMT(data[0].i);
    if( TA_POLYCMD_USELENGTH(data[0].i) ) {
        ta_status.max_vertex = TA_POLYCMD_LENGTH(data[0].i);
    }
    ta_status.clip_mode = TA_POLYCMD_CLIP(data[0].i);
    if( ta_status.clip_mode == 1 ) { /* Reserved - treat as CLIP_INSIDE */
        ta_status.clip_mode = TA_POLYCMD_CLIP_INSIDE;
    }
    ta_status.vertex_count = 0;
    ta_status.poly_context[0] = 
        (data[1].i & 0xFC1FFFFF) | ((data[0].i & 0x0B) << 22);
    ta_status.poly_context[1] = data[2].i;
    ta_status.poly_context[3] = data[4].i;
    ta_status.poly_parity = 0;
    if( data[0].i & TA_POLYCMD_TEXTURED ) {
        ta_status.current_vertex_type = data[0].i & 0x0D;
        ta_status.poly_context[2] = data[3].i;
        ta_status.poly_context[4] = data[5].i;
        if( data[0].i & TA_POLYCMD_SPECULAR ) {
            ta_status.poly_context[0] |= 0x01000000;
            ta_status.poly_vertex_size = 4;
        } else {
            ta_status.poly_vertex_size = 3;
        }
        if( data[0].i & TA_POLYCMD_UV16 ) {
            ta_status.poly_vertex_size--;
        }
    } else {
        ta_status.current_vertex_type = 0;
        ta_status.poly_vertex_size = 1;
        ta_status.poly_context[2] = 0;
        ta_status.poly_context[4] = 0;
    }

    ta_status.poly_pointer = (ta_status.poly_vertex_size << 21);
    ta_status.poly_context_size = 3;
    if( data[0].i & TA_POLYCMD_MODIFIED ) {
        ta_status.poly_pointer |= 0x01000000;
        if( data[0].i & TA_POLYCMD_FULLMOD ) {
            ta_status.poly_context_size = 5;
            ta_status.poly_vertex_size <<= 1;
            ta_status.current_vertex_type |= 0x40;
            /* Modified/float not supported - behaves as per last intensity */
            if( colourfmt == TA_POLYCMD_COLOURFMT_FLOAT ) {
                colourfmt = TA_POLYCMD_COLOURFMT_LASTINT;
            }
        }
    }

    if( colourfmt == TA_POLYCMD_COLOURFMT_INTENSITY ) {
        if( TA_POLYCMD_IS_FULLMOD(data[0].i) ||
                TA_POLYCMD_IS_SPECULAR(data[0].i) ) {
            ta_status.state = STATE_EXPECT_POLY_BLOCK2;
        } else {
            ta_status.intensity1 = 
                parse_float_colour( data[4].f, data[5].f, data[6].f, data[7].f );
        }
    } else if( colourfmt == TA_POLYCMD_COLOURFMT_LASTINT ) {
        colourfmt = TA_POLYCMD_COLOURFMT_INTENSITY;
    }

    ta_status.current_vertex_type |= colourfmt;
}

/**
 * Parse the modifier volume context block and setup the internal state to 
 * receive modifier vertexes.
 * @param data 32 bytes of parameter data.
 */
static void ta_parse_modifier_context( union ta_data *data ) {
    ta_status.current_vertex_type = TA_VERTEX_MOD_VOLUME;
    ta_status.poly_vertex_size = 0;
    ta_status.clip_mode = TA_POLYCMD_CLIP(data[0].i);
    if( ta_status.clip_mode == 1 ) { /* Reserved - treat as CLIP_INSIDE */
        ta_status.clip_mode = TA_POLYCMD_CLIP_INSIDE;
    }
    ta_status.poly_context_size = 3;
    ta_status.poly_context[0] = (data[1].i & 0xFC1FFFFF) |
    ((data[0].i & 0x0B)<<22);
    if( TA_POLYCMD_IS_SPECULAR(data[0].i) ) {
        ta_status.poly_context[0] |= 0x01000000;
    }
    ta_status.poly_context[1] = 0;
    ta_status.poly_context[2] = 0;
    ta_status.vertex_count = 0;
    ta_status.max_vertex = 3;
    ta_status.poly_pointer = 0;

    if (ta_status.modifier_last_volume) {
        ta_status.modifier_bounds.x1 = INT_MAX/32;
        ta_status.modifier_bounds.y1 = INT_MAX/32;
        ta_status.modifier_bounds.x2 = -1;
        ta_status.modifier_bounds.y2 = -1;
    }
    ta_status.modifier_last_volume = data[0].i & TA_POLYCMD_FULLMOD;
}

/**
 * Parse the sprite context block and setup the internal state to receive
 * vertexes.
 * @param data 32 bytes of parameter data.
 */
static void ta_parse_sprite_context( union ta_data *data ) {
    ta_status.poly_context_size = 3;
    ta_status.poly_context[0] = (data[1].i & 0xFC1FFFFF) |
    ((data[0].i & 0x0B)<<22) | 0x00400000;
    ta_status.clip_mode = TA_POLYCMD_CLIP(data[0].i);
    if( ta_status.clip_mode == 1 ) { /* Reserved - treat as CLIP_INSIDE */
        ta_status.clip_mode = TA_POLYCMD_CLIP_INSIDE;
    }
    if( TA_POLYCMD_IS_SPECULAR(data[0].i) ) {
        ta_status.poly_context[0] |= 0x01000000;
    }
    ta_status.poly_context[1] = data[2].i;
    ta_status.poly_context[2] = data[3].i;
    if( data[0].i & TA_POLYCMD_TEXTURED ) {
        ta_status.poly_vertex_size = 2;
        ta_status.poly_vertex[2].detail[1] = data[4].i;
        if( TA_POLYCMD_IS_SPECULAR(data[0].i) ) {
            ta_status.poly_vertex_size = 3;
            ta_status.poly_vertex[2].detail[2] = data[5].i;   /* offset colour */
        }
        ta_status.current_vertex_type = TA_VERTEX_TEX_SPRITE;
    } else {
        /* untextured: TA_POLYCMD_IS_SPECULAR requires the textured bit, so no offset */
        ta_status.poly_vertex_size = 1;
        ta_status.poly_vertex[2].detail[0] = data[4].i;
        ta_status.current_vertex_type = TA_VERTEX_SPRITE;
    }
    ta_status.vertex_count = 0;
    ta_status.max_vertex = 4;
    ta_status.poly_pointer = (ta_status.poly_vertex_size << 21);
}

/**
 * Copy the last read vertex into all vertexes up to max_vertex. Used for
 * Aborted polygons under some circumstances.
 */
static void ta_fill_vertexes( ) {
    int i;
    for( i=ta_status.vertex_count; i<ta_status.max_vertex; i++ ) {
        memcpy( &ta_status.poly_vertex[i], &ta_status.poly_vertex[ta_status.vertex_count-1],
                sizeof( struct pvr2_ta_vertex ) );
    }
}

static void ta_parse_vertex( union ta_data *data ) {
    struct pvr2_ta_vertex *vertex = &ta_status.poly_vertex[ta_status.vertex_count];
    vertex->x = data[1].f;
    vertex->y = data[2].f;
    vertex->z = data[3].f;

    switch( ta_status.current_vertex_type ) {
    case TA_VERTEX_PACKED:
        vertex->detail[0] = data[6].i;
        break;
    case TA_VERTEX_FLOAT:
        vertex->detail[0] = parse_float_colour( data[4].f, data[5].f, data[6].f, data[7].f );
        break;
    case TA_VERTEX_INTENSITY:
        vertex->detail[0] = parse_intensity_colour( ta_status.intensity1, data[6].f );
        break;

    case TA_VERTEX_TEX_SPEC_PACKED:
        vertex->detail[3] = data[7].i; /* ARGB */
        /* Fallthrough */
    case TA_VERTEX_TEX_PACKED:
        vertex->detail[0] = data[4].i; /* U */
        vertex->detail[1] = data[5].i; /* V */
        vertex->detail[2] = data[6].i; /* ARGB */
        break;
    case TA_VERTEX_TEX_UV16_SPEC_PACKED:
        vertex->detail[2] = data[7].i; /* ARGB */
        /* Fallthrough */
    case TA_VERTEX_TEX_UV16_PACKED:
        vertex->detail[0] = data[4].i; /* UV */
        vertex->detail[1] = data[6].i; /* ARGB */
        break;

    case TA_VERTEX_TEX_FLOAT:
    case TA_VERTEX_TEX_SPEC_FLOAT:
        vertex->detail[0] = data[4].i; /* U */
        vertex->detail[1] = data[5].i; /* UV */
        ta_status.state = STATE_EXPECT_VERTEX_BLOCK2;
        break;
    case TA_VERTEX_TEX_UV16_FLOAT:
    case TA_VERTEX_TEX_UV16_SPEC_FLOAT:
        vertex->detail[0] = data[4].i; /* UV */
        ta_status.state = STATE_EXPECT_VERTEX_BLOCK2;
        break;

    case TA_VERTEX_TEX_SPEC_INTENSITY:
        vertex->detail[3] = parse_intensity_colour( ta_status.intensity2, data[7].f );
        /* Fallthrough */
    case TA_VERTEX_TEX_INTENSITY:
        vertex->detail[0] = data[4].i; /* U */
        vertex->detail[1] = data[5].i; /* V */
        vertex->detail[2] = parse_intensity_colour( ta_status.intensity1, data[6].f );
        break;
    case TA_VERTEX_TEX_UV16_SPEC_INTENSITY:
        vertex->detail[2] = parse_intensity_colour( ta_status.intensity2, data[7].f );
        /* Fallthrough */
    case TA_VERTEX_TEX_UV16_INTENSITY:
        vertex->detail[0] = data[4].i; /* UV */
        vertex->detail[1] = parse_intensity_colour( ta_status.intensity1, data[6].f );
        break;

    case TA_VERTEX_PACKED_MOD:
        vertex->detail[0] = data[4].i; /* ARGB */
        vertex->detail[1] = data[5].i; /* ARGB */
        break;
    case TA_VERTEX_INTENSITY_MOD:
        vertex->detail[0] = parse_intensity_colour( ta_status.intensity1, data[4].f );
        vertex->detail[1] = parse_intensity_colour( ta_status.intensity2, data[5].f );
        break;

    case TA_VERTEX_TEX_SPEC_PACKED_MOD:
        vertex->detail[3] = data[7].i; /* ARGB0 */
        /* Fallthrough */
    case TA_VERTEX_TEX_PACKED_MOD:
        vertex->detail[0] = data[4].i; /* U0 */
        vertex->detail[1] = data[5].i; /* V0 */
        vertex->detail[2] = data[6].i; /* ARGB0 */
        ta_status.state = STATE_EXPECT_VERTEX_BLOCK2;
        break;
    case TA_VERTEX_TEX_UV16_SPEC_PACKED_MOD:
        vertex->detail[2] = data[7].i; /* ARGB0 */
        /* Fallthrough */
    case TA_VERTEX_TEX_UV16_PACKED_MOD:
        vertex->detail[0] = data[4].i; /* UV0 */
        vertex->detail[1] = data[6].i; /* ARGB0 */
        ta_status.state = STATE_EXPECT_VERTEX_BLOCK2;
        break;

    case TA_VERTEX_TEX_SPEC_INTENSITY_MOD:
        vertex->detail[3] = parse_intensity_colour( ta_status.intensity1, data[7].f );
        /* Fallthrough */
    case TA_VERTEX_TEX_INTENSITY_MOD:
        vertex->detail[0] = data[4].i; /* U0 */
        vertex->detail[1] = data[5].i; /* V0 */
        vertex->detail[2] = parse_intensity_colour( ta_status.intensity1, data[6].f );
        ta_status.state = STATE_EXPECT_VERTEX_BLOCK2;
        break;
    case TA_VERTEX_TEX_UV16_SPEC_INTENSITY_MOD:
        vertex->detail[2] = parse_intensity_colour( ta_status.intensity1, data[7].f );
        /* Fallthrough */
    case TA_VERTEX_TEX_UV16_INTENSITY_MOD:
        vertex->detail[0] = data[4].i; /* UV0 */
        vertex->detail[1] = parse_intensity_colour( ta_status.intensity1, data[6].f );
        ta_status.state = STATE_EXPECT_VERTEX_BLOCK2;
        break;

    case TA_VERTEX_SPRITE:
    case TA_VERTEX_TEX_SPRITE:
    case TA_VERTEX_MOD_VOLUME:
    case TA_VERTEX_LISTLESS:
        vertex++;
        vertex->x = data[4].f;
        vertex->y = data[5].f;
        vertex->z = data[6].f;
        vertex++;
        vertex->x = data[7].f;
        ta_status.vertex_count += 2;
        if (ta_status.current_vertex_type  == TA_VERTEX_SPRITE || ta_status.current_vertex_type == TA_VERTEX_TEX_SPRITE) {
            ta_status.state = STATE_EXPECT_END_VERTEX_BLOCK2;
        } else {
            ta_status.state = STATE_EXPECT_VERTEX_BLOCK2;
        }
        break;
    }
    ta_status.vertex_count++;
}

static void ta_parse_vertex_block2( union ta_data *data ) {
    struct pvr2_ta_vertex *vertex = &ta_status.poly_vertex[ta_status.vertex_count-1];

    switch( ta_status.current_vertex_type ) {
    case TA_VERTEX_TEX_SPEC_FLOAT:
        vertex->detail[3] = parse_float_colour( data[4].f, data[5].f, data[6].f, data[7].f );
        /* Fallthrough */
    case TA_VERTEX_TEX_FLOAT:
        vertex->detail[2] = parse_float_colour( data[0].f, data[1].f, data[2].f, data[3].f );
        break;
    case TA_VERTEX_TEX_UV16_SPEC_FLOAT:
        vertex->detail[2] = parse_float_colour( data[4].f, data[5].f, data[6].f, data[7].f );
        /* Fallthrough */
    case TA_VERTEX_TEX_UV16_FLOAT:
        vertex->detail[1] = parse_float_colour( data[0].f, data[1].f, data[2].f, data[3].f );
        break;
    case TA_VERTEX_TEX_PACKED_MOD:
        vertex->detail[3] = data[0].i; /* U1 */
        vertex->detail[4] = data[1].i; /* V1 */
        vertex->detail[5] = data[2].i; /* ARGB1 */
        break;
    case TA_VERTEX_TEX_SPEC_PACKED_MOD:
        vertex->detail[4] = data[0].i; /* U1 */
        vertex->detail[5] = data[1].i; /* V1 */
        vertex->detail[6] = data[2].i; /* ARGB1 */
        vertex->detail[7] = data[3].i; /* ARGB1 */
        break;
    case TA_VERTEX_TEX_UV16_PACKED_MOD:
        vertex->detail[2] = data[0].i; /* UV1 */
        vertex->detail[3] = data[2].i; /* ARGB1 */
        break;
    case TA_VERTEX_TEX_UV16_SPEC_PACKED_MOD:
        vertex->detail[3] = data[0].i; /* UV1 */
        vertex->detail[4] = data[2].i; /* ARGB1 */
        vertex->detail[5] = data[3].i; /* ARGB1 */
        break;

    case TA_VERTEX_TEX_INTENSITY_MOD:
        vertex->detail[3] = data[0].i; /* U1 */
        vertex->detail[4] = data[1].i; /* V1 */
        vertex->detail[5] = parse_intensity_colour( ta_status.intensity2, data[2].f ); /* ARGB1 */
        break;
    case TA_VERTEX_TEX_SPEC_INTENSITY_MOD:
        vertex->detail[4] = data[0].i; /* U1 */
        vertex->detail[5] = data[1].i; /* V1 */
        vertex->detail[6] = parse_intensity_colour( ta_status.intensity2, data[2].f ); /* ARGB1 */
        vertex->detail[7] = parse_intensity_colour( ta_status.intensity2, data[3].f ); /* ARGB1 */
        break;
    case TA_VERTEX_TEX_UV16_INTENSITY_MOD:
        vertex->detail[2] = data[0].i; /* UV1 */
        vertex->detail[3] = parse_intensity_colour( ta_status.intensity2, data[2].f ); /* ARGB1 */
        break;
    case TA_VERTEX_TEX_UV16_SPEC_INTENSITY_MOD:
        vertex->detail[3] = data[0].i; /* UV1 */
        vertex->detail[4] = parse_intensity_colour( ta_status.intensity2, data[2].f ); /* ARGB1 */
        vertex->detail[5] = parse_intensity_colour( ta_status.intensity2, data[3].f ); /* ARGB1 */
        break;

    case TA_VERTEX_SPRITE:
        vertex->y = data[0].f;
        vertex->z = data[1].f;
        vertex++;
        ta_status.vertex_count++;
        vertex->x = data[2].f;
        vertex->y = data[3].f;
        vertex->z = 0;
        vertex->detail[0] = 0;
        ta_status.poly_vertex[0].detail[0] = 0;
        ta_status.poly_vertex[1].detail[0] = 0;
        break;
    case TA_VERTEX_TEX_SPRITE:
        vertex->y = data[0].f;
        vertex->z = data[1].f;
        vertex++;
        ta_status.vertex_count++;
        vertex->x = data[2].f;
        vertex->y = data[3].f;
        vertex->z = 0;
        vertex->detail[0] = 0;
        vertex->detail[1] = 0;
        ta_status.poly_vertex[0].detail[0] = data[5].i;
        ta_status.poly_vertex[0].detail[1] = 0;
        ta_status.poly_vertex[1].detail[0] = data[6].i;
        ta_status.poly_vertex[1].detail[1] = 0;
        ta_status.poly_vertex[2].detail[0] = data[7].i;
        /* offset-colour slot: flat like the base colour - only v2 carries it (set from
         * the context), the rest are zero. Harmless when poly_vertex_size is 2, since
         * only 3+poly_vertex_size words per vertex are emitted. */
        ta_status.poly_vertex[0].detail[2] = 0;
        ta_status.poly_vertex[1].detail[2] = 0;
        vertex->detail[2] = 0;
        break;
    case TA_VERTEX_MOD_VOLUME:
    case TA_VERTEX_LISTLESS:
        vertex->y = data[0].f;
        vertex->z = data[1].f;
        break;
    }
    ta_status.state = STATE_IN_POLYGON;
}

/**
 * Process 1 32-byte block of ta data
 */
static void pvr2_ta_process_block( unsigned char *input ) {
    union ta_data *data = (union ta_data *)input;

    #ifdef MTTA_DECOUPLED
    u64 ta_ring_magic =  *(u64*)input;
    if (ta_ring_magic == TA_RING_DECOUPLED_MAGIC) {
        switch(data[2].i) {
            case TA_RING_DECOUPLED_OP_REGWRITE:
                (u32&)pvr_regs[data[3].i] = data[4].i;
                break;
            case TA_RING_DECOUPLED_OP_LISTINIT:
                lxd_ta_init(pvr2_main_ram_hidden);
                break;
            case TA_RING_DECOUPLED_OP_SOFTRESET:
                lxd_ta_reset();
                break;
            case TA_RING_DECOUPLED_OP_STARTPOLLY:
                void startpolly();
                startpolly();
                break;

            default:
                die("Bad ta_ring packet");
        }
        return;
    }
    #endif

    switch( ta_status.state ) {
    case STATE_ERROR:
        /* Fatal error raised - stop processing until reset */
        return;

    case STATE_EXPECT_POLY_BLOCK2:
        /* This is always a pair of floating-point colours */
        ta_status.intensity1 = 
            parse_float_colour( data[0].f, data[1].f, data[2].f, data[3].f );
        ta_status.intensity2 =
            parse_float_colour( data[4].f, data[5].f, data[6].f, data[7].f );
        ta_status.state = STATE_IN_LIST;
        break;

    case STATE_EXPECT_VERTEX_BLOCK2:
        ta_parse_vertex_block2( data );
        if( ta_status.vertex_count == ta_status.max_vertex ) {
            ta_split_polygon();
        }
        break;

    case STATE_EXPECT_END_VERTEX_BLOCK2:
        ta_parse_vertex_block2( data );
        if( ta_status.vertex_count < 3 ) {
            ta_bad_input_error();
        } else {
            ta_commit_polygon();
        }
        ta_status.vertex_count = 0;
        ta_status.poly_parity = 0;
        ta_status.state = STATE_IN_LIST;
        break;
    case STATE_IN_LIST:
    case STATE_IN_POLYGON:
    case STATE_IDLE:
        switch( TA_CMD( data->i ) ) {
        case TA_CMD_END_LIST:
            if( ta_status.state == STATE_IN_POLYGON ) {
                ta_bad_input_error();
                ta_end_list();
                ta_status.state = STATE_ERROR; /* Abort further processing */
            } else {
                ta_end_list();
            }
            break;
        case TA_CMD_CLIP:
            if( ta_status.state == STATE_IN_POLYGON ) {
                ta_bad_input_error();
                ta_status.accept_vertexes = FALSE;
                /* Enter stuffed up mode */
            }
            ta_status.clip.x1 = data[4].i & 0x3F;
            ta_status.clip.y1 = data[5].i & 0x0F;
            ta_status.clip.x2 = data[6].i & 0x3F;
            ta_status.clip.y2 = data[7].i & 0x0F;
            if( ta_status.clip.x2 >= ta_status.width )
                ta_status.clip.x2 = ta_status.width - 1;
            if( ta_status.clip.y2 >= ta_status.height )
                ta_status.clip.y2 = ta_status.height - 1;
            break;
        case TA_CMD_POLYGON_CONTEXT:
            if( ta_status.state == STATE_IDLE ) {
                ta_init_list( TA_POLYCMD_LISTTYPE( data->i ) );
            }

            if( ta_status.vertex_count != 0 ) {
                /* Error, and not a very well handled one either */
                ta_bad_input_error();
                ta_status.accept_vertexes = FALSE;
            } else {
                if( TA_IS_MODIFIER_LIST( ta_status.current_list_type ) ) {
                    ta_parse_modifier_context(data);
                } else {
                    ta_parse_polygon_context(data);
                }
            }
            break;
        case TA_CMD_SPRITE_CONTEXT:
            if( ta_status.state == STATE_IDLE ) {
                ta_init_list( TA_POLYCMD_LISTTYPE( data->i ) );
            }

            if( ta_status.vertex_count != 0 ) {
                ta_fill_vertexes();
                ta_commit_polygon();
            }

            ta_parse_sprite_context(data);
            break;
        case TA_CMD_VERTEX:
            ta_status.state = STATE_IN_POLYGON;
            ta_parse_vertex(data);

            if( ta_status.state == STATE_EXPECT_END_VERTEX_BLOCK2 ) {
                // do nothing here - ta_parse_vertex already set the end state (sprites)
            } else if( ta_status.state == STATE_EXPECT_VERTEX_BLOCK2 ) {
                if( TA_IS_END_VERTEX(data[0].i) ) {
                    ta_status.state = STATE_EXPECT_END_VERTEX_BLOCK2;
                }
            } else if( TA_IS_END_VERTEX(data->i) ) {
                if( ta_status.vertex_count < 3 ) {
                    ta_bad_input_error();
                } else {
                    ta_commit_polygon();
                }
                ta_status.vertex_count = 0;
                ta_status.poly_parity = 0;
                ta_status.state = STATE_IN_LIST;
            } else if( ta_status.vertex_count == ta_status.max_vertex ) {
                ta_split_polygon();
            }
            break;
        default:
            if( ta_status.state == STATE_IN_POLYGON ) {
                ta_bad_input_error();
            }
        }
        break;
    }

}

/**
 * Find the first polygon or sprite context in the supplied buffer of TA
 * data.
 * @return A pointer to the context, or NULL if it cannot be found 
 */
static uint32_t *pvr2_ta_find_polygon_context( uint32_t *buf, uint32_t length )
{
    uint32_t *poly;
    for( poly = buf; poly < buf+(length>>2); poly += 8 ) {
        if( TA_CMD(*poly) == TA_CMD_POLYGON_CONTEXT ||
            TA_CMD(*poly) == TA_CMD_SPRITE_CONTEXT ) {
            return poly;
        }
    }
    return NULL;
}

/**
 * Write a block of data to the tile accelerator, adding the data to the 
 * current scene. We don't make any particular attempt to interpret the data
 * at this stage, deferring that until render time.
 *
 * Currently copies the data verbatim to the vertex buffer, processing only
 * far enough to generate the correct end-of-list events. Tile buffer is
 * entirely ignored.
 */
void lxd_ta_write( unsigned char *buf, uint32_t length )
{
    for( ; length >=32; length -= 32 ) {
        pvr2_ta_process_block( buf );
        buf += 32;
    }
}

void lxd_ta_write_burst( sh4addr_t addr, unsigned char *data )
{
    pvr2_ta_process_block( data );
}

/*
    Multithreaded TA consumer.

    Runs on its own thread/core, draining the ta_ring that the "ta fsm"
    producer (ta.cpp) fills. Cortex-A9 SMP ordering:
      Consumer: read the published write index, then DMB ISH, then read the data.

    Like the producer, the barrier is amortized: when the consumer notices new
    published data it takes one dmb ish, processes the whole available batch with
    plain loads, then publishes read_pub once. It also caches write_pub in
    write_cache so an empty ring is detected without touching the producer's line.
*/
static void* ta_ring_consumer_thread(void* /*param*/) {
    // cpu0 takes the system's IRQ load; the spin loop tolerates it,
    // the sh4/jit thread (pinned to cpu1) does not
    ta_ring_pin_thread(0);

    extern void enable_runfast();
    enable_runfast();

    for (;;) {
        // Fast empty check against our private cache first.
        u32 rd = ta_ring.read_pub;
        if (rd == ta_ring.write_cache) {
            // Refresh from the shared line; still empty -> park until the
            // producer sevs a publish (or any interrupt wakes us).
            ta_ring.write_cache = ta_ring.write_pub;
            if (rd == ta_ring.write_cache) {
                TA_RING_WFE();
                continue;
            }

            // New data was published: observe it after the index that announced it.
            TA_RING_DMB();
        }

        // Drain everything currently published in one go (plain loads).
        u32 wr = ta_ring.write_cache;
        while (rd != wr) {
            pvr2_ta_process_block(&ta_ring.data[(rd & TA_RING_MASK) * TA_RING_BLOCK]);
            rd++;
        }

        // Retire the batch. One store, ordered after the reads above; the sev
        // wakes a producer parked on a full ring or in ta_ring_drain().
        TA_RING_DMB();
        ta_ring.read_pub = rd;
        TA_RING_SEV();
    }

    return NULL;
}

void ta_ring_consumer_start(u8* vram) {
    pvr2_main_ram_hidden = vram;
    static pthread_t consumer;
    ta_ring.write_pub = 0;
    ta_ring.write_priv = 0;
    ta_ring.read_cache = 0;
    ta_ring.read_pub = 0;
    ta_ring.write_cache = 0;
    pthread_create(&consumer, NULL, ta_ring_consumer_thread, NULL);
}
