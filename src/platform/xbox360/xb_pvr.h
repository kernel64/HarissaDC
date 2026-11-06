// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_PVR_H
#define XB_PVR_H

#include <xtl.h>
#include "../../core/types.h"

int xb_pvr_init(IDirect3DDevice9 *dev);
void xb_pvr_reset(void);
IDirect3DTexture9 *xb_pvr_display(void);
void xb_pvr_sync(void);
int xb_pvr_busy(void);
void xb_pvr_dump_next_scene(const char *path);
void xb_pvr_dump_frames(int count);
void xb_pvr_tex_stats(u32 *decodes, f64 *decodeMs, f64 *uploadMs);
void xb_pvr_render_stats(f64 *decodeMs, f64 *opaqueMs, f64 *transMs, u32 *frames);
void xb_pvr_orientation(f32 *across, f32 *down, f32 *up);

#endif
