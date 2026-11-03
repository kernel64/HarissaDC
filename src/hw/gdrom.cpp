// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "gdrom.h"

#include <string.h>

#include "holly.h"
#include "../core/log.h"
#include "../core/state.h"
#include "../core/sched.h"
#include "../disc/disc.h"
#include "../mem/mem.h"

#define GD_ALTSTAT_DEVCTRL 0x005F7018
#define GD_DATA            0x005F7080
#define GD_ERROR_FEATURES  0x005F7084
#define GD_IREASON_SECCNT  0x005F7088
#define GD_SECNUM          0x005F708C
#define GD_BYTECOUNT_LO    0x005F7090
#define GD_BYTECOUNT_HI    0x005F7094
#define GD_DRIVESEL        0x005F7098
#define GD_STATUS_COMMAND  0x005F709C

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DSC  0x10
#define ST_DRQ  0x08
#define ST_CHK  0x01

#define IR_COD 1
#define IR_IO  2

#define GD_BUSY    0
#define GD_PAUSE   1
#define GD_STANDBY 2
#define GD_OPEN    6
#define GD_NODISC  7

enum Phase { PH_IDLE, PH_PACKET, PH_DATA_OUT, PH_DATA_IN, PH_DMA };

static Disc *s_disc;
static u8 s_status, s_error, s_features, s_ireason, s_driveSel, s_devCtrl;
static u16 s_byteCount;
static u8 s_discState;
static Disc *s_nextDisc;
static int s_lidOpen, s_unitAttention, s_evSwap;
static int s_phase;

static u8 s_packet[12];
static u32 s_packetPos;

#define BUF_SIZE (64u * 1024u)
static u8 s_buf[BUF_SIZE];
static u32 s_bufLen, s_bufPos;
static u32 s_dataInOffset;

static u32 s_readFad, s_readLeft, s_readFormat;
static int s_readDma;
static u32 s_lastFad = 150;

static u8 s_mode[32];
static u8 s_senseKey, s_senseAsc;
static int s_evCmd, s_evDma;
static int s_pending;

static void intrq(void)
{
    LOG_DEBUG("[gdrom] INTRQ (status %02X, %s)", s_status, s_devCtrl & 2 ? "masked by nIEN" : "raised");
    if (!(s_devCtrl & 2)) holly_ext(HOLLY_EXT_GDROM, 1);
}

static void complete(void)
{
    s_phase = PH_IDLE;
    s_status = ST_DRDY | ST_DSC;
    s_ireason = IR_IO | IR_COD;
    intrq();
}

static void fail(u8 key, u8 asc)
{
    s_senseKey = key;
    s_senseAsc = asc;
    s_phase = PH_IDLE;
    s_error = (u8)(key << 4);
    s_status = ST_DRDY | ST_CHK;
    s_ireason = IR_IO | IR_COD;
    intrq();
}

static void data_out(const u8 *data, u32 len)
{
    if (len > BUF_SIZE) len = BUF_SIZE;
    if (data != s_buf) memcpy(s_buf, data, len);
    s_bufLen = len;
    s_bufPos = 0;
    if (len == 0)
    {
        complete();
        return;
    }
    s_phase = PH_DATA_OUT;
    s_byteCount = (u16)len;
    s_status = ST_DRDY | ST_DSC | ST_DRQ;
    s_ireason = IR_IO;
    intrq();
}

static void data_in(u32 offset, u32 len)
{
    s_dataInOffset = offset;
    s_bufLen = len;
    s_bufPos = 0;
    s_phase = PH_DATA_IN;
    s_byteCount = (u16)len;
    s_status = ST_DRDY | ST_DSC | ST_DRQ;
    s_ireason = 0;
    intrq();
}

static u8 disc_format(void)
{
    if (!s_disc) return 0;
    return s_disc->type == DISC_GDROM ? 8 : (u8)(s_disc->type >> 4);
}

static u8 secnum(void)
{
    return (u8)(s_discState | (disc_format() << 4));
}

