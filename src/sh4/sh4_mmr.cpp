// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "sh4_mmr.h"

#include <string.h>

#include "sh4.h"
#include "sh4_internal.h"
#include "../core/log.h"
#include "../core/prof.h"
#include "../core/state.h"
#include "../core/sched.h"
#include "../hw/pvr.h"
#include "../mem/mem.h"
#include "sh4_mmu.h"

Sh4Ccn g_ccn;
Sh4Intc g_intc;
Sh4Dmac g_dmac;
u32 g_sq[2][8];

void sh4_irq_reset(void);

static u32 s_bcr1, s_wcr1, s_wcr2, s_wcr3, s_mcr, s_pctra, s_pctrb;
static u16 s_bcr2, s_pcr, s_rtcsr, s_rtcnt, s_rtcor, s_pdtra, s_pdtrb, s_gpioic;
static int s_cable = CABLE_COMPOSITE;

void sh4_mmr_set_cable(int cable) { s_cable = cable; }

static u16 read_pdtra(void)
{
    u32 ctrl = s_pctra & 0xF, data = s_pdtra & 0xF;
    u16 v = (ctrl == 0x8 || ctrl == 0xB) ? 3 : 0;
    if (ctrl == 0xB && data == 2) v = 0;
    else if (ctrl == 0xC && data == 2) v = 3;
    return (u16)(v | (s_cable << 8));
}

static u16 s_frqcr = 0x0E0A;
static u8 s_stbcr, s_stbcr2;
static u16 s_wtcnt, s_wtcsr;
static u8 s_rtcRegs[16];
static u16 s_scsmr2, s_scscr2, s_scfcr2, s_scsptr2, s_sclsr2;
static u8 s_scbrr2 = 0xFF;
static char s_serialLine[160];
static u32 s_serialLen;

static void serial_out(u8 c)
{
    if (c == '\n' || s_serialLen >= sizeof(s_serialLine) - 1)
    {
        s_serialLine[s_serialLen] = 0;
        LOG_INFO("[serial] %s", s_serialLine);
        s_serialLen = 0;
        if (c == '\n') return;
    }
    if (c != '\r') s_serialLine[s_serialLen++] = (char)c;
}

struct Tmu
{
    u32 tcor;
    u32 tcnt;
    u64 start;
    u16 tcr;
    int running;
    int event;
};

static Tmu s_tmu[3];
static u8 s_tocr, s_tstr;
static u32 s_tcpr2;

static u32 tmu_divider(int ch)
{
    static const u32 div[8] = { 16, 64, 256, 1024, 4096, 4096, 4096, 4096 };
    return div[s_tmu[ch].tcr & 7];
}

static u32 tmu_count(int ch)
{
    Tmu *t = &s_tmu[ch];
    u64 ticks;
    if (!t->running) return t->tcnt;
    ticks = (sched_now_exact() - t->start) / tmu_divider(ch);
    return ticks > t->tcnt ? 0 : t->tcnt - (u32)ticks;
}

int g_sh4_tmu_poll_skip = 1;

static u32 tmu_read(int ch)
{
    static u64 last;
    static u32 streak;
    u64 now = sched_now_exact();
    streak = now - last < 1000 ? streak + 1 : 0;
    last = now;
    if (streak >= 16 && s_tmu[ch].running && g_sh4_tmu_poll_skip)
    {
        g_sh4_idle = 1;
        sh4_break_slice();
    }
    return tmu_count(ch);
}

static void tmu_update_irq(int ch)
{
    Tmu *t = &s_tmu[ch];
    sh4_irq_set(SH4_IRQ_TMU0 + ch, (t->tcr & 0x100) && (t->tcr & 0x20));
}

static void tmu_schedule(int ch)
{
    Tmu *t = &s_tmu[ch];
    if (!t->running) { sched_cancel(t->event); return; }
    sched_at(t->event, t->start + ((u64)t->tcnt + 1) * tmu_divider(ch));
}

static void tmu_underflow(void *arg, u32 late)
{
    int ch = (int)(size_t)arg;
    Tmu *t = &s_tmu[ch];
    t->tcr |= 0x100;
    t->tcnt = t->tcor;
    t->start = sched_now_exact() - late;
    tmu_update_irq(ch);
    tmu_schedule(ch);
}

