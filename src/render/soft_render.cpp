// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "soft_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ta_scene.h"
#include "texture.h"
#include "../core/log.h"
#include "../core/prof.h"
#include "../hw/pvr.h"
#include "../mem/mem.h"

#define MAX_W 1280
#define MAX_H 1024

static u32 *s_color;
static f32 *s_depth;
static u8 *s_stencil;
static TaScene s_scene;
static int s_depthPass;

struct TexEntry
{
    u32 tcw, sizeBits, palKey;
    u32 w, h;
    u32 *pixels;
};

#define TEX_MAX 1024
#define TEX_POOL (24u << 20)
static TexEntry s_tex[TEX_MAX];
static u32 s_texCount;
static u32 *s_texPool;
static u32 s_texPoolUsed;

static const TexEntry *get_texture(u32 tcw, u32 tsp)
{
    u32 sizeBits = tsp & 0x3F, w = 8u << ((tsp >> 3) & 7), h = 8u << (tsp & 7);
    u32 fmt = (tcw >> 27) & 7, palKey = 0, i;
    TexDesc d;
    TexEntry *e;
    static u32 palette[1024];
    if (fmt >= 5) palKey = pvr_reg(0x005F8108) & 3;
    for (i = 0; i < s_texCount; i++)
        if (s_tex[i].tcw == tcw && s_tex[i].sizeBits == sizeBits && s_tex[i].palKey == palKey) return &s_tex[i];
    if (s_texCount >= TEX_MAX || s_texPoolUsed + w * h > TEX_POOL / 4) return NULL;
    e = &s_tex[s_texCount++];
    e->tcw = tcw;
    e->sizeBits = sizeBits;
    e->palKey = palKey;
    e->w = w;
    e->h = h;
    e->pixels = s_texPool + s_texPoolUsed;
    s_texPoolUsed += w * h;
    d.tcw = tcw;
    d.width = w;
    d.height = h;
    d.strideWidth = (pvr_reg(0x005F80E4) & 31) * 32;
    d.palFormat = palKey;
    if (fmt >= 5)
        for (i = 0; i < 1024; i++) palette[i] = pvr_reg(0x005F9000 + i * 4);
    tex_decode(&d, g_vram, palette, e->pixels);
    return e;
}

struct DrawState
{
    u32 isp, tsp, list;
    const TexEntry *tex;
    int depthMode, zWrite, gouraud, offset, useAlpha, ignoreTexAlpha, shading, shadow, fog;
    int clampU, clampV, flipU, flipV;
    u32 srcInstr, dstInstr;
    u32 ptRef;
    u32 clip;
};

static __inline u32 mul8(u32 a, u32 b) { return (a * b + 127) / 255; }

static u32 sample(const DrawState *st, f32 u, f32 v)
{
    const TexEntry *t = st->tex;
    s32 x = (s32)floorf(u * (f32)t->w), y = (s32)floorf(v * (f32)t->h);
    s32 w = (s32)t->w, h = (s32)t->h;
    if (st->clampU) x = x < 0 ? 0 : x >= w ? w - 1 : x;
    else if (st->flipU) { x &= 2 * w - 1; if (x >= w) x = 2 * w - 1 - x; }
    else x &= w - 1;
    if (st->clampV) y = y < 0 ? 0 : y >= h ? h - 1 : y;
    else if (st->flipV) { y &= 2 * h - 1; if (y >= h) y = 2 * h - 1 - y; }
    else y &= h - 1;
    return t->pixels[y * w + x];
}

static u32 blend_factor(u32 instr, u32 src, u32 dst, int forSrc, u32 *outRGB)
{
    u32 other = forSrc ? dst : src, sa = src >> 24, da = dst >> 24, f;
    switch (instr)
    {
    case 0: *outRGB = 0; return 0;
    case 1: *outRGB = 0xFFFFFF; return 255;
    case 2: *outRGB = other & 0xFFFFFF; return other >> 24;
    case 3: *outRGB = ~other & 0xFFFFFF; return 255 - (other >> 24);
    case 4: f = sa; break;
    case 5: f = 255 - sa; break;
    case 6: f = da; break;
    default: f = 255 - da; break;
    }
    *outRGB = f * 0x010101u;
    return f;
}