static void put_fad(u8 *p, u32 fad)
{
    p[0] = (u8)(fad >> 16);
    p[1] = (u8)(fad >> 8);
    p[2] = (u8)fad;
}

static void toc_entry(u8 *p, u32 ctrl, u32 fad)
{
    p[0] = (u8)((ctrl << 4) | 1);
    put_fad(p + 1, fad);
}

static u32 build_toc(u8 *toc, int highDensity)
{
    u32 first = 1, last, i;
    memset(toc, 0xFF, 408);
    if (!s_disc) return 408;
    last = s_disc->trackCount;
    if (highDensity)
    {
        if (s_disc->type != DISC_GDROM || last < 3) return 408;
        first = 3;
    }
    else if (s_disc->type == DISC_GDROM && last > 2)
        last = 2;
    for (i = first; i <= last; i++) toc_entry(toc + (i - 1) * 4, s_disc->tracks[i - 1].ctrl, s_disc->tracks[i - 1].startFad);
    toc_entry(toc + 99 * 4, s_disc->tracks[first - 1].ctrl, first << 16);
    toc_entry(toc + 100 * 4, s_disc->tracks[last - 1].ctrl, last << 16);
    toc_entry(toc + 101 * 4, s_disc->tracks[last - 1].ctrl,
              last < s_disc->trackCount ? s_disc->tracks[last - 1].endFad + 1 : s_disc->leadOutFad);
    return 408;
}

static u32 fill_sectors(void)
{
    u32 n = BUF_SIZE / s_readFormat;
    if (n > 16) n = 16;
    if (n > s_readLeft) n = s_readLeft;
    if (n == 0) return 0;
    disc_read(s_disc, s_readFad, n, s_buf, s_readFormat);
    s_readFad += n;
    s_readLeft -= n;
    s_lastFad = s_readFad - 1;
    s_bufLen = n * s_readFormat;
    s_bufPos = 0;
    return n;
}

static void pio_next_chunk(void)
{
    if (!fill_sectors())
    {
        complete();
        return;
    }
    data_out(s_buf, s_bufLen);
}

static void cmd_read(const u8 *p)
{
    u32 start = ((u32)p[2] << 16) | ((u32)p[3] << 8) | p[4];
    u32 count = ((u32)p[8] << 16) | ((u32)p[9] << 8) | p[10];
    u32 dataSel = p[1] >> 4;
    if (p[1] & 1)
        start = ((start >> 16) * 60 + ((start >> 8) & 0xFF)) * 75 + (start & 0xFF);
    if (!s_disc)
    {
        fail(2, 0x3A);
        return;
    }
    s_readFad = start;
    s_readLeft = count;
    s_readFormat = dataSel == 0xF ? 2352 : 2048;
    s_readDma = s_features & 1;
    s_discState = GD_PAUSE;
    if (s_readDma)
    {
        s_bufLen = s_bufPos = 0;
        s_phase = PH_DMA;
        s_status = ST_DRDY | ST_DSC | ST_DRQ;
        s_ireason = IR_IO;
        return;
    }
    pio_next_chunk();
}

