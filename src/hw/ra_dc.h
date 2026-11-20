// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_RA_DC_H
#define EMU_RA_DC_H

#include "../core/types.h"

struct Disc;

int ra_dc_hash(Disc *d, char hash[33], char *error, int errorSize);

u32 ra_dc_read(u32 address, u8 *buffer, u32 count);

#endif
