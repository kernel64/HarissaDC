// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_HOLLY_H
#define EMU_HOLLY_H

#include "../core/types.h"

enum HollyNormalInt
{
    HOLLY_RENDER_DONE_VIDEO = 0,
    HOLLY_RENDER_DONE_ISP = 1,
    HOLLY_RENDER_DONE_TSP = 2,
    HOLLY_VBLANK_IN = 3,
    HOLLY_VBLANK_OUT = 4,
    HOLLY_HBLANK = 5,
    HOLLY_YUV_DONE = 6,
    HOLLY_OPAQUE_END = 7,
    HOLLY_OPAQUE_MOD_END = 8,
    HOLLY_TRANS_END = 9,
    HOLLY_TRANS_MOD_END = 10,
    HOLLY_PVR_DMA_END = 11,
    HOLLY_MAPLE_DMA_END = 12,
    HOLLY_MAPLE_VBLANK_OVER = 13,
    HOLLY_GDROM_DMA_END = 14,
    HOLLY_AICA_DMA_END = 15,
    HOLLY_EXT1_DMA_END = 16,
    HOLLY_EXT2_DMA_END = 17,
    HOLLY_DEV_DMA_END = 18,
    HOLLY_CH2_DMA_END = 19,
    HOLLY_SORT_DMA_END = 20,
    HOLLY_PUNCH_END = 21
};

enum HollyExtInt
{
    HOLLY_EXT_GDROM = 0,
    HOLLY_EXT_AICA = 1,
    HOLLY_EXT_MODEM = 2,
    HOLLY_EXT_EXPANSION = 3
};

void holly_init(void);
void holly_reset(void);
void holly_raise(int normalBit);
void holly_ext(int extBit, int level);

u32 holly_read(u32 addr, int size);
void holly_write(u32 addr, u32 value, int size);

u32 holly_reg(u32 addr);
void holly_set_reg(u32 addr, u32 value);
void holly_late_init(void);

#define SB_C2DSTAT 0x005F6800
#define SB_C2DLEN  0x005F6804
#define SB_C2DST   0x005F6808
#define SB_SDSTAW  0x005F6810
#define SB_SDBAAW  0x005F6814
#define SB_SDWLT   0x005F6818
#define SB_SDLAS   0x005F681C
#define SB_SDST    0x005F6820
#define SB_SDDIV   0x005F6860
#define SB_LMMODE0 0x005F6884
#define SB_LMMODE1 0x005F6888
#define SB_ISTNRM  0x005F6900
#define SB_ISTEXT  0x005F6904
#define SB_ISTERR  0x005F6908
#define SB_MDSTAR  0x005F6C04
#define SB_MDTSEL  0x005F6C10
#define SB_MDEN    0x005F6C14
#define SB_MDST    0x005F6C18
#define SB_MSYS    0x005F6C80
#define SB_GDSTAR  0x005F7404
#define SB_GDLEN   0x005F7408
#define SB_GDDIR   0x005F740C
#define SB_GDEN    0x005F7414
#define SB_GDST    0x005F7418
#define SB_GDSTARD 0x005F74F4
#define SB_GDLEND  0x005F74F8
#define SB_ADSTAG  0x005F7800
#define SB_ADSTAR  0x005F7804
#define SB_ADLEN   0x005F7808
#define SB_ADDIR   0x005F780C
#define SB_ADEN    0x005F7814
#define SB_ADST    0x005F7818
#define SB_PDSTAP  0x005F7C00
#define SB_PDSTAR  0x005F7C04
#define SB_PDLEN   0x005F7C08
#define SB_PDDIR   0x005F7C0C
#define SB_PDEN    0x005F7C14
#define SB_PDST    0x005F7C18

struct StateIO;
void holly_state(StateIO *io);

#endif
