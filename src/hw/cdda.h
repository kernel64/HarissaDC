// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_CDDA_H
#define EMU_CDDA_H

#include "../core/types.h"

#define CDDA_PLAYING 0x11
#define CDDA_PAUSED 0x12
#define CDDA_DONE 0x13
#define CDDA_NONE 0x15

void cdda_init(void);
void cdda_reset(void);

void cdda_play(u32 start, u32 end, u32 repeat);
void cdda_pause(void);
void cdda_resume(void);
void cdda_stop(void);

int cdda_playing(void);
u32 cdda_status(void);
u32 cdda_fad(void);
u32 cdda_repeats(void);

int cdda_frame(s32 *left, s32 *right);

struct StateIO;
void cdda_state(StateIO *io);

#endif
