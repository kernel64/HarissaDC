// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_pvr.h"

#include <xgraphics.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/log.h"
#include "../../core/prof.h"
#include "../../hw/pvr.h"
#include "../../mem/mem.h"
#include "../../render/ta_scene.h"
#include "../../render/texture.h"
#include "xb_fs.h"

#define RT_W 1120
#define RT_H 720
#define EDRAM_RT 720
#define EDRAM_DEPTH 1350

#define BATCH_VERTICES 3000
#define TEX_SLOTS 4096
#define TEX_MAX_COUNT 2500
#define TEX_MAX_BYTES (96u << 20)
#define FRAME_SLOTS 4
#define RETIRE_MAX 8192

struct XVtx
{
    float x, y, z, w;
    D3DCOLOR base, offs;
    float u, v;
    float fogW;
    float plane[3];
};

int g_xb_exact_depth;
static int s_exactDepth;
struct RawVtx { f32 x, y, d; };
static RawVtx s_raw[BATCH_VERTICES];

struct TexEntry
{
    u32 tcw, sizeBits, palGen;
    u32 used;
    IDirect3DTexture9 *tex;
    u32 stamp, vramOff, vramBytes, bytes;
    u32 *pixels;
};

struct FrameSlot
{
    u32 addr;
    u32 vramOff, vramBytes, stamp;
    int w, h;
    u32 lastUse;
    IDirect3DTexture9 *tex;
};

struct Retired
{
    IDirect3DTexture9 *tex;
    DWORD fence;
};

static IDirect3DDevice9 *s_dev;
static IDirect3DSurface9 *s_rt, *s_depth, *s_back;
static IDirect3DVertexDeclaration9 *s_decl;
static IDirect3DVertexShader9 *s_vs;
static IDirect3DPixelShader9 *s_ps[512];
static IDirect3DTexture9 *s_fogTex;
static u32 s_fogHash;
extern int g_xb_fog, g_xb_trans_depth, g_xb_tex_filter;
static TaScene s_scene;
static XVtx s_batch[BATCH_VERTICES];
static u16 s_idx[BATCH_VERTICES * 3];
static u32 s_idxN;
static u32 s_batchN;
static TexEntry s_tex[TEX_SLOTS];
static u32 s_texCount, s_texBytes;
static FrameSlot s_frames[FRAME_SLOTS];
static Retired s_retired[RETIRE_MAX];
static u32 s_retiredCount;
static u32 s_renders;
static u32 *s_decode;
static u32 s_palette[1024];
static float s_depthScale;
static int s_depthHuge;
int g_xb_huge_depth = 1;
static u32 s_hugeFrames;
static f32 s_hugeMax;
static f32 s_hugeLo, s_hugeInv;

static EMU_NOINLINE f32 depth_huge(f32 z)
{
    f32 d;
    if (z < 1e6f)
    {
        d = z * s_depthScale;
        return 0.5f * (d < 1.0f ? d : 1.0f);
    }
    if (z >= 1e37f) return 1.0f;
    d = 0.5f + 0.5f * (logf(z) * 1.44269504f - s_hugeLo) * s_hugeInv;
    return d < 1.0f ? d : 1.0f;
}

static EMU_INLINE f32 depth_of(f32 z)
{
    if (!s_depthHuge) return z * s_depthScale < 1.0f ? z * s_depthScale : 1.0f;
    return depth_huge(z);
}
static int s_rtW, s_rtH;
static f32 s_xScale, s_yScale;
static f32 s_xOff;
static f32 s_pullU, s_pullV, s_midU, s_midV;

struct RenderJob
{
    u32 *ta;
    u32 words;
    u32 regs[PVR_REG_WORDS];
    u32 palGen;
};
static RenderJob s_job;
static HANDLE s_jobSem, s_jobDone;
static volatile LONG s_jobBusy;
static int s_async, s_owns = 1;

static u32 jreg(u32 addr)
{
    return s_job.regs[(addr - 0x005F8000u) >> 2];
}

static const char s_vsSource[] =
    "struct VIN { float4 pos : POSITION; float4 base : COLOR0; float4 offs : COLOR1; float2 uv : TEXCOORD0; float fw : TEXCOORD1; float3 pl : TEXCOORD2; };\n"
    "struct VOUT { float4 pos : POSITION; float4 base : COLOR0; float4 offs : COLOR1; float2 uv : TEXCOORD0; float fw : TEXCOORD1; float3 pl : TEXCOORD2; };\n"
    "VOUT main(VIN v) { VOUT o; o.pos = v.pos; o.base = v.base; o.offs = v.offs; o.uv = v.uv; o.fw = v.fw; o.pl = v.pl; return o; }\n";

static const char s_psSource[] =
    "sampler tex : register(s0);\n"
    "sampler fogt : register(s1);\n"
    "float4 fogDensity : register(c0);\n"
    "float4 fogColRam : register(c1);\n"
    "float4 fogColVert : register(c2);\n"
    "float4 taMap : register(c3);\n"
    "float fog_lut(float fw)\n"
    "{\n"
    "  float z = clamp(fogDensity.x / fw, 1.0, 255.9999);\n"
    "  float e = floor(log2(z));\n"
    "  float m = z * 16.0 / exp2(e) - 16.0;\n"
    "  float4 t = tex2D(fogt, float2((floor(m) + e * 16.0 + 0.5) / 128.0, 0.5));\n"
    "  return lerp(t.r, t.g, m - floor(m));\n"
    "}\n"
    "struct POUT { float4 c : COLOR;\n"
    "#if EXACT_DEPTH\n"
    "  float d : DEPTH;\n"
    "#endif\n"
    "};\n"
    "POUT main(float4 base : COLOR0, float4 offs : COLOR1, float2 uv : TEXCOORD0, float fw : TEXCOORD1, float3 pl : TEXCOORD2\n"
    "#if EXACT_DEPTH\n"
    "  , float2 vpos : VPOS\n"
    "#endif\n"
    "  )\n"
    "{\n"
    "  POUT o;\n"
    "  float4 c = base;\n"
    "#if !USE_ALPHA\n"
    "  c.a = 1.0;\n"
    "#endif\n"
    "#if FOG == 3\n"
    "  c = float4(fogColRam.rgb, fog_lut(fw));\n"
    "#endif\n"
    "#if TEXTURED\n"
    "  float4 t = tex2D(tex, uv);\n"
    "#if IGNORE_TEX_ALPHA\n"
    "  t.a = 1.0;\n"
    "#endif\n"
    "#if SHADING == 0\n"
    "  c = t;\n"
    "#elif SHADING == 1\n"
    "  c = float4(t.rgb * c.rgb, t.a);\n"
    "#elif SHADING == 2\n"
    "  c = float4(lerp(c.rgb, t.rgb, t.a), c.a);\n"
    "#else\n"
    "  c = t * c;\n"
    "#endif\n"
    "#if OFFSET\n"
    "  c.rgb = saturate(c.rgb + offs.rgb);\n"
    "#endif\n"
    "#endif\n"
    "#if FOG == 0\n"
    "  c.rgb = lerp(c.rgb, fogColRam.rgb, fog_lut(fw));\n"
    "#elif FOG == 1 && OFFSET\n"
    "  c.rgb = lerp(c.rgb, fogColVert.rgb, offs.a);\n"
    "#endif\n"
    "  o.c = c;\n"
    "#if EXACT_DEPTH\n"
    "  o.d = saturate(dot(float3(vpos.x * taMap.x + taMap.y, vpos.y * taMap.z + taMap.w, 1.0), pl));\n"
    "#endif\n"
    "  return o;\n"
    "}\n";

