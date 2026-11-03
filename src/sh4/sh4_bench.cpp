// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "sh4_bench.h"

#include "sh4.h"
#include "sh4_jit.h"
#include "../hw/dreamcast.h"
#include "../mem/mem.h"

u32 sh4_bench_run(u32 iterations)
{
    static const u16 code[] = { 0x6212, 0x7201, 0x2122, 0x4310, 0x8BFA, 0xAFFE, 0x0009 };
    const u32 base = 0x8C010000u;
    u32 i, executed = 0;
    for (i = 0; i < EMU_ARRAY_COUNT(code); i++) mem_write16(base + i * 2, code[i]);
    mem_write32(0x8C020000u, 0);
    g_sh4.pc = base;
    g_sh4.r[1] = 0x8C020000u;
    g_sh4.r[3] = iterations;
    while (g_sh4.pc < base + 10)
    {
        if (sh4_jit_active()) sh4_jit_run(1000);
        else sh4_run(1000);
        executed += 1000 - (u32)(g_sh4.cycles > 0 ? g_sh4.cycles : 0);
    }
    return mem_read32(0x8C020000u) == iterations ? executed : 0;
}
