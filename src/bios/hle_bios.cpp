// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "hle_bios.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/log.h"
#include "../core/state.h"
#include "../core/sched.h"
#include "../disc/disc.h"
#include "../disc/isofs.h"
#include "../hw/aica.h"
#include "../hw/gdrom.h"
#include "../hw/cdda.h"
#include "../hw/holly.h"
#include "../mem/mem.h"
#include "../sh4/sh4.h"
#include "../sh4/sh4_mmr.h"
#include "../sh4/sh4_mmu.h"

static u32 hle_va(u32 a, int access)
{
    return g_mmu_on ? sh4_va(a, access) : a;
}

static void hle_probe(u32 a, u32 size, int access)
{
    u32 p;
    if (!g_mmu_on || !size) return;
    for (p = a & ~0x3FFu; p < a + size; p += 0x400) hle_va(p < a ? a : p, access);
}
#define mem_read8(a) mem_read8(hle_va((a), MMU_READ))
#define mem_read16(a) mem_read16(hle_va((a), MMU_READ))
#define mem_read32(a) mem_read32(hle_va((a), MMU_READ))
#define mem_write8(a, v) mem_write8(hle_va((a), MMU_WRITE), (v))
#define mem_write16(a, v) mem_write16(hle_va((a), MMU_WRITE), (v))
#define mem_write32(a, v) mem_write32(hle_va((a), MMU_WRITE), (v))

#define VEC_SYSINFO  0x8C0000B0u
#define VEC_FONT     0x8C0000B4u
#define VEC_FLASH    0x8C0000B8u
#define VEC_GDROM    0x8C0000BCu
#define VEC_GDROM2   0x8C0000C0u
#define VEC_MISC     0x8C0000E0u
#define TRAP_SYSINFO 0x8C001000u
#define TRAP_FONT    0x8C001002u
#define TRAP_FLASH   0x8C001004u
#define TRAP_GDROM   0x8C001006u
#define TRAP_MISC    0x8C001008u
#define TRAP_GDROM2  0x8C0010F0u
#define TRAP_IDLE    0x8C00100Au
#define SYSINFO_ADDR 0x8C000068u
#define FONT_ADDR    0xA0100020u

#define IP_BIN_ADDR  0x8C008000u
#define BOOT_ADDR    0x8C010000u

static int s_region = HLE_REGION_USA, s_language = HLE_LANG_ENGLISH;
static int s_bootStatus;
static u32 s_bootSectors;
#define WINCE_TABLE_ADDR 0x8CE01000u

#define R g_sh4.r

static const u32 s_partitions[5][2] = {
    { 0x1A000, 0x02000 }, { 0x18000, 0x02000 }, { 0x1C000, 0x04000 }, { 0x10000, 0x08000 }, { 0x00000, 0x10000 },
};

#define FLASH_BLOCK 64

static u16 flash_block_crc(const u8 *block)
{
    u32 n = 0xFFFF;
    int i, c;
    for (i = 0; i < 62; i++)
    {
        n ^= (u32)block[i] << 8;
        for (c = 0; c < 8; c++) n = (n & 0x8000) ? (n << 1) ^ 0x1021 : n << 1;
    }
    return (u16)(~n & 0xFFFF);
}

static void flash_write_bytes(u32 offset, const u8 *data, u32 size)
{
    u32 i;
    for (i = 0; i < size; i++) buf_w8(g_flash, offset + i, data[i]);
}

static void format_partition(int part)
{
    u8 header[FLASH_BLOCK];
    memset(header, 0xFF, sizeof(header));
    memcpy(header, "KATANA_FLASH____", 16);
    header[16] = (u8)part;
    header[17] = 0;
    flash_write_bytes(s_partitions[part][0], header, sizeof(header));
}

static void write_user_block(u8 *block, u16 id)
{
    u32 start = s_partitions[2][0], size = s_partitions[2][1];
    u32 physical = size / FLASH_BLOCK, bitmaps = (size + 32767) / 32768;
    u32 firstBitmap = physical - bitmaps;
    u16 crc;
    block[0] = (u8)id;
    block[1] = (u8)(id >> 8);
    crc = flash_block_crc(block);
    block[62] = (u8)crc;
    block[63] = (u8)(crc >> 8);
    flash_write_bytes(start + 1 * FLASH_BLOCK, block, FLASH_BLOCK);
    buf_w8(g_flash, start + firstBitmap * FLASH_BLOCK, 0x7F);
}