static u32 blend(const DrawState *st, u32 src, u32 dst)
{
    u32 sf, df, sfRGB, dfRGB, r, g, b, a;
    sf = blend_factor(st->srcInstr, src, dst, 1, &sfRGB);
    df = blend_factor(st->dstInstr, src, dst, 0, &dfRGB);
    r = mul8((src >> 16) & 255, (sfRGB >> 16) & 255) + mul8((dst >> 16) & 255, (dfRGB >> 16) & 255);
    g = mul8((src >> 8) & 255, (sfRGB >> 8) & 255) + mul8((dst >> 8) & 255, (dfRGB >> 8) & 255);
    b = mul8(src & 255, sfRGB & 255) + mul8(dst & 255, dfRGB & 255);
    a = mul8(src >> 24, sf) + mul8(dst >> 24, df);
    return ((a > 255 ? 255 : a) << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

static int depth_pass(int mode, f32 z, f32 old)
{
    switch (mode)
    {
    case 0: return 0;
    case 1: return z < old;
    case 2: return z == old;
    case 3: return z <= old;
    case 4: return z > old;
    case 5: return z != old;
    case 6: return z >= old;
    default: return 1;
    }
}

static f32 s_fogDensity;
static u32 s_fogColRam, s_fogColVert;

static void fog_setup(void)
{
    u32 den = pvr_reg(0x005F80B8);
    s_fogDensity = (f32)((den >> 8) & 0xFF) / 128.0f * (f32)ldexp(1.0, (int)(s8)(den & 0xFF));
    s_fogColRam = pvr_reg(0x005F80B0) & 0xFFFFFF;
    s_fogColVert = pvr_reg(0x005F80B4) & 0xFFFFFF;
}

static u32 fog_lut(f32 invW)
{
    f32 z = s_fogDensity * invW, m, f;
    int e;
    u32 entry, idx;
    if (!(z >= 1.0f)) z = 1.0f;
    if (z > 255.9999f) z = 255.9999f;
    m = frexpf(z, &e) * 32.0f - 16.0f;
    idx = (u32)m + (u32)(e - 1) * 16;
    if (idx > 127) idx = 127;
    f = m - floorf(m);
    entry = pvr_reg(0x005F8200 + idx * 4);
    return (u32)(((entry >> 8) & 0xFF) * (1.0f - f) + (entry & 0xFF) * f + 0.5f);
}

static u32 fog_mix(u32 c, u32 fogCol, u32 k)
{
    u32 r = (((c >> 16) & 255) * (255 - k) + ((fogCol >> 16) & 255) * k + 127) / 255;
    u32 g = (((c >> 8) & 255) * (255 - k) + ((fogCol >> 8) & 255) * k + 127) / 255;
    u32 b = ((c & 255) * (255 - k) + (fogCol & 255) * k + 127) / 255;
    return (c & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

static u32 shade_nofog(const DrawState *st, u32 base, u32 offs, f32 u, f32 v);

static u32 shade(const DrawState *st, u32 base, u32 offs, f32 u, f32 v, f32 invW)
{
    u32 c;
    if (st->fog == 3) base = (fog_lut(invW) << 24) | s_fogColRam;
    c = shade_nofog(st, base, offs, u, v);
    if (st->fog == 0) c = fog_mix(c, s_fogColRam, fog_lut(invW));
    else if (st->fog == 1 && st->offset) c = fog_mix(c, s_fogColVert, offs >> 24);
    return c;
}

static u32 shade_nofog(const DrawState *st, u32 base, u32 offs, f32 u, f32 v)
{
    u32 c = base;
    if (!st->useAlpha) c |= 0xFF000000u;
    if (st->tex)
    {
        u32 t = sample(st, u, v), ta, ba = c >> 24;
        if (st->ignoreTexAlpha) t |= 0xFF000000u;
        ta = t >> 24;
        switch (st->shading)
        {
        case 0:
            c = t;
            break;
        case 1:
            c = (ta << 24) | (mul8((t >> 16) & 255, (c >> 16) & 255) << 16) | (mul8((t >> 8) & 255, (c >> 8) & 255) << 8) |
                mul8(t & 255, c & 255);
            break;
        case 2:
            c = (ba << 24) | ((mul8((t >> 16) & 255, ta) + mul8((c >> 16) & 255, 255 - ta)) << 16) |
                ((mul8((t >> 8) & 255, ta) + mul8((c >> 8) & 255, 255 - ta)) << 8) | (mul8(t & 255, ta) + mul8(c & 255, 255 - ta));
            break;
        default:
            c = (mul8(ta, ba) << 24) | (mul8((t >> 16) & 255, (c >> 16) & 255) << 16) |
                (mul8((t >> 8) & 255, (c >> 8) & 255) << 8) | mul8(t & 255, c & 255);
            break;
        }
        if (st->offset)
        {
            u32 r = ((c >> 16) & 255) + ((offs >> 16) & 255), g = ((c >> 8) & 255) + ((offs >> 8) & 255), b = (c & 255) + (offs & 255);
            c = (c & 0xFF000000u) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
        }
    }
    return c;
}

static u32 lerp_color(u32 c0, u32 c1, u32 c2, f32 l0, f32 l1, f32 l2)
{
    u32 out = 0;
    int shiftBits;
    for (shiftBits = 0; shiftBits < 32; shiftBits += 8)
    {
        f32 v = ((c0 >> shiftBits) & 255) * l0 + ((c1 >> shiftBits) & 255) * l1 + ((c2 >> shiftBits) & 255) * l2;
        u32 b = v <= 0 ? 0 : v >= 255 ? 255 : (u32)(v + 0.5f);
        out |= b << shiftBits;
    }
    return out;
}

static void draw_triangle(const DrawState *st, const TaVertex *a, const TaVertex *b, const TaVertex *c, int w, int h)
{
    f32 area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y), inv;
    s32 minX, maxX, minY, maxY, x, y;
    int cull = (st->isp >> 27) & 3;
    if (area == 0) return;
    if ((cull == 2 && area < 0) || (cull == 3 && area > 0)) return;
    inv = 1.0f / area;
    minX = (s32)floorf(a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x));
    maxX = (s32)ceilf(a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x));
    minY = (s32)floorf(a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y));
    maxY = (s32)ceilf(a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y));
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > w - 1) maxX = w - 1;
    if (maxY > h - 1) maxY = h - 1;
    if (TA_CLIP_MODE(st->clip) == 2)
    {
        s32 cx0 = (s32)TA_CLIP_X0(st->clip) * 32, cy0 = (s32)TA_CLIP_Y0(st->clip) * 32;
        s32 cx1 = (s32)TA_CLIP_X1(st->clip) * 32 + 31, cy1 = (s32)TA_CLIP_Y1(st->clip) * 32 + 31;
        if (minX < cx0) minX = cx0;
        if (minY < cy0) minY = cy0;
        if (maxX > cx1) maxX = cx1;
        if (maxY > cy1) maxY = cy1;
    }
    for (y = minY; y <= maxY; y++)
    {
        f32 py = (f32)y + 0.5f;
        for (x = minX; x <= maxX; x++)
        {
            f32 px = (f32)x + 0.5f;
            f32 l0, l1, l2, z;
            u32 *dst, color;
            if (TA_CLIP_MODE(st->clip) == 3 && (u32)x >> 5 >= TA_CLIP_X0(st->clip) && (u32)x >> 5 <= TA_CLIP_X1(st->clip) &&
                (u32)y >> 5 >= TA_CLIP_Y0(st->clip) && (u32)y >> 5 <= TA_CLIP_Y1(st->clip))
                continue;
            l0 = ((b->x - px) * (c->y - py) - (c->x - px) * (b->y - py)) * inv;
            l1 = ((c->x - px) * (a->y - py) - (a->x - px) * (c->y - py)) * inv;
            l2 = 1.0f - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            z = a->z * l0 + b->z * l1 + c->z * l2;
            if (!depth_pass(st->depthMode, z, s_depth[y * w + x])) continue;
            {
                u32 base = st->gouraud ? lerp_color(a->base, b->base, c->base, l0, l1, l2) : c->base;
                u32 offs = st->gouraud ? lerp_color(a->offset, b->offset, c->offset, l0, l1, l2) : c->offset;
                f32 u = 0, v = 0;
                if (st->tex && z != 0)
                {
                    u = (a->u * a->z * l0 + b->u * b->z * l1 + c->u * c->z * l2) / z;
                    v = (a->v * a->z * l0 + b->v * b->z * l1 + c->v * c->z * l2) / z;
                }
                color = shade(st, base, offs, u, v, z);
            }
            if (st->list == TA_LIST_PUNCH && (color >> 24) < st->ptRef) continue;
            if (s_depthPass)
            {
                if ((color >> 24) == 255 && st->zWrite) s_depth[y * w + x] = z;
                continue;
            }
            dst = &s_color[y * w + x];
            *dst = st->list == TA_LIST_TRANS ? blend(st, color, *dst) : color;
            if (st->zWrite) s_depth[y * w + x] = z;
            s_stencil[y * w + x] = st->shadow ? 0x80 : 0;
        }
    }
}