static LPD3DXBUFFER compile(const char *src, const char *profile, const D3DXMACRO *defines)
{
    LPD3DXBUFFER code = NULL, err = NULL;
    if (FAILED(D3DXCompileShader(src, (UINT)strlen(src), defines, NULL, "main", profile, 0, &code, &err, NULL)))
    {
        LOG_ERROR("[xenos] %s compile failed: %s", profile, err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return NULL;
    }
    if (err) err->Release();
    return code;
}

static IDirect3DPixelShader9 *pixel_shader(u32 key)
{
    if (s_exactDepth) key |= 0x100;
    if (!s_ps[key])
    {
        static const char *const digits[] = { "0", "1", "2", "3" };
        D3DXMACRO defs[8];
        LPD3DXBUFFER code;
        defs[0].Name = "TEXTURED"; defs[0].Definition = digits[key & 1];
        defs[1].Name = "SHADING"; defs[1].Definition = digits[(key >> 1) & 3];
        defs[2].Name = "OFFSET"; defs[2].Definition = digits[(key >> 3) & 1];
        defs[3].Name = "IGNORE_TEX_ALPHA"; defs[3].Definition = digits[(key >> 4) & 1];
        defs[4].Name = "USE_ALPHA"; defs[4].Definition = digits[(key >> 5) & 1];
        defs[5].Name = "FOG"; defs[5].Definition = digits[(key >> 6) & 3];
        defs[6].Name = "EXACT_DEPTH"; defs[6].Definition = digits[(key >> 8) & 1];
        defs[7].Name = NULL; defs[7].Definition = NULL;
        code = compile(s_psSource, "ps_3_0", defs);
        if (!code) return NULL;
        s_dev->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &s_ps[key]);
        code->Release();
    }
    return s_ps[key];
}

static void retire(IDirect3DTexture9 *t)
{
    if (!t) return;
    if (s_retiredCount == RETIRE_MAX)
    {
        u32 i;
        s_dev->BlockUntilIdle();
        for (i = 0; i < s_retiredCount; i++) s_retired[i].tex->Release();
        s_retiredCount = 0;
    }
    s_retired[s_retiredCount].tex = t;
    s_retired[s_retiredCount].fence = 0;
    s_retiredCount++;
}

static void retire_poll(void)
{
    u32 i, j = 0;
    for (i = 0; i < s_retiredCount; i++)
    {
        if (s_retired[i].fence && !s_dev->IsFencePending(s_retired[i].fence)) s_retired[i].tex->Release();
        else s_retired[j++] = s_retired[i];
    }
    s_retiredCount = j;
}

static void retire_fence(void)
{
    DWORD fence = s_dev->InsertFence();
    u32 i;
    for (i = 0; i < s_retiredCount; i++)
        if (!s_retired[i].fence) s_retired[i].fence = fence;
}

static u32 texture_bytes(u32 tcw, u32 w, u32 h, u32 strideWidth)
{
    u32 fmt = (tcw >> 27) & 7, vq = (tcw >> 30) & 1, mip = (tcw >> 31) & 1, bytes;
    if (fmt < 5 && !vq && ((tcw >> 26) & 1) && ((tcw >> 25) & 1)) return (strideWidth ? strideWidth : w) * h * 2 + 64;
    if (vq) bytes = 2048 + w * h / 4;
    else if (fmt == 5) bytes = w * h / 2;
    else if (fmt == 6) bytes = w * h;
    else bytes = w * h * 2;
    if (mip) bytes *= 2;
    return bytes + 64;
}

#define PIX_MIN_TEXELS (256 * 256)
#define PIX_MAX_BYTES (48u << 20)
static u32 s_pixBytes;

static void pixels_free(TexEntry *e)
{
    if (!e->pixels) return;
    free(e->pixels);
    e->pixels = NULL;
    s_pixBytes -= e->bytes;
}

static void texture_cache_clear(void)
{
    u32 i;
    for (i = 0; i < TEX_SLOTS; i++)
        if (s_tex[i].used)
        {
            retire(s_tex[i].tex);
            pixels_free(&s_tex[i]);
            s_tex[i].used = 0;
        }
    s_texCount = 0;
    s_texBytes = 0;
}

#define PART_RECTS 2048
static u16 s_partX[PART_RECTS], s_partY[PART_RECTS];
static u32 s_partCount, s_partSide;
static u32 s_statPartial, s_statFull, s_statFullBig, s_statPages, s_statFallback;

static int tex_partial(TexEntry *e, const TexDesc *d)
{
    u32 w = d->width, h = d->height, side = w < h ? w : h, block, tpb2 = tex_texels_per_byte2(d);
    u32 first = (e->vramOff & (VRAM_SIZE - 1)) >> 12, last = ((e->vramOff & (VRAM_SIZE - 1)) + e->vramBytes - 1) >> 12, pg, dirty = 0;
    if (side > 32) side = 32;
    block = side * side;
    s_partCount = 0;
    s_partSide = side;
    if (last >= VRAM_PAGES) last = VRAM_PAGES - 1;
    for (pg = first; pg <= last; pg++) dirty += g_vram_page_gen[pg] > e->stamp;
    if (dirty * 2 > last - first + 1)
    {
        s_statFallback++;
        return -1;
    }
    s_statPages += dirty;
    for (pg = first; pg <= last; pg++)
    {
        u32 b0, b1, t0, t1, b;
        if (g_vram_page_gen[pg] <= e->stamp) continue;
        b0 = pg << 12;
        b1 = b0 + 4096;
        if (b0 < (e->vramOff & (VRAM_SIZE - 1))) b0 = e->vramOff & (VRAM_SIZE - 1);
        b0 -= e->vramOff & (VRAM_SIZE - 1);
        b1 -= e->vramOff & (VRAM_SIZE - 1);
        t0 = b0 * tpb2 / 2;
        t1 = (b1 * tpb2 + 1) / 2;
        if (t1 > w * h) t1 = w * h;
        for (b = t0 / block; b * block < t1; b++)
        {
            u32 x, y;
            tex_untwiddle(b * block, w, h, &x, &y);
            if (tex_decode_rect(d, g_vram, s_palette, e->pixels, x, y, x + side, y + side) != 0) return -1;
            if (s_partCount < PART_RECTS)
            {
                s_partX[s_partCount] = (u16)x;
                s_partY[s_partCount] = (u16)y;
            }
            s_partCount++;
        }
    }
    return 0;
}

static u32 s_bankHash[64], s_bankHashFor = ~0u;

static u32 palette_key(u32 tcw, u32 fmt)
{
    u32 sel = (tcw >> 21) & 63, k, h;
    if (s_bankHashFor != s_job.palGen)
    {
        u32 b;
        for (b = 0; b < 64; b++)
        {
            h = 2166136261u;
            for (k = 0; k < 16; k++) h = (h ^ jreg(0x005F9000 + (b * 16 + k) * 4)) * 16777619u;
            s_bankHash[b] = h;
        }
        s_bankHashFor = s_job.palGen;
    }
    if (fmt == 5) h = s_bankHash[sel];
    else
    {
        h = 2166136261u;
        for (k = 0; k < 16; k++) h = (h ^ s_bankHash[(sel & 0x30) + k]) * 16777619u;
    }
    h ^= (jreg(0x005F8108) & 3) * 0x9E3779B9u;
    return h | 1;
}

static u32 s_texDecodes;
static u64 s_texDecodeTicks, s_texUploadTicks;
static u64 s_rDecode, s_rOpaque, s_rTrans, s_rFrames;
static u64 s_rtT0, s_rtKeys, s_rtSort, s_rtDepth, s_rtTris;
static u64 s_rTexLookup;
static u64 s_rCalls, s_rVerts, s_rIdx;

static volatile f32 s_orient[3];

void xb_pvr_orientation(f32 *across, f32 *down, f32 *up)
{
    *across = s_orient[0];
    *down = s_orient[1];
    *up = s_orient[2];
    s_orient[0] = s_orient[1] = s_orient[2] = 0.0f;
}