static void make_flash(void)
{
    static const char regionChar[3] = { '0', '1', '2' };
    u8 factory[16], syscfg[FLASH_BLOCK];
    u32 i, now;
    for (i = 0; i < FLASH_SIZE; i++) buf_w8(g_flash, i, 0xFF);
    memcpy(factory, "00000Dreamcast  ", 16);
    factory[2] = (u8)regionChar[s_region];
    factory[3] = '0';
    flash_write_bytes(0x1A000, factory, 16);
    flash_write_bytes(0x1A0A0, factory, 16);
    for (i = 0; i < 8; i++) buf_w8(g_flash, 0x1A056 + i, (u8)(0x10 + i * 0x11));
    format_partition(2);
    format_partition(3);
    format_partition(4);
    memset(syscfg, 0xFF, sizeof(syscfg));
    now = host_time() + 631152000u;
    syscfg[2] = (u8)now;
    syscfg[3] = (u8)(now >> 8);
    syscfg[4] = (u8)(now >> 16);
    syscfg[5] = (u8)(now >> 24);
    syscfg[6] = 0;
    syscfg[7] = (u8)s_language;
    syscfg[8] = 0;
    syscfg[9] = 1;
    write_user_block(syscfg, 5);
}

enum { GDC_ERR = -1, GDC_OK = 0, GDC_BUSY = 1, GDC_COMPLETE = 2, GDC_CONTINUE = 3 };
enum
{
    CC_PIOREAD = 16, CC_DMAREAD = 17, CC_GETTOC = 18, CC_GETTOC2 = 19, CC_PLAY = 20, CC_PLAY2 = 21, CC_PAUSE = 22,
    CC_RELEASE = 23, CC_INIT = 24, CC_SEEK = 27, CC_REQ_MODE = 30, CC_SET_MODE = 31, CC_STOP = 33, CC_GETSCD = 34,
    CC_REQ_SES = 35, CC_REQ_STAT = 36, CC_STREAM_DMA = 38, CC_STREAM_PIO = 39, CC_GET_VERSION = 40,
    CC_STREAM_DMA_OLD = 28, CC_STREAM_PIO_OLD = 37
};

struct GdHle
{
    s32 status;
    u32 command;
    u32 params[4];
    u32 result[4];
    u32 lastRequest, nextRequest;
    u32 curFad;
    u32 dmaFad, dmaCount, dmaAddr;
    u32 mode[4];
    u32 streamFad, streamOff, streamLeft, streamTotal;
    u32 pioDest, pioSize, pioPending;
    u32 pioCallback, pioArg;
    u32 dmaCallback, dmaArg;
    u32 dmaEnded;
    u32 dmaQueue[8], dmaQueued;
};

static GdHle s_gd;
static int s_evDma;
static int s_evStreamDma;

static void reset_mode(void)
{
    s_gd.mode[0] = 0;
    s_gd.mode[1] = 0x00B4;
    s_gd.mode[2] = 0x19;
    s_gd.mode[3] = 0x08;
}

static Disc *disc(void) { return gdrom_disc(); }

static void read_sectors(u32 addr, u32 fad, u32 count, int dma)
{
    u8 buf[2048];
    u32 i, j;
    if (!dma) hle_probe(addr, count * 2048, MMU_WRITE);
    for (i = 0; i < count; i++)
    {
        disc_read(disc(), fad + i, 1, buf, 2048);
        if (!dma && (addr & 3))
        {
            for (j = 0; j < 2048; j++) mem_write8(addr + i * 2048 + j, buf[j]);
            continue;
        }
        for (j = 0; j < 2048; j += 4)
        {
            u32 a = addr + i * 2048 + j, v = buf[j] | ((u32)buf[j + 1] << 8) | ((u32)buf[j + 2] << 16) | ((u32)buf[j + 3] << 24);
            if (dma) (mem_write32)(a & 0x1FFFFFFFu, v);
            else mem_write32(a, v);
        }
    }
    s_gd.curFad = fad + count - 1;
}

static void dma_event(void *, u32)
{
    read_sectors(s_gd.dmaAddr, s_gd.dmaFad, s_gd.dmaCount, 1);
    s_gd.result[2] = s_gd.dmaCount * 2048;
    s_gd.status = GDC_COMPLETE;
}

