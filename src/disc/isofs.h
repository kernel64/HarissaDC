// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_ISOFS_H
#define EMU_ISOFS_H

#include "../core/types.h"

struct Disc;

u32 iso_base_fad(Disc *d);

int iso_find_root_file(Disc *d, u32 baseFad, const char *name, u32 *fad, u32 *size);
void iso_list_root(Disc *d, u32 baseFad, void (*fn)(const char *name, u32 fad, u32 size, int dir));

#endif
