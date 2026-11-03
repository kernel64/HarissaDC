// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "prof.h"

static u64 (*s_now)(void);
static u64 s_ticksPerSecond = 1;
static u64 s_total[PROF_COUNT];
static volatile u64 s_other[PROF_COUNT];
static int s_stack[8];
static u64 s_start[8];
static int s_depth;
volatile int g_prof_section = -1;

void prof_set_clock(u64 (*now)(void), u64 ticksPerSecond)
{
    s_now = now;
    s_ticksPerSecond = ticksPerSecond ? ticksPerSecond : 1;
}

void prof_begin(int section)
{
    u64 t;
    if (!s_now || s_depth >= 8) return;
    t = s_now();
    if (s_depth > 0) s_total[s_stack[s_depth - 1]] += t - s_start[s_depth - 1];
    s_stack[s_depth] = section;
    s_start[s_depth] = t;
    s_depth++;
    g_prof_section = section;
}

void prof_end(int section)
{
    u64 t;
    if (!s_now || s_depth == 0) return;
    t = s_now();
    s_depth--;
    s_total[s_stack[s_depth]] += t - s_start[s_depth];
    (void)section;
    if (s_depth > 0) s_start[s_depth - 1] = t;
    g_prof_section = s_depth > 0 ? s_stack[s_depth - 1] : -1;
}

void prof_report(int frames, f64 msPerFrame[PROF_COUNT])
{
    int i;
    for (i = 0; i < PROF_COUNT; i++)
        msPerFrame[i] = frames > 0 ? (f64)(s_total[i] + s_other[i]) * 1000.0 / (f64)s_ticksPerSecond / frames : 0;
}

void prof_reset(void)
{
    int i;
    for (i = 0; i < PROF_COUNT; i++) s_total[i] = s_other[i] = 0;
}

u64 prof_ticks(void)
{
    return s_now ? s_now() : 0;
}

u64 prof_tick_rate(void)
{
    return s_ticksPerSecond;
}

void prof_add(int section, u64 ticks)
{
    if (section >= 0 && section < PROF_COUNT) s_other[section] += ticks;
}

const char *prof_name(int section)
{
    static const char *const names[PROF_COUNT] = { "sh4", "arm7", "aica", "render", "present", "sound wait", "render wait", "ta" };
    return section >= 0 && section < PROF_COUNT ? names[section] : "?";
}
