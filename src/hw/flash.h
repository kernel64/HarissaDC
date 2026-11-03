// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_FLASH_H
#define EMU_FLASH_H

#include "../core/types.h"

void flash_reset(void);
u32 flash_read(u32 addr, int size);
void flash_write(u32 addr, u32 value, int size);
int flash_dirty(void);
void flash_clear_dirty(void);

struct StateIO;
void flash_state(StateIO *io);

#endif
