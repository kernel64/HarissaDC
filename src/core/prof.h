// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_PROF_H
#define EMU_PROF_H

#include "types.h"

enum ProfSection
{
    PROF_SH4,
    PROF_ARM7,
    PROF_AICA,
    PROF_RENDER,
    PROF_PRESENT,
    PROF_SOUND_WAIT,
    PROF_RENDER_WAIT,
    PROF_TA,
    PROF_COUNT
};

void prof_set_clock(u64 (*now)(void), u64 ticksPerSecond);
void prof_begin(int section);
void prof_end(int section);
void prof_report(int frames, f64 msPerFrame[PROF_COUNT]);
void prof_reset(void);
u64 prof_ticks(void);
u64 prof_tick_rate(void);
void prof_add(int section, u64 ticks);
const char *prof_name(int section);
extern volatile int g_prof_section;

#endif
