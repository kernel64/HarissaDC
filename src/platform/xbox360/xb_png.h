// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_PNG_H
#define XB_PNG_H

#include "../../core/types.h"

u8 *png_decode(const u8 *data, u32 size, int *w, int *h, u32 maxPixels);

void image_fit(int w, int h, int maxW, int maxH, int *outW, int *outH);

u8 *image_scale(const u8 *src, int sw, int sh, int dw, int dh);

#endif