static void run_packet(void)
{
    const u8 *p = s_packet;
    u8 out[408];
    LOG_DEBUG("[gdrom] SPI %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
              p[8], p[9], p[10], p[11]);
    if (s_unitAttention && p[0] != 0x10 && p[0] != 0x11 && p[0] != 0x13)
    {
        s_unitAttention = 0;
        fail(6, 0x28);
        return;
    }
    switch (p[0])
    {
    case 0x00:
        complete();
        return;
    case 0x10:
    {
        DiscTrack *t = s_disc ? disc_track_at(s_disc, s_lastFad) : NULL;
        u32 off = p[2] < 10 ? p[2] : 10, len = p[4];
        memset(out, 0, 10);
        out[0] = s_discState;
        out[1] = (u8)(disc_format() << 4);
        out[2] = (u8)(((t ? t->ctrl : 4) << 4) | 1);
        out[3] = (u8)(t ? t->number : 1);
        out[4] = 1;
        put_fad(out + 5, s_lastFad);
        if (off + len > 10) len = 10 - off;
        data_out(out + off, len);
        return;
    }
    case 0x11:
    {
        u32 off = p[2] < 32 ? p[2] : 32, len = p[4];
        if (off + len > 32) len = 32 - off;
        data_out(s_mode + off, len);
        return;
    }
    case 0x12:
    {
        u32 off = p[2] < 32 ? p[2] : 32, len = p[4];
        if (off + len > 32) len = 32 - off;
        data_in(off, len);
        return;
    }
    case 0x13:
    {
        u32 len = p[4] < 10 ? p[4] : 10;
        memset(out, 0, 10);
        out[0] = 0xF0;
        out[2] = s_senseKey;
        out[8] = s_senseAsc;
        s_senseKey = s_senseAsc = 0;
        data_out(out, len);
        return;
    }
    case 0x14:
    {
        u32 len = ((u32)p[3] << 8) | p[4];
        if (!s_disc) { fail(2, 0x3A); return; }
        build_toc(out, p[1] & 1);
        data_out(out, len < 408 ? len : 408);
        return;
    }
    case 0x15:
    {
        u32 session = p[2], len = p[4] < 6 ? p[4] : 6;
        if (!s_disc) { fail(2, 0x3A); return; }
        memset(out, 0, 6);
        out[0] = s_discState;
        if (session == 0)
        {
            out[2] = (u8)(s_disc->type == DISC_GDROM && s_disc->trackCount > 2 ? 2 : 1);
            put_fad(out + 3, s_disc->leadOutFad);
        }
        else
        {
            u32 track = (session == 2 && s_disc->trackCount > 2) ? 3 : 1;
            out[2] = (u8)track;
            put_fad(out + 3, s_disc->tracks[track - 1].startFad);
        }
        data_out(out, len);
        return;
    }
    case 0x16:
    case 0x20:
    case 0x21:
    case 0x22:
        if (s_disc) s_discState = GD_PAUSE;
        complete();
        return;
    case 0x30:
    case 0x31:
        cmd_read(p);
        return;
    case 0x40:
    {
        u32 len = ((u32)p[3] << 8) | p[4];
        memset(out, 0, 100);
        out[1] = 0x15;
        out[3] = (p[1] & 15) == 0 ? 100 : 14;
        data_out(out, len < out[3] ? len : out[3]);
        return;
    }
    case 0x70:
        complete();
        return;
    case 0x71:
        LOG_WARN("[gdrom] security check (0x71) not emulated");
        fail(5, 0x20);
        return;
    default:
        LOG_WARN("[gdrom] unknown SPI command %02X", p[0]);
        fail(5, 0x20);
        return;
    }
}

static void cmd_event(void *, u32)
{
    if (s_pending == 0) run_packet();
    else pio_next_chunk();
}

static void ata_command(u8 cmd)
{
    LOG_DEBUG("[gdrom] ATA %02X (features %02X)", cmd, s_features);
    switch (cmd)
    {
    case 0xA0:
        s_packetPos = 0;
        s_phase = PH_PACKET;
        s_status = ST_DRDY | ST_DSC | ST_DRQ;
        s_ireason = IR_COD;
        return;
    case 0x08:
        s_phase = PH_IDLE;
        s_status = ST_DRDY | ST_DSC;
        s_error = 0;
        return;
    case 0xEF:
        complete();
        return;
    case 0xA1:
    {
        u8 id[80];
        memset(id, 0, sizeof(id));
        memcpy(id, s_mode, 32);
        data_out(id, sizeof(id));
        return;
    }
    default:
        LOG_WARN("[gdrom] unknown ATA command %02X", cmd);
        s_error = 0x04;
        s_status = ST_DRDY | ST_CHK;
        s_ireason = IR_IO | IR_COD;
        intrq();
        return;
    }
}