static void stream_copy(u32 *dest, u32 *size, int dma)
{
    u8 buf[2048];
    u32 bufFad = 0xFFFFFFFFu;
    while (*size > 0 && s_gd.streamLeft > 0)
    {
        u32 v;
        if (bufFad != s_gd.streamFad)
        {
            disc_read(disc(), s_gd.streamFad, 1, buf, 2048);
            bufFad = s_gd.streamFad;
        }
        if (!(*dest & 3) && !(s_gd.streamOff & 3) && *size >= 4 && s_gd.streamLeft >= 4)
        {
            v = buf[s_gd.streamOff] | ((u32)buf[s_gd.streamOff + 1] << 8) | ((u32)buf[s_gd.streamOff + 2] << 16) |
                ((u32)buf[s_gd.streamOff + 3] << 24);
            if (dma) (mem_write32)(*dest & 0x1FFFFFFFu, v);
            else mem_write32(*dest, v);
            *dest += 4;
            *size -= 4;
            s_gd.streamLeft -= 4;
            s_gd.streamOff += 4;
        }
        else
        {
            if (dma) (mem_write8)(*dest & 0x1FFFFFFFu, buf[s_gd.streamOff]);
            else mem_write8(*dest, buf[s_gd.streamOff]);
            *dest += 1;
            *size -= 1;
            s_gd.streamLeft -= 1;
            s_gd.streamOff += 1;
        }
        if (s_gd.streamOff == 2048)
        {
            s_gd.streamOff = 0;
            s_gd.curFad = s_gd.streamFad++;
        }
    }
}

static u32 stream_dma_time(u32 size)
{
    return 10000 + size * 100;
}

static void stream_dma_event(void *, u32)
{
    u32 i;
    s_gd.dmaEnded = 1;
    LOG_DEBUG("[hle] stream DMA piece ended (%u queued)", s_gd.dmaQueued);
    holly_raise(HOLLY_GDROM_DMA_END);
    if (s_gd.dmaQueued <= 1 && !s_gd.streamLeft) holly_ext(HOLLY_EXT_GDROM, 1);
    if (s_gd.dmaQueued) s_gd.dmaQueued--;
    for (i = 0; i < s_gd.dmaQueued; i++) s_gd.dmaQueue[i] = s_gd.dmaQueue[i + 1];
    if (s_gd.dmaQueued) sched_after(s_evStreamDma, stream_dma_time(s_gd.dmaQueue[0]));
}

static void write_toc(u32 area, u32 dest)
{
    Disc *d = disc();
    u32 first = 1, last, i, entries[102];
    for (i = 0; i < 102; i++) entries[i] = 0xFFFFFFFFu;
    if (d && !(area == 1 && d->type != DISC_GDROM))
    {
        last = d->trackCount;
        if (area == 1) first = 3;
        else if (d->type == DISC_GDROM && last > 2) last = 2;
        if (first <= last && last <= d->trackCount)
        {
            for (i = first; i <= last; i++)
                entries[i - 1] = ((d->tracks[i - 1].ctrl << 4 | 1) << 24) | d->tracks[i - 1].startFad;
            entries[99] = ((d->tracks[first - 1].ctrl << 4 | 1) << 24) | (first << 16);
            entries[100] = ((d->tracks[last - 1].ctrl << 4 | 1) << 24) | (last << 16);
            entries[101] = ((d->tracks[last - 1].ctrl << 4 | 1) << 24) |
                           (last < d->trackCount ? d->tracks[last - 1].endFad + 1 : d->leadOutFad);
        }
    }
    for (i = 0; i < 102; i++) mem_write32(dest + i * 4, entries[i]);
}

static void write_subcode(u32 format, u32 size, u32 dest)
{
    u8 q[100];
    u32 fad = cdda_fad(), rel, i, n = format == 0 ? 100 : 14;
    DiscTrack *t = disc() ? disc_track_at(disc(), fad) : NULL;
    memset(q, 0, sizeof(q));
    rel = t && fad >= t->startFad ? fad - t->startFad : 0;
    q[1] = (u8)cdda_status();
    q[2] = (u8)(n >> 8);
    q[3] = (u8)n;
    q[4] = (u8)(((t ? t->ctrl : 4) << 4) | 1);
    q[5] = (u8)(t ? t->number : 1);
    q[6] = 1;
    q[7] = (u8)(rel >> 16);
    q[8] = (u8)(rel >> 8);
    q[9] = (u8)rel;
    q[11] = (u8)(fad >> 16);
    q[12] = (u8)(fad >> 8);
    q[13] = (u8)fad;
    if (size < n) n = size;
    for (i = 0; i < n; i++) mem_write8(dest + i, q[i]);
}

