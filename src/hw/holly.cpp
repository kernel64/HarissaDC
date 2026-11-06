// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "holly.h"

#include <string.h>

#include "gdrom.h"
#include "maple.h"
#include "pvr.h"
#include "../core/log.h"
#include "../core/state.h"
#include "../core/sched.h"
#include "../mem/mem.h"
#include "../sh4/sh4.h"
#include "../sh4/sh4_mmr.h"

#define SB_BASE 0x005F6800u
#define SB_END  0x005F8000u
static u32 s_regs[(SB_END - SB_BASE) / 4];

static u32 s_istnrm, s_istext, s_isterr;
static int s_evCh2, s_evPvrDma, s_evG2Dma, s_evSort = -1;

#define REG(a) s_regs[((a) - SB_BASE) >> 2]

#define SB_IML2NRM 0x005F6910
#define SB_IML2EXT 0x005F6914
#define SB_IML2ERR 0x005F6918
#define SB_IML4NRM 0x005F6920
#define SB_IML4EXT 0x005F6924
#define SB_IML4ERR 0x005F6928
#define SB_IML6NRM 0x005F6930
#define SB_IML6EXT 0x005F6934
#define SB_IML6ERR 0x005F6938
#define SB_SFRES   0x005F6890
#define SB_SBREV   0x005F689C
#define SB_FFST    0x005F688C
#define SB_TFREM   0x005F6880
#define SB_G2ID    0x005F7880

u32 holly_reg(u32 addr) { return REG(addr); }
void holly_set_reg(u32 addr, u32 value) { REG(addr) = value; }

static void update_irqs(void)
{
    sh4_irq_set(SH4_IRQ_IRL9, ((s_istnrm & REG(SB_IML6NRM)) | (s_istext & REG(SB_IML6EXT)) | (s_isterr & REG(SB_IML6ERR))) != 0);
    sh4_irq_set(SH4_IRQ_IRL11, ((s_istnrm & REG(SB_IML4NRM)) | (s_istext & REG(SB_IML4EXT)) | (s_isterr & REG(SB_IML4ERR))) != 0);
    sh4_irq_set(SH4_IRQ_IRL13, ((s_istnrm & REG(SB_IML2NRM)) | (s_istext & REG(SB_IML2EXT)) | (s_isterr & REG(SB_IML2ERR))) != 0);
}

void holly_raise(int bit)
{
    s_istnrm |= 1u << bit;
    update_irqs();
}

void holly_ext(int bit, int level)
{
    if (level) s_istext |= 1u << bit;
    else s_istext &= ~(1u << bit);
    update_irqs();
}

static u32 dma_cycles(u32 bytes) { return 200 + bytes / 4; }

static void ch2_dma(void)
{
    u32 dst = REG(SB_C2DSTAT), len = REG(SB_C2DLEN), src = g_dmac.sar[2], i;
    if (!(g_dmac.dmaor & 1) || !(g_dmac.chcr[2] & 1))
        LOG_WARN("[holly] ch2 DMA started with the SH-4 DMAC off (DMAOR %08X CHCR2 %08X)", g_dmac.dmaor, g_dmac.chcr[2]);
    if (len == 0) len = g_dmac.dmatcr[2] * 32;
    LOG_DEBUG("[holly] ch2 DMA %08X -> %08X, %u bytes", src, dst, len);
    if ((dst & 0x1F000000u) == 0x10000000u)
        pvr_ta_dma(dst, src, len);
    else
    {
        int mode32 = ((dst >> 25) & 1) ? (REG(SB_LMMODE1) & 1) : (REG(SB_LMMODE0) & 1);
        u32 vbase = mode32 ? 0x05000000u : 0x04000000u;
        i = 0;
        if (!mode32)
            for (; i + 32 <= len && !((src + i) & 31) && !((dst + i) & 31); i += 32)
            {
                u32 avail;
                const u32 *p = (const u32 *)mem_word_ptr(src + i, &avail);
                if (!p || avail < 32 || !mem_write_burst(vbase | ((dst + i) & 0x00FFFFFFu), p, 8)) break;
            }
        for (; i < len; i += 4) mem_write32(vbase | ((dst + i) & 0x00FFFFFFu), mem_read32(src + i));
        REG(SB_C2DSTAT) = dst + len;
    }
    g_dmac.sar[2] = src + len;
    g_dmac.dmatcr[2] = 0;
    g_dmac.chcr[2] |= 2;
    REG(SB_C2DLEN) = 0;
    sched_after(s_evCh2, dma_cycles(len));
}