void xb_pvr_render_stats(f64 *decodeMs, f64 *opaqueMs, f64 *transMs, u32 *frames)
{
    f64 k = 1000.0 / (f64)prof_tick_rate();
    *decodeMs = s_rDecode * k;
    *opaqueMs = s_rOpaque * k;
    *transMs = s_rTrans * k;
    *frames = (u32)s_rFrames;
    if (s_rFrames)
        LOG_INFO("[xenos] translucent per frame: keys %.2f ms, sort %.2f ms, depth pass %.2f ms, %u triangles", s_rtKeys * k / s_rFrames,
                 s_rtSort * k / s_rFrames, s_rtDepth * k / s_rFrames, (u32)(s_rtTris / s_rFrames));
    if (s_rFrames) LOG_INFO("[xenos] texture lookups per frame: %.2f ms", s_rTexLookup * k / s_rFrames);
    if (s_rFrames)
        LOG_INFO("[xenos] texture decodes per frame: %.1f partial (%.1f pages), %.1f whole (%.1f big, %.1f with too much written)",
                 s_statPartial / (f64)s_rFrames, s_statPages / (f64)s_rFrames, s_statFull / (f64)s_rFrames, s_statFullBig / (f64)s_rFrames,
                 s_statFallback / (f64)s_rFrames);
    s_statPartial = s_statFull = s_statFullBig = s_statPages = s_statFallback = 0;
    if (s_rFrames && s_hugeFrames) LOG_INFO("[xenos] %u of %u frames with 1/W beyond 1e6 (up to %g)", s_hugeFrames, (u32)s_rFrames, s_hugeMax);
    s_hugeFrames = 0;
    s_hugeMax = 0;
    if (s_rFrames)
        LOG_INFO("[xenos] draw calls per frame: %u, %u vertices, %u indices", (u32)(s_rCalls / s_rFrames), (u32)(s_rVerts / s_rFrames),
                 (u32)(s_rIdx / s_rFrames));
    s_rCalls = s_rVerts = s_rIdx = 0;
    s_rTexLookup = 0;
    s_rtKeys = s_rtSort = s_rtDepth = s_rtTris = 0;
    s_rDecode = s_rOpaque = s_rTrans = s_rFrames = 0;
}

void xb_pvr_tex_stats(u32 *decodes, f64 *decodeMs, f64 *uploadMs)
{
    f64 k = 1000.0 / (f64)prof_tick_rate();
    *decodes = s_texDecodes;
    *decodeMs = s_texDecodeTicks * k;
    *uploadMs = s_texUploadTicks * k;
    s_texDecodes = 0;
    s_texDecodeTicks = s_texUploadTicks = 0;
}

static IDirect3DTexture9 *get_texture_inner(u32 tcw, u32 tsp);

static IDirect3DTexture9 *get_texture(u32 tcw, u32 tsp)
{
    u64 t = prof_ticks();
    IDirect3DTexture9 *r = get_texture_inner(tcw, tsp);
    s_rTexLookup += prof_ticks() - t;
    return r;
}

static IDirect3DTexture9 *get_texture_inner(u32 tcw, u32 tsp)
{
    u64 t0, t1;
    u32 sizeBits = tsp & 0x3F, w = 8u << ((tsp >> 3) & 7), h = 8u << (tsp & 7);
    u32 fmt = (tcw >> 27) & 7, palGen = fmt >= 5 ? palette_key(tcw, fmt) : 0;
    u32 hash = (tcw * 2654435761u ^ sizeBits * 40503u) & (TEX_SLOTS - 1), i, y;
    u32 strideWidth = (jreg(0x005F80E4) & 31) * 32;
    TexEntry *e = NULL;
    TexDesc d;
    D3DLOCKED_RECT lr;
    for (i = 0; i < TEX_SLOTS; i++)
    {
        TexEntry *t = &s_tex[(hash + i) & (TEX_SLOTS - 1)];
        if (!t->used)
        {
            if (!e) e = t;
            break;
        }
        if (t->tcw == tcw && t->sizeBits == sizeBits && t->palGen == palGen)
        {
            if (!vram_written_since(t->vramOff, t->vramBytes, t->stamp)) return t->tex;
            e = t;
            break;
        }
    }
    if (!e || (!e->used && (s_texCount >= TEX_MAX_COUNT || s_texBytes >= TEX_MAX_BYTES)))
    {
        texture_cache_clear();
        e = &s_tex[hash];
    }
    d.tcw = tcw;
    d.width = w;
    d.height = h;
    d.strideWidth = strideWidth;
    d.palFormat = jreg(0x005F8108) & 3;
    if (fmt >= 5)
        for (i = 0; i < 1024; i++) s_palette[i] = jreg(0x005F9000 + i * 4);
    t0 = prof_ticks();
    {
        const u32 *src = NULL;
        if (e->used && e->pixels && e->tcw == tcw && e->sizeBits == sizeBits && e->palGen == palGen && tex_partial(e, &d) == 0)
        {
            src = e->pixels;
            s_statPartial++;
            if (s_partCount <= PART_RECTS)
            {
                u32 k, side = s_partSide;
                t1 = prof_ticks();
                e->tex->BlockUntilNotBusy();
                if (SUCCEEDED(e->tex->LockRect(0, &lr, NULL, 0)))
                {
                    for (k = 0; k < s_partCount; k++)
                        for (y = s_partY[k]; y < (u32)s_partY[k] + side; y++)
                            memcpy((u8 *)lr.pBits + y * lr.Pitch + s_partX[k] * 4, src + y * w + s_partX[k], side * 4);
                    e->tex->UnlockRect(0);
                }
                s_texDecodes++;
                s_texDecodeTicks += t1 - t0;
                s_texUploadTicks += prof_ticks() - t1;
                e->stamp = vram_cache_stamp();
                return e->tex;
            }
        }
        if (e->used)
        {
            retire(e->tex);
            s_texBytes -= e->bytes;
            s_texCount--;
            if (!src) pixels_free(e);
        }
        e->used = 0;
        if (!src)
        {
            if (tex_decode(&d, g_vram, s_palette, s_decode) != 0) return NULL;
            src = s_decode;
            s_statFull++;
            if (w * h >= PIX_MIN_TEXELS) s_statFullBig++;
            if (w * h >= PIX_MIN_TEXELS && tex_rect_ok(&d) && s_pixBytes + w * h * 4 <= PIX_MAX_BYTES)
            {
                e->pixels = (u32 *)malloc(w * h * 4);
                if (e->pixels)
                {
                    memcpy(e->pixels, s_decode, w * h * 4);
                    s_pixBytes += w * h * 4;
                }
            }
        }
        t1 = prof_ticks();
        if (FAILED(s_dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &e->tex, NULL)) || !e->tex)
        {
            pixels_free(e);
            return NULL;
        }
        if (SUCCEEDED(e->tex->LockRect(0, &lr, NULL, 0)))
        {
            for (y = 0; y < h; y++) memcpy((u8 *)lr.pBits + y * lr.Pitch, src + y * w, w * 4);
            e->tex->UnlockRect(0);
        }
    }
    s_texDecodes++;
    s_texDecodeTicks += t1 - t0;
    s_texUploadTicks += prof_ticks() - t1;
    e->used = 1;
    e->tcw = tcw;
    e->sizeBits = sizeBits;
    e->palGen = palGen;
    e->vramOff = (tcw & 0x1FFFFF) << 3;
    e->vramBytes = texture_bytes(tcw, w, h, strideWidth);
    e->stamp = vram_cache_stamp();
    e->bytes = w * h * 4;
    s_texBytes += e->bytes;
    s_texCount++;
    return e->tex;
}

static void set_plane(XVtx *o, const RawVtx *r)
{
    f64 x1 = (f64)r[1].x - r[0].x, y1 = (f64)r[1].y - r[0].y, d1 = (f64)r[1].d - r[0].d;
    f64 x2 = (f64)r[2].x - r[0].x, y2 = (f64)r[2].y - r[0].y, d2 = (f64)r[2].d - r[0].d;
    f64 det = x1 * y2 - x2 * y1, a = 0, b = 0, c;
    u32 k;
    if (det != 0)
    {
        a = (d1 * y2 - d2 * y1) / det;
        b = (x1 * d2 - x2 * d1) / det;
    }
    c = (f64)r[0].d - a * r[0].x - b * r[0].y;
    for (k = 0; k < 3; k++)
    {
        o[k].plane[0] = (f32)a;
        o[k].plane[1] = (f32)b;
        o[k].plane[2] = (f32)c;
    }
}

