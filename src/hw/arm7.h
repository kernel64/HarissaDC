// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_ARM7_H
#define EMU_ARM7_H

#include "../core/types.h"

#define ARM7_CLOCK_HZ 22579200u

struct Arm7Ctx
{
    u32 r[16];
    u32 cpsr;
    u32 spsr;
    u32 usr[7], fiq[7], irq[2], svc[2], abt[2], und[2];
    u32 spsrFiq, spsrIrq, spsrSvc, spsrAbt, spsrUnd;
    int enabled;
    int fiqLine;
    s32 cycles;
    u32 jitWrote;
    u32 jitBack;
    u32 jitTemp[2];
};

extern Arm7Ctx g_arm7;

void arm7_reset(void);
void arm7_set_enabled(int on);
void arm7_set_fiq(int asserted);
void arm7_run(s32 cycles);
u32 arm7_idle_skips(void);
extern void (*g_arm7_step_hook)(u32 pc);
void arm7_step(void);

struct StateIO;
void arm7_state(StateIO *io);

#endif
