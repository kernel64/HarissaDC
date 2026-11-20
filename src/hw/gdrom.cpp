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
static int s_dmaArmed;
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
        if (s_dmaArmed)
        {
            s_dmaArmed = 0;
            gdrom_dma_start();
        }
        return;
    }
    pio_next_chunk();
}

static const u8 s_reply71[1012] = {
    0x96,0x0B,0x45,0xF0,0x7E,0xFF,0x3D,0x06,0x4D,0x7D,0x10,0xBF,0x07,0x00,0x73,0xCF,0x9C,0x00,0xBC,0x0C,
    0x1C,0xAF,0x1C,0x30,0xE7,0xA7,0x03,0xA8,0x98,0x00,0xBD,0x0F,0xBD,0x5B,0xAA,0x50,0x23,0x39,0x31,0x10,
    0x0E,0x69,0x13,0xE5,0x00,0xD2,0x0D,0x66,0x54,0xBF,0x5F,0xFD,0x37,0x74,0xF4,0x5B,0x22,0x00,0xC6,0x09,
    0x0F,0xCA,0x93,0xE8,0xA4,0xAB,0x00,0x61,0x0E,0x2E,0xE1,0x4B,0x76,0x8B,0x6A,0xA5,0x9C,0xE6,0x23,0xC4,
    0x00,0x4B,0x06,0x1B,0x91,0x01,0x00,0xE2,0x0D,0xCF,0xCA,0x38,0x3A,0xB9,0xE7,0x91,0xE5,0xEF,0x4B,0x00,
    0xD6,0x09,0xD3,0x68,0x3E,0xC4,0xAF,0x2D,0x00,0x2A,0x0D,0xF9,0xFC,0x78,0xED,0xAE,0x99,0xB3,0x32,0x5A,
    0xE7,0x00,0x4C,0x0A,0x22,0x97,0x5B,0x82,0x06,0x7A,0x4C,0x00,0x42,0x0E,0x57,0x78,0x46,0xF5,0x20,0xFC,
    0x6B,0xCB,0x01,0x5B,0x86,0x00,0xE4,0x0E,0xB2,0x26,0xCD,0x71,0xE3,0xA5,0x33,0x06,0x8E,0x9A,0x50,0x00,
    0x07,0x07,0xF5,0x34,0xEF,0xE6,0x00,0x32,0x0F,0x13,0x41,0x59,0x56,0x0F,0x02,0x38,0x2A,0x64,0x2A,0x07,
    0x3E,0x00,0x52,0x11,0x2A,0x1D,0x5F,0x76,0x66,0xA0,0xB2,0x2F,0x97,0xC7,0x5E,0x6E,0x52,0xE2,0x00,0x58,
    0x09,0xCA,0x89,0xA5,0xDF,0x0A,0xDE,0x00,0x50,0x06,0x49,0xB8,0xB4,0x00,0x77,0x05,0x24,0xE8,0x00,0xBB,
    0x0C,0x91,0x89,0xA2,0x8B,0x62,0xDE,0x6A,0xC6,0x60,0x00,0xE7,0x0F,0x0F,0x11,0x96,0x55,0xD2,0xBF,0xE6,
    0x48,0x0B,0x5C,0xAB,0xDC,0x00,0xBA,0x0A,0x30,0xD7,0x48,0x0E,0x78,0x63,0x0C,0x00,0xD2,0x0D,0xFB,0x8A,
    0xA3,0xFE,0xF8,0x3A,0xDD,0x88,0xA9,0x4B,0x00,0xA2,0x0A,0x75,0x5D,0x0D,0x37,0x24,0xC5,0x9D,0x00,0xF7,
    0x0B,0x25,0xEF,0xDB,0x41,0xE0,0x52,0x3E,0x4E,0x00,0xB7,0x03,0x00,0xE5,0x11,0xB9,0xDE,0x5A,0x57,0xCF,
    0xB9,0x1A,0xFC,0x7F,0x26,0xEE,0x7B,0xCD,0x2B,0x00,0x4B,0x08,0xB8,0x09,0x70,0x6A,0x9F,0x00,0x4B,0x11,
    0x8C,0x15,0x87,0xA3,0x05,0x4F,0x37,0x8E,0x63,0xDE,0xEF,0x39,0xFC,0x4B,0x00,0xAB,0x10,0x0B,0x91,0xAA,
    0x0F,0xE1,0xE9,0xAE,0x69,0x3A,0xF8,0x03,0x69,0xD2,0x00,0xE2,0x07,0xC1,0x5C,0x3D,0x82,0x00,0xA9,0x08,
    0x68,0xC4,0xAD,0x2E,0xD1,0x00,0xF7,0x0E,0xC6,0x47,0xC8,0xCD,0x8E,0x7C,0x00,0x5C,0x95,0xB9,0xF4,0x00,
    0xE3,0x04,0x5B,0x00,0x74,0x07,0x65,0xC7,0x84,0x8E,0x00,0xC6,0x07,0x61,0x80,0x44,0x3F,0x00,0xC8,0x0E,
    0x72,0x78,0x47,0xD3,0xC2,0x4D,0xAF,0xC0,0x54,0x13,0x31,0x00,0xF7,0x0D,0x48,0xD8,0xE2,0x92,0x9F,0x7F,
    0x2F,0x44,0x68,0x33,0x00,0x0D,0x10,0xAB,0xFE,0xEA,0x8E,0x19,0x81,0xF8,0x6F,0x7C,0xDE,0xE1,0xB3,0x06,
    0x00,0x4D,0x11,0x66,0xAE,0x4C,0xF9,0xB7,0x2F,0xEE,0xB0,0x8E,0x7E,0xE1,0x8D,0x95,0x6F,0x00,0xF4,0x0D,
    0x88,0x9D,0xCA,0xE3,0xC4,0xB2,0x47,0xBB,0xA0,0x69,0x00,0xF3,0x0B,0x48,0x17,0x41,0x64,0xA0,0x0E,0x71,
    0x82,0x00,0x34,0x1E,0x18,0x4D,0x85,0x80,0x4C,0xA9,0x0B,0x66,0x9B,0x75,0x13,0x61,0x70,0x27,0x81,0x7A,
    0x02,0xCD,0x57,0xAB,0xDF,0x02,0x93,0x52,0x83,0xDF,0x48,0xA8,0xA6,0x9E,0x74,0x6F,0x89,0x03,0x28,0x25,
    0x52,0x96,0xFF,0x67,0x7A,0xD8,0x3C,0xB1,0x2C,0x46,0x84,0xEF,0xE1,0xC1,0xC6,0xC9,0xDC,0x96,0xAA,0xA9,
    0xC4,0x82,0x58,0x27,0x57,0x75,0x67,0x34,0xFB,0x3B,0x25,0xBF,0xFB,0x3B,0xF6,0x13,0xEC,0x96,0xE5,0x16,
    0x26,0xFD,0xA8,0xDA,0x1B,0xC6,0x50,0x7F,0x47,0xFF,0x08,0x55,0x08,0xED,0x00,0x93,0x9B,0xC4,0x71,0x67,
    0xEC,0xA6,0xCC,0x16,0x20,0x87,0x47,0x07,0xA6,0x00,0x79,0x5D,0x4F,0xAB,0xA1,0x6F,0x7A,0x6B,0x27,0xC4,
    0xDA,0xA3,0xC3,0x94,0x4F,0x7F,0xF3,0xE5,0x1B,0x6F,0xCC,0xE5,0xF0,0xE5,0x9D,0xC9,0xAE,0xFD,0x39,0xAC,
    0x4C,0xE5,0x58,0x83,0x25,0x65,0x92,0x74,0x9E,0x81,0xA0,0xB6,0xA9,0x02,0x9B,0x07,0xB6,0xE7,0x79,0x57,
    0xD9,0x4A,0xCE,0xFA,0xB4,0x94,0x05,0xCC,0x86,0x3C,0xDD,0x06,0xCD,0xA6,0x24,0x24,0xFA,0xC1,0xF9,0x48,
    0xC9,0x0C,0x6C,0xC4,0x96,0x82,0x17,0xF6,0x31,0x09,0xC4,0xE2,0x77,0xFD,0xCF,0x46,0x18,0xB2,0x5F,0x01,
    0x6B,0xD1,0x7B,0x56,0xB8,0x94,0x4A,0xE5,0x6C,0x19,0xF0,0xC0,0xB6,0x70,0x93,0xF7,0xD3,0xD1,0x2B,0x6E,
    0x7C,0x53,0x6D,0x85,0xD1,0x0C,0x8B,0x77,0xEE,0x90,0xDA,0x15,0x55,0xE0,0x58,0x09,0x56,0xFC,0x31,0x9F,
    0xAF,0x46,0xCB,0xC3,0x8D,0x71,0x75,0xF2,0x2C,0xC3,0xBB,0xA1,0xC4,0xCF,0x27,0x56,0x7C,0x9B,0xFE,0xAF,
    0x3E,0x4E,0xB4,0xCD,0x6A,0xAA,0xF5,0xF3,0xE3,0x22,0x82,0xE1,0xA5,0x68,0xB3,0xDB,0x8F,0x9E,0x5E,0x7B,
    0x90,0xF0,0x79,0x3F,0x52,0x8C,0x61,0x88,0x76,0xAE,0x14,0x63,0x19,0x0F,0x1D,0xCE,0xA1,0x63,0x10,0xB2,
    0xE2,0xD7,0x94,0xB1,0x33,0xCB,0x28,0x85,0x7D,0x9B,0xF5,0xF4,0x25,0x50,0x9B,0xDB,0x35,0xA5,0xB0,0x9C,
    0x09,0x92,0xE3,0x31,0x40,0xAB,0x4D,0xF4,0x35,0xE8,0xB3,0x0A,0x21,0xC3,0x86,0x9C,0xCB,0x29,0xA4,0x77,
    0x57,0xBC,0xD8,0xDA,0xA5,0x82,0x80,0xE8,0xCF,0x72,0x81,0xAD,0x2E,0x28,0xFF,0xD8,0xB6,0xD1,0x2B,0x97,
    0x00,0xFF,0xE1,0x06,0x44,0x39,0x1C,0x4B,0xAB,0x19,0x5B,0x4D,0xD6,0x3E,0x1B,0x5C,0x64,0xBB,0x32,0x68,
    0xF5,0x7C,0xC9,0x9E,0xE8,0xB4,0x29,0x1B,0x7F,0x4D,0x80,0x80,0x7E,0x8B,0x1C,0x0A,0xE6,0x9A,0xBF,0x49,
    0x1E,0xC5,0xB6,0x67,0x7D,0x05,0xE4,0x90,0x40,0x4B,0xAF,0x9B,0x52,0xDE,0x17,0x80,0x81,0x56,0xEA,0x3A,
    0x53,0x82,0x8C,0x62,0xFB,0x96,0x97,0x6F,0xC1,0x16,0x78,0xD4,0x7B,0xE7,0xB9,0x5A,0x2A,0xEB,0x87,0x68,
    0x33,0xD3,0x31,0x45,0xFA,0xFE,0xF4,0x1C,0x90,0x86,0x73,0x77,0xD9,0xA9,0xD1,0x4A,0x4A,0xCF,0xAE,0x23,
    0xDB,0xF9,0x09,0xD8,0x18,0xDC,0x6A,0x0D,0xE4,0x19,0x8C,0x65,0xC6,0x64,0xC7,0xDC,0xA9,0xE3,0x91,0xB1,
    0x4C,0xC8,0xC1,0x9E,0x3B,0x7F,0xCB,0xA3,0xCF,0xDD,0xF0,0x1D,0x07,0x6E,0xDC,0xCE,0x0D,0xCD,0x7E,0x1E,
    0x55,0x11,0x8B,0xDF,0x3A,0xAB,0xB6,0x3B,0x6E,0x52,0x7F,0xA7,0x00,0xD1,0x33,0xBE,0xF2,0x9B,0xFC,0x4A,
    0xCF,0x9D,0x8F,0xC6,0xC4,0x7B,0xDA,0xE7,0x2A,0x1C,0x26,0x6E,
};

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
            if (s_disc->sessionCount) out[2] = (u8)s_disc->sessionCount;
            else out[2] = (u8)(s_disc->type == DISC_GDROM && s_disc->trackCount > 2 ? 2 : 1);
            put_fad(out + 3, s_disc->leadOutFad);
        }
        else
        {
            u32 track = (session == 2 && s_disc->trackCount > 2) ? 3 : 1;
            if (s_disc->sessionCount)
                track = s_disc->sessionFirstTrack[(session <= s_disc->sessionCount ? session : s_disc->sessionCount) - 1];
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
        if (s_disc) s_discState = GD_PAUSE;
        data_out(s_reply71, sizeof(s_reply71));
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
    s_dmaArmed = 0;
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
    if (s_phase == PH_PACKET || (s_phase == PH_IDLE && (s_status & ST_BSY)))
    {
        s_dmaArmed = 1;
        return;
    }
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
    s_dmaArmed = 0;
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