static void setup_state(DrawState *st, const TaStrip *s)
{
    st->clip = s->clip;
    st->isp = s->isp;
    st->tsp = s->tsp;
    st->list = s->list;
    st->depthMode = (int)(s->isp >> 29);
    if (s->list == TA_LIST_TRANS && !(pvr_reg(0x005F8098) & 1) && !s_depthPass && ta_strip_can_cover(&s_scene, s))
        st->depthMode = st->depthMode == 4 ? 6 : st->depthMode == 1 ? 3 : st->depthMode;
    st->zWrite = !((s->isp >> 26) & 1);
    st->gouraud = (s->pcw >> 1) & 1;
    st->shadow = (s->pcw >> 7) & 1;
    st->offset = (s->pcw >> 2) & 1;
    st->useAlpha = (s->tsp >> 20) & 1;
    st->ignoreTexAlpha = (s->tsp >> 19) & 1;
    st->shading = (int)((s->tsp >> 6) & 3);
    st->fog = (int)((s->tsp >> 22) & 3);
    st->flipU = (s->tsp >> 18) & 1;
    st->flipV = (s->tsp >> 17) & 1;
    st->clampU = (s->tsp >> 16) & 1;
    st->clampV = (s->tsp >> 15) & 1;
    st->srcInstr = s->tsp >> 29;
    st->dstInstr = (s->tsp >> 26) & 7;
    st->ptRef = pvr_reg(0x005F811C) & 0xFF;
    st->tex = ((s->pcw >> 3) & 1) ? get_texture(s->tcw, s->tsp) : NULL;
    if (st->list == TA_LIST_TRANS && !(pvr_reg(0x005F8098) & 1)) st->zWrite = s_depthPass && st->zWrite;
}