static void run_command(void)
{
    u32 *p = s_gd.params;
    switch (s_gd.command)
    {
    case CC_PIOREAD:
        while (p[1] > 0)
        {
            read_sectors(p[2], p[0] & 0xFFFFFF, 1, 0);
            p[0]++;
            p[1]--;
            p[2] += 2048;
            s_gd.result[2] += 2048;
        }
        break;
    case CC_DMAREAD:
        s_gd.dmaFad = p[0] & 0xFFFFFF;
        s_gd.dmaCount = p[1];
        s_gd.dmaAddr = p[2];
        sched_after(s_evDma, 20000 + p[1] * 200000);
        return;
    case CC_GETTOC2:
        write_toc(p[0] & 1, p[1]);
        break;
    case CC_STREAM_DMA:
    case CC_STREAM_PIO:
    case CC_STREAM_DMA_OLD:
    case CC_STREAM_PIO_OLD:
        s_gd.streamFad = p[0] & 0xFFFFFF;
        s_gd.streamOff = 0;
        s_gd.streamLeft = s_gd.streamTotal = p[1] * 2048;
        s_gd.pioPending = 0;
        s_gd.status = GDC_CONTINUE;
        return;
    case CC_REQ_STAT:
    {
        u32 fad = cdda_status() == CDDA_PLAYING || cdda_status() == CDDA_PAUSED ? cdda_fad() : s_gd.curFad;
        DiscTrack *t = disc() ? disc_track_at(disc(), fad) : NULL;
        mem_write32(p[0], (cdda_repeats() << 8) | (cdda_playing() ? 3u : 1u));
        mem_write32(p[1], t ? t->number : 1);
        mem_write32(p[2], ((t ? t->ctrl : 4) << 24) | (1u << 28) | (fad & 0xFFFFFF));
        mem_write32(p[3], 1);
        s_gd.result[2] = 10;
        break;
    }
    case CC_REQ_MODE:
    {
        u32 i;
        for (i = 0; i < 4; i++) mem_write32(p[0] + i * 4, s_gd.mode[i]);
        s_gd.result[2] = 10;
        break;
    }
    case CC_SET_MODE:
        memcpy(s_gd.mode, p, sizeof(s_gd.mode));
        s_gd.result[2] = 10;
        break;
    case CC_GET_VERSION:
    {
        static const char ver[] = "GDC Version 1.10 1999-03-31 ";
        u32 i, n = (u32)sizeof(ver) - 1;
        for (i = 0; i < n; i++) mem_write8(p[0] + i, (u8)(i == n - 1 ? 0x02 : ver[i]));
        break;
    }
    case CC_PLAY:
    {
        Disc *d = disc();
        if (d && p[0] >= 1 && p[0] <= d->trackCount)
        {
            u32 last = p[1] >= p[0] && p[1] <= d->trackCount ? p[1] : p[0];
            cdda_play(d->tracks[p[0] - 1].startFad, d->tracks[last - 1].endFad, p[2] & 15);
        }
        break;
    }
    case CC_PLAY2:
        cdda_play(p[0] & 0xFFFFFF, p[1] & 0xFFFFFF, p[2] & 15);
        break;
    case CC_PAUSE:
        cdda_pause();
        break;
    case CC_RELEASE:
        cdda_resume();
        break;
    case CC_INIT:
    case CC_STOP:
        cdda_stop();
        break;
    case CC_SEEK:
        if (p[0] == 3) cdda_stop();
        else cdda_pause();
        break;
    case CC_GETSCD:
        write_subcode(p[0], p[1], p[2]);
        s_gd.result[2] = p[1];
        break;
    case CC_GETTOC:
    case CC_REQ_SES:
        break;
    default:
        LOG_WARN("[hle] GD-ROM command %u not emulated", s_gd.command);
        break;
    }
    s_gd.status = GDC_COMPLETE;
}

