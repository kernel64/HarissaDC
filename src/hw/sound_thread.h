// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_SOUND_THREAD_H
#define EMU_SOUND_THREAD_H

#include "../core/types.h"

int sound_thread_start(int hwThread);
void sound_thread_stop(void);
int sound_thread_on(void);

void sound_thread_advance(u64 sh4Now);
void sound_thread_sync(void);
void sound_thread_hold(void);
void sound_thread_restart(void);
void sound_thread_set_time(u64 sh4Now);

struct SoundWaitStats
{
    u32 reads, writes, queueFull, ahead;
    u32 fastReads;
};
void sound_thread_wait_stats(SoundWaitStats *out);

u32 sound_sh4_read(u32 addr, int size);
void sound_sh4_write(u32 addr, u32 value, int size);

#endif
