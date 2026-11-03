// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_display.h"

#include <stdio.h>
#include <string.h>

#include "../../core/log.h"
#include "xb_ui.h"

static const char s_vsSrc[] =
    "struct VIN { float2 pos : POSITION; float2 tc : TEXCOORD0; };\n"
    "struct VOUT { float4 pos : POSITION; float2 tc : TEXCOORD0; };\n"
    "VOUT main(VIN v) { VOUT o; o.pos = float4(v.pos, 0.0, 1.0); o.tc = v.tc; return o; }\n";

static const char s_psSrc[] =
    "sampler t0 : register(s0);\n"
    "float4 rcp : register(c0);\n"
    "float4 prm : register(c1);\n"
    "float4 prm2 : register(c2);\n"
    "float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float3 tap(float2 uv) { return tex2D(t0, clamp(uv, 0.5 * rcp.xy, 1.0 - 0.5 * rcp.xy)).rgb; }\n"
    "float3 base(float2 uv) {\n"
    "#if SMOOTH\n"
    "  uv = clamp(uv, 0.5 * rcp.xy, 1.0 - 0.5 * rcp.xy);\n"
    "  float3 rgbM = tex2D(t0, uv).rgb;\n"
    "  float lNW = luma(tap(uv + float2(-0.5, -0.5) * rcp.xy));\n"
    "  float lNE = luma(tap(uv + float2( 0.5, -0.5) * rcp.xy));\n"
    "  float lSW = luma(tap(uv + float2(-0.5,  0.5) * rcp.xy));\n"
    "  float lSE = luma(tap(uv + float2( 0.5,  0.5) * rcp.xy));\n"
    "  float lM = luma(rgbM);\n"
    "  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));\n"
    "  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    "  float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
    "  float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);\n"
    "  float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
    "  dir = clamp(dir * rcpMin, -8.0, 8.0) * rcp.xy;\n"
    "  float3 a = 0.5 * (tap(uv + dir * (1.0 / 3.0 - 0.5)) + tap(uv + dir * (2.0 / 3.0 - 0.5)));\n"
    "  float3 b = a * 0.5 + 0.25 * (tap(uv - dir * 0.5) + tap(uv + dir * 0.5));\n"
    "  float lB = luma(b);\n"
    "  return (lB < lMin || lB > lMax) ? a : b;\n"
    "#else\n"
    "  return tap(uv);\n"
    "#endif\n"
    "}\n"
    "float3 picture(float2 uv) {\n"
    "  float3 c = base(uv);\n"
    "#if BLUR\n"
    "  float2 d = 1.0 / prm.zw * (BLUR == 1 ? 0.75 : 1.5);\n"
    "  c = (4.0 * c + 2.0 * (tap(uv + float2(d.x, 0.0)) + tap(uv - float2(d.x, 0.0)) + tap(uv + float2(0.0, d.y)) + tap(uv - float2(0.0, d.y)))\n"
    "       + tap(uv + d) + tap(uv - d) + tap(uv + float2(d.x, -d.y)) + tap(uv + float2(-d.x, d.y))) / 16.0;\n"
    "#elif SHARPEN\n"
    "  float3 n = tap(uv - float2(0.0, rcp.y)), so = tap(uv + float2(0.0, rcp.y));\n"
    "  float3 w = tap(uv - float2(rcp.x, 0.0)), e = tap(uv + float2(rcp.x, 0.0));\n"
    "  float3 mn = min(c, min(min(n, so), min(w, e))), mx = max(c, max(max(n, so), max(w, e)));\n"
    "  float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 0.0001)));\n"
    "  float3 wt = amp * (SHARPEN == 1 ? -0.125 : -0.2);\n"
    "  c = saturate((c + wt * (n + so + w + e)) / (1.0 + 4.0 * wt));\n"
    "#endif\n"
    "  return c;\n"
    "}\n"
    "float4 main(float2 tc : TEXCOORD0) : COLOR {\n"
    "  float2 n = tc;\n"
    "#if SCREEN == 3\n"
    "  float2 cc = n * 2.0 - 1.0;\n"
    "  cc *= 1.0 + float2(0.045, 0.06) * (cc.yx * cc.yx);\n"
    "  n = cc * 0.5 + 0.5;\n"
    "  float2 edge = saturate((0.5 - abs(n - 0.5)) * float2(80.0, 60.0));\n"
    "  if (edge.x * edge.y <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);\n"
    "#endif\n"
    "  float2 uv = n;\n"
    "#if SCREEN == 2 || SCREEN == 3\n"
    "  float lines = prm2.x;\n"
    "  float y = n.y * lines - 0.5;\n"
    "  float y0 = floor(y), f = y - y0;\n"
    "  float2 px = float2(0.5 / prm.z, 0.0);\n"
    "  float2 uA = float2(uv.x, (y0 + 0.5) / lines);\n"
    "  float2 uB = float2(uv.x, (y0 + 1.5) / lines);\n"
    "  float3 cA = 0.25 * (tap(uA - px) + tap(uA + px)) + 0.5 * tap(uA);\n"
    "  float3 cB = 0.25 * (tap(uB - px) + tap(uB + px)) + 0.5 * tap(uB);\n"
    "  float kA = lerp(18.0, 6.0, luma(cA)), kB = lerp(18.0, 6.0, luma(cB));\n"
    "  float3 c = cA * exp(-f * f * kA) + cB * exp(-(1.0 - f) * (1.0 - f) * kB);\n"
    "  float3 glow = 0.25 * (tap(uv + float2(4.0, 0.0) * rcp.xy) + tap(uv - float2(4.0, 0.0) * rcp.xy) + tap(uv + float2(0.0, 4.0) * rcp.xy) + tap(uv - float2(0.0, 4.0) * rcp.xy));\n"
    "  c = c * 1.3 + glow * 0.12;\n"
    "#if SCREEN == 3\n"
    "  float2 v = n * (1.0 - n);\n"
    "  c *= pow(saturate(v.x * v.y * 24.0), 0.18) * edge.x * edge.y;\n"
    "#endif\n"
    "#elif SCREEN == 1\n"
    "  float3 c = picture(uv);\n"
    "  float d = frac(n.y * prm2.x) - 0.5;\n"
    "  c *= exp(-d * d * lerp(10.0, 4.0, luma(c))) * 1.3;\n"
    "#elif SCREEN == 5\n"
    "  float3 c = picture(uv);\n"
    "  float d = frac(n.y * prm2.x) - 0.5;\n"
    "  c *= lerp(0.78, 1.08, exp(-d * d * 8.0));\n"
    "#elif SCREEN == 4\n"
    "  float3 c = picture(uv);\n"
    "  float2 g = frac(n * prm.zw);\n"
    "  float2 gap = smoothstep(0.0, 0.18, g) * smoothstep(1.0, 0.82, g);\n"
    "  c *= lerp(0.55, 1.08, gap.x * gap.y);\n"
    "#else\n"
    "  float3 c = picture(uv);\n"
    "#endif\n"
    "  return float4(pow(saturate(c), prm2.yyy), 1.0);\n"
    "}\n";