static void ch2_done(void *, u32)
{
    REG(SB_C2DST) = 0;
    holly_raise(HOLLY_CH2_DMA_END);
}

static u32 sort_start_link(void)
{
    u32 table = 0x0C000000u | (REG(SB_SDSTAW) & 0x00FFFFE0u), n = REG(SB_SDDIV)++;
    return REG(SB_SDWLT) & 1 ? mem_read32(table + n * 4) : mem_read16(table + n * 2);
}

static void sort_dma(void)
{
    u32 link, blocks = 0, bytes = 0, guard;
    REG(SB_SDDIV) = 0;
    link = sort_start_link();
    for (guard = 0; link != 2 && guard < 0x10000; guard++)
    {
        u32 ea, size, next, i;
        link = REG(SB_SDLAS) & 1 ? link * 32 : link & ~31u;
        ea = 0x0C000000u | ((REG(SB_SDBAAW) + link) & 0x00FFFFE0u);
        size = mem_read32(ea + 0x18);
        next = mem_read32(ea + 0x1C);
        if (size > 0x8000) size = 0x8000;
        for (i = 0; i < size; i++)
        {
            u32 avail, w[8], k;
            const u32 *p = (const u32 *)mem_word_ptr(ea + i * 32, &avail);
            if (!p || avail < 32)
            {
                for (k = 0; k < 8; k++) w[k] = mem_read32(ea + i * 32 + k * 4);
                p = w;
            }
            pvr_ta_fifo(p, 8);
        }
        blocks++;
        bytes += size * 32;
        link = next == 1 ? sort_start_link() : next;
    }
    if (guard >= 0x10000) LOG_WARN("[holly] sort DMA: link list without an end");
    LOG_DEBUG("[holly] sort DMA: %u blocks, %u bytes", blocks, bytes);
    REG(SB_SDSTAW) += 32;
    sched_after(s_evSort, dma_cycles(bytes));
}

static void sort_done(void *, u32)
{
    REG(SB_SDST) = 0;
    holly_raise(HOLLY_SORT_DMA_END);
}

static void pvr_dma(void)
{
    u32 pvrAddr = REG(SB_PDSTAP), ram = REG(SB_PDSTAR), len = REG(SB_PDLEN), i;
    int toRam = REG(SB_PDDIR) & 1;
    for (i = 0; i < len; i += 4)
    {
        if (toRam) mem_write32(ram + i, mem_read32(pvrAddr + i));
        else mem_write32(pvrAddr + i, mem_read32(ram + i));
    }
    sched_after(s_evPvrDma, dma_cycles(len));
}

static void pvr_dma_done(void *, u32)
{
    REG(SB_PDST) = 0;
    holly_raise(HOLLY_PVR_DMA_END);
}

static void g2_dma(void)
{
    u32 aica = REG(SB_ADSTAG), ram = REG(SB_ADSTAR), lenReg = REG(SB_ADLEN), len = lenReg & 0x7FFFFFE0u, i;
    int toRam = REG(SB_ADDIR) & 1;
    LOG_DEBUG("[g2] DMA %s %08X <-> %08X, %u bytes", toRam ? "to RAM" : "to AICA", aica, ram, len);
    for (i = 0; i < len; i += 4)
    {
        if (toRam) mem_write32(ram + i, mem_read32(aica + i));
        else mem_write32(aica + i, mem_read32(ram + i));
    }
    REG(SB_ADSTAG) = aica + len;
    REG(SB_ADSTAR) = ram + len;
    REG(SB_ADLEN) = 0;
    if (lenReg & 0x80000000u) REG(SB_ADEN) = 0;
    sched_after(s_evG2Dma, dma_cycles(len));
}

