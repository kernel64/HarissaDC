// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_WIDESCREEN_H
#define EMU_WIDESCREEN_H

#include "../core/types.h"

struct DiscInfo;

#define WIDE_PATCH_MAX 16

struct WidePatch
{
    const char *product;
    const char *area;
    u32 count;
    u32 offset[WIDE_PATCH_MAX];
    u32 value[WIDE_PATCH_MAX];
    u32 orig[WIDE_PATCH_MAX];
};

const WidePatch *wide_patch_find(const DiscInfo *info);
const WidePatch *wide_patch_find_arcade(const char *name, const char *romSet);
u32 wide_patch_count(void);
u32 wide_patch_count_arcade(void);

void wide_patch_set(const WidePatch *p);
const WidePatch *wide_patch_active(void);
void wide_patch_apply(void);

#endif