static void poll_wait(void)
{
    static u64 lastPoll;
    static u32 streak;
    u64 now = sched_now_exact(), gap = now - lastPoll;
    int waiting = (s_gd.status == GDC_BUSY && sched_is_scheduled(s_evDma)) ||
                  (s_gd.status == GDC_CONTINUE && !s_gd.pioPending && sched_is_scheduled(s_evStreamDma));
    streak = gap < 1000 ? streak + 1 : 0;
    if ((waiting && gap < 4000) || streak >= 16)
    {
        g_sh4_idle = 1;
        sh4_break_slice();
    }
    lastPoll = now;
}

static void syscall_gdrom(int second)
{
    if (R[6] != 0 && !second)
    {
        R[0] = 0;
        return;
    }
    switch (R[7])
    {
    case 0:
    {
        u32 i;
        if (s_gd.status != GDC_OK)
        {
            R[0] = 0;
            break;
        }
        for (i = 0; i < 4; i++) s_gd.params[i] = R[5] ? mem_read32(R[5] + i * 4) : 0;
        memset(s_gd.result, 0, sizeof(s_gd.result));
        if (s_gd.nextRequest == 0 || s_gd.nextRequest == 0xFFFFFFFFu) s_gd.nextRequest = 1;
        s_gd.lastRequest = s_gd.nextRequest++;
        s_gd.command = R[4];
        s_gd.status = GDC_BUSY;
        R[0] = s_gd.lastRequest;
        break;
    }
    case 1:
        holly_ext(HOLLY_EXT_GDROM, 0);
        poll_wait();
        mem_write32(R[5], s_gd.result[0]);
        mem_write32(R[5] + 4, s_gd.result[1]);
        mem_write32(R[5] + 8, s_gd.result[2]);
        mem_write32(R[5] + 12, s_gd.result[3]);
        if (s_gd.status == GDC_OK || s_gd.status == GDC_BUSY) R[0] = (u32)s_gd.status;
        else if (R[4] != s_gd.lastRequest) R[0] = GDC_OK;
        else if (s_gd.status == GDC_CONTINUE)
        {
            R[0] = s_gd.pioPending ? (u32)GDC_BUSY : (u32)GDC_CONTINUE;
            if (!s_gd.streamLeft && !s_gd.pioPending) s_gd.status = GDC_COMPLETE;
        }
        else
        {
            R[0] = (u32)s_gd.status;
            s_gd.status = GDC_OK;
            s_gd.lastRequest = 0xFFFFFFFFu;
        }
        break;
    case 2:
        holly_ext(HOLLY_EXT_GDROM, 0);
        poll_wait();
        if (s_gd.status == GDC_BUSY && !sched_is_scheduled(s_evDma)) run_command();
        else if (s_gd.status == GDC_CONTINUE && s_gd.pioPending)
        {
            stream_copy(&s_gd.pioDest, &s_gd.pioSize, 0);
            s_gd.pioPending = 0;
            s_gd.result[2] = s_gd.streamTotal - s_gd.streamLeft;
            s_gd.result[3] = 0;
            if (s_gd.pioCallback)
            {
                R[4] = s_gd.pioArg;
                g_sh4.pc = s_gd.pioCallback;
            }
        }
        break;
    case 3:
        memset(&s_gd, 0, sizeof(s_gd));
        s_gd.curFad = 150;
        reset_mode();
        sched_cancel(s_evDma);
        sched_cancel(s_evStreamDma);
        R[0] = 0;
        break;
    case 4:
        poll_wait();
        if (!disc())
        {
            mem_write32(R[4], gdrom_lid_open() ? 6 : 7);
            mem_write32(R[4] + 4, 0);
        }
        else
        {
            mem_write32(R[4], s_gd.status == GDC_BUSY || s_gd.status == GDC_CONTINUE || cdda_playing() ? 3 : 1);
            mem_write32(R[4] + 4, disc()->type == DISC_GDROM ? 0x80 : 0x20);
        }
        R[0] = 0;
        break;
    case 5:
        s_gd.dmaCallback = R[4];
        s_gd.dmaArg = R[5];
        R[0] = 0;
        if (s_gd.dmaCallback && s_gd.dmaEnded)
        {
            s_gd.dmaEnded = 0;
            R[4] = s_gd.dmaArg;
            g_sh4.pc = s_gd.dmaCallback;
        }
        break;
    case 6:
    case 12:
    {
        u32 dest = mem_read32(R[5]), size = mem_read32(R[5] + 4);
        LOG_DEBUG("[hle]   transfer %s to %08X, %u bytes", R[7] == 6 ? "DMA" : "PIO", dest, size);
        if (s_gd.status != GDC_CONTINUE || size > s_gd.streamLeft || (R[7] == 6 && size == 0))
        {
            R[0] = (u32)GDC_ERR;
            break;
        }
        if (R[7] == 12)
        {
            s_gd.pioDest = dest;
            s_gd.pioSize = size;
            s_gd.pioPending = 1;
        }
        else
        {
            u32 left = size;
            stream_copy(&dest, &left, 1);
            s_gd.result[2] = size;
            s_gd.result[3] = s_gd.streamLeft ? 1 : 0;
            s_gd.dmaEnded = 0;
            if (s_gd.dmaQueued < 8) s_gd.dmaQueue[s_gd.dmaQueued++] = size;
            if (!sched_is_scheduled(s_evStreamDma)) sched_after(s_evStreamDma, stream_dma_time(size));
        }
        R[0] = 0;
        break;
    }
    case 7:
        poll_wait();
        mem_write32(R[5], s_gd.streamLeft);
        R[0] = s_gd.status != GDC_CONTINUE && s_gd.dmaQueued ? (u32)GDC_BUSY : 0;
        break;
    case 11:
        s_gd.pioCallback = R[4];
        s_gd.pioArg = R[5];
        R[0] = 0;
        break;
    case 13:
        if (s_gd.status == GDC_CONTINUE)
        {
            mem_write32(R[5], s_gd.streamLeft);
            R[0] = 0;
        }
        else R[0] = (u32)GDC_ERR;
        break;
    case 8:
        sched_cancel(s_evDma);
        sched_cancel(s_evStreamDma);
        s_gd.dmaQueued = 0;
        s_gd.streamLeft = 0;
        s_gd.pioPending = 0;
        if (s_gd.status == GDC_BUSY || s_gd.status == GDC_CONTINUE) s_gd.status = GDC_OK;
        R[0] = 0;
        break;
    case 9:
        s_gd.status = GDC_OK;
        s_gd.lastRequest = 0xFFFFFFFFu;
        R[0] = 0;
        break;
    case 10:
        R[0] = 0;
        break;
    default:
        LOG_WARN("[hle] GD-ROM syscall r7=%u not emulated", R[7]);
        R[0] = (u32)GDC_ERR;
        break;
    }
}

