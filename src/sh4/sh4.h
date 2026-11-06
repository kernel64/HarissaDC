// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_SH4_H
#define EMU_SH4_H

#include "../core/types.h"

#define SR_T     0x00000001u
#define SR_S     0x00000002u
#define SR_IMASK 0x000000F0u
#define SR_Q     0x00000100u
#define SR_M     0x00000200u
#define SR_FD    0x00008000u
#define SR_BL    0x10000000u
#define SR_RB    0x20000000u
#define SR_MD    0x40000000u
#define SR_MASK  0x700083F3u

#define FPSCR_RM    0x00000003u
#define FPSCR_FLAGS 0x0000007Cu
#define FPSCR_CAUSE 0x0003F000u
#define FPSCR_DN    0x00040000u
#define FPSCR_PR    0x00080000u
#define FPSCR_SZ    0x00100000u
#define FPSCR_FR    0x00200000u
#define FPSCR_MASK  0x003FFFFFu

struct Sh4Ctx
{
    u32 r[16];
    u32 rbank[8];
    u32 sr;
    u32 t;
    u32 gbr, vbr, ssr, spc, sgr, dbr;
    u32 mach, macl, pr;
    u32 pc;
    u32 fpscr, fpul;
    u32 fr[16];
    u32 xf[16];
    int sleeping;
    s32 cycles;
    u8 *jitRam;
    u8 *jitCode;
    u32 jitTemp[4];
    u64 jitTemp64;
    f32 jitFloatMin;
    f32 jitFloatZero;
    f32 jitFloatOne;
    int irqReady;
    int breakSlice;
    int jitWrote;
    u32 jitWhy;
    u32 jitWhyKey[3];
    u32 jitAsidTag;
    u32 jitSqKey;
    u32 *jitSq;
    u32 jitSqTag;
    u32 jitSqPa;
};

extern Sh4Ctx g_sh4;

void sh4_init(void);
void sh4_reset(void);

void sh4_run(s32 cycles);
void sh4_break_slice(void);
#define g_sh4_break g_sh4.breakSlice
#define g_sh4_irq_ready g_sh4.irqReady

void sh4_step(void);

u32 sh4_get_sr(void);
void sh4_set_sr(u32 sr);
void sh4_set_fpscr(u32 fpscr);
void sh4_apply_rounding(void);
void sh4_refresh_rounding(void);
void sh4_host_rounding(void);

enum Sh4Irq
{
    SH4_IRQ_IRL9,
    SH4_IRQ_IRL11,
    SH4_IRQ_IRL13,
    SH4_IRQ_TMU0,
    SH4_IRQ_TMU1,
    SH4_IRQ_TMU2,
    SH4_IRQ_RTC_ATI,
    SH4_IRQ_RTC_PRI,
    SH4_IRQ_RTC_CUI,
    SH4_IRQ_DMTE0,
    SH4_IRQ_DMTE1,
    SH4_IRQ_DMTE2,
    SH4_IRQ_DMTE3,
    SH4_IRQ_DMAE,
    SH4_IRQ_SCIF_ERI,
    SH4_IRQ_SCIF_RXI,
    SH4_IRQ_SCIF_BRI,
    SH4_IRQ_SCIF_TXI,
    SH4_IRQ_COUNT
};
void sh4_irq_set(int irq, int pending);
extern u32 g_sh4_irq_counts[64];
void sh4_irq_update(void);
int sh4_irq_check(void);

void sh4_exception(u32 expevt, u32 pc);
void sh4_trapa(u32 imm);

#define SH4_HLE_TRAP_OPCODE 0xFFFD
typedef void (*Sh4HleTrap)(u32 addr);
extern Sh4HleTrap g_sh4_hle_trap;
extern void (*g_sh4_illegal_hook)(u32 pc);
extern u32 g_sh4_exc_counts[64];
extern void (*g_sh4_exception_hook)(u32 expevt);

typedef void (*Sh4TraceFn)(u32 pc, u16 op);
extern Sh4TraceFn g_sh4_trace;

typedef void (*Sh4OpFn)(u16 op);
extern Sh4OpFn g_sh4_ops[0x10000];
const char *sh4_op_name(u16 op);
int sh4_op_ends_block(u16 op);
int sh4_op_writes_memory(u16 op);

extern int g_sh4_idle;
int sh4_op_unknown_count(void);

struct StateIO;
void sh4_state(StateIO *io);

#endif
