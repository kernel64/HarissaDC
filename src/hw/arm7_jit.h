// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_ARM7_JIT_H
#define EMU_ARM7_JIT_H

#include "../core/types.h"

int arm7_jit_init(void *execMem, u32 size, void (*flushIcache)(void *addr, u32 len));
void arm7_jit_enable(int on);
int arm7_jit_active(void);

const void *arm7_jit_block(u32 pc);
void arm7_jit_enter(const void *code);
void arm7_jit_flush(void);
void arm7_jit_code_written(u32 off);
void arm7_jit_sh4_wrote(u32 off);

extern u8 *g_arm7_jit_code;

struct Arm7JitStats
{
    u64 blocksRun, blocksCompiled, flushes, fallbacks;
    u32 codeBytes;
};
void arm7_jit_stats(Arm7JitStats *out);

u32 arm7_jit_read32(u32 addr);
u32 arm7_jit_read16(u32 addr);
u32 arm7_jit_read8(u32 addr);
void arm7_jit_write32(u32 addr, u32 v, u32 sp);
void arm7_jit_write16(u32 addr, u32 v, u32 sp);
void arm7_jit_write8(u32 addr, u32 v, u32 sp);
void arm7_interp_op(u32 op);

#endif
