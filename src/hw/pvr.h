// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_PVR_H
#define EMU_PVR_H

#include "../core/types.h"

void pvr_init(void);
void pvr_reset(void);

u32 pvr_read(u32 addr, int size);
void pvr_write(u32 addr, u32 value, int size);

u32 pvr_area4_read(u32 addr, int size);
void pvr_area4_write(u32 addr, u32 value, int size);
void pvr_ta_fifo(const u32 *words, u32 n);
void pvr_ta_sq(const u32 *words);
void pvr_ta_dma(u32 dst, u32 src, u32 len);

u32 pvr_frame_count(void);
u32 pvr_yuv_blocks(void);

int pvr_read_framebuffer(u32 *out, int *w, int *h);

struct TaStats
{
    u32 params, vertices, polygons, sprites, modvols, listEnds, renders;
};
void pvr_ta_stats(TaStats *out);

const u32 *pvr_ta_data(u32 *words);

static __inline u32 vram_offset64(u32 off32)
{
    return ((off32 & 0x003FFFFCu) << 1) | ((off32 & 0x00400000u) >> 20) | (off32 & 3u);
}

u32 pvr_reg(u32 addr);
void pvr_light_gun(u32 x, u32 y);

typedef void (*PvrRenderFn)(void);
extern PvrRenderFn g_pvr_render;
extern void (*g_pvr_render_wait)(void);

#define PVR_REG_WORDS 2048
void pvr_regs_copy(u32 *out);
void pvr_debug_set(const u32 *regs, const u32 *ta, u32 words);

#define SCENE_DUMP_MAGIC 0x53434448u
extern u32 g_pvr_palette_gen;
extern int g_pvr_ta_skip;

struct StateIO;
void pvr_state(StateIO *io);

#endif
