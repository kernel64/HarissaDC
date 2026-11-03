// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "flash.h"
#include "../core/state.h"

#include "../core/log.h"
#include "../mem/mem.h"

enum { FS_READ, FS_UNLOCK1, FS_UNLOCK2, FS_PROGRAM, FS_ERASE_PREFIX, FS_ERASE_UNLOCK1, FS_ERASE_UNLOCK2 };

#define SECTOR_SIZE (16u * 1024u)

static int s_state;
static int s_dirty;

static int is_addr(u32 off, u32 a) { return (off & 0x7FFF) == a || (off & 0xFFFF) == a * 2; }

void flash_reset(void)
{
    s_state = FS_READ;
}

int flash_dirty(void) { return s_dirty; }
void flash_clear_dirty(void) { s_dirty = 0; }

u32 flash_read(u32 addr, int size)
{
    u32 off = addr & (FLASH_SIZE - 1);
    if (size == 4) return buf_r32(g_flash, off & ~3u);
    if (size == 2) return buf_r16(g_flash, off & ~1u);
    return buf_r8(g_flash, off);
}

void flash_write(u32 addr, u32 value, int size)
{
    u32 off = addr & (FLASH_SIZE - 1);
    u8 v = (u8)value;
    (void)size;
    switch (s_state)
    {
    case FS_READ:
        if (v == 0xAA && is_addr(off, 0x5555)) s_state = FS_UNLOCK1;
        return;
    case FS_UNLOCK1:
        s_state = (v == 0x55 && is_addr(off, 0x2AAA)) ? FS_UNLOCK2 : FS_READ;
        return;
    case FS_UNLOCK2:
        if (v == 0xA0) s_state = FS_PROGRAM;
        else if (v == 0x80) s_state = FS_ERASE_PREFIX;
        else s_state = FS_READ;
        return;
    case FS_PROGRAM:
        buf_w8(g_flash, off, (u8)(buf_r8(g_flash, off) & v));
        s_dirty = 1;
        s_state = FS_READ;
        return;
    case FS_ERASE_PREFIX:
        s_state = (v == 0xAA && is_addr(off, 0x5555)) ? FS_ERASE_UNLOCK1 : FS_READ;
        return;
    case FS_ERASE_UNLOCK1:
        s_state = (v == 0x55 && is_addr(off, 0x2AAA)) ? FS_ERASE_UNLOCK2 : FS_READ;
        return;
    case FS_ERASE_UNLOCK2:
    {
        u32 start = 0, len = 0, i;
        if (v == 0x30) { start = off & ~(SECTOR_SIZE - 1); len = SECTOR_SIZE; }
        else if (v == 0x10) { start = 0; len = FLASH_SIZE; }
        for (i = 0; i < len; i++) buf_w8(g_flash, start + i, 0xFF);
        if (len)
        {
            s_dirty = 1;
            LOG_DEBUG("[flash] erased %u bytes at %05X", len, start);
        }
        s_state = FS_READ;
        return;
    }
    }
}

void flash_state(StateIO *io)
{
    state_tag(io, "FLSH");
    STATE(io, s_state);
}
