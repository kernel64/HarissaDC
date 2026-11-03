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

#endif