struct TriKey
{
    u32 z, seq, strip, k;
};

static void draw_strip_tri(const DrawState *st, const TaStrip *s, u32 k, int w, int h)
{
    const TaVertex *v = &s_scene.verts[s->first + k];
    if (k & 1) draw_triangle(st, &v[1], &v[0], &v[2], w, h);
    else draw_triangle(st, &v[0], &v[1], &v[2], w, h);
}

static void sort_tri_keys(TriKey *keys, u32 n)
{
    static TriKey *tmp;
    static u32 tmpCap;
    static u32 count[2048];
    u32 pass, i;
    if (n < 2) return;
    if (tmpCap < n)
    {
        free(tmp);
        tmpCap = n + 4096;
        tmp = (TriKey *)malloc(tmpCap * sizeof(TriKey));
        if (!tmp)
        {
            tmpCap = 0;
            return;
        }
    }
    for (pass = 0; pass < 3; pass++)
    {
        TriKey *src = (pass & 1) ? tmp : keys, *dst = (pass & 1) ? keys : tmp;
        u32 shift = pass * 11, sum = 0;
        memset(count, 0, sizeof(count));
        for (i = 0; i < n; i++) count[(src[i].z >> shift) & 0x7FF]++;
        for (i = 0; i < 2048; i++)
        {
            u32 c = count[i];
            count[i] = sum;
            sum += c;
        }
        for (i = 0; i < n; i++) dst[count[(src[i].z >> shift) & 0x7FF]++] = src[i];
    }
    memcpy(keys, tmp, n * sizeof(TriKey));
}