static void flush(void)
{
    if (s_exactDepth)
    {
        u32 i;
        if (s_idxN)
            for (i = 0; i + 2 < s_idxN; i += 3)
            {
                RawVtx r[3];
                r[0] = s_raw[s_idx[i]];
                r[1] = s_raw[s_idx[i + 1]];
                r[2] = s_raw[s_idx[i + 2]];
                set_plane(&s_batch[s_idx[i]], r);
            }
        else
            for (i = 0; i + 2 < s_batchN; i += 3) set_plane(&s_batch[i], &s_raw[i]);
    }
    if (s_batchN >= 3)
    {
        s_rCalls++;
        s_rVerts += s_batchN;
        s_rIdx += s_idxN;
    }
    if (s_idxN)
        s_dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, s_batchN, s_idxN / 3, s_idx, D3DFMT_INDEX16, s_batch, sizeof(XVtx));
    else if (s_batchN >= 3)
        s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, s_batchN / 3, s_batch, sizeof(XVtx));
    s_batchN = 0;
    s_idxN = 0;
}

static const DWORD s_zfunc[8] = { D3DCMP_NEVER, D3DCMP_LESS, D3DCMP_EQUAL, D3DCMP_LESSEQUAL,
                                  D3DCMP_GREATER, D3DCMP_NOTEQUAL, D3DCMP_GREATEREQUAL, D3DCMP_ALWAYS };

static DWORD blend_factor(u32 instr, int src)
{
    switch (instr)
    {
    case 0: return D3DBLEND_ZERO;
    case 1: return D3DBLEND_ONE;
    case 2: return src ? D3DBLEND_DESTCOLOR : D3DBLEND_SRCCOLOR;
    case 3: return src ? D3DBLEND_INVDESTCOLOR : D3DBLEND_INVSRCCOLOR;
    case 4: return D3DBLEND_SRCALPHA;
    case 5: return D3DBLEND_INVSRCALPHA;
    case 6: return D3DBLEND_DESTALPHA;
    default: return D3DBLEND_INVDESTALPHA;
    }
}

static DWORD address_mode(int clamp, int flip)
{
    return clamp ? D3DTADDRESS_CLAMP : flip ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
}

static u32 uv_inside(const TaStrip *s)
{
    u32 k, r = 3;
    const f32 lo = -0.001f, hi = 1.001f;
    for (k = 0; k < s->count && r; k++)
    {
        const TaVertex *v = &s_scene.verts[s->first + k];
        if (v->u < lo || v->u > hi) r &= ~1u;
        if (v->v < lo || v->v > hi) r &= ~2u;
    }
    return r;
}

static int s_modVols;
static int s_transAutosort;
static int s_depthPass;

struct RsEntry
{
    D3DRENDERSTATETYPE state;
    DWORD value;
};
static RsEntry s_rsCache[32];
static u32 s_rsCount;
static DWORD s_sampCache[8];
static IDirect3DTexture9 *s_texCache;
static IDirect3DPixelShader9 *s_psCache;
static int s_stateValid;

static u32 s_scissor;

static void state_cache_reset(void)
{
    s_scissor = ~0u;
    s_rsCount = 0;
    memset(s_sampCache, 0, sizeof(s_sampCache));
    s_stateValid = 0;
}

static void rs(D3DRENDERSTATETYPE state, DWORD value)
{
    u32 i;
    for (i = 0; i < s_rsCount; i++)
        if (s_rsCache[i].state == state)
        {
            if (s_rsCache[i].value == value) return;
            s_rsCache[i].value = value;
            s_dev->SetRenderState(state, value);
            return;
        }
    if (s_rsCount < 32)
    {
        s_rsCache[s_rsCount].state = state;
        s_rsCache[s_rsCount].value = value;
        s_rsCount++;
    }
    s_dev->SetRenderState(state, value);
}

static void ss(u32 slot, D3DSAMPLERSTATETYPE state, DWORD value)
{
    if (s_sampCache[slot] == value + 1) return;
    s_sampCache[slot] = value + 1;
    s_dev->SetSamplerState(0, state, value);
}

static void set_tex_ps(IDirect3DTexture9 *tex, IDirect3DPixelShader9 *ps)
{
    if (!s_stateValid || tex != s_texCache) s_dev->SetTexture(0, tex);
    if (!s_stateValid || ps != s_psCache) s_dev->SetPixelShader(ps);
    s_texCache = tex;
    s_psCache = ps;
    s_stateValid = 1;
}

static void set_clip(u32 clip)
{
    if (TA_CLIP_MODE(clip) != 2) clip = 0;
    if (clip == s_scissor) return;
    s_scissor = clip;
    if (clip)
    {
        RECT r;
        f32 x0 = (f32)(TA_CLIP_X0(clip) * 32), x1 = (f32)(TA_CLIP_X1(clip) * 32 + 32);
        f32 y0 = (f32)(TA_CLIP_Y0(clip) * 32), y1 = (f32)(TA_CLIP_Y1(clip) * 32 + 32);
        r.left = (LONG)((x0 * s_xScale - s_xOff + 1.0f) * 0.5f * s_rtW + 0.5f);
        r.right = (LONG)((x1 * s_xScale - s_xOff + 1.0f) * 0.5f * s_rtW + 0.5f);
        r.top = (LONG)(y0 * s_yScale * 0.5f * s_rtH + 0.5f);
        r.bottom = (LONG)(y1 * s_yScale * 0.5f * s_rtH + 0.5f);
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > s_rtW) r.right = s_rtW;
        if (r.bottom > s_rtH) r.bottom = s_rtH;
        if (r.right < r.left) r.right = r.left;
        if (r.bottom < r.top) r.bottom = r.top;
        s_dev->SetScissorRect(&r);
    }
    s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, clip ? TRUE : FALSE);
}