static void g2_dma_done(void *, u32)
{
    REG(SB_ADST) = 0;
    holly_raise(HOLLY_AICA_DMA_END);
}

u32 holly_read(u32 addr, int size)
{
    u32 a = addr & ~3u, v;
    if (a < SB_BASE || a >= SB_END) return 0;
    switch (a)
    {
    case SB_ISTNRM: v = s_istnrm | (s_istext ? 0x40000000u : 0) | (s_isterr ? 0x80000000u : 0); break;
    case SB_ISTEXT: v = s_istext; break;
    case SB_ISTERR: v = s_isterr; break;
    case SB_SBREV: v = 0x0B; break;
    case SB_FFST: v = 0; break;
    case SB_TFREM: v = 8; break;
    case SB_G2ID: v = 0x12; break;
    default: v = REG(a); break;
    }
    return reg_lane_read(v, addr, size);
}

void holly_write(u32 addr, u32 value, int size)
{
    u32 a = addr & ~3u;
    if (a < SB_BASE || a >= SB_END) return;
    value = reg_lane_write(REG(a), addr, value, size);
    switch (a)
    {
    case SB_ISTNRM:
        if (value & s_istnrm & (1u << HOLLY_GDROM_DMA_END)) LOG_DEBUG("[holly] GD-ROM DMA end acknowledged");
        s_istnrm &= ~(value & 0x003FFFFFu);
        update_irqs();
        return;
    case SB_ISTEXT:
        return;
    case SB_ISTERR:
        s_isterr &= ~value;
        update_irqs();
        return;
    case SB_IML2NRM: case SB_IML2EXT: case SB_IML2ERR:
    case SB_IML4NRM: case SB_IML4EXT: case SB_IML4ERR:
    case SB_IML6NRM: case SB_IML6EXT: case SB_IML6ERR:
        REG(a) = value;
        update_irqs();
        return;
    case SB_SFRES:
        if (value == 0x7611) LOG_WARN("[holly] software reset requested (not emulated)");
        return;
    case SB_C2DST:
        REG(a) = value & 1;
        if (value & 1) ch2_dma();
        return;
    case SB_SDST:
        REG(a) = value & 1;
        if (value & 1) sort_dma();
        return;
    case SB_PDST:
        REG(a) = value & 1;
        if ((value & 1) && (REG(SB_PDEN) & 1)) pvr_dma();
        return;
    case SB_ADST:
        REG(a) = value & 1;
        if ((value & 1) && (REG(SB_ADEN) & 1)) g2_dma();
        return;
    case SB_MDST:
        REG(a) = value & 1;
        if ((value & 1) && (REG(SB_MDEN) & 1)) maple_dma_start();
        return;
    case SB_GDST:
        REG(a) = value & 1;
        if ((value & 1) && (REG(SB_GDEN) & 1)) gdrom_dma_start();
        return;
    }
    REG(a) = value;
}

void holly_init(void)
{
    s_evCh2 = sched_register("ch2dma", ch2_done, NULL);
    s_evPvrDma = sched_register("pvrdma", pvr_dma_done, NULL);
    s_evG2Dma = sched_register("g2dma", g2_dma_done, NULL);
}

void holly_reset(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    s_istnrm = s_istext = s_isterr = 0;
    update_irqs();
}

void holly_state(StateIO *io)
{
    state_tag(io, "HOLY");
    STATE(io, s_regs);
    STATE(io, s_istnrm);
    STATE(io, s_istext);
    STATE(io, s_isterr);
}

void holly_late_init(void)
{
    if (s_evSort < 0) s_evSort = sched_register("sortdma", sort_done, NULL);
}