struct DispVtx
{
    float x, y, u, v;
};

static const D3DVERTEXELEMENT9 s_declElems[] = {
    { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    D3DDECL_END()
};

#define PS_KEYS 256
static IDirect3DVertexShader9 *s_vs;
static IDirect3DVertexDeclaration9 *s_decl;
static IDirect3DPixelShader9 *s_ps[PS_KEYS];
static unsigned char s_psFailed[PS_KEYS];
static int s_ready;

static LPD3DXBUFFER compile(const char *src, const char *profile, const D3DXMACRO *defines)
{
    LPD3DXBUFFER code = NULL, err = NULL;
    if (FAILED(D3DXCompileShader(src, (UINT)strlen(src), defines, NULL, "main", profile, 0, &code, &err, NULL)))
    {
        LOG_ERROR("[display] %s shader compile failed: %s", profile, err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return NULL;
    }
    if (err) err->Release();
    return code;
}

static IDirect3DPixelShader9 *pixel_shader(IDirect3DDevice9 *dev, int smooth, int sharpen, int blur, int screen)
{
    int key = (smooth ? 1 : 0) | (sharpen << 1) | (blur << 3) | (screen << 5);
    char v[4][8];
    D3DXMACRO defs[5];
    LPD3DXBUFFER code;
    if (key >= PS_KEYS) return NULL;
    if (s_ps[key] || s_psFailed[key]) return s_ps[key];
    sprintf(v[0], "%d", smooth ? 1 : 0);
    sprintf(v[1], "%d", sharpen);
    sprintf(v[2], "%d", blur);
    sprintf(v[3], "%d", screen);
    defs[0].Name = "SMOOTH"; defs[0].Definition = v[0];
    defs[1].Name = "SHARPEN"; defs[1].Definition = v[1];
    defs[2].Name = "BLUR"; defs[2].Definition = v[2];
    defs[3].Name = "SCREEN"; defs[3].Definition = v[3];
    defs[4].Name = NULL; defs[4].Definition = NULL;
    code = compile(s_psSrc, "ps_3_0", defs);
    if (code)
    {
        dev->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &s_ps[key]);
        code->Release();
    }
    if (!s_ps[key]) s_psFailed[key] = 1;
    return s_ps[key];
}

static float s_rectX, s_rectY, s_rectW = UI_WIDTH, s_rectH = UI_HEIGHT;

void xb_display_rect(float *x, float *y, float *w, float *h)
{
    *x = s_rectX;
    *y = s_rectY;
    *w = s_rectW;
    *h = s_rectH;
}

void xb_display_draw(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex, int srcW, int srcLines, const Config *cfg)
{
    static const float bright[3] = { 1.0f, 0.8f, 0.65f };
    D3DSURFACE_DESC td;
    D3DVIEWPORT9 vp;
    DispVtx q[4];
    IDirect3DPixelShader9 *ps;
    float c[4], w, h, x0, y0, zoom;
    int crt, i, screen, rot;
    if (!tex) return;
    if (!s_ready)
    {
        LPD3DXBUFFER code = compile(s_vsSrc, "vs_3_0", NULL);
        if (code)
        {
            dev->CreateVertexShader((const DWORD *)code->GetBufferPointer(), &s_vs);
            code->Release();
        }
        dev->CreateVertexDeclaration(s_declElems, &s_decl);
        s_ready = s_vs && s_decl ? 1 : -1;
    }
    screen = cfg->screen >= 0 && cfg->screen <= 5 ? cfg->screen : 0;
    crt = screen == 2 || screen == 3;
    ps = s_ready > 0 ? pixel_shader(dev, crt ? 0 : cfg->fxaa, crt ? 0 : cfg->sharpen & 3, crt ? 0 : cfg->blur & 3, screen) : NULL;
    tex->GetLevelDesc(0, &td);
    zoom = (cfg->zoom >= 50 && cfg->zoom <= 150 ? cfg->zoom : 100) / 100.0f;
    h = UI_HEIGHT * zoom;
    rot = cfg->rotate == 90 || cfg->rotate == 270 ? cfg->rotate : 0;
    if (rot) w = h * 3.0f / 4.0f;
    else w = cfg->aspect || td.Width * 9 >= td.Height * 15 ? UI_WIDTH * zoom : h * 4.0f / 3.0f;
    x0 = (UI_WIDTH - w) * 0.5f + cfg->posX;
    y0 = (UI_HEIGHT - h) * 0.5f + cfg->posY;
    s_rectX = x0;
    s_rectY = y0;
    s_rectW = w;
    s_rectH = h;
    if (!ps)
    {
        UiImage(tex, x0, y0, w, h, 0, D3DCOLOR_XRGB(255, 255, 255));
        return;
    }
    UiSuspend();
    if (srcW <= 0) srcW = 640;
    if (srcLines <= 0) srcLines = 480;
    for (i = 0; i < 4; i++)
    {
        float px = x0 + ((i & 1) ? w : 0.0f), py = y0 + ((i & 2) ? h : 0.0f);
        q[i].x = px * (2.0f / UI_WIDTH) - 1.0f;
        q[i].y = 1.0f - py * (2.0f / UI_HEIGHT);
        float sx = (i & 1) ? 1.0f : 0.0f, sy = (i & 2) ? 1.0f : 0.0f;
        q[i].u = rot == 270 ? 1.0f - sy : rot == 90 ? sy : sx;
        q[i].v = rot == 270 ? sx : rot == 90 ? 1.0f - sx : sy;
    }
    vp.X = 0; vp.Y = 0; vp.Width = UI_WIDTH; vp.Height = UI_HEIGHT; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);
    dev->SetVertexDeclaration(s_decl);
    dev->SetVertexShader(s_vs);
    dev->SetPixelShader(ps);
    c[0] = 1.0f / td.Width; c[1] = 1.0f / td.Height; c[2] = (float)td.Width; c[3] = (float)td.Height;
    dev->SetPixelShaderConstantF(0, c, 1);
    c[0] = 1.0f; c[1] = 1.0f; c[2] = (float)srcW; c[3] = (float)srcLines;
    dev->SetPixelShaderConstantF(1, c, 1);
    c[0] = (float)(srcLines > 300 ? srcLines / 2 : srcLines);
    c[1] = bright[cfg->brightness >= 0 && cfg->brightness <= 2 ? cfg->brightness : 0];
    c[2] = c[3] = 0.0f;
    dev->SetPixelShaderConstantF(2, c, 1);
    dev->SetTexture(0, tex);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, cfg->smooth || crt ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, cfg->smooth || crt ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(DispVtx));
    UiResume();
}

