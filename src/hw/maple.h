// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_MAPLE_H
#define EMU_MAPLE_H

#include "../core/types.h"

#define DC_BTN_C      (1u << 0)
#define DC_BTN_B      (1u << 1)
#define DC_BTN_A      (1u << 2)
#define DC_BTN_START  (1u << 3)
#define DC_BTN_UP     (1u << 4)
#define DC_BTN_DOWN   (1u << 5)
#define DC_BTN_LEFT   (1u << 6)
#define DC_BTN_RIGHT  (1u << 7)
#define DC_BTN_Z      (1u << 8)
#define DC_BTN_Y      (1u << 9)
#define DC_BTN_X      (1u << 10)
#define DC_BTN_D      (1u << 11)

struct MaplePad
{
    int connected;
    u16 buttons;
    u8 lt, rt;
    u8 jx, jy;
    u8 gun, gunOff;
    u16 gunX, gunY;
};

void maple_init(void);
void maple_reset(void);
void maple_dma_start(void);
void maple_vblank(void);
void maple_set_pad(int port, const MaplePad *pad);

void maple_set_rumble_pack(int plugged);
int maple_take_rumble(int port, float *power, u32 *ms);

#define VMU_SIZE (128u * 1024u)
u8 *maple_vmu_image(void);
void maple_vmu_load(const u8 *data);
int maple_vmu_take_dirty(void);
void maple_vmu_format(u8 *out);

#endif
