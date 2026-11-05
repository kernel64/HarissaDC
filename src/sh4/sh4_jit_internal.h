// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_SH4_JIT_INTERNAL_H
#define EMU_SH4_JIT_INTERNAL_H

#include "sh4_jit.h"
#include "ppc_emit.h"

#define SH4_JIT_FP_ANY 0x00000001u
#define SH4_JIT_FP_BITS 0x001C0000u
static inline u32 sh4_jit_any_key(u32 mode) { return (mode & ~SH4_JIT_FP_BITS) | SH4_JIT_FP_ANY; }
static inline int sh4_jit_key_match(u32 wanted, u32 key) { return key == wanted || key == sh4_jit_any_key(wanted); }

struct Sh4JitGenInfo
{
    u32 fpMode;
    int pure;
    int link;
    const u32 *exit;
    int tier2;
    int dry;
    int busy;
    u32 initDirty;
    u32 initDirtyFr;
    u32 sqRegs;
    u32 ocRegs;
    int mirror;
    u32 coldPos, coldCap;
    u32 *nearRefill[2][2];
    struct Sh4JitGenOut *out;
    int mmu;
    u32 physStart;
    u32 linkMmu;
    int fpAny;
    int md;
    int libPoll;
};

extern int g_sh4_jit_exit_why;

void sh4_jit_lib_poll(u32 pc);

int sh4_jit_mmu_op(u32 op, u32 instPc);
extern int g_sh4_jit_mmu_abort;
u32 sh4_jit_mmu_read8(u32 addr, u32 instPc);
u32 sh4_jit_mmu_read16(u32 addr, u32 instPc);
u32 sh4_jit_mmu_read32(u32 addr, u32 instPc);
void sh4_jit_mmu_write8(u32 addr, u32 value, u32 instPc);
void sh4_jit_mmu_write16(u32 addr, u32 value, u32 instPc);
void sh4_jit_mmu_write32(u32 addr, u32 value, u32 instPc);
void sh4_jit_mmu_read_pair(u32 addr, u32 instPc);
int sh4_jit_is_fpu_op(u16 op);
void sh4_jit_fpu_disabled(u32 instPc, u32 inSlot);
void sh4_jit_mmu_write_pair(u32 addr, u32 instPc);
void sh4_jit_mmu_pref(u32 addr, u32 instPc);

struct Sh4JitGenOut
{
    u32 loops;
    u32 loopDirty, loopDirtyFr;
    u32 coldEnd;
};

u32 sh4_jit_gen_block(PpcCode *c, u32 pc, const u32 *ops, u32 n, int endsWithBranch, const Sh4JitGenInfo *info);

int sh4_jit_gen_trampoline(PpcCode *c, u32 **enter, u32 **exit);
void sh4_jit_gen_refill_copy(PpcCode *c, u32 *out[2][2]);

struct Sh4JitLookup
{
    const u32 *ramBlock;
    const u8 *blocks;
    u32 blockSize, offPc, offCode, offFpMode, offNextVersion;
    const u32 *jcache;
};
#define SH4_JIT_JCACHE 8192
extern u32 g_sh4_jit_jcache[SH4_JIT_JCACHE * 8];
const Sh4JitLookup *sh4_jit_lookup(void);

int sh4_jit_side_exit(u16 op);
int sh4_jit_op_pure(u16 op);

int sh4_jit_add_fast_site(const u32 *fault, const u32 *site, const u32 *stub);
int sh4_jit_fast_room(u32 n);

void sh4_jit_add_link(u32 *site, u32 *exit, u32 targetPc, u32 targetOff, u32 fpMode, int deferred);

void sh4_jit_note_literal(u32 addr, u32 bytes);
int sh4_jit_literal_written(u32 addr);
void sh4_jit_counted_op(u32 op);
int sh4_jit_has_delay_slot(u16 op);
u32 sh4_jit_skip_target(const u32 *ops, u32 i, u32 n);

#endif