static void draw_list_sorted(u32 list, int w, int h)
{
    static TriKey *keys;
    static u32 keyCap;
    u32 i, n = 0, prevStrip = ~0u;
    DrawState st;
    if (keyCap < s_scene.vertCount)
    {
        free(keys);
        keyCap = s_scene.vertCount + 4096;
        keys = (TriKey *)malloc(keyCap * sizeof(TriKey));
        if (!keys)
        {
            keyCap = 0;
            return;
        }
    }
    for (i = 0; i < s_scene.stripCount; i++)
    {
        const TaStrip *s = &s_scene.strips[i];
        const TaVertex *v = &s_scene.verts[s->first];
        u32 k;
        if (s->list != list || s->count < 3) continue;
        for (k = 0; k + 2 < s->count; k++)
        {
            f32 z = v[k].z;
            union { f32 f; u32 u; } bits;
            if (v[k + 1].z < z) z = v[k + 1].z;
            if (v[k + 2].z < z) z = v[k + 2].z;
            bits.f = z > 0 ? z : 0;
            keys[n].z = g_ta_trans_always_last && (s->isp >> 29) == 7 ? 0xFFFFFFFFu : bits.u;
            keys[n].seq = n;
            keys[n].strip = i;
            keys[n].k = k;
            n++;
        }
    }
    sort_tri_keys(keys, n);
    s_depthPass = 1;
    for (i = 0; i < s_scene.stripCount; i++)
    {
        const TaStrip *s = &s_scene.strips[i];
        u32 k;
        if (s->list != list || s->count < 3 || !ta_strip_can_cover(&s_scene, s)) continue;
        setup_state(&st, s);
        for (k = 0; k + 2 < s->count; k++) draw_strip_tri(&st, s, k, w, h);
    }
    s_depthPass = 0;
    prevStrip = ~0u;
    for (i = 0; i < n; i++)
    {
        const TaStrip *s = &s_scene.strips[keys[i].strip];
        if (keys[i].strip != prevStrip)
        {
            setup_state(&st, s);
            prevStrip = keys[i].strip;
        }
        draw_strip_tri(&st, s, keys[i].k, w, h);
    }
}

static void draw_list(u32 list, int w, int h)
{
    u32 i, k;
    if (list == TA_LIST_TRANS && !(pvr_reg(0x005F8098) & 1))
    {
        draw_list_sorted(list, w, h);
        return;
    }
    for (i = 0; i < s_scene.stripCount; i++)
    {
        const TaStrip *s = &s_scene.strips[i];
        DrawState st;
        if (s->list != list || s->count < 3) continue;
        setup_state(&st, s);
        for (k = 0; k + 2 < s->count; k++) draw_strip_tri(&st, s, k, w, h);
    }
}

enum { MV_XOR, MV_OR, MV_INCLUDE, MV_EXCLUDE };

static void mod_triangle(const TaModTri *t, int op, int cull, int w, int h)
{
    f32 area = (t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) - (t->x[2] - t->x[0]) * (t->y[1] - t->y[0]), inv;
    s32 minX, maxX, minY, maxY, x, y;
    if (area == 0) return;
    if ((cull == 2 && area < 0) || (cull == 3 && area > 0)) return;
    inv = 1.0f / area;
    minX = (s32)floorf(t->x[0] < t->x[1] ? (t->x[0] < t->x[2] ? t->x[0] : t->x[2]) : (t->x[1] < t->x[2] ? t->x[1] : t->x[2]));
    maxX = (s32)ceilf(t->x[0] > t->x[1] ? (t->x[0] > t->x[2] ? t->x[0] : t->x[2]) : (t->x[1] > t->x[2] ? t->x[1] : t->x[2]));
    minY = (s32)floorf(t->y[0] < t->y[1] ? (t->y[0] < t->y[2] ? t->y[0] : t->y[2]) : (t->y[1] < t->y[2] ? t->y[1] : t->y[2]));
    maxY = (s32)ceilf(t->y[0] > t->y[1] ? (t->y[0] > t->y[2] ? t->y[0] : t->y[2]) : (t->y[1] > t->y[2] ? t->y[1] : t->y[2]));
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > w - 1) maxX = w - 1;
    if (maxY > h - 1) maxY = h - 1;
    for (y = minY; y <= maxY; y++)
    {
        f32 py = (f32)y + 0.5f;
        for (x = minX; x <= maxX; x++)
        {
            f32 px = (f32)x + 0.5f;
            f32 l0 = ((t->x[1] - px) * (t->y[2] - py) - (t->x[2] - px) * (t->y[1] - py)) * inv;
            f32 l1 = ((t->x[2] - px) * (t->y[0] - py) - (t->x[0] - px) * (t->y[2] - py)) * inv;
            f32 l2 = 1.0f - l0 - l1;
            u8 *s = &s_stencil[y * w + x];
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            switch (op)
            {
            case MV_XOR:
            case MV_OR:
                if (t->z[0] * l0 + t->z[1] * l1 + t->z[2] * l2 > s_depth[y * w + x]) *s = op == MV_XOR ? *s ^ 2 : *s | 2;
                break;
            case MV_INCLUDE: *s = (*s & 3) ? (*s & ~3) | 1 : *s & ~3; break;
            default: break;
            }
        }
    }
}

