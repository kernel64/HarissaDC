// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_SH4_JIT_H
#define EMU_SH4_JIT_H

#include "../core/types.h"

#if defined(_XBOX)
#define SH4_JIT_ABI_XBOX 1
#define SH4_JIT_CAN_RUN 1
#elif defined(__powerpc64__) && defined(__BIG_ENDIAN__) && (!defined(_CALL_ELF) || _CALL_ELF == 1)
#define SH4_JIT_ABI_ELFV1 1
#define SH4_JIT_CAN_RUN 1
#else
#define SH4_JIT_CAN_RUN 0
#endif

#define SH4_JIT_MAX_INSNS 64
#define SH4_JIT_MAX_INSNS2 128

struct Sh4JitStats
{
    u64 blocksRun, blocksCompiled, flushes, invalidations, interpSteps, idleSkips, links;
    u64 busyHeads;
    u64 tier2Blocks, tier2Loops;
    u64 slices;
    u64 codePartWrites;
    u32 codeBytes;
};

int sh4_jit_init(void *execMem, u32 size, void (*flushIcache)(void *addr, u32 len));
void sh4_jit_enable(int on);
int sh4_jit_active(void);
void sh4_jit_flush(void);
void sh4_jit_run(s32 cycles);
void sh4_jit_stats(Sh4JitStats *out);
void sh4_jit_stop_chain(void);
s32 sh4_jit_cycles_left(void);
u32 sh4_jit_block_code(u32 pc, const u32 **code);
int sh4_jit_ram_has_code(u32 off);
void sh4_jit_block_map(void (*fn)(void *ctx, const u32 *code, u32 words, u32 pc, u32 off, u32 mode), void *ctx);
extern u32 *g_sh4_jit_fallbacks;

void sh4_jit_set_reference(int on);
extern void (*g_sh4_jit_block_hook)(u32 pc);
extern void (*g_sh4_jit_dispatch_hook)(u32 pc);
extern int g_sh4_jit_mark_blocks;
enum { SH4_PHASE_CODE, SH4_PHASE_DISPATCH, SH4_PHASE_COMPILE, SH4_PHASE_MMU_SLOW, SH4_PHASE_MMU_OP, SH4_PHASE_HANDLER,
       SH4_PHASE_COUNT };
extern volatile int g_sh4_jit_phase;
extern u32 *g_sh4_jit_mmu_op_counts;
extern int g_sh4_jit_direct_indirect;
extern int g_sh4_jit_side_exits;
extern int g_sh4_jit_tier2;
extern int g_sh4_jit_tier2_log;
extern int g_sh4_jit_fast_fpu;
extern int g_sh4_jit_split;
extern int g_sh4_jit_skips;
extern int g_sh4_jit_fastmem;
extern int g_sh4_jit_idle_skip;
extern int g_sh4_jit_countdown_skip;

void sh4_jit_ram_written(u32 off);

#endif
