// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_SH4_MMU_H
#define EMU_SH4_MMU_H

#include "../core/types.h"
#include "sh4.h"

#define MMU_READ 0
#define MMU_WRITE 1
#define MMU_FETCH 2

extern int g_mmu_on;
extern int g_mmu_sq_only_ok;
extern int g_mmu_sq_only;

void sh4_mmu_reset(void);
void sh4_mmu_write_mmucr(u32 value);
void sh4_mmu_write_pteh(u32 value);
void sh4_mmu_ldtlb(void);
u32 sh4_mmu_array_read(u32 addr);
void sh4_mmu_array_write(u32 addr, u32 value);

u32 sh4_mmu_translate(u32 va, int access);
u32 sh4_mmu_translate_sq(u32 va);
int sh4_mmu_translate_quiet(u32 va, int access, u32 *pa);
int sh4_mmu_translate_sq_quiet(u32 va, u32 *pa);
u32 sh4_mmu_peek(u32 va, int *ok);
void sh4_mmu_code_changed(u32 off);
int sh4_mmu_fetch_refill(u32 va);

#define MMU_TC_READ 1u
#define MMU_TC_WRITE 2u
#define MMU_TC_RAM 4u
#define MMU_TC_ENTRY 32
#define MMU_TC_OFF_TAG_READ 16
#define MMU_TC_OFF_TAG_WRITE 20
#define MMU_TC_OFF_RAM_DELTA 24
const void *sh4_mmu_tc(void);

static __inline u32 sh4_va(u32 va, int access)
{
    if (g_mmu_on && (va < 0x80000000u || (va >> 29) == 6 || !(g_sh4.sr & SR_MD))) return sh4_mmu_translate(va, access);
    return va;
}

struct StateIO;
void sh4_mmu_state(StateIO *io);

extern int g_sh4_wince_tlb;

extern u32 *g_mmu_lut[2];
void sh4_mmu_lut_init(void);

#endif