static void draw_mod_volumes(int w, int h)
{
    u32 i, k, base = ~0u, any = 0, scale = pvr_reg(0x005F8074) & 0xFF;
    for (i = 0; i < s_scene.modVolCount; i++)
    {
        const TaModVol *m = &s_scene.modVols[i];
        u32 mode = m->isp >> 29, cull = (m->isp >> 27) & 3;
        if (m->list != TA_LIST_OPAQUE_MOD || !m->count) continue;
        any = 1;
        if (base == ~0u) base = m->first;
        for (k = 0; k < m->count; k++)
            mod_triangle(&s_scene.modTris[m->first + k], !m->last && mode ? MV_OR : MV_XOR, (int)cull, w, h);
        if (mode == 1 || mode == 2)
        {
            for (k = base; k < m->first + m->count; k++)
                mod_triangle(&s_scene.modTris[k], mode == 1 ? MV_INCLUDE : MV_EXCLUDE, (int)cull, w, h);
            base = ~0u;
        }
    }
    if (!any) return;
    for (i = 0; i < (u32)(w * h); i++)
    {
        if ((s_stencil[i] & 0x81) == 0x81)
        {
            u32 c = s_color[i];
            u32 r = ((c >> 16) & 255) * scale >> 8, g = ((c >> 8) & 255) * scale >> 8, b = (c & 255) * scale >> 8;
            u32 a = 255 - scale, ca = c >> 24;
            ca = (a * a + ca * (255 - a) + 127) / 255;
            s_color[i] = (ca << 24) | (r << 16) | (g << 8) | b;
        }
        s_stencil[i] &= ~3;
    }
}

static void vram32_write16(u32 off32, u32 v) { buf_w16(g_vram, vram_offset64(off32 & 0x007FFFFEu), (u16)v); }
static void vram32_write32(u32 off32, u32 v) { buf_w32(g_vram, vram_offset64(off32 & 0x007FFFFCu), v); }
static void vram32_write8(u32 off32, u32 v) { buf_w8(g_vram, vram_offset64(off32 & 0x007FFFFFu), (u8)v); }

static void write_framebuffer(int w, int h)
{
    u32 ctrl = pvr_reg(0x005F8048), base = pvr_reg(0x005F8060), stride = (pvr_reg(0x005F804C) & 0x1FF) * 8;
    u32 mode = ctrl & 7, kval = (ctrl >> 8) & 0xFF, threshold = (ctrl >> 16) & 0xFF;
    int hscale = (pvr_reg(0x005F80F4) >> 16) & 1, outW = hscale ? w / 2 : w, x, y;
    for (y = 0; y < h; y++)
    {
        u32 line = base + (u32)y * stride;
        for (x = 0; x < outW; x++)
        {
            u32 c = s_color[y * w + (hscale ? x * 2 : x)], r, g, b, a = c >> 24;
            if (hscale)
            {
                u32 c2 = s_color[y * w + x * 2 + 1];
                c = (((c >> 1) & 0x7F7F7F7Fu) + ((c2 >> 1) & 0x7F7F7F7Fu));
                a = c >> 24;
            }
            r = (c >> 16) & 255;
            g = (c >> 8) & 255;
            b = c & 255;
            switch (mode)
            {
            case 0: vram32_write16(line + x * 2, ((kval & 0x80) << 8) | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)); break;
            case 1: vram32_write16(line + x * 2, (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); break;
            case 2: vram32_write16(line + x * 2, (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)); break;
            case 3: vram32_write16(line + x * 2, (a >= threshold ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)); break;
            case 4:
                vram32_write8(line + x * 3, b);
                vram32_write8(line + x * 3 + 1, g);
                vram32_write8(line + x * 3 + 2, r);
                break;
            case 5: vram32_write32(line + x * 4, (kval << 24) | (c & 0xFFFFFF)); break;
            default: vram32_write32(line + x * 4, c); break;
            }
        }
    }
}

