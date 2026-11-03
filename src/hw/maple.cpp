// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "maple.h"

#include <string.h>

#include "holly.h"
#include "pvr.h"
#include "../core/log.h"
#include "../core/sched.h"
#include "../mem/mem.h"

#define MAPLE_CMD_DEVICE_REQUEST 1
#define MAPLE_CMD_ALL_STATUS     2
#define MAPLE_CMD_RESET          3
#define MAPLE_CMD_SHUTDOWN       4
#define MAPLE_CMD_GET_CONDITION  9
#define MAPLE_CMD_GET_MEDIA_INFO 10
#define MAPLE_CMD_BLOCK_READ     11
#define MAPLE_CMD_BLOCK_WRITE    12
#define MAPLE_CMD_GET_LAST_ERROR 13
#define MAPLE_CMD_SET_CONDITION  14
#define MAPLE_RESP_DEVICE_INFO   5
#define MAPLE_RESP_ALL_STATUS    6
#define MAPLE_RESP_ACK           7
#define MAPLE_RESP_DATA          8
#define MAPLE_RESP_UNKNOWN_CMD   0xFD
#define MAPLE_RESP_FILE_ERROR    0xFB
#define MAPLE_FUNC_CONTROLLER    0x01000000u
#define MAPLE_FUNC_STORAGE       0x02000000u
#define MAPLE_FUNC_LCD           0x04000000u
#define MAPLE_FUNC_CLOCK         0x08000000u
#define MAPLE_FUNC_VIBRATION     0x00010000u
#define MAPLE_FUNC_LIGHT_GUN     0x80000000u

#define VMU_BLOCKS     256
#define VMU_ROOT       255
#define VMU_FAT        254
#define VMU_DIR        253
#define VMU_DIR_BLOCKS 13
#define VMU_USER       200
#define FAT_FREE       0xFFFCu
#define FAT_END        0xFFFAu

static MaplePad s_pads[4];
static int s_evDone;
static u32 s_logged;
static u8 s_vmu[VMU_SIZE];
static int s_vmuDirty;

struct Rumble
{
    u32 vibset;
    u8 ast;
    int pending;
    float power;
    u32 ms;
};
static Rumble s_rumble[4];
static int s_rumblePlugged = 1;

void maple_set_pad(int port, const MaplePad *pad)
{
    if (port >= 0 && port < 4) s_pads[port] = *pad;
}

static void write_string(u32 addr, const char *s, u32 len)
{
    u32 i, n = (u32)strlen(s);
    for (i = 0; i < len; i++) mem_write8(addr + i, (u8)(i < n ? s[i] : ' '));
}

static u32 response(u32 recv, u32 code, u32 to, u32 from, u32 words)
{
    mem_write32(recv, code | (to << 8) | (from << 16) | (words << 24));
    return recv + 4;
}

