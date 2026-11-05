// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "sh4.h"
#include "sh4_internal.h"
#include "sh4_mmr.h"
#include "sh4_jit.h"
#include "sh4_jit_internal.h"

#include <setjmp.h>
#include <stddef.h>
#include <string.h>

#include "sh4_mmu.h"

#include "../core/log.h"
#include "../mem/mem.h"
#include "../core/state.h"

Sh4Ctx g_sh4;
Sh4TraceFn g_sh4_trace;
int g_sh4_idle;

#define C g_sh4

#define s_breakSlice g_sh4_break

void sh4_reset(void)
{
    memset(&C, 0, sizeof(C));
    C.sr = SR_MD | SR_RB | SR_BL | SR_IMASK;
    C.t = 0;
    C.pc = 0xA0000000u;
    C.vbr = 0;
    C.fpscr = 0x00040001u;
    g_sh4_irq_ready = 0;
}

static __inline int bank_of(u32 sr)
{
    return (sr & SR_MD) && (sr & SR_RB);
}

u32 sh4_get_sr(void)
{
    return C.sr | C.t;
}

void sh4_set_sr(u32 sr)
{
    u32 newSr = sr & SR_MASK & ~SR_T;
    if (bank_of(newSr) != bank_of(C.sr))
    {
        int i;
        for (i = 0; i < 8; i++)
        {
            u32 t = C.r[i];
            C.r[i] = C.rbank[i];
            C.rbank[i] = t;
        }
    }
    C.sr = newSr;
    C.t = sr & SR_T;
}

void sh4_set_fpscr(u32 fpscr)
{
    fpscr &= FPSCR_MASK;
    if ((fpscr ^ C.fpscr) & FPSCR_FR) sh4_fpu_bank_swap();
    C.fpscr = fpscr;
}

static void enter_handler(u32 spc, u32 vectorOffset)
{
    C.spc = spc;
    C.ssr = sh4_get_sr();
    C.sgr = C.r[15];
    sh4_set_sr(sh4_get_sr() | SR_MD | SR_RB | SR_BL);
    C.pc = C.vbr + vectorOffset;
    C.sleeping = 0;
}

u32 g_sh4_inst_pc;
static jmp_buf s_abort;
static int s_abortArmed;

u32 g_sh4_exc_counts[64];
void (*g_sh4_exception_hook)(u32 expevt);

static void wince_debug_string(u32 va)
{
    char s[160];
    u32 i;
    for (i = 0; i + 1 < sizeof(s); i++)
    {
        int ok;
        u32 pa = sh4_mmu_peek(va + i * 2, &ok);
        u16 ch = ok ? mem_read16(pa) : 0;
        if (!ch) break;
        s[i] = ch >= 32 && ch < 127 ? (char)ch : (ch == 10 || ch == 13 ? ' ' : '?');
    }
    s[i] = 0;
    LOG_DEBUG("[wince] %s", s);
}

static void raise_exception(u32 expevt, u32 vectorOffset)
{
    g_sh4_exc_counts[(expevt >> 5) & 63]++;
    if (expevt == 0x0E0 && (g_ccn.tea == 0xFFFFFDD3u || g_ccn.tea == 0xFFFFD5EFu) && log_level() >= LOG_LEVEL_DEBUG)
        wince_debug_string(C.r[4]);
    LOG_DEBUG("[sh4] exception %03X at %08X, TEA %08X, pr %08X, asid %02X, r4 %08X r5 %08X r6 %08X r7 %08X sp %08X", expevt, g_sh4_inst_pc,
              g_ccn.tea, C.pr, g_ccn.pteh & 0xFF, C.r[4], C.r[5], C.r[6], C.r[7], C.r[15]);
    g_ccn.expevt = expevt;
    if (g_sh4_exception_hook) g_sh4_exception_hook(expevt);
    enter_handler(g_sh4_inst_pc, vectorOffset);
}

void sh4_abort_exception(u32 expevt, u32 vectorOffset)
{
    raise_exception(expevt, vectorOffset);
    if (s_abortArmed) longjmp(s_abort, 1);
    LOG_ERROR("[sh4] exception %03X outside the run loop (pc %08X)", expevt, g_sh4_inst_pc);
}

void sh4_fpu_disabled(void)
{
    sh4_abort_exception(g_sh4_inst_pc != C.pc - 2 ? 0x820 : 0x800, 0x100);
}

void sh4_jit_fpu_disabled(u32 instPc, u32 inSlot)
{
    g_sh4_inst_pc = instPc;
    raise_exception(inSlot ? 0x820 : 0x800, 0x100);
}

int g_sh4_jit_mmu_abort;
u32 g_sh4_jit_mmu_slow[6];