static void tmu_set_running(int ch, int on)
{
    Tmu *t = &s_tmu[ch];
    if (on == t->running) return;
    if (on)
    {
        t->start = sched_now_exact();
        t->running = 1;
    }
    else
    {
        t->tcnt = tmu_count(ch);
        t->running = 0;
    }
    tmu_schedule(ch);
}

static void tmu_write_tcnt(int ch, u32 v)
{
    Tmu *t = &s_tmu[ch];
    t->tcnt = v;
    t->start = sched_now_exact();
    tmu_schedule(ch);
}

static void tmu_write_tcr(int ch, u16 v)
{
    Tmu *t = &s_tmu[ch];
    u32 count = tmu_count(ch);
    u16 unf = (u16)(t->tcr & v & 0x100);
    t->tcr = (u16)((v & 0x3FF & ~0x100) | unf);
    t->tcnt = count;
    t->start = sched_now_exact();
    tmu_update_irq(ch);
    tmu_schedule(ch);
}

static u8 s_ocram[0x2000];
#define CCR_ORA 0x20u
#define CCR_OIX 0x80u

static u32 ocram_offset(u32 addr)
{
    u32 half = (g_ccn.ccr & CCR_OIX) ? (addr >> 25) & 1 : (addr >> 13) & 1;
    return (half << 12) | (addr & 0xFFF);
}

u8 *sh4_ocram_bytes(void) { return s_ocram; }
int sh4_ocram_simple(void) { return (g_ccn.ccr & (CCR_ORA | CCR_OIX)) == CCR_ORA; }

u32 sh4_ocram_read(u32 addr, int size)
{
    u32 off = ocram_offset(addr);
    if (!(g_ccn.ccr & CCR_ORA)) return 0;
    if (size == 4) return buf_r32(s_ocram, off & ~3u);
    if (size == 2) return buf_r16(s_ocram, off & ~1u);
    return buf_r8(s_ocram, off);
}

void sh4_ocram_write(u32 addr, u32 value, int size)
{
    u32 off = ocram_offset(addr);
    if (!(g_ccn.ccr & CCR_ORA)) return;
    if (size == 4) buf_w32(s_ocram, off & ~3u, value);
    else if (size == 2) buf_w16(s_ocram, off & ~1u, (u16)value);
    else buf_w8(s_ocram, off, (u8)value);
}

void sh4_sq_flush(u32 addr)
{
    int q = (addr >> 5) & 1;
    u32 qacr = q ? g_ccn.qacr1 : g_ccn.qacr0;
    if (g_mmu_sq_only)
    {
        static int warned;
        u32 pa;
        if (sh4_mmu_translate_sq_quiet(addr, &pa))
        {
            sh4_sq_flush_to(pa, addr);
            return;
        }
        if (!warned++) LOG_WARN("[sh4] store queue %08X not mapped by the UTLB: sent through QACR", addr);
    }
    sh4_sq_flush_to((addr & 0x03FFFFE0u) | (((qacr >> 2) & 7) << 26), addr);
}

void sh4_sq_flush_to(u32 dst, u32 addr)
{
    int q = (addr >> 5) & 1, i;
    dst &= 0x1FFFFFE0u;
    LOG_DEBUG("[sh4] SQ%d send %08X -> %08X", q, addr, dst);
    if ((dst & 0x1F800000u) == 0x10000000u)
    {
        int outer = g_prof_section;
        g_prof_section = PROF_TA;
        pvr_ta_sq(g_sq[q]);
        g_prof_section = outer;
        return;
    }
    if (mem_write_burst(dst, g_sq[q], 8)) return;
    for (i = 0; i < 8; i++) mem_write32(dst + i * 4, g_sq[q][i]);
}

static u32 s_logged;

static void log_unknown(const char *what, u32 addr, u32 value)
{
    if (s_logged < 64)
    {
        s_logged++;
        LOG_WARN("[sh4] %s unhandled register %08X (%08X)", what, addr, value);
    }
}

