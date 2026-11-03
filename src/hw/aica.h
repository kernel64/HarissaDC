// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_AICA_H
#define EMU_AICA_H

#include "../core/types.h"

void aica_init(void);
void aica_reset(void);
u32 aica_read(u32 addr, int size);
void aica_write(u32 addr, u32 value, int size);

void aica_rtc_set(u32 secondsSince1950);
extern u32 g_fixed_time;
u32 host_time(void);

u32 aica_audio_read(s16 *out, u32 frames);
u32 aica_audio_available(void);

void aica_set_threaded(int on);
void aica_sample(void);
int aica_sh4_irq(void);

struct StateIO;
void aica_state(StateIO *io);

#endif
