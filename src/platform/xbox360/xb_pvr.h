// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_PVR_H
#define XB_PVR_H

#include <xtl.h>
#include "../../core/types.h"

enum { WIDE_OFF = 0, WIDE_REVEAL = 1, WIDE_ANAMORPHIC = 2 };
extern int g_xb_widescreen;

int xb_pvr_init(IDirect3DDevice9 *dev);
void xb_pvr_reset(void);
IDirect3DTexture9 *xb_pvr_display(void);
int xb_pvr_display_view(IDirect3DTexture9 *tex, f32 *u0, f32 *u1);
int xb_pvr_display_size(IDirect3DTexture9 *tex, int *w, int *h, f32 *scale);
void xb_pvr_sync(void);
int xb_pvr_busy(void);
void xb_pvr_dump_next_scene(const char *path);
void xb_pvr_dump_frames(int count);
void xb_pvr_tex_stats(u32 *decodes, f64 *decodeMs, f64 *uploadMs);
void xb_pvr_render_stats(f64 *decodeMs, f64 *opaqueMs, f64 *transMs, u32 *frames);
void xb_pvr_fb_stats(u32 *bands, f64 *ms, f64 *waitMs);
int xb_pvr_test_render(u32 *out, int maxPixels, int *w, int *h, int keep);
void xb_pvr_orientation(f32 *across, f32 *down, f32 *up);

#endif