u32 sh4_p4_read(u32 addr, int size)
{
    u32 a;
    if (addr < 0xE4000000u) return 0;
    if ((addr >> 24) == 0xF2 || (addr >> 24) == 0xF3 || (addr >> 24) == 0xF6 || (addr >> 24) == 0xF7) return sh4_mmu_array_read(addr);
    if (addr < 0xFF000000u) return 0;
    a = addr & 0xFFFFFFFCu;
    switch (a)
    {
    case 0xFF000000: return g_ccn.pteh;
    case 0xFF000004: return g_ccn.ptel;
    case 0xFF000008: return g_ccn.ttb;
    case 0xFF00000C: return g_ccn.tea;
    case 0xFF000010: return g_ccn.mmucr;
    case 0xFF000014: return g_ccn.basra;
    case 0xFF000018: return g_ccn.basrb;
    case 0xFF00001C: return g_ccn.ccr;
    case 0xFF000020: return g_ccn.tra;
    case 0xFF000024: return g_ccn.expevt;
    case 0xFF000028: return g_ccn.intevt;
    case 0xFF000030: return 0x040205C1u;
    case 0xFF000034: return g_ccn.ptea;
    case 0xFF000038: return g_ccn.qacr0;
    case 0xFF00003C: return g_ccn.qacr1;
    case 0xFF800000: return s_bcr1;
    case 0xFF800004: return reg_lane_read(s_bcr2, addr, size);
    case 0xFF800008: return s_wcr1;
    case 0xFF80000C: return s_wcr2;
    case 0xFF800010: return s_wcr3;
    case 0xFF800014: return s_mcr;
    case 0xFF800018: return s_pcr;
    case 0xFF80001C: return s_rtcsr;
    case 0xFF800020: return s_rtcnt;
    case 0xFF800024: return s_rtcor;
    case 0xFF800028: return (u32)(sched_now_exact() >> 12) & 0x3FF;
    case 0xFF80002C: return s_pctra;
    case 0xFF800030: return read_pdtra();
    case 0xFF800040: return s_pctrb;
    case 0xFF800044: return s_pdtrb;
    case 0xFF800048: return s_gpioic;
    case 0xFFA00040: return g_dmac.dmaor;
    case 0xFFC00000: return s_frqcr;
    case 0xFFC00004: return s_stbcr;
    case 0xFFC00008: return s_wtcnt;
    case 0xFFC0000C: return s_wtcsr;
    case 0xFFC00010: return s_stbcr2;
    case 0xFFD00000: return g_intc.icr;
    case 0xFFD00004: return g_intc.ipra;
    case 0xFFD00008: return g_intc.iprb;
    case 0xFFD0000C: return g_intc.iprc;
    case 0xFFD00010: return g_intc.iprd;
    case 0xFFD80000: return s_tocr;
    case 0xFFD80004: return s_tstr;
    case 0xFFD80008: return s_tmu[0].tcor;
    case 0xFFD8000C: return tmu_read(0);
    case 0xFFD80010: return s_tmu[0].tcr;
    case 0xFFD80014: return s_tmu[1].tcor;
    case 0xFFD80018: return tmu_read(1);
    case 0xFFD8001C: return s_tmu[1].tcr;
    case 0xFFD80020: return s_tmu[2].tcor;
    case 0xFFD80024: return tmu_read(2);
    case 0xFFD80028: return s_tmu[2].tcr;
    case 0xFFD8002C: return s_tcpr2;
    case 0xFFE80000: return s_scsmr2;
    case 0xFFE80004: return s_scbrr2;
    case 0xFFE80008: return s_scscr2;
    case 0xFFE80010: return 0x0060;
    case 0xFFE80014: return 0;
    case 0xFFE80018: return s_scfcr2;
    case 0xFFE8001C: return 0;
    case 0xFFE80020: return s_scsptr2;
    case 0xFFE80024: return s_sclsr2;
    }
    if (a >= 0xFFA00000u && a < 0xFFA00040u)
    {
        int ch = (a >> 4) & 3;
        switch (a & 0xC)
        {
        case 0x0: return g_dmac.sar[ch];
        case 0x4: return g_dmac.dar[ch];
        case 0x8: return g_dmac.dmatcr[ch];
        default: return g_dmac.chcr[ch];
        }
    }
    if (a >= 0xFFC80000u && a < 0xFFC80040u) return s_rtcRegs[(a >> 2) & 15];
    if (a >= 0xFF200000u && a < 0xFF200100u) return 0;
    if (a >= 0xFFE00000u && a < 0xFFE00100u) return 0;
    log_unknown("read", addr, 0);
    return 0;
}

