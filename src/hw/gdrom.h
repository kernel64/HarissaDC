// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_GDROM_H
#define EMU_GDROM_H

#include "../core/types.h"

struct Disc;

void gdrom_init(void);
void gdrom_reset(void);
void gdrom_set_disc(Disc *disc);
void gdrom_swap_disc(Disc *disc);
int gdrom_lid_open(void);
Disc *gdrom_disc(void);

u32 gdrom_read(u32 addr, int size);
void gdrom_write(u32 addr, u32 value, int size);
void gdrom_dma_start(void);

struct StateIO;
void gdrom_state(StateIO *io);

#endif