static void apply_state(const TaStrip *s, u32 inside)
{
    set_clip(s->clip);
    u32 isp = s->isp, tsp = s->tsp, list = s->list, textured = (s->pcw >> 3) & 1, key;
    IDirect3DTexture9 *tex = textured ? get_texture(s->tcw, tsp) : NULL;
    if (!tex) textured = 0;
    key = textured | (((tsp >> 6) & 3) << 1) | (((s->pcw >> 2) & 1) << 3) | (((tsp >> 19) & 1) << 4) | (((tsp >> 20) & 1) << 5) |
          ((g_xb_fog ? (tsp >> 22) & 3 : 2) << 6);
    set_tex_ps(tex, pixel_shader(key));
    if (tex)
    {
        DWORD filter = g_xb_tex_filter == 1 ? D3DTEXF_POINT : g_xb_tex_filter == 2 || ((tsp >> 13) & 3) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        ss(0, D3DSAMP_MINFILTER, filter);
        if (filter == D3DTEXF_POINT)
        {
            s_pullU = 1.0f / (128.0f * (f32)(8u << ((tsp >> 3) & 7)));
            s_pullV = 1.0f / (128.0f * (f32)(8u << (tsp & 7)));
        }
        else
            s_pullU = s_pullV = 0.0f;
        ss(1, D3DSAMP_MAGFILTER, filter);
        ss(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        ss(3, D3DSAMP_ADDRESSU, address_mode(((tsp >> 16) & 1) || (inside & 1), (tsp >> 18) & 1));
        ss(4, D3DSAMP_ADDRESSV, address_mode(((tsp >> 15) & 1) || (inside & 2), (tsp >> 17) & 1));
    }
    else
        s_pullU = s_pullV = 0.0f;
    {
        u32 zmode = isp >> 29;
        if (list == TA_LIST_TRANS && s_transAutosort && g_xb_trans_depth && !s_depthPass && ta_strip_can_cover(&s_scene, s))
            zmode = zmode == 4 ? 6 : zmode == 1 ? 3 : zmode;
        rs(D3DRS_ZFUNC, s_zfunc[zmode]);
    }
    rs(D3DRS_ZWRITEENABLE, !((isp >> 26) & 1) && !(list == TA_LIST_TRANS && s_transAutosort));
    if (list == TA_LIST_TRANS)
    {
        rs(D3DRS_ALPHABLENDENABLE, TRUE);
        rs(D3DRS_SRCBLEND, blend_factor(tsp >> 29, 1));
        rs(D3DRS_DESTBLEND, blend_factor((tsp >> 26) & 7, 0));
    }
    else
        rs(D3DRS_ALPHABLENDENABLE, FALSE);
    if (s_modVols && list != TA_LIST_TRANS) rs(D3DRS_STENCILREF, (s->pcw >> 7) & 1 ? 0x80 : 0);
    if (list == TA_LIST_PUNCH)
    {
        rs(D3DRS_ALPHATESTENABLE, TRUE);
        rs(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        rs(D3DRS_ALPHAREF, jreg(0x005F811C) & 0xFF);
    }
    else
        rs(D3DRS_ALPHATESTENABLE, FALSE);
    if (s_depthPass)
    {
        rs(D3DRS_ALPHABLENDENABLE, FALSE);
        rs(D3DRS_ALPHATESTENABLE, TRUE);
        rs(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        rs(D3DRS_ALPHAREF, 0xFF);
        rs(D3DRS_ZWRITEENABLE, !((isp >> 26) & 1));
    }
}

static EMU_INLINE void put_vertex(XVtx *o, const TaVertex *v, const TaVertex *colors)
{
    f32 z = v->z > 1e-9f ? (v->z < 1e6f ? v->z : 1e6f) : 1e-9f, w = 1.0f / z;
    o->x = (v->x * s_xScale - s_xOff) * w;
    o->y = (1.0f - v->y * s_yScale) * w;
    o->z = depth_of(v->z) * w;
    o->w = w;
    o->base = colors->base;
    o->offs = colors->offset;
    o->u = v->u;
    o->v = v->v;
    if (s_pullU != 0.0f)
    {
        o->u += v->u < s_midU ? s_pullU : v->u > s_midU ? -s_pullU : 0.0f;
        o->v += v->v < s_midV ? s_pullV : v->v > s_midV ? -s_pullV : 0.0f;
    }
    o->fogW = w;
    if (s_exactDepth)
    {
        RawVtx *r = &s_raw[o - s_batch];
        r->x = v->x;
        r->y = v->y;
        r->d = z * s_depthScale;
    }
}

static void draw_strip_tri(const TaStrip *s, u32 k)
{
    u32 cull = (s->isp >> 27) & 3, gouraud = (s->pcw >> 1) & 1;
    const TaVertex *v = &s_scene.verts[s->first + k];
    const TaVertex *a = (k & 1) ? &v[1] : &v[0], *b = (k & 1) ? &v[0] : &v[1], *c = &v[2];
    f32 area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    XVtx *o;
    if (area == 0 || (cull == 2 && area < 0) || (cull == 3 && area > 0)) return;
    if (s_batchN + 3 > BATCH_VERTICES) flush();
    o = &s_batch[s_batchN];
    s_midU = (a->u + b->u + c->u) * (1.0f / 3.0f);
    s_midV = (a->v + b->v + c->v) * (1.0f / 3.0f);
    put_vertex(&o[0], a, gouraud ? a : c);
    put_vertex(&o[1], b, gouraud ? b : c);
    put_vertex(&o[2], c, c);
    s_idx[s_idxN] = (u16)s_batchN;
    s_idx[s_idxN + 1] = (u16)(s_batchN + 1);
    s_idx[s_idxN + 2] = (u16)(s_batchN + 2);
    s_idxN += 3;
    s_batchN += 3;
}

static void draw_strip(const TaStrip *s)
{
    u32 k, n = s->count, base, cull = (s->isp >> 27) & 3;
    const TaVertex *v = &s_scene.verts[s->first];
    if (!((s->pcw >> 1) & 1) || n > BATCH_VERTICES || s_exactDepth)
    {
        for (k = 0; k + 2 < n; k++) draw_strip_tri(s, k);
        return;
    }
    if (s_batchN + n > BATCH_VERTICES || s_idxN + 3 * (n - 2) > BATCH_VERTICES * 3) flush();
    base = s_batchN;
    if (s_pullU != 0.0f)
    {
        f32 su = 0.0f, sv = 0.0f;
        for (k = 0; k < n; k++)
        {
            su += v[k].u;
            sv += v[k].v;
        }
        s_midU = su / (f32)n;
        s_midV = sv / (f32)n;
    }
    for (k = 0; k < n; k++) put_vertex(&s_batch[base + k], &v[k], &v[k]);
    s_batchN += n;
    for (k = 0; k + 2 < n; k++)
    {
        u32 i0 = (k & 1) ? k + 1 : k, i1 = (k & 1) ? k : k + 1, i2 = k + 2;
        const TaVertex *a = &v[i0], *b = &v[i1], *c = &v[i2];
        f32 area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
        if (area == 0 || (cull == 2 && area < 0) || (cull == 3 && area > 0)) continue;
        s_idx[s_idxN] = (u16)(base + i0);
        s_idx[s_idxN + 1] = (u16)(base + i1);
        s_idx[s_idxN + 2] = (u16)(base + i2);
        s_idxN += 3;
    }
}

static int same_state(const TaStrip *a, const TaStrip *b)
{
    if (!(a->list == b->list && (a->isp >> 26) == (b->isp >> 26) && a->tsp == b->tsp &&
          (a->pcw & 0x8C) == (b->pcw & 0x8C) && (!((a->pcw >> 3) & 1) || a->tcw == b->tcw) && a->clip == b->clip))
        return 0;
    if (a->list == TA_LIST_TRANS && s_transAutosort && g_xb_trans_depth)
        return ta_strip_can_cover(&s_scene, a) == ta_strip_can_cover(&s_scene, b);
    return 1;
}

struct TriKey
{
    u32 z;
    u32 seq;
    u32 strip, k;
};

int g_xb_trans_sort = 1;
int g_xb_mod_vols = 1;
int g_xb_trans_depth = 1;
int g_xb_fog = 1;
int g_xb_orient = 1;
int g_xb_widescreen;
int g_xb_tex_filter;

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

static void draw_list_sorted(u32 list)
{
    state_cache_reset();
    s_rtT0 = prof_ticks();
    static TriKey *keys;
    static u32 keyCap;
    const TaStrip *prev = NULL;
    u32 i, n = 0, prevInside = 0, prevStrip = ~0u, inside = 0;
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
    s_rtKeys += prof_ticks() - s_rtT0;
    s_rtT0 = prof_ticks();
    sort_tri_keys(keys, n);
    s_rtSort += prof_ticks() - s_rtT0;
    s_rtT0 = prof_ticks();
    s_rtTris += n;
    s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    s_depthPass = 1;
    for (i = 0; g_xb_trans_depth && i < s_scene.stripCount; i++)
    {
        const TaStrip *s = &s_scene.strips[i];
        if (s->list != list || s->count < 3 || !ta_strip_can_cover(&s_scene, s)) continue;
        inside = (s->pcw >> 3) & 1 ? uv_inside(s) : 0;
        if (!prev || !same_state(prev, s) || inside != prevInside)
        {
            flush();
            apply_state(s, inside);
            prev = s;
            prevInside = inside;
        }
        draw_strip(s);
    }
    flush();
    s_depthPass = 0;
    s_rtDepth += prof_ticks() - s_rtT0;
    s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALL);
    prev = NULL;
    prevStrip = ~0u;
    for (i = 0; i < n; i++)
    {
        const TaStrip *s = &s_scene.strips[keys[i].strip];
        if (keys[i].strip != prevStrip)
        {
            inside = (s->pcw >> 3) & 1 ? uv_inside(s) : 0;
            if (!prev || !same_state(prev, s) || inside != prevInside)
            {
                flush();
                apply_state(s, inside);
                prev = s;
                prevInside = inside;
            }
            prevStrip = keys[i].strip;
        }
        draw_strip_tri(s, keys[i].k);
    }
    flush();
    set_clip(0);
}

static void draw_list(u32 list)
{
    state_cache_reset();
    const TaStrip *prev = NULL;
    u32 i, prevInside = 0;
    if (list == TA_LIST_TRANS && s_transAutosort)
    {
        draw_list_sorted(list);
        return;
    }
    for (i = 0; i < s_scene.stripCount; i++)
    {
        const TaStrip *s = &s_scene.strips[i];
        u32 inside;
        __dcbt(8 * sizeof(TaStrip), s);
        if (s->list != list || s->count < 3) continue;
        if (i + 1 < s_scene.stripCount)
        {
            const TaVertex *nv = &s_scene.verts[s[1].first];
            __dcbt(0, nv);
            __dcbt(128, nv);
        }
        inside = (s->pcw >> 3) & 1 ? uv_inside(s) : 0;
        if (!prev || !same_state(prev, s) || inside != prevInside)
        {
            flush();
            apply_state(s, inside);
            prev = s;
            prevInside = inside;
        }
        draw_strip(s);
    }
    flush();
    set_clip(0);
}

static void mod_tris(u32 first, u32 count, u32 cull)
{
    u32 k;
    for (k = 0; k < count; k++)
    {
        const TaModTri *t = &s_scene.modTris[first + k];
        f32 area = (t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) - (t->x[2] - t->x[0]) * (t->y[1] - t->y[0]);
        TaVertex v[3];
        u32 i;
        if (area == 0 || (cull == 2 && area < 0) || (cull == 3 && area > 0)) continue;
        if (s_batchN + 3 > BATCH_VERTICES) flush();
        memset(v, 0, sizeof(v));
        for (i = 0; i < 3; i++)
        {
            v[i].x = t->x[i];
            v[i].y = t->y[i];
            v[i].z = t->z[i];
            put_vertex(&s_batch[s_batchN + i], &v[i], &v[i]);
        }
        s_batchN += 3;
    }
    flush();
}

static void draw_mod_volumes(int w, int h)
{
    u32 i, base = ~0u, scale = jreg(0x005F8074) & 0xFF;
    TaVertex q[4];
    XVtx *o;
    s_dev->SetPixelShader(pixel_shader(0x20 | (2 << 6)));
    s_dev->SetTexture(0, NULL);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    for (i = 0; i < s_scene.modVolCount; i++)
    {
        const TaModVol *m = &s_scene.modVols[i];
        u32 mode = m->isp >> 29, cull = (m->isp >> 27) & 3;
        if (m->list != TA_LIST_OPAQUE_MOD || !m->count) continue;
        if (base == ~0u) base = m->first;
        s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        s_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER);
        s_dev->SetRenderState(D3DRS_STENCILWRITEMASK, 2);
        s_dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
        s_dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
        s_dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
        if (!m->last && mode)
        {
            s_dev->SetRenderState(D3DRS_STENCILREF, 2);
            s_dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
        }
        else
            s_dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_INVERT);
        mod_tris(m->first, m->count, cull);
        if (mode == 1 || mode == 2)
        {
            s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
            s_dev->SetRenderState(D3DRS_STENCILWRITEMASK, 3);
            s_dev->SetRenderState(D3DRS_STENCILMASK, 3);
            s_dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_LESSEQUAL);
            s_dev->SetRenderState(D3DRS_STENCILREF, 1);
            s_dev->SetRenderState(D3DRS_STENCILPASS, mode == 1 ? D3DSTENCILOP_REPLACE : D3DSTENCILOP_KEEP);
            s_dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_ZERO);
            mod_tris(base, m->first + m->count - base, cull);
            s_dev->SetRenderState(D3DRS_STENCILMASK, 0xFF);
            base = ~0u;
        }
    }
    s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALL);
    s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    s_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    s_dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    s_dev->SetRenderState(D3DRS_STENCILREF, 0x81);
    s_dev->SetRenderState(D3DRS_STENCILMASK, 0x81);
    s_dev->SetRenderState(D3DRS_STENCILWRITEMASK, 3);
    s_dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_ZERO);
    s_dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_ZERO);
    memset(q, 0, sizeof(q));
    q[0].x = q[2].x = g_xb_widescreen ? -(f32)w / 6 : 0.0f;
    q[1].x = q[3].x = g_xb_widescreen ? (f32)w * 7 / 6 : (f32)w;
    q[2].y = q[3].y = (f32)h;
    for (i = 0; i < 4; i++)
    {
        q[i].z = 1.0f;
        q[i].base = (255 - scale) << 24;
    }
    flush();
    o = &s_batch[0];
    put_vertex(&o[0], &q[0], &q[0]);
    put_vertex(&o[1], &q[1], &q[1]);
    put_vertex(&o[2], &q[2], &q[2]);
    put_vertex(&o[3], &q[2], &q[2]);
    put_vertex(&o[4], &q[1], &q[1]);
    put_vertex(&o[5], &q[3], &q[3]);
    s_batchN = 6;
    flush();
    s_dev->SetRenderState(D3DRS_STENCILMASK, 0xFF);
    s_dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0xFF);
    s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
}