static void write_sysinfo(void)
{
    u32 i;
    for (i = 0; i < 8; i++) mem_write8(SYSINFO_ADDR + i, buf_r8(g_flash, 0x1A056 + i));
    for (i = 0; i < 5; i++) mem_write8(SYSINFO_ADDR + 8 + i, buf_r8(g_flash, 0x1A000 + i));
    for (i = 13; i < 24; i++) mem_write8(SYSINFO_ADDR + i, 0);
}

static void syscall_sysinfo(void)
{
    switch (R[7])
    {
    case 0: write_sysinfo(); R[0] = 0; break;
    case 2: R[0] = R[4] > 9 ? 0xFFFFFFFFu : 704; break;
    case 3: R[0] = SYSINFO_ADDR; break;
    default: R[0] = 0xFFFFFFFFu; break;
    }
}

static void syscall_font(void)
{
    R[0] = R[1] == 0 ? FONT_ADDR : 0;
}

static void syscall_flash(void)
{
    u32 i;
    switch (R[7])
    {
    case 0:
        if (R[4] < 5)
        {
            mem_write32(R[5], s_partitions[R[4]][0]);
            mem_write32(R[5] + 4, s_partitions[R[4]][1]);
            R[0] = 0;
        }
        else R[0] = 0xFFFFFFFFu;
        break;
    case 1:
        hle_probe(R[5], R[6], MMU_WRITE);
        for (i = 0; i < R[6] && R[4] + i < FLASH_SIZE; i++) mem_write8(R[5] + i, buf_r8(g_flash, R[4] + i));
        R[0] = 0;
        break;
    case 2:
        for (i = 0; i < R[6] && R[4] + i < FLASH_SIZE; i++)
            buf_w8(g_flash, R[4] + i, (u8)(buf_r8(g_flash, R[4] + i) & mem_read8(R[5] + i)));
        R[0] = R[6];
        break;
    case 3:
    {
        u32 p;
        R[0] = 0xFFFFFFFFu;
        for (p = 0; p < 5; p++)
            if (s_partitions[p][0] == R[4])
            {
                for (i = 0; i < s_partitions[p][1]; i++) buf_w8(g_flash, R[4] + i, 0xFF);
                R[0] = 0;
            }
        break;
    }
    default:
        R[0] = 0xFFFFFFFFu;
        break;
    }
}