void sh4_p4_write(u32 addr, u32 value, int size)
{
    u32 a;
    if (addr < 0xE4000000u)
    {
        u32 *w = &g_sq[(addr >> 5) & 1][(addr >> 2) & 7];
        *w = reg_lane_write(*w, addr, value, size);
        return;
    }
    if ((addr >> 24) == 0xF2 || (addr >> 24) == 0xF3 || (addr >> 24) == 0xF6 || (addr >> 24) == 0xF7)
    {
        sh4_mmu_array_write(addr, value);
        return;
    }
    if (addr < 0xFF000000u) return;
    a = addr & 0xFFFFFFFCu;
    switch (a)
    {
    case 0xFF000000: sh4_mmu_write_pteh(value); return;
    case 0xFF000004: g_ccn.ptel = value; return;
    case 0xFF000008: g_ccn.ttb = value; return;
    case 0xFF00000C: g_ccn.tea = value; return;
    case 0xFF000010: sh4_mmu_write_mmucr(value); return;
    case 0xFF000014: g_ccn.basra = (u8)value; return;
    case 0xFF000018: g_ccn.basrb = (u8)value; return;
    case 0xFF00001C: g_ccn.ccr = value & ~0x0808u; return;
    case 0xFF000020: g_ccn.tra = value; return;
    case 0xFF000024: g_ccn.expevt = value; return;
    case 0xFF000028: g_ccn.intevt = value; return;
    case 0xFF000034: g_ccn.ptea = value; return;
    case 0xFF000038: g_ccn.qacr0 = value; return;
    case 0xFF00003C: g_ccn.qacr1 = value; return;

    case 0xFF800000: s_bcr1 = value; return;
    case 0xFF800004: s_bcr2 = (u16)value; return;
    case 0xFF800008: s_wcr1 = value; return;
    case 0xFF80000C: s_wcr2 = value; return;
    case 0xFF800010: s_wcr3 = value; return;
    case 0xFF800014: s_mcr = value; return;
    case 0xFF800018: s_pcr = (u16)value; return;
    case 0xFF80001C: s_rtcsr = (u16)value; return;
    case 0xFF800020: s_rtcnt = (u16)value; return;
    case 0xFF800024: s_rtcor = (u16)value; return;
    case 0xFF800028: return;
    case 0xFF80002C: s_pctra = value; return;
    case 0xFF800030: s_pdtra = (u16)value; return;
    case 0xFF800040: s_pctrb = value; return;
    case 0xFF800044: s_pdtrb = (u16)value; return;
    case 0xFF800048: s_gpioic = (u16)value; return;

    case 0xFFA00040: g_dmac.dmaor = value; return;

    case 0xFFC00000: s_frqcr = (u16)value; return;
    case 0xFFC00004: s_stbcr = (u8)value; return;
    case 0xFFC00008: s_wtcnt = (u16)value; return;
    case 0xFFC0000C: s_wtcsr = (u16)value; return;
    case 0xFFC00010: s_stbcr2 = (u8)value; return;

    case 0xFFD00000: g_intc.icr = (u16)value; sh4_irq_update(); return;
    case 0xFFD00004: g_intc.ipra = (u16)value; sh4_irq_update(); return;
    case 0xFFD00008: g_intc.iprb = (u16)value; sh4_irq_update(); return;
    case 0xFFD0000C: g_intc.iprc = (u16)value; sh4_irq_update(); return;
    case 0xFFD00010: g_intc.iprd = (u16)value; sh4_irq_update(); return;

    case 0xFFD80000: s_tocr = (u8)value; return;
    case 0xFFD80004:
    {
        int ch;
        s_tstr = (u8)(value & 7);
        for (ch = 0; ch < 3; ch++) tmu_set_running(ch, (s_tstr >> ch) & 1);
        return;
    }
    case 0xFFD80008: s_tmu[0].tcor = value; return;
    case 0xFFD8000C: tmu_write_tcnt(0, value); return;
    case 0xFFD80010: tmu_write_tcr(0, (u16)value); return;
    case 0xFFD80014: s_tmu[1].tcor = value; return;
    case 0xFFD80018: tmu_write_tcnt(1, value); return;
    case 0xFFD8001C: tmu_write_tcr(1, (u16)value); return;
    case 0xFFD80020: s_tmu[2].tcor = value; return;
    case 0xFFD80024: tmu_write_tcnt(2, value); return;
    case 0xFFD80028: tmu_write_tcr(2, (u16)value); return;
    case 0xFFD8002C: s_tcpr2 = value; return;

    case 0xFFE80000: s_scsmr2 = (u16)value; return;
    case 0xFFE80004: s_scbrr2 = (u8)value; return;
    case 0xFFE80008: s_scscr2 = (u16)value; return;
    case 0xFFE8000C: serial_out((u8)value); return;
    case 0xFFE80010: return;
    case 0xFFE80018: s_scfcr2 = (u16)value; return;
    case 0xFFE80020: s_scsptr2 = (u16)value; return;
    case 0xFFE80024: s_sclsr2 = (u16)value; return;
    }
    if (a >= 0xFFA00000u && a < 0xFFA00040u)
    {
        int ch = (a >> 4) & 3;
        switch (a & 0xC)
        {
        case 0x0: g_dmac.sar[ch] = value; return;
        case 0x4: g_dmac.dar[ch] = value; return;
        case 0x8: g_dmac.dmatcr[ch] = value & 0x00FFFFFF; return;
        default: g_dmac.chcr[ch] = value; return;
        }
    }
    if (a >= 0xFFC80000u && a < 0xFFC80040u) { s_rtcRegs[(a >> 2) & 15] = (u8)value; return; }
    if (a >= 0xFF200000u && a < 0xFF200100u) return;
    if ((a & 0xFFFF0000u) == 0xFF900000u || (a & 0xFFFF0000u) == 0xFF940000u) return;
    if (a >= 0xFFE00000u && a < 0xFFE00100u) return;
    log_unknown("write", addr, value);
}