static void fog_setup(void)
{
    u32 h = 2166136261u, i, den = jreg(0x005F80B8);
    f32 c[12];
    if (!g_xb_fog) return;
    for (i = 0; i < 128; i++) h = (h ^ (jreg(0x005F8200 + i * 4) & 0xFFFF)) * 16777619u;
    if (!s_fogTex || h != s_fogHash)
    {
        IDirect3DTexture9 *t = NULL;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(s_dev->CreateTexture(128, 1, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL)) && t &&
            SUCCEEDED(t->LockRect(0, &lr, NULL, 0)))
        {
            u32 *p = (u32 *)lr.pBits;
            for (i = 0; i < 128; i++)
            {
                u32 e = jreg(0x005F8200 + i * 4);
                p[i] = 0xFF000000u | (((e >> 8) & 0xFF) << 16) | ((e & 0xFF) << 8);
            }
            t->UnlockRect(0);
            retire(s_fogTex);
            s_fogTex = t;
            s_fogHash = h;
        }
        else if (t)
            t->Release();
    }
    c[0] = (f32)((den >> 8) & 0xFF) / 128.0f * (f32)ldexp(1.0, (int)(s8)(den & 0xFF));
    c[1] = c[2] = c[3] = 0;
    for (i = 0; i < 2; i++)
    {
        u32 col = jreg(i ? 0x005F80B4 : 0x005F80B0);
        c[4 + i * 4] = ((col >> 16) & 0xFF) / 255.0f;
        c[5 + i * 4] = ((col >> 8) & 0xFF) / 255.0f;
        c[6 + i * 4] = (col & 0xFF) / 255.0f;
        c[7 + i * 4] = 1.0f;
    }
    s_dev->SetPixelShaderConstantF(0, c, 3);
    s_dev->SetTexture(1, s_fogTex);
    s_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    s_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    s_dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    s_dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

static void gpu_dump(int w, int h);
static void scene_dump_to(const char *path, const u32 *ta, u32 words, const u32 *regs);

static FrameSlot *frame_slot(u32 addr, int w, int h)
{
    FrameSlot *best = &s_frames[0];
    u32 i;
    for (i = 0; i < FRAME_SLOTS; i++)
    {
        if (s_frames[i].tex && s_frames[i].addr == addr)
        {
            best = &s_frames[i];
            break;
        }
        if (s_frames[i].lastUse < best->lastUse) best = &s_frames[i];
    }
    if (!best->tex || best->w != w || best->h != h)
    {
        retire(best->tex);
        best->tex = NULL;
        if (FAILED(s_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &best->tex, NULL))) best->tex = NULL;
        best->w = w;
        best->h = h;
    }
    best->addr = addr;
    best->lastUse = s_renders;
    return best;
}

