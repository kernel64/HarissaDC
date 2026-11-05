// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_SH4_MMR_H
#define EMU_SH4_MMR_H

#include "../core/types.h"

struct Sh4Ccn
{
    u32 pteh, ptel, ttb, tea, mmucr, ccr, tra, expevt, intevt, ptea, qacr0, qacr1;
    u8 basra, basrb;
};

struct Sh4Intc
{
    u16 icr, ipra, iprb, iprc, iprd;
};

struct Sh4Dmac
{
    u32 sar[4], dar[4], dmatcr[4], chcr[4], dmaor;
};

extern Sh4Ccn g_ccn;
extern Sh4Intc g_intc;
extern Sh4Dmac g_dmac;

#define CABLE_VGA 0
#define CABLE_RGB 2
#define CABLE_COMPOSITE 3
void sh4_mmr_set_cable(int cable);

void sh4_mmr_init(void);
void sh4_mmr_reset(void);
u32 sh4_p4_read(u32 addr, int size);
void sh4_p4_write(u32 addr, u32 value, int size);

u32 sh4_ocram_read(u32 addr, int size);
u8 *sh4_ocram_bytes(void);
int sh4_ocram_simple(void);
void sh4_ocram_write(u32 addr, u32 value, int size);

void sh4_sq_flush_to(u32 dst, u32 addr);

extern u32 g_sq[2][8];
extern int g_sh4_tmu_poll_skip;

struct StateIO;
void sh4_mmr_state(StateIO *io);

#endif