static void syscall_misc(void)
{
    LOG_DEBUG("[hle] misc syscall r4=%u r5=%08X r6=%08X r7=%08X from %08X", R[4], R[5], R[6], R[7], g_sh4.pr);
    switch (R[4])
    {
    case 0:
        holly_set_reg(SB_GDSTARD, 0x0C010000u + s_bootSectors * 2048u);
        R[0] = 0x00C0BEBCu;
        break;
    case 1:
    case 3:
        LOG_INFO("[hle] return to the BIOS menu requested (r4=%u, from %08X)", R[4], g_sh4.pr);
        g_sh4.pc = TRAP_IDLE;
        break;
    case 2:
        R[0] = disc() ? 0 : 0xFFFFFFFFu;
        break;
    default:
        LOG_WARN("[hle] misc syscall r4=%u", R[4]);
        break;
    }
}

static void set_vector(u32 vector, u32 trap)
{
    mem_write32(vector, trap);
    mem_write16(trap, SH4_HLE_TRAP_OPCODE);
}

static void trim_name(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0)) s[--n] = 0;
}

static int load_boot_file(void)
{
    Disc *d = disc();
    u32 base, fad, size, i;
    char name[17];
    u8 *buf, *data;
    if (!d)
    {
        LOG_ERROR("[hle] no disc: nothing to boot");
        return -1;
    }
    base = iso_base_fad(d);
    if (!base)
    {
        LOG_ERROR("[hle] the disc has no data track");
        return -1;
    }
    read_sectors(IP_BIN_ADDR, base, 16, 0);
    for (i = 0; i < 16; i++) name[i] = (char)mem_read8(IP_BIN_ADDR + 0x60 + i);
    name[16] = 0;
    trim_name(name);
    if (mem_read32(IP_BIN_ADDR) != 0x41474553u)
        LOG_WARN("[hle] IP.BIN does not start with SEGA");
    LOG_INFO("[hle] IP.BIN at FAD %u, boot file '%s'", base, name);
    if (!name[0] || iso_find_root_file(d, base, name, &fad, &size) != 0) return -1;
    if (size > RAM_SIZE - 0x10000)
    {
        LOG_ERROR("[hle] boot file too large (%u bytes)", size);
        return -1;
    }
    buf = (u8 *)malloc(size + 2048);
    data = (u8 *)malloc(size + 2048);
    if (!buf || !data)
    {
        free(buf);
        free(data);
        return -1;
    }
    disc_read(d, fad, (size + 2047) / 2048, buf, 2048);
    if (d->type != DISC_GDROM) hle_descramble(buf, data, size);
    else memcpy(data, buf, size);
    {
        u32 skip = 0;
        if (d->type == DISC_GDROM && size > 2048 && (mem_read8(IP_BIN_ADDR + 0x3E) == '1' || !strcmp(name, "0WINCEOS.BIN")))
        {
            for (i = 0; i < 2048; i++) mem_write8(WINCE_TABLE_ADDR + i, data[i]);
            skip = 2048;
            LOG_INFO("[hle] Windows CE disc: load table at %08X", WINCE_TABLE_ADDR);
        }
        for (i = skip; i < size; i++) mem_write8(BOOT_ADDR + i - skip, data[i]);
        s_bootSectors = (size - skip + 2047) / 2048;
    }
    free(buf);
    free(data);
    LOG_INFO("[hle] %s: %u bytes loaded at %08X%s", name, size, BOOT_ADDR, d->type != DISC_GDROM ? " (unscrambled)" : "");
    return 0;
}

