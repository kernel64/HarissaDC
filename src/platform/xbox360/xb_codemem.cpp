// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_codemem.h"

#include <xtl.h>
#include <ppcintrinsics.h>

extern "C" VOID NTAPI KeSweepIcacheRange(PVOID Address, SIZE_T Size);

#pragma section(".jitc", read, write, execute)
__declspec(allocate(".jitc")) static u32 s_code[XB_CODEMEM_BYTES / 4] = { 1 };

u32 *xb_codemem_base(void)
{
    return s_code;
}

void xb_codemem_flush(void *p, u32 size)
{
    u32 a = (u32)p & ~127u, end = (u32)p + size;
    for (; a < end; a += 128)
        __dcbst(0, (void *)a);
    __sync();
    KeSweepIcacheRange(p, size);
    __emit(0x4C00012C);
}
