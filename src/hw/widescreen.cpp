// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "widescreen.h"

#include <string.h>

#include "../core/log.h"
#include "../disc/disc_info.h"
#include "../mem/mem.h"

static const WidePatch s_patches[] = {
#include "widescreen_patches.inc"
};

static const WidePatch s_arcade[] = {
#include "widescreen_patches_arcade.inc"
};

struct WideConversion
{
    const char *product, *title, *arcade;
};
static const WideConversion s_conversions[] = {
    { "T0004M", "Knights of Valour - The Seven Spirits", "KNIGHTS OF VALOUR THE 7 SPIRITS" },
    { "T0005M", "Faster than speed", "FASTER THAN SPEED" },
};

#define PATCH_COUNT (sizeof(s_patches) / sizeof(s_patches[0]))
#define ARCADE_COUNT (sizeof(s_arcade) / sizeof(s_arcade[0]))

u32 wide_patch_count_arcade(void)
{
    return (u32)ARCADE_COUNT;
}

static int same_spaced(const char *a, const char *b)
{
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    for (;;)
    {
        if (*a == ' ' || *b == ' ')
        {
            int sa = *a == ' ', sb = *b == ' ';
            while (*a == ' ') a++;
            while (*b == ' ') b++;
            if (!*a || !*b) return !*a && !*b;
            if (sa != sb) return 0;
            continue;
        }
        if (*a != *b) return 0;
        if (!*a) return 1;
        a++;
        b++;
    }
}

const WidePatch *wide_patch_find_arcade(const char *name, const char *romSet)
{
    u32 i;
    const WidePatch *any = NULL;
    if (!name) return NULL;
    for (i = 0; i < ARCADE_COUNT; i++)
    {
        const WidePatch *p = &s_arcade[i];
        if (!same_spaced(p->product, name)) continue;
        if (p->area && romSet && same_spaced(p->area, romSet)) return p;
        if (!p->area && !any) any = p;
    }
    return any;
}

u32 wide_patch_count(void)
{
    return (u32)PATCH_COUNT;
}

static int same_trimmed(const char *a, const char *b)
{
    size_t na, nb;
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    na = strlen(a);
    nb = strlen(b);
    while (na && a[na - 1] == ' ') na--;
    while (nb && b[nb - 1] == ' ') nb--;
    return na == nb && !memcmp(a, b, na);
}

const WidePatch *wide_patch_find(const DiscInfo *info)
{
    u32 i;
    if (!info || !info->product[0]) return NULL;
    if ((!strcmp(info->product, "HDR-0164") || !strcmp(info->product, "MK-5118450")) && strstr(info->device, "4/4")) return NULL;
    for (i = 0; i < PATCH_COUNT; i++)
    {
        const WidePatch *p = &s_patches[i];
        if (strcmp(p->product, info->product)) continue;
        if (!p->area || same_trimmed(p->area, info->areas) || same_trimmed(p->area, info->version) || same_trimmed(p->area, info->title))
            return p;
    }
    for (i = 0; i < sizeof(s_conversions) / sizeof(s_conversions[0]); i++)
        if (!strcmp(s_conversions[i].product, info->product) && same_trimmed(s_conversions[i].title, info->title))
            return wide_patch_find_arcade(s_conversions[i].arcade, NULL);
    return NULL;
}

static const WidePatch *s_active;
static u32 s_kept[WIDE_PATCH_MAX];
static u32 s_keptMask;

static u32 ram_read32(u32 off)
{
    u32 a = 0x8C000000u | (off & (RAM_SIZE - 1));
    if (!(a & 3)) return mem_read32(a);
    return (u32)mem_read8(a) | (u32)mem_read8(a + 1) << 8 | (u32)mem_read8(a + 2) << 16 | (u32)mem_read8(a + 3) << 24;
}

static void ram_write32(u32 off, u32 v)
{
    u32 a = 0x8C000000u | (off & (RAM_SIZE - 1));
    if (!(a & 3))
    {
        mem_write32(a, v);
        return;
    }
    mem_write8(a, (u8)v);
    mem_write8(a + 1, (u8)(v >> 8));
    mem_write8(a + 2, (u8)(v >> 16));
    mem_write8(a + 3, (u8)(v >> 24));
}

void wide_patch_set(const WidePatch *p)
{
    u32 i;
    if (p == s_active) return;
    if (s_active)
    {
        for (i = 0; i < s_active->count; i++)
            if ((s_keptMask >> i) & 1 && ram_read32(s_active->offset[i]) == s_active->value[i]) ram_write32(s_active->offset[i], s_kept[i]);
        LOG_INFO("[wide] 16:9 patch of %s off", s_active->product);
    }
    s_active = p;
    s_keptMask = 0;
    if (p) LOG_INFO("[wide] 16:9 patch of %s on (%u values)", p->product, p->count);
}

const WidePatch *wide_patch_active(void)
{
    return s_active;
}

void wide_patch_apply(void)
{
    const WidePatch *p = s_active;
    u32 i;
    if (!p) return;
    for (i = 0; i < p->count; i++)
    {
        u32 cur = ram_read32(p->offset[i]);
        if (cur == p->value[i] || (p->orig[i] && p->orig[i] != cur)) continue;
        if (!((s_keptMask >> i) & 1))
        {
            s_kept[i] = cur;
            s_keptMask |= 1u << i;
        }
        ram_write32(p->offset[i], p->value[i]);
    }
}
