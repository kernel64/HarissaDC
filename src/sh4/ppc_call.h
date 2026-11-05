// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_PPC_CALL_H
#define EMU_PPC_CALL_H

#include <stdint.h>

#include "ppc_emit.h"
#include "sh4_jit.h"

static inline u64 ppc_addr_of(const void *p) { return (u64)(uintptr_t)p; }

static inline void ppc_load_ptr_const(PpcCode *c, u32 rt, u64 v)
{
#if defined(SH4_JIT_ABI_XBOX)
    ppc_li32u(c, rt, (u32)v);
#else
    ppc_li64(c, rt, v);
#endif
}

static inline void ppc_load_ptr(PpcCode *c, u32 rt, s32 off, u32 ra)
{
#if defined(SH4_JIT_ABI_XBOX)
    ppc_lwz(c, rt, off, ra);
#else
    ppc_ld(c, rt, off, ra);
#endif
}

static inline void ppc_store_ptr(PpcCode *c, u32 rs, s32 off, u32 ra)
{
#if defined(SH4_JIT_ABI_XBOX)
    ppc_stw(c, rs, off, ra);
#else
    ppc_std(c, rs, off, ra);
#endif
}

static inline void ppc_li32(PpcCode *c, u32 rt, u32 v)
{
    if ((s32)v >= -32768 && (s32)v <= 32767) ppc_li(c, rt, (s32)v);
    else
    {
        ppc_lis(c, rt, (s32)(s16)(v >> 16));
        if (v & 0xFFFF) ppc_ori(c, rt, rt, v & 0xFFFF);
    }
}

static inline void ppc_call(PpcCode *c, const void *fn)
{
#if defined(SH4_JIT_ABI_ELFV1)
    ppc_li64(c, 12, ppc_addr_of(fn));
    ppc_ld(c, 0, 0, 12);
    ppc_ld(c, 2, 8, 12);
    ppc_mtctr(c, 0);
#else
    ppc_load_ptr_const(c, 12, ppc_addr_of(fn));
    ppc_mtctr(c, 12);
#endif
    ppc_bctrl(c);
}

#endif
