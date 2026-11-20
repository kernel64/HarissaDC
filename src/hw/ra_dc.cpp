// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "ra_dc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../disc/disc.h"
#include "../disc/isofs.h"
#include "../mem/mem.h"

extern "C" {
#include "../third_party/rcheevos/src/util/md5.h"
}

#define RA_MAX_FILE (64u << 20)

static int fail(char *error, int size, const char *text)
{
    if (error && size > 0)
    {
        strncpy(error, text, size - 1);
        error[size - 1] = 0;
    }
    return -1;
}

int ra_dc_hash(Disc *d, char hash[33], char *error, int errorSize)
{
    u8 sector[2048];
    char boot[17];
    u32 base, fad, size, i;
    md5_state_t md5;
    md5_byte_t digest[16];
    u8 *chunk;
    hash[0] = 0;
    if (!d) return fail(error, errorSize, "no disc");
    base = iso_base_fad(d);
    if (!base || disc_read(d, base, 1, sector, 2048) != 1 || memcmp(sector, "SEGA SEGAKATANA ", 16) != 0)
        return fail(error, errorSize, "not a Dreamcast disc");
    md5_init(&md5);
    md5_append(&md5, sector, 256);
    for (i = 0; i < 16 && sector[96 + i] > ' '; i++) boot[i] = (char)sector[96 + i];
    boot[i] = 0;
    if (!i) return fail(error, errorSize, "no boot file in IP.BIN");
    if (iso_find_root_file(d, base, boot, &fad, &size) != 0) return fail(error, errorSize, "boot file not found");
    if (size > RA_MAX_FILE) size = RA_MAX_FILE;
    chunk = (u8 *)malloc(64 * 2048);
    if (!chunk) return fail(error, errorSize, "out of memory");
    while (size)
    {
        u32 n = (size + 2047) / 2048, bytes;
        if (n > 64) n = 64;
        if (disc_read(d, fad, n, chunk, 2048) != n)
        {
            free(chunk);
            return fail(error, errorSize, "cannot read the boot file");
        }
        bytes = n * 2048 < size ? n * 2048 : size;
        md5_append(&md5, chunk, (int)bytes);
        size -= bytes;
        fad += n;
    }
    free(chunk);
    md5_finish(&md5, digest);
    for (i = 0; i < 16; i++) sprintf(hash + 2 * i, "%02x", digest[i]);
    hash[32] = 0;
    return 0;
}

u32 ra_dc_read(u32 address, u8 *buffer, u32 count)
{
    u32 i;
    if (!g_ram || address >= RAM_SIZE || count > RAM_SIZE - address) return 0;
    for (i = 0; i < count; i++) buffer[i] = buf_r8(g_ram, address + i);
    return count;
}
