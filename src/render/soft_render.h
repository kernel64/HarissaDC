// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_SOFT_RENDER_H
#define EMU_SOFT_RENDER_H

#include "../core/types.h"

void soft_render_install(void);
void soft_render_frame(void);
const u32 *soft_render_image(int *w, int *h);

#endif