static void boot(void)
{
    u32 i;
    for (i = 0; i < 0x10000; i += 4) mem_write32(0x8C000000u + i, 0xFFFFFFFFu);
    set_vector(VEC_SYSINFO, TRAP_SYSINFO);
    set_vector(VEC_FONT, TRAP_FONT);
    set_vector(VEC_FLASH, TRAP_FLASH);
    set_vector(VEC_GDROM, TRAP_GDROM);
    set_vector(VEC_GDROM2, TRAP_GDROM2);
    set_vector(VEC_MISC, TRAP_MISC);
    mem_write16(TRAP_IDLE, SH4_HLE_TRAP_OPCODE);
    write_sysinfo();
    memset(&s_gd, 0, sizeof(s_gd));
    s_gd.curFad = 150;
    reset_mode();

    sh4_set_sr(0x400000F0u);
    g_sh4.t = 1;
    sh4_set_fpscr(0x00040001u);
    R[15] = 0x8D000000u;
    g_sh4.gbr = 0x8C000000u;
    g_sh4.vbr = 0x8C000000u;
    g_sh4.ssr = 0x40000001u;
    g_sh4.spc = 0x8C000776u;
    g_sh4.sgr = 0x8D000000u;
    g_sh4.dbr = 0x8C000010u;
    g_sh4.pr = 0xAC00043Cu;
    g_sh4.fpul = 0;
    g_dmac.dmaor = 0x8201;
    mem_write32(0xA070289Cu, 0x48);
    mem_write32(0xA07028A8u, 0x18);
    mem_write32(0xA07028ACu, 0x50);
    mem_write32(0xA07028B0u, 0x08);
    mem_write32(0x00800000u, 0xEAFFFFFEu);

    if (load_boot_file() != 0)
    {
        s_bootStatus = -1;
        g_sh4.pc = TRAP_IDLE;
        return;
    }
    s_bootStatus = 1;
    g_sh4.pc = 0xAC008300u;
}

static void trap(u32 addr)
{
    u32 a = (addr & 0x1FFFFFFFu) | 0x80000000u, pc = g_sh4.pc;
    switch (a)
    {
    case 0x80000000u: boot(); return;
    case TRAP_IDLE: g_sh4.pc = TRAP_IDLE; g_sh4.sleeping = 1; return;
    }
    LOG_DEBUG("[hle] syscall %08X r4=%08X r5=%08X r6=%08X r7=%08X pr=%08X", a, R[4], R[5], R[6], R[7], g_sh4.pr);
    switch (a)
    {
    case TRAP_SYSINFO: syscall_sysinfo(); break;
    case TRAP_FONT: syscall_font(); break;
    case TRAP_FLASH: syscall_flash(); break;
    case TRAP_GDROM: syscall_gdrom(0); break;
    case TRAP_GDROM2: syscall_gdrom(1); break;
    case TRAP_MISC: syscall_misc(); break;
    default:
        LOG_ERROR("[hle] unknown trap at %08X", addr);
        break;
    }
    LOG_DEBUG("[hle]   -> r0 %08X, status %d, stream left %u", R[0], s_gd.status, s_gd.streamLeft);
    if (g_sh4.pc == pc) g_sh4.pc = g_sh4.pr;
}

void hle_bios_install(int region, int language)
{
    s_region = region >= 0 && region <= 2 ? region : HLE_REGION_USA;
    s_language = language >= HLE_LANG_JAPANESE && language <= HLE_LANG_ITALIAN ? language : HLE_LANG_ENGLISH;
    memset(g_bios, 0, BIOS_SIZE);
    buf_w16(g_bios, 0, SH4_HLE_TRAP_OPCODE);
    make_flash();
    g_sh4_hle_trap = trap;
    s_evDma = sched_register("hle_gd_dma", dma_event, NULL);
    s_evStreamDma = sched_register("hle_gd_stream_dma", stream_dma_event, NULL);
    s_bootStatus = 0;
}

void hle_bios_reset(void)
{
    memset(&s_gd, 0, sizeof(s_gd));
    reset_mode();
    s_bootStatus = 0;
}

int hle_bios_boot_status(void)
{
    return s_bootStatus;
}

void hle_bios_state(StateIO *io)
{
    u32 head = (u32)offsetof(GdHle, streamFad);
    state_tag(io, "HLE ");
    state_io(io, &s_gd, head);
    if (io->loading && io->buf && io->pos + 16 <= io->size && !memcmp(io->buf + io->pos + 12, "END ", 4))
        memset((u8 *)&s_gd + head, 0, sizeof(s_gd) - head);
    else
        state_io(io, (u8 *)&s_gd + head, (u32)sizeof(s_gd) - head);
    STATE(io, s_bootStatus);
}
