// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_AUDIO_H
#define XB_AUDIO_H

#include "../../core/types.h"

#define XB_AUDIO_RATE 44100

int xb_audio_init(void);
void xb_audio_shutdown(void);

int xb_audio_tone(int hz);

int xb_audio_feed(u32 (*read)(s16 *out, u32 frames), u32 (*available)(void));

int xb_audio_start_thread(u32 (*read)(s16 *out, u32 frames), u32 (*available)(void), int stretch);

void xb_audio_set_target_ms(int ms);

struct XbAudioStats
{
    u32 underruns, fillMs, ratioPermille;
};
void xb_audio_stats(XbAudioStats *out);

#endif
