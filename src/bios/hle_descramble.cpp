// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "hle_bios.h"

#define MAX_WINDOW (2u << 20)

struct ScrambleRng
{
    u32 seed;
};

static u32 next_rand(ScrambleRng *r)
{
    r->seed = (r->seed * 2109 + 9273) & 0x7FFF;
    return (r->seed + 0xC000) & 0xFFFF;
}

static void window_order(ScrambleRng *r, u32 *order, u32 slices)
{
    u32 *idx = order + slices;
    int i;
    for (i = 0; i < (int)slices; i++) idx[i] = (u32)i;
    for (i = (int)slices - 1; i >= 0; i--)
    {
        u32 x = (next_rand(r) * (u32)i) >> 16, t = idx[i];
        idx[i] = idx[x];
        idx[x] = t;
        order[slices - 1 - i] = idx[i];
    }
}

static void process(const u8 *src, u8 *dst, u32 size, int unscramble)
{
    static u32 order[2 * (MAX_WINDOW / 32)];
    ScrambleRng r;
    u32 window;
    r.seed = size & 0xFFFF;
    for (window = MAX_WINDOW; window >= 32; window >>= 1)
    {
        while (size >= window)
        {
            u32 slices = window / 32, k;
            window_order(&r, order, slices);
            for (k = 0; k < slices; k++)
            {
                if (unscramble) memcpy(dst + order[k] * 32, src + k * 32, 32);
                else memcpy(dst + k * 32, src + order[k] * 32, 32);
            }
            src += window;
            dst += window;
            size -= window;
        }
    }
    if (size) memcpy(dst, src, size);
}

void hle_descramble(const u8 *src, u8 *dst, u32 size) { process(src, dst, size, 1); }
void hle_scramble(const u8 *src, u8 *dst, u32 size) { process(src, dst, size, 0); }
