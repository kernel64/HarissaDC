// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "ta_scene.h"

#include <stdlib.h>
#include <string.h>

#include "../core/log.h"
#include "../hw/pvr.h"
#include "../mem/mem.h"

#define PARA_END_OF_LIST 0
#define PARA_USER_CLIP 1
#define PARA_OBJ_LIST_SET 2
#define PARA_POLYGON 4
#define PARA_SPRITE 5
#define PARA_VERTEX 7

static u32 vram32_read32(u32 off32)
{
    return buf_r32(g_vram, vram_offset64(off32 & 0x007FFFFCu));
}

static f32 fl(u32 bits)
{
    f32 f;
    memcpy(&f, &bits, 4);
    return f;
}

static u32 to_byte(f32 v)
{
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (u32)(v * 255.0f + 0.5f);
}

static u32 argb_from_floats(f32 a, f32 r, f32 g, f32 b)
{
    return (to_byte(a) << 24) | (to_byte(r) << 16) | (to_byte(g) << 8) | to_byte(b);
}

static u32 intensity_fix16(u32 bits)
{
    u32 e = bits >> 23, shift;
    if ((s32)bits <= 0) return 0;
    if (e >= 127) return 65536;
    shift = 134 - e;
    return shift >= 32 ? 0 : ((bits & 0x7FFFFFu) | 0x800000u) >> shift;
}