static int s_lastW, s_lastH;

const u32 *soft_render_image(int *w, int *h)
{
    *w = s_lastW;
    *h = s_lastH;
    return s_color;
}

void soft_render_frame(void)
{
    u32 words, xclip = pvr_reg(0x005F8068), yclip = pvr_reg(0x005F806C), i;
    const u32 *data = pvr_ta_data(&words);
    int w = (int)((xclip >> 16) & 0x7FF) + 1, h = (int)((yclip >> 16) & 0x3FF) + 1;
    if ((pvr_reg(0x005F80F4) >> 16) & 1) w *= 2;
    if (w > MAX_W) w = MAX_W;
    if (h > MAX_H) h = MAX_H;
    if (w < 8 || h < 8) return;
    prof_begin(PROF_RENDER);
    if (!s_color)
    {
        s_color = (u32 *)malloc(MAX_W * MAX_H * 4);
        s_depth = (f32 *)malloc(MAX_W * MAX_H * 4);
        s_stencil = (u8 *)malloc(MAX_W * MAX_H);
        s_texPool = (u32 *)malloc(TEX_POOL);
        ta_scene_init(&s_scene);
        if (!s_color || !s_depth || !s_stencil || !s_texPool)
        {
            prof_end(PROF_RENDER);
            return;
        }
    }
    ta_scene_decode(&s_scene, data, words, NULL);
    if (log_level() >= LOG_LEVEL_DEBUG)
    {
        f32 minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (i = 0; i < s_scene.vertCount; i++)
        {
            const TaVertex *v = &s_scene.verts[i];
            if (v->x < minX) minX = v->x;
            if (v->x > maxX) maxX = v->x;
            if (v->y < minY) minY = v->y;
            if (v->y > maxY) maxY = v->y;
        }
        LOG_DEBUG("[soft] %u strips, %u vertices, x %.1f..%.1f, y %.1f..%.1f, %dx%d", s_scene.stripCount, s_scene.vertCount, minX,
                  maxX, minY, maxY, w, h);
        for (i = 0; i < s_scene.stripCount && i < 6; i++)
        {
            const TaStrip *st = &s_scene.strips[i];
            const TaVertex *v = &s_scene.verts[st->first];
            LOG_DEBUG("[soft]  strip %u: list %u pcw %08X isp %08X tsp %08X tcw %08X, %u verts, v0 (%.1f, %.1f, %f) uv (%.3f, %.3f)", i,
                      st->list, st->pcw, st->isp, st->tsp, st->tcw, st->count, v->x, v->y, v->z, v->u, v->v);
        }
    }
    s_texCount = 0;
    s_texPoolUsed = 0;
    for (i = 0; i < (u32)(w * h); i++)
    {
        s_color[i] = s_scene.bgColor;
        s_depth[i] = s_scene.bgDepth;
    }
    memset(s_stencil, 0, (size_t)(w * h));
    fog_setup();
    draw_list(TA_LIST_OPAQUE, w, h);
    draw_list(TA_LIST_PUNCH, w, h);
    draw_mod_volumes(w, h);
    draw_list(TA_LIST_TRANS, w, h);
    s_lastW = w;
    s_lastH = h;
    write_framebuffer(w, h);
    prof_end(PROF_RENDER);
}

void soft_render_install(void)
{
    g_pvr_render = soft_render_frame;
}