static void controller_info(u32 data)
{
    mem_write32(data + 0, MAPLE_FUNC_CONTROLLER);
    mem_write32(data + 4, 0xFE060F00u);
    mem_write32(data + 8, 0);
    mem_write32(data + 12, 0);
    mem_write8(data + 16, 0xFF);
    mem_write8(data + 17, 0);
    write_string(data + 18, "Dreamcast Controller", 30);
    write_string(data + 48, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
    mem_write16(data + 108, 0x01AE);
    mem_write16(data + 110, 0x01F4);
}

static void gun_info(u32 data)
{
    mem_write32(data + 0, MAPLE_FUNC_LIGHT_GUN | MAPLE_FUNC_CONTROLLER);
    mem_write32(data + 4, 0);
    mem_write32(data + 8, 0xFE000000u);
    mem_write32(data + 12, 0);
    mem_write8(data + 16, 0xFF);
    mem_write8(data + 17, 0);
    write_string(data + 18, "Dreamcast Gun", 30);
    write_string(data + 48, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
    mem_write16(data + 108, 0x0069);
    mem_write16(data + 110, 0x0120);
}

static void put16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

void maple_vmu_format(u8 *out)
{
    u8 *root = out + VMU_ROOT * 512, *fat = out + VMU_FAT * 512;
    u32 i;
    memset(out, 0, VMU_SIZE);
    memset(root, 0x55, 16);
    root[0x10] = 1;
    root[0x11] = 0xFF; root[0x12] = 0xC0; root[0x13] = 0x80; root[0x14] = 0xFF;
    root[0x30] = 0x20; root[0x31] = 0x00; root[0x32] = 0x01; root[0x33] = 0x01;
    put16(root + 0x46, VMU_FAT);
    put16(root + 0x48, 1);
    put16(root + 0x4A, VMU_DIR);
    put16(root + 0x4C, VMU_DIR_BLOCKS);
    put16(root + 0x4E, 0);
    put16(root + 0x50, VMU_USER);
    for (i = 0; i < VMU_BLOCKS; i++) put16(fat + i * 2, FAT_FREE);
    for (i = VMU_DIR; i > VMU_DIR - VMU_DIR_BLOCKS + 1; i--) put16(fat + i * 2, i - 1);
    put16(fat + (VMU_DIR - VMU_DIR_BLOCKS + 1) * 2, FAT_END);
    put16(fat + VMU_FAT * 2, FAT_END);
    put16(fat + VMU_ROOT * 2, FAT_END);
}

u8 *maple_vmu_image(void)
{
    return s_vmu;
}

void maple_vmu_load(const u8 *data)
{
    if (data) memcpy(s_vmu, data, VMU_SIZE);
    else maple_vmu_format(s_vmu);
    s_vmuDirty = 0;
}

int maple_vmu_take_dirty(void)
{
    int d = s_vmuDirty;
    s_vmuDirty = 0;
    return d;
}

static void vmu_info(u32 data)
{
    mem_write32(data + 0, MAPLE_FUNC_STORAGE | MAPLE_FUNC_LCD | MAPLE_FUNC_CLOCK);
    mem_write32(data + 4, 0x403F7E7Eu);
    mem_write32(data + 8, 0x00100500u);
    mem_write32(data + 12, 0x00410F00u);
    mem_write8(data + 16, 0xFF);
    mem_write8(data + 17, 0);
    write_string(data + 18, "Visual Memory", 30);
    write_string(data + 48, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
    mem_write16(data + 108, 0x007C);
    mem_write16(data + 110, 0x0082);
}

static u32 vmu_location(u32 word, u32 *phase)
{
    u32 be = (word >> 24) | ((word >> 8) & 0xFF00u) | ((word << 8) & 0xFF0000u) | (word << 24);
    *phase = (be >> 16) & 0xFF;
    return be & 0xFFFF;
}

static int vmu_command(int port, u32 cmd, u32 from, u32 dataAddr, u32 words, u32 recv)
{
    u32 self = (u32)(port << 6) | 0x01, func = words ? mem_read32(dataAddr) : 0, d, i;
    LOG_DEBUG("[maple] VMU command %u function %08X, %u words, location %08X", cmd, func, words, words > 1 ? mem_read32(dataAddr + 4) : 0);
    switch (cmd)
    {
    case MAPLE_CMD_DEVICE_REQUEST:
    case MAPLE_CMD_ALL_STATUS:
        vmu_info(response(recv, cmd == MAPLE_CMD_DEVICE_REQUEST ? MAPLE_RESP_DEVICE_INFO : MAPLE_RESP_ALL_STATUS, from, self, 28));
        return 1;
    case MAPLE_CMD_RESET:
    case MAPLE_CMD_SHUTDOWN:
    case MAPLE_CMD_SET_CONDITION:
    case MAPLE_CMD_GET_LAST_ERROR:
        response(recv, MAPLE_RESP_ACK, from, self, 0);
        return 1;
    case MAPLE_CMD_GET_MEDIA_INFO:
        if (func == MAPLE_FUNC_LCD)
        {
            d = response(recv, MAPLE_RESP_DATA, from, self, 2);
            mem_write32(d, MAPLE_FUNC_LCD);
            mem_write8(d + 4, 47);
            mem_write8(d + 5, 31);
            mem_write8(d + 6, 0x10);
            mem_write8(d + 7, 2);
            return 1;
        }
        if (func != MAPLE_FUNC_STORAGE) break;
        d = response(recv, MAPLE_RESP_DATA, from, self, 7);
        mem_write32(d, MAPLE_FUNC_STORAGE);
        mem_write16(d + 4, VMU_BLOCKS - 1);
        mem_write16(d + 6, 0);
        mem_write16(d + 8, VMU_ROOT);
        mem_write16(d + 10, VMU_FAT);
        mem_write16(d + 12, 1);
        mem_write16(d + 14, VMU_DIR);
        mem_write16(d + 16, VMU_DIR_BLOCKS);
        mem_write16(d + 18, 0);
        mem_write16(d + 20, VMU_USER);
        mem_write16(d + 22, 31);
        mem_write32(d + 24, 0);
        return 1;
    case MAPLE_CMD_BLOCK_READ:
    {
        u32 phase, block;
        if (func != MAPLE_FUNC_STORAGE || words < 2) break;
        block = vmu_location(mem_read32(dataAddr + 4), &phase);
        if (block >= VMU_BLOCKS)
        {
            response(recv, MAPLE_RESP_FILE_ERROR, from, self, 0);
            return 1;
        }
        d = response(recv, MAPLE_RESP_DATA, from, self, 2 + 128);
        mem_write32(d, MAPLE_FUNC_STORAGE);
        mem_write32(d + 4, mem_read32(dataAddr + 4));
        for (i = 0; i < 512; i += 4)
        {
            const u8 *p = &s_vmu[block * 512 + i];
            mem_write32(d + 8 + i, (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24));
        }
        return 1;
    }
    case MAPLE_CMD_BLOCK_WRITE:
    {
        u32 phase, block, n;
        if (func == MAPLE_FUNC_LCD || func == MAPLE_FUNC_CLOCK)
        {
            response(recv, MAPLE_RESP_ACK, from, self, 0);
            return 1;
        }
        if (func != MAPLE_FUNC_STORAGE || words < 2) break;
        block = vmu_location(mem_read32(dataAddr + 4), &phase);
        n = (words - 2) * 4;
        if (block >= VMU_BLOCKS || phase > 3 || n > 128)
        {
            response(recv, MAPLE_RESP_FILE_ERROR, from, self, 0);
            return 1;
        }
        for (i = 0; i < n; i += 4)
        {
            u32 v = mem_read32(dataAddr + 8 + i);
            u8 *p = &s_vmu[block * 512 + phase * 128 + i];
            p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
        }
        s_vmuDirty = 1;
        response(recv, MAPLE_RESP_ACK, from, self, 0);
        return 1;
    }
    }
    if (s_logged < 16)
    {
        s_logged++;
        LOG_WARN("[maple] VMU: unhandled command %u (function %08X)", cmd, func);
    }
    response(recv, MAPLE_RESP_UNKNOWN_CMD, from, self, 0);
    return 1;
}

void maple_set_rumble_pack(int plugged) { s_rumblePlugged = plugged != 0; }

int maple_take_rumble(int port, float *power, u32 *ms)
{
    Rumble *r;
    if (port < 0 || port > 3 || !s_rumble[port].pending) return 0;
    r = &s_rumble[port];
    r->pending = 0;
    *power = r->power;
    *ms = r->ms;
    return 1;
}

static void rumble_info(u32 data)
{
    mem_write32(data + 0, MAPLE_FUNC_VIBRATION);
    mem_write32(data + 4, 0x00000101u);
    mem_write32(data + 8, 0);
    mem_write32(data + 12, 0);
    mem_write8(data + 16, 0xFF);
    mem_write8(data + 17, 0);
    write_string(data + 18, "Puru Puru Pack", 30);
    write_string(data + 48, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
    mem_write16(data + 108, 0x00C8);
    mem_write16(data + 110, 0x0640);
}

static void rumble_set(Rumble *r, u32 v)
{
    u32 up = (v >> 8) & 7, down = (v >> 12) & 7, hz = (v >> 16) & 0xFF, cycles = v >> 24;
    u32 autoStop = ((u32)r->ast + 1) * 250;
    int ramp = (v & 0x0800) || (v & 0x8000), continuous = v & 1;
    r->vibset = v;
    r->power = (float)(up > down ? up : down) / 7.0f;
    if (continuous || !hz)
        r->ms = autoStop;
    else
    {
        u32 ms = 1000 * (ramp && cycles ? cycles : 1) / hz;
        r->ms = ms < autoStop ? ms : autoStop;
    }
    r->pending = 1;
}

static int rumble_command(int port, u32 cmd, u32 from, u32 dataAddr, u32 words, u32 recv)
{
    Rumble *r = &s_rumble[port];
    u32 self = (u32)(port << 6) | 0x02, func = words ? mem_read32(dataAddr) : 0, d;
    switch (cmd)
    {
    case MAPLE_CMD_DEVICE_REQUEST:
    case MAPLE_CMD_ALL_STATUS:
        rumble_info(response(recv, cmd == MAPLE_CMD_DEVICE_REQUEST ? MAPLE_RESP_DEVICE_INFO : MAPLE_RESP_ALL_STATUS, from, self, 28));
        return 1;
    case MAPLE_CMD_RESET:
    case MAPLE_CMD_SHUTDOWN:
        response(recv, MAPLE_RESP_ACK, from, self, 0);
        return 1;
    case MAPLE_CMD_GET_CONDITION:
        if (func != MAPLE_FUNC_VIBRATION) break;
        d = response(recv, MAPLE_RESP_DATA, from, self, 2);
        mem_write32(d, MAPLE_FUNC_VIBRATION);
        mem_write32(d + 4, r->vibset);
        return 1;
    case MAPLE_CMD_GET_MEDIA_INFO:
        if (func != MAPLE_FUNC_VIBRATION) break;
        d = response(recv, MAPLE_RESP_DATA, from, self, 2);
        mem_write32(d, MAPLE_FUNC_VIBRATION);
        mem_write32(d + 4, 0x3B07E010u);
        return 1;
    case MAPLE_CMD_BLOCK_READ:
        if (func != MAPLE_FUNC_VIBRATION) break;
        d = response(recv, MAPLE_RESP_DATA, from, self, 3);
        mem_write32(d, MAPLE_FUNC_VIBRATION);
        mem_write32(d + 4, 0);
        mem_write32(d + 8, 2u | ((u32)r->ast << 16));
        return 1;
    case MAPLE_CMD_BLOCK_WRITE:
        if (func != MAPLE_FUNC_VIBRATION || words < 3) break;
        r->ast = mem_read8(dataAddr + 10);
        response(recv, MAPLE_RESP_ACK, from, self, 0);
        return 1;
    case MAPLE_CMD_SET_CONDITION:
        if (func != MAPLE_FUNC_VIBRATION || words < 2) break;
        rumble_set(r, mem_read32(dataAddr + 4));
        response(recv, MAPLE_RESP_ACK, from, self, 0);
        return 1;
    }
    if (s_logged < 16)
    {
        s_logged++;
        LOG_WARN("[maple] vibration pack: unhandled command %u (function %08X)", cmd, func);
    }
    response(recv, MAPLE_RESP_UNKNOWN_CMD, from, self, 0);
    return 1;
}

static int device_command(int port, u32 frame, u32 dataAddr, u32 recv)
{
    const MaplePad *pad = &s_pads[port];
    u32 cmd = frame & 0xFF, to = (frame >> 8) & 0xFF, from = (frame >> 16) & 0xFF, words = frame >> 24;
    u32 self = (u32)(port << 6) | 0x20 | (port == 0 ? 0x01 : 0) | (s_rumblePlugged ? 0x02 : 0);
    if (!pad->connected) return 0;
    if ((to & 0x20) == 0)
    {
        if (port == 0 && (to & 0x1F) == 0x01) return vmu_command(port, cmd, from, dataAddr, words, recv);
        if (s_rumblePlugged && (to & 0x1F) == 0x02) return rumble_command(port, cmd, from, dataAddr, words, recv);
        return 0;
    }
    switch (cmd)
    {
    case MAPLE_CMD_DEVICE_REQUEST:
    case MAPLE_CMD_ALL_STATUS:
        if (pad->gun)
            gun_info(response(recv, cmd == 1 ? MAPLE_RESP_DEVICE_INFO : MAPLE_RESP_ALL_STATUS, from, self, 28));
        else
            controller_info(response(recv, cmd == 1 ? MAPLE_RESP_DEVICE_INFO : MAPLE_RESP_ALL_STATUS, from, self, 28));
        return 1;
    case MAPLE_CMD_RESET:
    case MAPLE_CMD_SHUTDOWN:
        response(recv, MAPLE_RESP_ACK, from, self, 0);
        return 1;
    case MAPLE_CMD_GET_CONDITION:
    {
        u32 func = mem_read32(dataAddr), d;
        if (func != MAPLE_FUNC_CONTROLLER)
        {
            response(recv, MAPLE_RESP_UNKNOWN_CMD, from, self, 0);
            return 1;
        }
        d = response(recv, MAPLE_RESP_DATA, from, self, 3);
        mem_write32(d, MAPLE_FUNC_CONTROLLER);
        mem_write32(d + 4, (u32)(u16)~pad->buttons | ((u32)pad->rt << 16) | ((u32)pad->lt << 24));
        mem_write32(d + 8, (u32)pad->jx | ((u32)pad->jy << 8) | 0x80800000u);
        return 1;
    }
    default:
        if (s_logged < 16)
        {
            s_logged++;
            LOG_WARN("[maple] port %d: unhandled command %u", port, cmd);
        }
        response(recv, MAPLE_RESP_UNKNOWN_CMD, from, self, 0);
        return 1;
    }
}

void maple_dma_start(void)
{
    u32 addr = holly_reg(SB_MDSTAR) & 0x1FFFFFE0u, guard;
    for (guard = 0; guard < 64; guard++)
    {
        u32 header = mem_read32(addr), recv = mem_read32(addr + 4) & 0x1FFFFFE0u;
        u32 last = header >> 31, port = (header >> 16) & 3, pattern = (header >> 8) & 7, words = header & 0xFF;
        if (pattern == 0)
        {
            u32 frame = mem_read32(addr + 8);
            if (!device_command((int)port, frame, addr + 12, recv)) mem_write32(recv, 0xFFFFFFFFu);
            addr += 12 + words * 4;
        }
        else
        {
            if (pattern == 2 && s_pads[port].connected && s_pads[port].gun && !s_pads[port].gunOff)
                pvr_light_gun(s_pads[port].gunX, s_pads[port].gunY);
            addr += 4;
        }
        if (last) break;
    }
    sched_after(s_evDone, 20000);
}

static void dma_done(void *, u32)
{
    holly_set_reg(SB_MDST, 0);
    holly_raise(HOLLY_MAPLE_DMA_END);
}

void maple_vblank(void)
{
    if ((holly_reg(SB_MDEN) & 1) && (holly_reg(SB_MDTSEL) & 1) && !holly_reg(SB_MDST))
    {
        holly_set_reg(SB_MDST, 1);
        maple_dma_start();
    }
}

void maple_init(void)
{
    s_evDone = sched_register("maple_dma", dma_done, NULL);
    maple_vmu_format(s_vmu);
}

void maple_reset(void)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        memset(&s_pads[i], 0, sizeof(s_pads[i]));
        s_pads[i].jx = s_pads[i].jy = 128;
    }
    s_pads[0].connected = 1;
    s_logged = 0;
    memset(s_rumble, 0, sizeof(s_rumble));
    for (i = 0; i < 4; i++) s_rumble[i].ast = 19;
}