static u32 slow_kind(u32 phys, int write)
{
    return ((phys & 0x1C000000u) == 0x0C000000u ? 0 : 2) + (write ? 1 : 0);
}

#define MMU_READ_FN(name, type, fn) \
    static EMU_NOINLINE u32 name##_slow(u32 addr, u32 instPc) \
    { \
        u32 v; \
        g_sh4_jit_phase = SH4_PHASE_MMU_SLOW; \
        if (setjmp(s_abort)) \
        { \
            s_abortArmed = 0; \
            g_sh4_jit_mmu_abort = 1; \
            g_sh4_jit_phase = SH4_PHASE_CODE; \
            return 0; \
        } \
        s_abortArmed = 1; \
        g_sh4_inst_pc = instPc; \
        { u32 pa = sh4_va(addr, MMU_READ); g_sh4_jit_mmu_slow[slow_kind(pa, 0)]++; v = (u32)fn(pa); } \
        s_abortArmed = 0; \
        g_sh4_jit_phase = SH4_PHASE_CODE; \
        return v; \
    } \
    u32 name(u32 addr, u32 instPc) \
    { \
        u32 qa; \
        g_sh4_jit_mmu_abort = 0; \
        if (sh4_mmu_translate_quiet(addr, MMU_READ, &qa)) \
        { \
            g_sh4_inst_pc = instPc; \
            g_sh4_jit_mmu_slow[slow_kind(qa, 0)]++; \
            return (u32)fn(qa); \
        } \
        return name##_slow(addr, instPc); \
    }
#define MMU_WRITE_FN(name, type, fn) \
    static EMU_NOINLINE void name##_slow(u32 addr, u32 value, u32 instPc) \
    { \
        g_sh4_jit_phase = SH4_PHASE_MMU_SLOW; \
        if (setjmp(s_abort)) \
        { \
            s_abortArmed = 0; \
            g_sh4_jit_mmu_abort = 1; \
            g_sh4_jit_phase = SH4_PHASE_CODE; \
            return; \
        } \
        s_abortArmed = 1; \
        g_sh4_inst_pc = instPc; \
        { u32 pa = sh4_va(addr, MMU_WRITE); g_sh4_jit_mmu_slow[slow_kind(pa, 1)]++; fn(pa, (type)value); } \
        s_abortArmed = 0; \
        g_sh4_jit_phase = SH4_PHASE_CODE; \
    } \
    void name(u32 addr, u32 value, u32 instPc) \
    { \
        u32 qa; \
        g_sh4_jit_mmu_abort = 0; \
        if (sh4_mmu_translate_quiet(addr, MMU_WRITE, &qa)) \
        { \
            g_sh4_inst_pc = instPc; \
            g_sh4_jit_mmu_slow[slow_kind(qa, 1)]++; \
            fn(qa, (type)value); \
            return; \
        } \
        name##_slow(addr, value, instPc); \
    }
MMU_READ_FN(sh4_jit_mmu_read8, u8, mem_read8)
MMU_READ_FN(sh4_jit_mmu_read16, u16, mem_read16)
MMU_READ_FN(sh4_jit_mmu_read32, u32, mem_read32)
MMU_WRITE_FN(sh4_jit_mmu_write8, u8, mem_write8)
MMU_WRITE_FN(sh4_jit_mmu_write16, u16, mem_write16)
MMU_WRITE_FN(sh4_jit_mmu_write32, u32, mem_write32)

void sh4_jit_mmu_read_pair(u32 addr, u32 instPc)
{
    u32 *w = (u32 *)(void *)&C.jitTemp64;
    g_sh4_jit_mmu_abort = 0;
    g_sh4_jit_phase = SH4_PHASE_MMU_SLOW;
    if (setjmp(s_abort))
    {
        s_abortArmed = 0;
        g_sh4_jit_mmu_abort = 1;
        g_sh4_jit_phase = SH4_PHASE_CODE;
        return;
    }
    s_abortArmed = 1;
    g_sh4_inst_pc = instPc;
    g_sh4_jit_mmu_slow[4]++;
    w[0] = mem_read32(sh4_va(addr, MMU_READ));
    w[1] = mem_read32(sh4_va(addr + 4, MMU_READ));
    s_abortArmed = 0;
    g_sh4_jit_phase = SH4_PHASE_CODE;
}

void sh4_jit_mmu_write_pair(u32 addr, u32 instPc)
{
    const u32 *w = (const u32 *)(const void *)&C.jitTemp64;
    g_sh4_jit_mmu_abort = 0;
    g_sh4_jit_phase = SH4_PHASE_MMU_SLOW;
    if (setjmp(s_abort))
    {
        s_abortArmed = 0;
        g_sh4_jit_mmu_abort = 1;
        g_sh4_jit_phase = SH4_PHASE_CODE;
        return;
    }
    s_abortArmed = 1;
    g_sh4_inst_pc = instPc;
    {
        u32 a0 = sh4_va(addr, MMU_WRITE), a1 = sh4_va(addr + 4, MMU_WRITE);
        g_sh4_jit_mmu_slow[4]++;
        mem_write32(a0, w[0]);
        mem_write32(a1, w[1]);
    }
    s_abortArmed = 0;
    g_sh4_jit_phase = SH4_PHASE_CODE;
}

