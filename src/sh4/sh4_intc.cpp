// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "sh4.h"
#include "sh4_mmr.h"
#include "sh4_jit.h"

void sh4_take_interrupt(u32 intevt);

struct IrqInfo
{
    u32 intevt;
    int iprReg;
    int shift;
};

static const IrqInfo s_info[SH4_IRQ_COUNT] = {
    { 0x320, -1, 6 },
    { 0x360, -1, 4 },
    { 0x3A0, -1, 2 },
    { 0x400, 0, 12 },
    { 0x420, 0, 8 },
    { 0x440, 0, 4 },
    { 0x480, 0, 0 },
    { 0x4A0, 0, 0 },
    { 0x4C0, 0, 0 },
    { 0x640, 2, 8 },
    { 0x660, 2, 8 },
    { 0x680, 2, 8 },
    { 0x6A0, 2, 8 },
    { 0x6C0, 2, 8 },
    { 0x700, 2, 4 },
    { 0x720, 2, 4 },
    { 0x740, 2, 4 },
    { 0x760, 2, 4 },
};

static u32 s_pending;

static int level_of(int irq)
{
    const IrqInfo *i = &s_info[irq];
    u16 ipr;
    if (i->iprReg < 0) return i->shift;
    ipr = i->iprReg == 0 ? g_intc.ipra : i->iprReg == 1 ? g_intc.iprb : g_intc.iprc;
    return (ipr >> i->shift) & 15;
}

static int best_pending(int *levelOut)
{
    int irq, best = -1, bestLevel = 0;
    if (!s_pending) return -1;
    for (irq = 0; irq < SH4_IRQ_COUNT; irq++)
    {
        if (s_pending & (1u << irq))
        {
            int l = level_of(irq);
            if (l > bestLevel)
            {
                bestLevel = l;
                best = irq;
            }
        }
    }
    *levelOut = bestLevel;
    return best;
}

void sh4_irq_update(void)
{
    int level = 0, irq = best_pending(&level);
    u32 imask = (g_sh4.sr & SR_IMASK) >> 4;
    g_sh4_irq_ready = irq >= 0 && level > (int)imask && !(g_sh4.sr & SR_BL);
    if (g_sh4_irq_ready) sh4_jit_stop_chain();
}

void sh4_irq_set(int irq, int pending)
{
    u32 bit = 1u << irq;
    u32 old = s_pending;
    if (pending) s_pending |= bit;
    else s_pending &= ~bit;
    if (old != s_pending) sh4_irq_update();
}

int sh4_irq_check(void)
{
    int level = 0, irq;
    sh4_irq_update();
    if (!g_sh4_irq_ready) return 0;
    irq = best_pending(&level);
    sh4_take_interrupt(s_info[irq].intevt);
    g_sh4_irq_ready = 0;
    return 1;
}

void sh4_irq_reset(void)
{
    s_pending = 0;
    g_sh4_irq_ready = 0;
}