void sh4_mmr_init(void)
{
    static const char *const names[3] = { "tmu0", "tmu1", "tmu2" };
    int ch;
    for (ch = 0; ch < 3; ch++) s_tmu[ch].event = sched_register(names[ch], tmu_underflow, (void *)(size_t)ch);
}

void sh4_mmr_reset(void)
{
    int ch;
    memset(&g_ccn, 0, sizeof(g_ccn));
    memset(&g_intc, 0, sizeof(g_intc));
    memset(&g_dmac, 0, sizeof(g_dmac));
    memset(g_sq, 0, sizeof(g_sq));
    memset(s_ocram, 0, sizeof(s_ocram));
    for (ch = 0; ch < 3; ch++)
    {
        int ev = s_tmu[ch].event;
        memset(&s_tmu[ch], 0, sizeof(s_tmu[ch]));
        s_tmu[ch].event = ev;
        s_tmu[ch].tcor = s_tmu[ch].tcnt = 0xFFFFFFFFu;
        sched_cancel(ev);
    }
    s_tocr = s_tstr = 0;
    s_bcr1 = s_wcr1 = s_wcr2 = s_wcr3 = s_mcr = s_pctra = s_pctrb = 0;
    s_bcr2 = 0x3FFC;
    s_pcr = s_rtcsr = s_rtcnt = s_rtcor = s_pdtra = s_pdtrb = s_gpioic = 0;
    s_serialLen = 0;
    s_logged = 0;
    sh4_mmu_reset();
    sh4_irq_reset();
}

void sh4_mmr_state(StateIO *io)
{
    state_tag(io, "MMR ");
    STATE(io, g_ccn);
    STATE(io, g_intc);
    STATE(io, g_dmac);
    STATE(io, g_sq);
    STATE(io, s_bcr1); STATE(io, s_wcr1); STATE(io, s_wcr2); STATE(io, s_wcr3); STATE(io, s_mcr);
    STATE(io, s_pctra); STATE(io, s_pctrb);
    STATE(io, s_bcr2); STATE(io, s_pcr); STATE(io, s_rtcsr); STATE(io, s_rtcnt); STATE(io, s_rtcor);
    STATE(io, s_pdtra); STATE(io, s_pdtrb); STATE(io, s_gpioic);
    STATE(io, s_frqcr); STATE(io, s_stbcr); STATE(io, s_stbcr2); STATE(io, s_wtcnt); STATE(io, s_wtcsr);
    STATE(io, s_rtcRegs);
    STATE(io, s_scsmr2); STATE(io, s_scscr2); STATE(io, s_scfcr2); STATE(io, s_scsptr2); STATE(io, s_sclsr2);
    STATE(io, s_scbrr2);
    STATE(io, s_tmu); STATE(io, s_tocr); STATE(io, s_tstr); STATE(io, s_tcpr2);
    STATE(io, s_ocram);
}