u32 gdrom_read(u32 addr, int size)
{
    switch (addr & ~3u)
    {
    case GD_ALTSTAT_DEVCTRL: return s_status;
    case GD_STATUS_COMMAND:
        holly_ext(HOLLY_EXT_GDROM, 0);
        return s_status;
    case GD_DATA:
        if (s_phase == PH_DATA_OUT && s_bufPos < s_bufLen)
        {
            u32 v = s_buf[s_bufPos] | ((u32)(s_bufPos + 1 < s_bufLen ? s_buf[s_bufPos + 1] : 0) << 8);
            s_bufPos += 2;
            if (s_bufPos >= s_bufLen)
            {
                if (s_readLeft && !s_readDma)
                {
                    s_status = ST_BSY;
                    s_pending = 1;
                    sched_after(s_evCmd, 10000);
                }
                else
                    complete();
            }
            return size == 4 ? v : v & 0xFFFF;
        }
        return 0;
    case GD_ERROR_FEATURES: return s_error;
    case GD_IREASON_SECCNT: return s_ireason;
    case GD_SECNUM: return secnum();
    case GD_BYTECOUNT_LO: return s_byteCount & 0xFF;
    case GD_BYTECOUNT_HI: return s_byteCount >> 8;
    case GD_DRIVESEL: return s_driveSel;
    }
    return 0;
}

void gdrom_write(u32 addr, u32 value, int size)
{
    (void)size;
    switch (addr & ~3u)
    {
    case GD_ALTSTAT_DEVCTRL:
        s_devCtrl = (u8)value;
        return;
    case GD_STATUS_COMMAND:
        ata_command((u8)value);
        return;
    case GD_DATA:
        if (s_phase == PH_PACKET)
        {
            if (s_packetPos < 12) s_packet[s_packetPos++] = (u8)value;
            if (s_packetPos < 12) s_packet[s_packetPos++] = (u8)(value >> 8);
            if (s_packetPos >= 12)
            {
                s_phase = PH_IDLE;
                s_status = ST_BSY;
                s_pending = 0;
                sched_after(s_evCmd, 2000);
            }
        }
        else if (s_phase == PH_DATA_IN)
        {
            if (s_bufPos < s_bufLen) s_mode[(s_dataInOffset + s_bufPos++) & 31] = (u8)value;
            if (s_bufPos < s_bufLen) s_mode[(s_dataInOffset + s_bufPos++) & 31] = (u8)(value >> 8);
            if (s_bufPos >= s_bufLen) complete();
        }
        return;
    case GD_ERROR_FEATURES: s_features = (u8)value; return;
    case GD_IREASON_SECCNT: return;
    case GD_SECNUM: return;
    case GD_BYTECOUNT_LO: s_byteCount = (u16)((s_byteCount & 0xFF00) | (value & 0xFF)); return;
    case GD_BYTECOUNT_HI: s_byteCount = (u16)((s_byteCount & 0x00FF) | ((value & 0xFF) << 8)); return;
    case GD_DRIVESEL: s_driveSel = (u8)value; return;
    }
}

void gdrom_dma_start(void)
{
    u32 dst = holly_reg(SB_GDSTAR) & 0x1FFFFFE0u, len = holly_reg(SB_GDLEN), done = 0;
    if (s_phase != PH_DMA || !(holly_reg(SB_GDDIR) & 1))
    {
        LOG_WARN("[gdrom] DMA start without a DMA read pending");
        sched_after(s_evDma, 1000);
        return;
    }
    while (done < len)
    {
        u32 chunk;
        if (s_bufPos >= s_bufLen && !fill_sectors()) break;
        chunk = s_bufLen - s_bufPos;
        if (chunk > len - done) chunk = len - done;
        {
            u32 i;
            for (i = 0; i + 4 <= chunk; i += 4)
            {
                const u8 *b = s_buf + s_bufPos + i;
                mem_write32(dst + done + i, b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24));
            }
        }
        s_bufPos += chunk;
        done += chunk;
    }
    holly_set_reg(SB_GDSTARD, dst + done);
    holly_set_reg(SB_GDLEND, done);
    sched_after(s_evDma, 2000 + done / 8);
}

static void dma_done(void *, u32)
{
    holly_set_reg(SB_GDST, 0);
    holly_raise(HOLLY_GDROM_DMA_END);
    if (s_phase == PH_DMA && s_readLeft == 0 && s_bufPos >= s_bufLen) complete();
}