struct Preset
{
    const char *name;
    int smooth, fxaa, sharpen, blur, screen, texFilter;
};

static const Preset s_presets[DISPLAY_PRESETS] = {
    { "Original", 1, 0, 0, 0, 0, 0 },
    { "Enhanced", 1, 1, 1, 0, 0, 2 },
    { "Smooth", 1, 1, 0, 1, 0, 2 },
    { "CRT", 1, 0, 0, 0, 3, 0 },
};

void xb_display_preset(Config *c, int preset)
{
    const Preset *p;
    if (preset < 0 || preset >= DISPLAY_PRESETS) return;
    p = &s_presets[preset];
    c->smooth = p->smooth;
    c->fxaa = p->fxaa;
    c->sharpen = p->sharpen;
    c->blur = p->blur;
    c->screen = p->screen;
    c->texFilter = p->texFilter;
}

int xb_display_preset_of(const Config *c)
{
    int i;
    for (i = 0; i < DISPLAY_PRESETS; i++)
    {
        const Preset *p = &s_presets[i];
        if (c->smooth == p->smooth && c->fxaa == p->fxaa && c->sharpen == p->sharpen && c->blur == p->blur && c->screen == p->screen &&
            c->texFilter == p->texFilter)
            return i;
    }
    return -1;
}

const char *xb_display_preset_name(int preset)
{
    return preset >= 0 && preset < DISPLAY_PRESETS ? s_presets[preset].name : "Custom";
}