static void render_frame(void)
{
    u32 words = s_job.words, xclip = jreg(0x005F8068), yclip = jreg(0x005F806C), i;
    const u32 *data = s_job.ta;
    int w = (int)((xclip >> 16) & 0x7FF) + 1, h = (int)((yclip >> 16) & 0x3FF) + 1, outW, rtW, rtH;
    int hscale = (jreg(0x005F80F4) >> 16) & 1;
    f32 scale, maxZ, wide;
    u32 addr = jreg(0x005F8060) & 0x7FFFFFu, stride = (jreg(0x005F804C) & 0x1FF) * 8;
    u64 t0;
    D3DVIEWPORT9 vp;
    D3DRECT src;
    DWORD halfPixel;
    FrameSlot *slot;
    if (hscale) w *= 2;
    outW = hscale ? w / 2 : w;
    if (outW < 8 || h < 8 || outW > 1024 || h > 1024) return;
    t0 = prof_ticks();
    s_renders++;
    retire_poll();
    ta_scene_decode(&s_scene, data, words, s_job.regs);
    if (g_xb_orient && (s_renders & 3) == 0)
    {
        f32 a, d, u;
        ta_scene_orientation(&s_scene, hscale ? 0.5f : 1.0f, &a, &d, &u);
        s_orient[0] += 4.0f * a;
        s_orient[1] += 4.0f * d;
        s_orient[2] += 4.0f * u;
    }
    s_rDecode += prof_ticks() - t0;
    s_rFrames++;

    wide = g_xb_widescreen ? 4.0f / 3.0f : 1.0f;
    scale = (f32)RT_W / (outW * wide);
    if ((f32)RT_H / h < scale) scale = (f32)RT_H / h;
    rtW = (int)(outW * wide * scale + 0.5f);
    rtH = (int)(h * scale + 0.5f);
    s_rtW = rtW;
    s_rtH = rtH;
    s_xScale = 2.0f / (w * wide);
    s_xOff = 1.0f / wide;
    s_yScale = 2.0f / h;
    maxZ = s_scene.zMax > s_scene.bgDepth ? s_scene.zMax : s_scene.bgDepth;
    {
        f32 maxHuge = s_scene.zHuge;
        s_depthHuge = maxHuge > 0 && g_xb_huge_depth;
        if (maxHuge > 0)
        {
            s_hugeFrames++;
            if (maxHuge > s_hugeMax) s_hugeMax = maxHuge;
        }
        if (s_depthHuge)
        {
            f32 range = logf(maxHuge) * 1.44269504f - 19.9315686f;
            s_hugeLo = 19.9315686f;
            s_hugeInv = range > 1e-3f ? 1.0f / range : 1.0f;
        }
    }
    s_depthScale = maxZ > 0 ? 1.0f / maxZ : 1.0f;
    s_exactDepth = g_xb_exact_depth && !s_depthHuge;
    if (s_exactDepth)
    {
        f32 m[4];
        m[0] = 2.0f / ((f32)rtW * s_xScale);
        m[1] = (1.0f / (f32)rtW - 1.0f + s_xOff) / s_xScale;
        m[2] = 2.0f / ((f32)rtH * s_yScale);
        m[3] = 1.0f / ((f32)rtH * s_yScale);
        s_dev->SetPixelShaderConstantF(3, m, 1);
    }

    s_dev->GetRenderState(D3DRS_HALFPIXELOFFSET, &halfPixel);
    s_dev->SetRenderTarget(0, s_rt);
    s_dev->SetDepthStencilSurface(s_depth);
    vp.X = 0; vp.Y = 0; vp.Width = rtW; vp.Height = rtH; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    s_dev->SetViewport(&vp);
    s_dev->SetRenderState(D3DRS_HALFPIXELOFFSET, TRUE);
    s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    s_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    s_dev->SetVertexDeclaration(s_decl);
    s_dev->SetVertexShader(s_vs);
    s_transAutosort = g_xb_trans_sort && !(jreg(0x005F8098) & 1);
    s_modVols = 0;
    if (g_xb_mod_vols)
        for (i = 0; i < s_scene.modVolCount && !s_modVols; i++)
            s_modVols = s_scene.modVols[i].list == TA_LIST_OPAQUE_MOD && s_scene.modVols[i].count;
    fog_setup();
    s_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, s_scene.bgColor, depth_of(s_scene.bgDepth), 0);
    if (s_modVols)
    {
        s_dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        s_dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0xFF);
        s_dev->SetRenderState(D3DRS_STENCILMASK, 0xFF);
        s_dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
        s_dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
        s_dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
        s_dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    }
    {
        u64 t1 = prof_ticks();
        draw_list(TA_LIST_OPAQUE);
    draw_list(TA_LIST_PUNCH);
    if (s_modVols) draw_mod_volumes(w, h);
    s_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        s_rOpaque += prof_ticks() - t1;
        t1 = prof_ticks();
        draw_list(TA_LIST_TRANS);
        s_rTrans += prof_ticks() - t1;
    }

    slot = frame_slot(addr, rtW, rtH);
    if (slot->tex)
    {
        src.x1 = 0; src.y1 = 0; src.x2 = rtW; src.y2 = rtH;
        s_dev->Resolve(D3DRESOLVE_RENDERTARGET0, &src, slot->tex, NULL, 0, 0, NULL, 0.0f, 0, NULL);
        slot->vramOff = (addr & 0x3FFFFCu) << 1;
        {
            u32 vs = jreg(0x005F80F4) & 0xFFFF, lines = (u32)h;
            if (vs > 0x400) lines = (u32)h * 0x400 / vs;
            slot->vramBytes = stride * lines * 2 + 8;
        }
        slot->stamp = vram_cache_stamp();
    }
    gpu_dump(rtW, rtH);
    s_dev->SetTexture(0, NULL);
    s_dev->SetTexture(1, NULL);
    s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_HALFPIXELOFFSET, halfPixel);
    s_dev->SetRenderTarget(0, s_back);
    s_dev->SetDepthStencilSurface(NULL);
    retire_fence();
    prof_add(PROF_RENDER, prof_ticks() - t0);
}

static void wait_job(void)
{
    if (s_jobBusy)
    {
        prof_begin(PROF_RENDER_WAIT);
        while (s_jobBusy) WaitForSingleObject(s_jobDone, INFINITE);
        prof_end(PROF_RENDER_WAIT);
    }
    __lwsync();
}

static char s_dumpPath[128];

static volatile LONG s_gpuDumpLeft;
static u32 s_gpuDumpNo;
static IDirect3DTexture9 *s_gpuDumpTex;
static int s_gpuDumpW, s_gpuDumpH;

void xb_pvr_dump_frames(int count)
{
    s_gpuDumpLeft = count;
}

static void gpu_dump(int w, int h)
{
    D3DRECT src;
    D3DLOCKED_RECT lr;
    char path[96], name[32];
    FILE *f;
    int y;
    if (s_gpuDumpLeft <= 0) return;
    if (!s_gpuDumpTex || s_gpuDumpW != w || s_gpuDumpH != h)
    {
        if (s_gpuDumpTex) s_gpuDumpTex->Release();
        s_gpuDumpTex = NULL;
        if (FAILED(s_dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &s_gpuDumpTex, NULL))) return;
        s_gpuDumpW = w;
        s_gpuDumpH = h;
    }
    src.x1 = 0; src.y1 = 0; src.x2 = w; src.y2 = h;
    s_dev->Resolve(D3DRESOLVE_RENDERTARGET0, &src, s_gpuDumpTex, NULL, 0, 0, NULL, 0.0f, 0, NULL);
    s_dev->BlockUntilIdle();
    _snprintf(name, sizeof(name), "gpu_%03u.bmp", s_gpuDumpNo++);
    xb_fs_data_path(path, sizeof(path), "saves", name);
    f = fopen(path, "wb");
    if (f && SUCCEEDED(s_gpuDumpTex->LockRect(0, &lr, NULL, D3DLOCK_READONLY)))
    {
        u8 hd[54];
        u32 size = (u32)(w * h * 4), v;
        static u32 row[2048];
        memset(hd, 0, sizeof(hd));
        hd[0] = 'B'; hd[1] = 'M';
        v = 54 + size; memcpy(hd + 2, &v, 4); v = _byteswap_ulong(v); memcpy(hd + 2, &v, 4);
        v = _byteswap_ulong(54u); memcpy(hd + 10, &v, 4);
        v = _byteswap_ulong(40u); memcpy(hd + 14, &v, 4);
        v = _byteswap_ulong((u32)w); memcpy(hd + 18, &v, 4);
        v = _byteswap_ulong((u32)h); memcpy(hd + 22, &v, 4);
        hd[26] = 1; hd[28] = 32;
        fwrite(hd, 1, 54, f);
        for (y = h - 1; y >= 0; y--)
        {
            const u32 *p = (const u32 *)((const u8 *)lr.pBits + y * lr.Pitch);
            int x;
            for (x = 0; x < w && x < 2048; x++) row[x] = _byteswap_ulong(p[x]);
            fwrite(row, 4, (size_t)w, f);
        }
        s_gpuDumpTex->UnlockRect(0);
    }
    if (f) fclose(f);
    {
        _snprintf(name, sizeof(name), "gpu_%03u.scene", s_gpuDumpNo - 1);
        xb_fs_data_path(path, sizeof(path), "saves", name);
        scene_dump_to(path, s_job.ta, s_job.words, s_job.regs);
    }
    InterlockedDecrement(&s_gpuDumpLeft);
}

