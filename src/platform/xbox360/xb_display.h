// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_DISPLAY_H
#define XB_DISPLAY_H

#include <xtl.h>

#include "../../core/config.h"

void xb_display_draw(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex, int srcW, int srcLines, const Config *cfg);
void xb_display_rect(float *x, float *y, float *w, float *h);

#define DISPLAY_PRESETS 4
void xb_display_preset(Config *c, int preset);
int xb_display_preset_of(const Config *c);
const char *xb_display_preset_name(int preset);

extern int g_xb_auto_rotate;

#endif