static u32 scale_color(u32 face, u32 intensityBits)
{
    u32 k = intensity_fix16(intensityBits);
    u32 r = (((face >> 16) & 255) * k + 32768) >> 16, g = (((face >> 8) & 255) * k + 32768) >> 16,
        b = ((face & 255) * k + 32768) >> 16;
    return (face & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

void ta_scene_orientation(const TaScene *s, f32 xScale, f32 *across, f32 *down, f32 *up)
{
    u32 k, i;
    f32 a = 0.0f, d = 0.0f, u = 0.0f;
    for (k = 0; k < s->stripCount; k++)
    {
        const TaStrip *st = &s->strips[k];
        if (!(st->pcw & 8) || st->count < 3) continue;
        for (i = st->first; i + 2 < st->first + st->count; i++)
        {
            const TaVertex *p = &s->verts[i], *q = &s->verts[i + 1], *r = &s->verts[i + 2];
            f32 x1 = (q->x - p->x) * xScale, y1 = q->y - p->y, x2 = (r->x - p->x) * xScale, y2 = r->y - p->y;
            f32 det = x1 * y2 - x2 * y1, area, du1, du2, dudx, dudy;
            if (p->z != q->z || p->z != r->z) continue;
            area = det < 0.0f ? -det : det;
            if (area < 4.0f) continue;
            du1 = q->u - p->u;
            du2 = r->u - p->u;
            dudx = (du1 * y2 - du2 * y1) / det;
            dudy = (du2 * x1 - du1 * x2) / det;
            if (dudx < 0.0f) dudx = -dudx;
            if (dudx > 2.0f * (dudy < 0.0f ? -dudy : dudy)) a += 1.0f;
            else if (dudy > 2.0f * dudx) d += 1.0f;
            else if (-dudy > 2.0f * dudx) u += 1.0f;
        }
    }
    *across = a;
    *down = d;
    *up = u;
}

int ta_strip_can_cover(const TaScene *sc, const TaStrip *s)
{
    u32 src = s->tsp >> 29, dst = (s->tsp >> 26) & 7, k;
    if ((s->isp >> 26) & 1) return 0;
    if (!((src == 4 && dst == 5) || (src == 1 && dst == 0))) return 0;
    if ((s->tsp >> 20) & 1)
        for (k = 0; k < s->count; k++)
            if ((sc->verts[s->first + k].base >> 24) != 255) return 0;
    return 1;
}

void ta_scene_init(TaScene *s)
{
    memset(s, 0, sizeof(*s));
}

void ta_scene_free(TaScene *s)
{
    free(s->verts);
    free(s->strips);
    free(s->modTris);
    free(s->modVols);
    memset(s, 0, sizeof(*s));
}

static EMU_NOINLINE int grow_vertices(TaScene *s)
{
    u32 cap = s->vertCap ? s->vertCap * 2 : 4096;
    TaVertex *v = (TaVertex *)realloc(s->verts, cap * sizeof(TaVertex));
    if (!v) return 0;
    s->verts = v;
    s->vertCap = cap;
    return 1;
}

static EMU_INLINE void note_z(TaScene *s, f32 z)
{
    if (z < 1e6f)
    {
        if (z > s->zMax) s->zMax = z;
    }
    else if (z < 1e37f && z > s->zHuge)
        s->zHuge = z;
}

static EMU_INLINE TaVertex *add_vertex(TaScene *s)
{
    if (s->vertCount == s->vertCap && !grow_vertices(s)) return NULL;
    return &s->verts[s->vertCount++];
}

static TaStrip *add_strip(TaScene *s)
{
    if (s->stripCount == s->stripCap)
    {
        u32 cap = s->stripCap ? s->stripCap * 2 : 1024;
        TaStrip *t = (TaStrip *)realloc(s->strips, cap * sizeof(TaStrip));
        if (!t) return NULL;
        s->strips = t;
        s->stripCap = cap;
    }
    s->strips[s->stripCount].clip = 0;
    return &s->strips[s->stripCount++];
}

static TaModTri *add_mod_tri(TaScene *s)
{
    if (s->modTriCount == s->modTriCap)
    {
        u32 cap = s->modTriCap ? s->modTriCap * 2 : 1024;
        TaModTri *t = (TaModTri *)realloc(s->modTris, cap * sizeof(TaModTri));
        if (!t) return NULL;
        s->modTris = t;
        s->modTriCap = cap;
    }
    return &s->modTris[s->modTriCount++];
}

static TaModVol *add_mod_vol(TaScene *s, u32 isp, u32 list, u32 last)
{
    TaModVol *m;
    if (s->modVolCount == s->modVolCap)
    {
        u32 cap = s->modVolCap ? s->modVolCap * 2 : 256;
        TaModVol *t = (TaModVol *)realloc(s->modVols, cap * sizeof(TaModVol));
        if (!t) return NULL;
        s->modVols = t;
        s->modVolCap = cap;
    }
    m = &s->modVols[s->modVolCount++];
    m->isp = isp;
    m->list = list;
    m->last = last;
    m->first = s->modTriCount;
    m->count = 0;
    return m;
}

struct Header
{
    u32 pcw, isp, tsp, tcw;
    u32 faceColor, faceOffset;
    int sprite;
    u32 spriteBase, spriteOffset;
    u32 list;
    u32 clipRect;
    u32 clip;
    u32 pass;
};

static u32 param_words(u32 pcw, u32 list, u32 vertexWords)
{
    u32 type = pcw >> 29;
    if (type == PARA_VERTEX) return vertexWords;
    if (type == PARA_POLYGON)
    {
        if (list == TA_LIST_OPAQUE_MOD || list == TA_LIST_TRANS_MOD) return 8;
        return (((pcw >> 4) & 3) == 2 && (((pcw >> 2) & 1) || ((pcw >> 6) & 1))) ? 16 : 8;
    }
    return 8;
}

static u32 vertex_words(u32 pcw, u32 list, int sprite)
{
    u32 tex = (pcw >> 3) & 1, col = (pcw >> 4) & 3;
    if (sprite || list == TA_LIST_OPAQUE_MOD || list == TA_LIST_TRANS_MOD) return 16;
    if ((pcw >> 6) & 1) return tex ? 16 : 8;
    return (tex && col == 1) ? 16 : 8;
}

static void put_bits(f32 *dst, u32 bits)
{
    memcpy(dst, &bits, 4);
}

static void decode_vertex(TaVertex *v, const u32 *p, const Header *h)
{
    u32 pcw = h->pcw, tex = (pcw >> 3) & 1, col = (pcw >> 4) & 3, uv16 = pcw & 1, two = (pcw >> 6) & 1;
    memcpy(&v->x, &p[1], 12);
    v->offset = 0;
    if (!tex)
    {
        put_bits(&v->u, 0);
        put_bits(&v->v, 0);
        switch (col)
        {
        case 0: v->base = p[6]; break;
        case 1: v->base = argb_from_floats(fl(p[4]), fl(p[5]), fl(p[6]), fl(p[7])); break;
        default: v->base = scale_color(h->faceColor, p[two ? 4 : 6]); break;
        }
        if (two && col == 0) v->base = p[4];
        return;
    }
    if (uv16)
    {
        put_bits(&v->u, p[4] & 0xFFFF0000u);
        put_bits(&v->v, p[4] << 16);
    }
    else
        memcpy(&v->u, &p[4], 8);
    if (two)
    {
        if (col == 0) { v->base = p[6]; v->offset = p[7]; }
        else { v->base = scale_color(h->faceColor, p[6]); v->offset = scale_color(h->faceOffset, p[7]); }
        return;
    }
    switch (col)
    {
    case 0:
        v->base = p[6];
        v->offset = p[7];
        break;
    case 1:
        v->base = argb_from_floats(fl(p[8]), fl(p[9]), fl(p[10]), fl(p[11]));
        v->offset = argb_from_floats(fl(p[12]), fl(p[13]), fl(p[14]), fl(p[15]));
        break;
    default:
        v->base = scale_color(h->faceColor, p[6]);
        v->offset = scale_color(h->faceOffset, p[7]);
        break;
    }
}

static void decode_sprite(TaScene *s, const u32 *p, const Header *h)
{
    TaStrip *st = add_strip(s);
    f32 ax = fl(p[1]), ay = fl(p[2]), az = fl(p[3]), bx = fl(p[4]), by = fl(p[5]), bz = fl(p[6]);
    f32 cx = fl(p[7]), cy = fl(p[8]), cz = fl(p[9]), dx = fl(p[10]), dy = fl(p[11]);
    f32 au = fl(p[13] & 0xFFFF0000u), av = fl(p[13] << 16), bu = fl(p[14] & 0xFFFF0000u), bv = fl(p[14] << 16);
    f32 cu = fl(p[15] & 0xFFFF0000u), cv = fl(p[15] << 16);
    TaVertex *v;
    if (!st) return;
    st->isp = h->isp;
    st->tsp = h->tsp;
    st->tcw = h->tcw;
    st->pcw = h->pcw;
    st->list = h->list;
    st->clip = h->clip;
    st->pass = h->pass;
    st->first = s->vertCount;
    st->count = 4;
#define SPRITE_VERTEX(X, Y, Z, U, V) \
    v = add_vertex(s); \
    if (!v) return; \
    v->x = X; v->y = Y; v->z = Z; v->u = U; v->v = V; v->base = h->spriteBase; v->offset = h->spriteOffset;     note_z(s, v->z);
    SPRITE_VERTEX(ax, ay, az, au, av);
    SPRITE_VERTEX(bx, by, bz, bu, bv);
    SPRITE_VERTEX(dx, dy, az + cz - bz, au + cu - bu, av + cv - bv);
    SPRITE_VERTEX(cx, cy, cz, cu, cv);
#undef SPRITE_VERTEX
}

static u32 reg_of(const u32 *regs, u32 addr)
{
    return regs ? regs[(addr - 0x005F8000u) >> 2] : pvr_reg(addr);
}

static void decode_background(TaScene *s, const u32 *regs)
{
    u32 tagReg = reg_of(regs, 0x005F808C), base = reg_of(regs, 0x005F8020) & 0x00F00000u;
    u32 addr = base + ((tagReg >> 3) & 0x1FFFFF) * 4, isp = vram32_read32(addr);
    u32 skip = (tagReg >> 24) & 7, vertexStart = addr + 12, colorWord = 3 + ((isp >> 25) & 1 ? 2 : 0);
    (void)skip;
    s->bgColor = vram32_read32(vertexStart + colorWord * 4);
    s->bgDepth = fl(reg_of(regs, 0x005F8088));
}

int g_ta_trans_always_last;

void ta_scene_decode(TaScene *s, const u32 *data, u32 words, const u32 *regs)
{
    Header h;
    u32 pos = 0, list = 0xFF, vertexWords = 8, ended = 0, pass = 0;
    TaStrip *strip = NULL;
    TaModVol *vol = NULL;
    memset(&h, 0, sizeof(h));
    h.faceColor = h.faceOffset = 0xFFFFFFFFu;
    s->vertCount = 0;
    s->stripCount = 0;
    s->zMax = 0.0f;
    s->zHuge = 0.0f;
    s->modTriCount = 0;
    s->modVolCount = 0;
    decode_background(s, regs);
    while (pos + 8 <= words)
    {
        const u32 *p = data + pos;
        u32 pcw = p[0], type = pcw >> 29, size = param_words(pcw, list, vertexWords);
        if (pos + size > words) break;
        switch (type)
        {
        case PARA_END_OF_LIST:
            if (list != 0xFF) ended |= 1u << list;
            list = 0xFF;
            strip = NULL;
            vol = NULL;
            break;
        case PARA_POLYGON:
        case PARA_SPRITE:
            if (list == 0xFF)
            {
                list = (pcw >> 24) & 7;
                if (ended & (1u << list))
                {
                    pass++;
                    ended = 0;
                }
            }
            h.pcw = pcw;
            h.clip = ((pcw >> 16) & 3) >= 2 ? (((pcw >> 16) & 3) << 24) | h.clipRect : 0;
            h.isp = p[1];
            h.tsp = p[2];
            h.tcw = p[3];
            h.list = list;
            h.pass = pass;
            h.sprite = type == PARA_SPRITE;
            if (h.sprite)
            {
                h.spriteBase = p[4];
                h.spriteOffset = p[5];
            }
            else if (((pcw >> 4) & 3) == 2)
            {
                if (size == 16 && !((pcw >> 6) & 1))
                {
                    h.faceColor = argb_from_floats(fl(p[8]), fl(p[9]), fl(p[10]), fl(p[11]));
                    h.faceOffset = argb_from_floats(fl(p[12]), fl(p[13]), fl(p[14]), fl(p[15]));
                }
                else
                    h.faceColor = argb_from_floats(fl(p[4]), fl(p[5]), fl(p[6]), fl(p[7]));
            }
            vertexWords = vertex_words(pcw, list, h.sprite);
            strip = NULL;
            vol = list == TA_LIST_OPAQUE_MOD || list == TA_LIST_TRANS_MOD ? add_mod_vol(s, h.isp, list, (h.pcw >> 6) & 1) : NULL;
            break;
        case PARA_VERTEX:
            if (list == TA_LIST_OPAQUE_MOD || list == TA_LIST_TRANS_MOD)
            {
                TaModTri *t;
                u32 k;
                if (!vol) vol = add_mod_vol(s, h.isp, list, (h.pcw >> 6) & 1);
                t = vol ? add_mod_tri(s) : NULL;
                if (!t) return;
                for (k = 0; k < 3; k++)
                {
                    t->x[k] = fl(p[1 + k * 3]);
                    t->y[k] = fl(p[2 + k * 3]);
                    t->z[k] = fl(p[3 + k * 3]);
                }
                vol->count++;
                break;
            }
            if (h.sprite)
            {
                decode_sprite(s, p, &h);
                break;
            }
            if (!strip)
            {
                strip = add_strip(s);
                if (!strip) return;
                strip->isp = h.isp;
                strip->tsp = h.tsp;
                strip->tcw = h.tcw;
                strip->pcw = h.pcw;
                strip->list = h.list;
                strip->clip = h.clip;
                strip->pass = h.pass;
                strip->first = s->vertCount;
                strip->count = 0;
            }
            {
                TaVertex *v = add_vertex(s);
                if (!v) return;
                decode_vertex(v, p, &h);
                note_z(s, v->z);
                strip->count++;
            }
            if ((pcw >> 28) & 1) strip = NULL;
            break;
        case PARA_USER_CLIP:
            h.clipRect = (p[4] & 63) | ((p[5] & 63) << 6) | ((p[6] & 63) << 12) | ((p[7] & 63) << 18);
            break;
        default:
            break;
        }
        pos += size;
    }
}