static EMU_NOINLINE void mmu_pref_slow(u32 addr, u32 instPc);

void sh4_jit_mmu_pref(u32 addr, u32 instPc)
{
    u32 pa;
    g_sh4_jit_mmu_abort = 0;
    if (sh4_mmu_translate_sq_quiet(addr, &pa))
    {
        g_sh4_jit_mmu_slow[5]++;
        sh4_sq_flush_to(pa, addr);
        return;
    }
    mmu_pref_slow(addr, instPc);
}

static EMU_NOINLINE void mmu_pref_slow(u32 addr, u32 instPc)
{
    g_sh4_jit_phase = SH4_PHASE_MMU_SLOW;
    if (setjmp(s_abort))
    {
        s_abortArmed = 0;
        g_sh4_jit_mmu_abort = 1;
        g_sh4_jit_phase = SH4_PHASE_CODE;
        return;
    }
    s_abortArmed = 1;
    g_sh4_inst_pc = instPc;
    g_sh4_jit_mmu_slow[5]++;
    sh4_sq_flush_to(sh4_mmu_translate_sq(addr), addr);
    s_abortArmed = 0;
    g_sh4_jit_phase = SH4_PHASE_CODE;
}

int sh4_jit_mmu_op(u32 op, u32 instPc)
{
    int phase = g_sh4_jit_phase;
    g_sh4_jit_phase = SH4_PHASE_MMU_OP;
    if (g_sh4_jit_mmu_op_counts) g_sh4_jit_mmu_op_counts[op & 0xFFFF]++;
    if (setjmp(s_abort))
    {
        s_abortArmed = 0;
        g_sh4_jit_phase = phase;
        return 1;
    }
    s_abortArmed = 1;
    g_sh4_inst_pc = instPc;
    g_sh4_ops[op]((u16)op);
    s_abortArmed = 0;
    g_sh4_jit_phase = phase;
    return 0;
}

static __inline u16 fetch(u32 pc)
{
    if (pc & 1)
    {
        g_ccn.tea = pc;
        sh4_abort_exception(0x0E0, 0x100);
    }
    return mem_fetch16(g_mmu_on ? sh4_va(pc, MMU_FETCH) : pc);
}

void sh4_exception(u32 expevt, u32 pc)
{
    g_ccn.expevt = expevt;
    enter_handler(pc, 0x100);
}

void sh4_trapa(u32 imm)
{
    g_ccn.tra = imm << 2;
    g_ccn.expevt = 0x160;
    enter_handler(C.pc, 0x100);
}

u32 g_sh4_irq_counts[64];

void sh4_take_interrupt(u32 intevt)
{
    g_sh4_irq_counts[(intevt >> 5) & 63]++;
    g_ccn.intevt = intevt;
    enter_handler(C.pc, 0x600);
}

void sh4_break_slice(void)
{
    s_breakSlice = 1;
    sh4_jit_stop_chain();
}

void sh4_step(void)
{
    u16 op;
    if (setjmp(s_abort))
    {
        s_abortArmed = 0;
        return;
    }
    s_abortArmed = 1;
    g_sh4_inst_pc = C.pc;
    op = fetch(C.pc);
    C.pc += 2;
    C.cycles--;
    g_sh4_ops[op](op);
    s_abortArmed = 0;
    if (g_sh4_irq_ready) sh4_irq_check();
}

void sh4_run(s32 cycles)
{
    C.cycles = cycles;
    s_breakSlice = 0;
    if (g_sh4_irq_ready) sh4_irq_check();
    if (setjmp(s_abort)) C.cycles--;
    s_abortArmed = 1;
    while (C.cycles > 0 && !s_breakSlice)
    {
        u16 op;
        if (C.sleeping)
        {
            C.cycles = 0;
            g_sh4_idle = 1;
            break;
        }
        g_sh4_inst_pc = C.pc;
        op = fetch(C.pc);
        if (g_sh4_trace) g_sh4_trace(C.pc, op);
        C.pc += 2;
        C.cycles--;
        g_sh4_ops[op](op);
        if (g_sh4_irq_ready) sh4_irq_check();
    }
    s_abortArmed = 0;
}

void sh4_state(StateIO *io)
{
    state_tag(io, "SH4 ");
    state_io(io, &g_sh4, (u32)offsetof(Sh4Ctx, jitRam));
}
