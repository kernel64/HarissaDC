// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_ARM7_JIT_INTERNAL_H
#define EMU_ARM7_JIT_INTERNAL_H

#include "arm7_jit.h"
#include "../sh4/ppc_emit.h"

struct Arm7JitGenInfo
{
    const u32 *exit;
    const u8 *codePages;
    u32 fallbacks;
};

u32 arm7_jit_gen_block(PpcCode *c, u32 pc, Arm7JitGenInfo *info);

int arm7_jit_gen_trampoline(PpcCode *c, u32 **enter, u32 **exit);

#endif
