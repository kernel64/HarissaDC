// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_TA_SCENE_H
#define EMU_TA_SCENE_H

#include "../core/types.h"

enum TaList { TA_LIST_OPAQUE = 0, TA_LIST_OPAQUE_MOD = 1, TA_LIST_TRANS = 2, TA_LIST_TRANS_MOD = 3, TA_LIST_PUNCH = 4 };

struct TaVertex
{
    f32 x, y, z;
    f32 u, v;
    u32 base, offset;
};

extern int g_ta_trans_always_last;

struct TaStrip
{
    u32 isp, tsp, tcw;
    u32 pcw;
    u32 list;
    u32 first, count;
    u32 clip;
    u32 pass;
};

#define TA_CLIP_MODE(c) ((c) >> 24)
#define TA_CLIP_X0(c) ((c) & 63u)
#define TA_CLIP_Y0(c) (((c) >> 6) & 63u)
#define TA_CLIP_X1(c) (((c) >> 12) & 63u)
#define TA_CLIP_Y1(c) (((c) >> 18) & 63u)

struct TaModTri
{
    f32 x[3], y[3], z[3];
};

struct TaModVol
{
    u32 isp;
    u32 list;
    u32 last;
    u32 first, count;
};

struct TaScene
{
    TaVertex *verts;
    u32 vertCount, vertCap;
    TaStrip *strips;
    u32 stripCount, stripCap;
    TaModTri *modTris;
    u32 modTriCount, modTriCap;
    TaModVol *modVols;
    u32 modVolCount, modVolCap;
    u32 bgColor;
    f32 bgDepth;
};

int ta_strip_can_cover(const TaScene *sc, const TaStrip *s);

void ta_scene_orientation(const TaScene *s, f32 xScale, f32 *across, f32 *down, f32 *up);

void ta_scene_init(TaScene *s);
void ta_scene_free(TaScene *s);
void ta_scene_decode(TaScene *s, const u32 *data, u32 words, const u32 *regs);

#endif