void xb_pvr_dump_next_scene(const char *path)
{
    strncpy(s_dumpPath, path, sizeof(s_dumpPath) - 1);
    s_dumpPath[sizeof(s_dumpPath) - 1] = 0;
}

static int dump_words(HANDLE h, const u32 *p, u32 n)
{
    static u32 buf[16384];
    while (n)
    {
        u32 k = n > 16384 ? 16384 : n, i;
        DWORD done = 0;
        for (i = 0; i < k; i++) buf[i] = _byteswap_ulong(p[i]);
        if (!WriteFile(h, buf, k * 4, &done, NULL) || done != k * 4) return -1;
        p += k;
        n -= k;
    }
    return 0;
}

static void scene_dump(const u32 *ta, u32 words, const u32 *regs)
{
    scene_dump_to(s_dumpPath, ta, words, regs);
}

static void scene_dump_to(const char *path, const u32 *ta, u32 words, const u32 *regs)
{
    u32 hd[8] = { SCENE_DUMP_MAGIC, 1, PVR_REG_WORDS, words, VRAM_SIZE, 0, 0, 0 };
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok;
    if (h == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("[xenos] cannot write %s", path);
        return;
    }
    ok = dump_words(h, hd, 8) == 0 && dump_words(h, regs, PVR_REG_WORDS) == 0 && dump_words(h, ta, words) == 0 &&
         dump_words(h, (const u32 *)g_vram, VRAM_SIZE / 4) == 0;
    CloseHandle(h);
    if (ok) LOG_INFO("[xenos] scene written to %s (%u TA words)", path, words);
    else LOG_ERROR("[xenos] writing %s failed", path);
}

static void render_submit(void)
{
    u32 words;
    const u32 *data;
    if (s_async) wait_job();
    data = pvr_ta_data(&words);
    memcpy(s_job.ta, data, words * 4);
    s_job.words = words;
    pvr_regs_copy(s_job.regs);
    if (s_dumpPath[0])
    {
        scene_dump(s_job.ta, words, s_job.regs);
        s_dumpPath[0] = 0;
    }
    s_job.palGen = g_pvr_palette_gen;
    if (!s_async)
    {
        render_frame();
        return;
    }
    if (s_owns)
    {
        s_dev->ReleaseThreadOwnership();
        s_owns = 0;
    }
    __lwsync();
    s_jobBusy = 1;
    ReleaseSemaphore(s_jobSem, 1, NULL);
}

static DWORD WINAPI render_thread(LPVOID)
{
    for (;;)
    {
        WaitForSingleObject(s_jobSem, INFINITE);
        s_dev->AcquireThreadOwnership();
        render_frame();
        s_dev->ReleaseThreadOwnership();
        __lwsync();
        s_jobBusy = 0;
        SetEvent(s_jobDone);
    }
}

int g_xb_pvr_late_wait;

static void render_wait(void)
{
    if (s_async && !g_xb_pvr_late_wait) wait_job();
}

int xb_pvr_busy(void)
{
    return s_async && s_jobBusy;
}

void xb_pvr_sync(void)
{
    if (!s_async) return;
    wait_job();
    if (!s_owns)
    {
        s_dev->AcquireThreadOwnership();
        s_owns = 1;
    }
}

IDirect3DTexture9 *xb_pvr_display(void)
{
    u32 addr = pvr_reg(0x005F8050) & 0x7FFFFFu, i;
    if (!s_dev || !(pvr_reg(0x005F8044) & 1)) return NULL;
    xb_pvr_sync();
    for (i = 0; i < FRAME_SLOTS; i++)
    {
        FrameSlot *f = &s_frames[i];
        if (!f->tex || f->addr != addr || !f->vramBytes) continue;
        if (vram_written_since(f->vramOff, f->vramBytes, f->stamp)) return NULL;
        f->lastUse = s_renders;
        return f->tex;
    }
    return NULL;
}

void xb_pvr_reset(void)
{
    u32 i;
    if (!s_dev) return;
    xb_pvr_sync();
    texture_cache_clear();
    for (i = 0; i < FRAME_SLOTS; i++)
    {
        retire(s_frames[i].tex);
        memset(&s_frames[i], 0, sizeof(s_frames[i]));
    }
    retire_fence();
}

int xb_pvr_init(IDirect3DDevice9 *dev)
{
    static const D3DVERTEXELEMENT9 decl[] = {
        { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        { 0, 20, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1 },
        { 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 32, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        { 0, 36, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
        D3DDECL_END()
    };
    D3DSURFACE_PARAMETERS sp;
    LPD3DXBUFFER code;
    if (s_dev) return 0;
    s_decode = (u32 *)XPhysicalAlloc(1024 * 1024 * 4, MAXULONG_PTR, 0, PAGE_READWRITE);
    memset(&sp, 0, sizeof(sp));
    sp.Base = EDRAM_RT;
    sp.HierarchicalZBase = 0xFFFFFFFF;
    dev->CreateRenderTarget(RT_W, RT_H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s_rt, &sp);
    sp.Base = EDRAM_DEPTH;
    sp.HierarchicalZBase = 0;
    if (FAILED(dev->CreateDepthStencilSurface(RT_W, RT_H, D3DFMT_D24FS8, D3DMULTISAMPLE_NONE, 0, FALSE, &s_depth, &sp)))
        dev->CreateDepthStencilSurface(RT_W, RT_H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &s_depth, &sp);
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s_back);
    dev->CreateVertexDeclaration(decl, &s_decl);
    code = compile(s_vsSource, "vs_3_0", NULL);
    if (code)
    {
        dev->CreateVertexShader((const DWORD *)code->GetBufferPointer(), &s_vs);
        code->Release();
    }
    s_dev = dev;
    if (!s_rt || !s_depth || !s_back || !s_decl || !s_vs || !s_decode || !pixel_shader(1))
    {
        LOG_ERROR("[xenos] initialisation failed (rt %p depth %p decl %p vs %p)", (void *)s_rt, (void *)s_depth, (void *)s_decl,
                  (void *)s_vs);
        s_dev = NULL;
        return -1;
    }
    ta_scene_init(&s_scene);
    s_job.ta = (u32 *)malloc(4u << 20);
    s_jobSem = CreateSemaphore(NULL, 0, 16, NULL);
    s_jobDone = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (s_job.ta && s_jobSem && s_jobDone)
    {
        HANDLE t = CreateThread(NULL, 256 * 1024, render_thread, NULL, CREATE_SUSPENDED, NULL);
        if (t)
        {
            XSetThreadProcessor(t, 4);
            ResumeThread(t);
            CloseHandle(t);
            s_async = 1;
        }
    }
    if (!s_job.ta) return -1;
    g_pvr_render = render_submit;
    g_pvr_render_wait = render_wait;
    LOG_INFO("[xenos] renderer ready (target %dx%d), %s", RT_W, RT_H, s_async ? "on hardware thread 4" : "on the emulation thread");
    return 0;
}