static void init_mode_table(void)
{
    memset(s_mode, 0, sizeof(s_mode));
    s_mode[4] = 0x00;
    s_mode[5] = 0xB4;
    s_mode[6] = 0x19;
    s_mode[9] = 0x08;
    memcpy(s_mode + 10, "SE      ", 8);
    memcpy(s_mode + 18, "Rev 6.43", 8);
    memcpy(s_mode + 26, "990408", 6);
}

static void swap_done(void *, u32)
{
    s_disc = s_nextDisc;
    s_nextDisc = NULL;
    s_lidOpen = 0;
    s_discState = s_disc ? GD_STANDBY : GD_NODISC;
    s_lastFad = 150;
    s_unitAttention = 1;
}

void gdrom_init(void)
{
    s_evSwap = sched_register("gdrom_swap", swap_done, NULL);
    s_evCmd = sched_register("gdrom_cmd", cmd_event, NULL);
    s_evDma = sched_register("gdrom_dma", dma_done, NULL);
}

void gdrom_reset(void)
{
    s_status = ST_DRDY | ST_DSC;
    s_error = s_features = s_driveSel = s_devCtrl = 0;
    s_ireason = IR_IO | IR_COD;
    s_byteCount = 0;
    s_phase = PH_IDLE;
    s_bufLen = s_bufPos = 0;
    s_readLeft = 0;
    s_senseKey = s_senseAsc = 0;
    s_discState = s_disc ? GD_STANDBY : GD_NODISC;
    init_mode_table();
    holly_ext(HOLLY_EXT_GDROM, 0);
}

Disc *gdrom_disc(void)
{
    return s_disc;
}

void gdrom_swap_disc(Disc *disc)
{
    if (s_disc && s_disc != disc) disc_close(s_disc);
    if (s_nextDisc && s_nextDisc != disc) disc_close(s_nextDisc);
    s_disc = NULL;
    s_nextDisc = disc;
    s_lidOpen = 1;
    s_discState = GD_OPEN;
    sched_cancel(s_evSwap);
    sched_after(s_evSwap, SH4_CLOCK_HZ / 2 * 3);
}

int gdrom_lid_open(void)
{
    return s_lidOpen;
}

void gdrom_set_disc(Disc *disc)
{
    sched_cancel(s_evSwap);
    if (s_nextDisc && s_nextDisc != disc) disc_close(s_nextDisc);
    s_nextDisc = NULL;
    s_lidOpen = 0;
    s_unitAttention = 0;
    if (s_disc && s_disc != disc) disc_close(s_disc);
    s_disc = disc;
    s_discState = disc ? GD_STANDBY : GD_NODISC;
    s_lastFad = 150;
}

void gdrom_state(StateIO *io)
{
    state_tag(io, "GDRM");
    STATE(io, s_status); STATE(io, s_error); STATE(io, s_features); STATE(io, s_ireason); STATE(io, s_driveSel);
    STATE(io, s_devCtrl); STATE(io, s_byteCount); STATE(io, s_discState); STATE(io, s_lidOpen); STATE(io, s_unitAttention);
    STATE(io, s_phase); STATE(io, s_packet); STATE(io, s_packetPos); STATE(io, s_buf); STATE(io, s_bufLen); STATE(io, s_bufPos);
    STATE(io, s_dataInOffset); STATE(io, s_readFad); STATE(io, s_readLeft); STATE(io, s_readFormat); STATE(io, s_readDma);
    STATE(io, s_lastFad); STATE(io, s_mode); STATE(io, s_senseKey); STATE(io, s_senseAsc); STATE(io, s_pending);
    if (io->loading)
    {
        if (s_nextDisc)
        {
            s_disc = s_nextDisc;
            s_nextDisc = NULL;
        }
        sched_cancel(s_evSwap);
        if (s_lidOpen)
        {
            s_lidOpen = 0;
            s_discState = s_disc ? GD_STANDBY : GD_NODISC;
        }
    }
}
