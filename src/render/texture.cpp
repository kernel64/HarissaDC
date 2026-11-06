// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "texture.h"

#include "../mem/mem.h"

#define VRAM_MASK (VRAM_SIZE - 1)

static u32 log2u(u32 v)
{
    u32 n = 0;
    while ((1u << n) < v) n++;
    return n;
}

static u32 twiddle_square(u32 x, u32 y, u32 s)
{
    u32 r = 0, bit = 0, m;
    for (m = 1; m < s; m <<= 1)
    {
        if (y & m) r |= 1u << bit;
        if (x & m) r |= 1u << (bit + 1);
        bit += 2;
    }
    return r;
}

u32 tex_twiddle(u32 x, u32 y, u32 w, u32 h)
{
    if (w >= h) return (x / h) * h * h + twiddle_square(x % h, y, h);
    return (y / w) * w * w + twiddle_square(x, y % w, w);
}

static u32 from1555(u32 c)
{
    u32 r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
    return ((c & 0x8000) ? 0xFF000000u : 0) | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

static u32 from565(u32 c)
{
    u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

static u32 from4444(u32 c)
{
    return (((c >> 12) & 15) * 17u << 24) | (((c >> 8) & 15) * 17u << 16) | (((c >> 4) & 15) * 17u << 8) | ((c & 15) * 17u);
}

static u32 clamp255(s32 v) { return v < 0 ? 0 : v > 255 ? 255 : (u32)v; }

static u32 from_yuv(u32 y, u32 u, u32 v)
{
    s32 cu = (s32)u - 128, cv = (s32)v - 128;
    s32 r = (s32)y + (cv * 11 / 8), g = (s32)y - (cu * 11 / 32) - (cv * 23 / 32), b = (s32)y + (cu * 111 / 64);
    return 0xFF000000u | (clamp255(r) << 16) | (clamp255(g) << 8) | clamp255(b);
}

static u32 convert16(u32 fmt, u32 c)
{
    switch (fmt)
    {
    case 0: return from1555(c);
    case 1: return from565(c);
    case 2: return from4444(c);
    default: return 0xFF000000u | (c & 0xFF) * 0x010101u;
    }
}

static u32 palette_entry(const u32 *pal, u32 fmt, u32 index)
{
    u32 c = pal[index & 1023];
    switch (fmt)
    {
    case 0: return from1555(c & 0xFFFF);
    case 1: return from565(c & 0xFFFF);
    case 2: return from4444(c & 0xFFFF);
    default: return c;
    }
}

static u32 mip_offset(u32 n)
{
    return 3 + ((1u << (2 * n)) - 1) / 3;
}

static u32 vq_mip_offset(u32 n)
{
    return n == 0 ? 0 : 1 + ((1u << (2 * (n - 1))) - 1) / 3;
}

static __inline u32 spread(u32 v)
{
    v = (v | (v << 8)) & 0x00FF00FFu;
    v = (v | (v << 4)) & 0x0F0F0F0Fu;
    v = (v | (v << 2)) & 0x33333333u;
    return (v | (v << 1)) & 0x55555555u;
}

static void twiddle_tables(u32 w, u32 h, u32 *xo, u32 *yo)
{
    u32 i;
    if (w >= h)
    {
        for (i = 0; i < w; i++) xo[i] = (i / h) * h * h + (spread(i % h) << 1);
        for (i = 0; i < h; i++) yo[i] = spread(i);
    }
    else
    {
        for (i = 0; i < w; i++) xo[i] = spread(i) << 1;
        for (i = 0; i < h; i++) yo[i] = (i / w) * w * w + spread(i % w);
    }
}

#define FOR_TEXELS16(CONV)                                                             \
    for (y = 0; y < h; y++)                                                            \
    {                                                                                  \
        u32 *o = out + y * w;                                                          \
        if (scan)                                                                      \
            for (x = 0; x < w; x++) o[x] = CONV(buf_r16(vram, (addr + (y * stride + x) * 2) & VRAM_MASK)); \
        else                                                                           \
            for (x = 0; x < w; x++) o[x] = CONV(buf_r16(vram, (addr + (xo[x] + yo[y]) * 2) & VRAM_MASK)); \
    }

static u32 fromBump(u32 c) { return 0xFF000000u | (c & 0xFF) * 0x010101u; }

int tex_rect_ok(const TexDesc *d)
{
    u32 tcw = d->tcw, fmt = (tcw >> 27) & 7;
    if ((tcw >> 31) & 1 || (tcw >> 30) & 1 || fmt == 3 || fmt > 6) return 0;
    return fmt >= 5 || !((tcw >> 26) & 1);
}

u32 tex_texels_per_byte2(const TexDesc *d)
{
    u32 fmt = (d->tcw >> 27) & 7;
    return fmt == 5 ? 4 : fmt == 6 ? 2 : 1;
}

static __inline u32 gather(u32 v)
{
    v &= 0x55555555u;
    v = (v | (v >> 1)) & 0x33333333u;
    v = (v | (v >> 2)) & 0x0F0F0F0Fu;
    v = (v | (v >> 4)) & 0x00FF00FFu;
    return (v | (v >> 8)) & 0x0000FFFFu;
}

void tex_untwiddle(u32 i, u32 w, u32 h, u32 *x, u32 *y)
{
    u32 s = w < h ? w : h, q = i / (s * s), r = i % (s * s);
    *x = gather(r >> 1);
    *y = gather(r);
    if (w >= h) *x += q * s;
    else *y += q * s;
}

int tex_decode_rect(const TexDesc *d, const u8 *vram, const u32 *palette, u32 *out, u32 x0, u32 y0, u32 x1, u32 y1)
{
    u32 tcw = d->tcw, w = d->width, h = d->height, x, y;
    u32 fmt = (tcw >> 27) & 7, addr = (tcw & 0x1FFFFF) << 3;
    u32 xo[1024], yo[1024];
    if (!tex_rect_ok(d) || w > 1024 || h > 1024 || x1 > w || y1 > h) return -1;
    for (x = x0; x < x1; x++) xo[x] = w >= h ? (x / h) * h * h + (spread(x % h) << 1) : spread(x) << 1;
    for (y = y0; y < y1; y++) yo[y] = w >= h ? spread(y) : (y / w) * w * w + spread(y % w);
    if (fmt >= 5)
    {
        u32 bank = fmt == 5 ? ((tcw >> 21) & 63) * 16 : ((tcw >> 25) & 3) * 256, pal[256], i;
        for (i = 0; i < (fmt == 5 ? 16u : 256u); i++) pal[i] = palette_entry(palette, d->palFormat, bank + i);
        for (y = y0; y < y1; y++)
        {
            u32 *o = out + y * w;
            if (fmt == 5)
                for (x = x0; x < x1; x++)
                {
                    u32 t = xo[x] + yo[y];
                    u8 b = buf_r8(vram, (addr + t / 2) & VRAM_MASK);
                    o[x] = pal[(t & 1) ? (b >> 4) : (b & 15)];
                }
            else
                for (x = x0; x < x1; x++) o[x] = pal[buf_r8(vram, (addr + xo[x] + yo[y]) & VRAM_MASK)];
        }
        return 0;
    }
    for (y = y0; y < y1; y++)
    {
        u32 *o = out + y * w;
        for (x = x0; x < x1; x++)
        {
            u32 c = buf_r16(vram, (addr + (xo[x] + yo[y]) * 2) & VRAM_MASK);
            o[x] = fmt == 0 ? from1555(c) : fmt == 1 ? from565(c) : fmt == 2 ? from4444(c) : fromBump(c);
        }
    }
    return 0;
}

int tex_decode(const TexDesc *d, const u8 *vram, const u32 *palette, u32 *out)
{
    u32 tcw = d->tcw, w = d->width, h = d->height, x, y;
    u32 fmt = (tcw >> 27) & 7, addr = (tcw & 0x1FFFFF) << 3;
    int mipmapped = (tcw >> 31) & 1, vq = (tcw >> 30) & 1, scan = (tcw >> 26) & 1;
    u32 n = log2u(w);
    u32 xo[1024], yo[1024];

    if (w > 1024 || h > 1024) return -1;

    if (fmt >= 5)
    {
        u32 bank = fmt == 5 ? ((tcw >> 21) & 63) * 16 : ((tcw >> 25) & 3) * 256, pal[256], i;
        for (i = 0; i < (fmt == 5 ? 16u : 256u); i++) pal[i] = palette_entry(palette, d->palFormat, bank + i);
        if (mipmapped) addr += fmt == 5 ? mip_offset(n) / 2 : mip_offset(n);
        twiddle_tables(w, h, xo, yo);
        for (y = 0; y < h; y++)
        {
            u32 *o = out + y * w;
            if (fmt == 5)
                for (x = 0; x < w; x++)
                {
                    u32 t = xo[x] + yo[y];
                    u8 b = buf_r8(vram, (addr + t / 2) & VRAM_MASK);
                    o[x] = pal[(t & 1) ? (b >> 4) : (b & 15)];
                }
            else
                for (x = 0; x < w; x++) o[x] = pal[buf_r8(vram, (addr + xo[x] + yo[y]) & VRAM_MASK)];
        }
        return 0;
    }

    if (vq)
    {
        u32 idxAddr = addr + 2048 + (mipmapped ? vq_mip_offset(n) : 0), cb[1024], i;
        u32 w2 = w / 2 ? w / 2 : 1, h2 = h / 2 ? h / 2 : 1, cf = fmt == 3 ? 1 : fmt;
        for (i = 0; i < 1024; i++) cb[i] = convert16(cf, buf_r16(vram, (addr + i * 2) & VRAM_MASK));
        twiddle_tables(w2, h2, xo, yo);
        for (y = 0; y < h; y++)
        {
            u32 *o = out + y * w, yy = yo[y >> 1 < h2 ? y >> 1 : h2 - 1];
            for (x = 0; x < w; x++)
            {
                u32 block = buf_r8(vram, (idxAddr + xo[x >> 1 < w2 ? x >> 1 : w2 - 1] + yy) & VRAM_MASK);
                o[x] = cb[block * 4 + (((x & 1) << 1) | (y & 1))];
            }
        }
        return 0;
    }

    if (mipmapped && !scan) addr += mip_offset(n) * 2;
    if (!scan) twiddle_tables(w, h, xo, yo);

    if (fmt == 3)
    {
        u32 stride = (scan && ((tcw >> 25) & 1) && d->strideWidth) ? d->strideWidth : w;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x += 2)
            {
                u32 o0 = scan ? (y * stride + x) * 2 : (xo[x] + yo[y]) * 2;
                u32 o1 = scan ? o0 + 2 : (xo[x + 1] + yo[y]) * 2;
                u32 c0 = buf_r16(vram, (addr + o0) & VRAM_MASK), c1 = buf_r16(vram, (addr + o1) & VRAM_MASK);
                out[y * w + x] = from_yuv(c0 >> 8, c0 & 0xFF, c1 & 0xFF);
                out[y * w + x + 1] = from_yuv(c1 >> 8, c0 & 0xFF, c1 & 0xFF);
            }
        return 0;
    }

    {
        u32 stride = (scan && ((tcw >> 25) & 1) && d->strideWidth) ? d->strideWidth : w;
        switch (fmt)
        {
        case 0: FOR_TEXELS16(from1555); break;
        case 1: FOR_TEXELS16(from565); break;
        case 2: FOR_TEXELS16(from4444); break;
        default: FOR_TEXELS16(fromBump); break;
        }
    }
    return 0;
}
