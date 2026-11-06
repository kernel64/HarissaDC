// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_TEXTURE_H
#define EMU_TEXTURE_H

#include "../core/types.h"

struct TexDesc
{
    u32 tcw;
    u32 width, height;
    u32 strideWidth;
    u32 palFormat;
};

u32 tex_twiddle(u32 x, u32 y, u32 w, u32 h);

int tex_decode(const TexDesc *d, const u8 *vram, const u32 *palette, u32 *out);

int tex_rect_ok(const TexDesc *d);
u32 tex_texels_per_byte2(const TexDesc *d);
void tex_untwiddle(u32 i, u32 w, u32 h, u32 *x, u32 *y);
int tex_decode_rect(const TexDesc *d, const u8 *vram, const u32 *palette, u32 *out, u32 x0, u32 y0, u32 x1, u32 y1);

#endif
