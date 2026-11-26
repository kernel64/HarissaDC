// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_emu.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/config.h"
#include "../../core/log.h"
#include "../../core/prof.h"
#include "../../core/sched.h"
#include "../../bios/hle_bios.h"
#include "../../disc/disc.h"
#include "../../disc/disc_info.h"
#include "../../disc/game_files.h"
#include "../../hw/aica.h"
#include "../../hw/arm7_jit.h"
#include "../../hw/cdda.h"
#include "../../hw/cheat.h"
#include "../../hw/dreamcast.h"
#include "../../hw/flash.h"
#include "../../hw/gdrom.h"
#include "../../hw/maple.h"
#include "../../hw/pvr.h"
#include "../../mem/mem.h"
#include "../../hw/sound_thread.h"
#include "../../render/soft_render.h"
#include "../../render/ta_scene.h"
#include "../../sh4/sh4.h"
#include "../../sh4/sh4_jit.h"
#include "../../sh4/sh4_mmr.h"
#include "../../sh4/sh4_mmu.h"
#include "xb_ach.h"
#include "xb_audio.h"
#include "xb_codemem.h"
#include "xb_diag.h"
#include "xb_display.h"
#include "xb_lang.h"
#include "xb_fs.h"
#include "xb_log.h"
#include "xb_menu.h"
#include "xb_pad.h"
#include "xb_pvr.h"
#include "../../hw/widescreen.h"
#include "xb_ra.h"
#include "xb_sampler.h"
#include "xb_ui.h"
extern "C" LONG NTAPI XexGetModuleHandle(PCSTR name, PHANDLE handle);
extern "C" LONG NTAPI XexGetProcedureAddress(HANDLE module, DWORD ordinal, PVOID *address);

static HANDLE s_probe;
static volatile LONG s_probeDone;

static DWORD WINAPI fault_probe(LPVOID)
{
    volatile u32 *p = (volatile u32 *)(g_mem_window ? g_mem_window + 0x005F74E4u : (u8 *)0x10);
    __try
    {
        *p = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    s_probeDone = 1;
    return 0;
}

static int debugger_takes_faults(void)
{
    int i;
    if (s_probe) ResumeThread(s_probe);
    else
    {
        s_probeDone = 0;
        s_probe = CreateThread(NULL, 0, fault_probe, NULL, 0, NULL);
        if (!s_probe) return 0;
    }
    for (i = 0; i < 30 && !s_probeDone; i++) Sleep(10);
    if (s_probeDone)
    {
        CloseHandle(s_probe);
        s_probe = NULL;
        return 0;
    }
    SuspendThread(s_probe);
    return 1;
}

#ifdef EMU_TRACE
typedef BOOL (*TraceStartFn)(const char *file);
typedef BOOL (*TraceStopFn)(void);

static const char *trace_start(void)
{
    static const char *const files[] = { "devkit:\\harissadc_trace.pix2", "Hdd:\\harissadc_trace.pix2", "game:\\harissadc_trace.pix2" };
    HANDLE xbdm = NULL;
    PVOID start = NULL;
    LONG st;
    u32 i;
    st = XexGetModuleHandle("xbdm.xex", &xbdm);
    if (st < 0)
    {
        LOG_ERROR("[trace] xbdm.xex not loaded (%08X)", (u32)st);
        return NULL;
    }
    st = XexGetProcedureAddress(xbdm, 133, &start);
    if (st < 0 || !start)
    {
        LOG_ERROR("[trace] xbdm has no DmTraceStartRecording (%08X)", (u32)st);
        return NULL;
    }
    for (i = 0; i < EMU_ARRAY_COUNT(files); i++)
    {
        if (((TraceStartFn)start)(files[i])) return files[i];
        LOG_WARN("[trace] DmTraceStartRecording(%s) refused", files[i]);
    }
    return NULL;
}

static void trace_stop(void)
{
    HANDLE xbdm = NULL;
    PVOID stop = NULL;
    if (XexGetModuleHandle("xbdm.xex", &xbdm) >= 0 && XexGetProcedureAddress(xbdm, 136, &stop) >= 0 && stop) ((TraceStopFn)stop)();
}
#endif

#define FB_MAX_W 1024
#define FB_MAX_H 1024
#define BACK_MENU_MS 600
#define BACK_LIST_MS 1500
#define ARM_CODE_BYTES (2u << 20)

static u32 *s_fb;
static IDirect3DTexture9 *s_tex[2];
static int s_texW[2], s_texH[2], s_texNext;
static int s_initDone;

struct Screen
{
    IDirect3DTexture9 *tex;
    const Config *cfg;
};

static u64 qpc_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)t.QuadPart;
}

static void pad_from_xinput(const XINPUT_GAMEPAD *g, MaplePad *p, const Config *cfg, int player)
{
    const u32 *map = cfg->padMap[player];
    static const u16 dcButton[PADIN_COUNT] = { DC_BTN_A, DC_BTN_B, DC_BTN_X, DC_BTN_Y, DC_BTN_START, 0, 0,
                                               DC_BTN_UP, DC_BTN_DOWN, DC_BTN_LEFT, DC_BTN_RIGHT };
    int stick = cfg->padStick[player];
    SHORT sx = stick == PAD_STICK_RIGHT ? g->sThumbRX : stick == PAD_STICK_NONE ? 0 : g->sThumbLX;
    SHORT sy = stick == PAD_STICK_RIGHT ? g->sThumbRY : stick == PAD_STICK_NONE ? 0 : g->sThumbLY;
    float fx = sx / 32767.0f, fy = sy / 32767.0f, mag = sqrtf(fx * fx + fy * fy), dead = cfg->padDead[player] / 100.0f;
    int x, y, in;
    if (mag <= dead || dead >= 1.0f)
        fx = fy = 0;
    else
    {
        float k = (mag - dead) / (1.0f - dead) / mag * cfg->padRange[player] / 100.0f;
        fx *= k;
        fy *= k;
    }
    if (cfg->padInvert[player] & 1) fx = -fx;
    if (cfg->padInvert[player] & 2) fy = -fy;
    x = 128 + (int)(fx * 127.0f);
    y = 128 - (int)(fy * 127.0f);
    p->connected = 1;
    p->buttons = 0;
    for (in = 0; in < PADIN_COUNT; in++)
        if (dcButton[in] && XbPadSources(g, map[in]) > 128) p->buttons |= dcButton[in];
    p->lt = (u8)XbPadSources(g, map[PADIN_L]);
    p->rt = (u8)XbPadSources(g, map[PADIN_R]);
    p->jx = (u8)(x < 0 ? 0 : x > 255 ? 255 : x);
    p->jy = (u8)(y < 0 ? 0 : y > 255 ? 255 : y);
}

static struct { u32 pc, n; } s_disp[4096];

static void jitmap_line(void *ctx, const u32 *code, u32 words, u32 pc, u32 off, u32 mode)
{
    fprintf((FILE *)ctx, "%08X %u %08X %08X %X\n", (u32)(uintptr_t)code, words, pc, off, mode);
}

#define TRAIL 64
static u32 s_trail[TRAIL], s_trailPr[TRAIL], s_trailPos;

static void trail_record(u32 pc)
{
    s_trail[s_trailPos & (TRAIL - 1)] = pc;
    s_trailPr[s_trailPos & (TRAIL - 1)] = g_sh4.pr;
    s_trailPos++;
}

static void trail_dump(u32 pc)
{
    static int done;
    u32 i;
    if (done) return;
    done = 1;
    LOG_ERROR("[trail] illegal instruction at %08X: r0-r3 %08X %08X %08X %08X r13 %08X r14 %08X sr %08X fpscr %08X", pc, g_sh4.r[0],
              g_sh4.r[1], g_sh4.r[2], g_sh4.r[3], g_sh4.r[13], g_sh4.r[14], g_sh4.sr, g_sh4.fpscr);
    for (i = s_trailPos > TRAIL ? s_trailPos - TRAIL : 0; i < s_trailPos; i++)
        LOG_ERROR("[trail] %u: pc %08X pr %08X", i, s_trail[i & (TRAIL - 1)], s_trailPr[i & (TRAIL - 1)]);
    xb_log_flush();
}

static void dispatch_count(u32 pc)
{
    u32 h = (pc * 2654435761u) >> 20, k;
    for (k = 0; k < 16; k++, h = (h + 1) & 4095)
    {
        if (s_disp[h].pc == pc) { s_disp[h].n++; return; }
        if (!s_disp[h].n) { s_disp[h].pc = pc; s_disp[h].n = 1; return; }
    }
}

static void dispatch_report(u32 fields)
{
    char line[256];
    int n = _snprintf(line, sizeof(line), "[emu] dispatcher entries per field:"), k, i;
    for (k = 0; k < 10 && n > 0 && n < (int)sizeof(line); k++)
    {
        int best = -1;
        for (i = 0; i < 4096; i++)
            if (s_disp[i].n && (best < 0 || s_disp[i].n > s_disp[best].n)) best = i;
        if (best < 0) break;
        n += _snprintf(line + n, sizeof(line) - n, " %08X %u", s_disp[best].pc, s_disp[best].n / (fields ? fields : 1));
        s_disp[best].n = 0;
    }
    line[sizeof(line) - 1] = 0;
    LOG_INFO("%s", line);
    memset(s_disp, 0, sizeof(s_disp));
}

static float s_gunX = 320.0f, s_gunY = 240.0f;

static void gun_from_xinput(const XINPUT_GAMEPAD *g, MaplePad *p)
{
    float fx = g->sThumbLX / 32767.0f, fy = g->sThumbLY / 32767.0f;
    const float dead = 0.15f;
    if (fx > -dead && fx < dead) fx = 0;
    if (fy > -dead && fy < dead) fy = 0;
    s_gunX += fx * (fx < 0 ? -fx : fx) * 14.0f;
    s_gunY -= fy * (fy < 0 ? -fy : fy) * 14.0f;
    if (s_gunX < 0) s_gunX = 0;
    if (s_gunX > 639) s_gunX = 639;
    if (s_gunY < 0) s_gunY = 0;
    if (s_gunY > 479) s_gunY = 479;
    memset(p, 0, sizeof(*p));
    p->connected = 1;
    p->gun = 1;
    p->jx = p->jy = 128;
    p->gunX = (u16)s_gunX;
    p->gunY = (u16)s_gunY;
    p->gunOff = (g->wButtons & XINPUT_GAMEPAD_B) != 0;
    if (g->bRightTrigger > 128 || (g->wButtons & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B))) p->buttons |= DC_BTN_A;
    if (g->wButtons & XINPUT_GAMEPAD_X) p->buttons |= DC_BTN_B;
    if (g->wButtons & XINPUT_GAMEPAD_START) p->buttons |= DC_BTN_START;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_UP) p->buttons |= DC_BTN_UP;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) p->buttons |= DC_BTN_DOWN;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) p->buttons |= DC_BTN_LEFT;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) p->buttons |= DC_BTN_RIGHT;
}

static const char *const s_gunPortC[] = {
    "T0016M",
    "T0017M",
    "T0018M",
    "T0025M",
};

static const char *const s_transAlwaysLast[] = {
    "T0015M",
};

static const struct { const char *product; u32 pc; } s_countdownLoop[] = {
    { "T15113N", 0x8C0DDEFAu },
};

static const char *const s_wceMsgPoll[] = {
    "T1205N",
};

static int wce_msg_poll_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_wceMsgPoll) / sizeof(s_wceMsgPoll[0]); k++)
        if (!strcmp(product, s_wceMsgPoll[k])) return 1;
    return 0;
}

static const char *const s_noTmuPollSkip[] = {
    "T1401N",
    "T1401D",
};

static const char *const s_lateRenderWait[] = {
    "T0015M",
    "T17716N",
    "T15113N",
};

static const char *const s_fbReadback[] = {
    "T13002N",
    "T-8118N",
};

static const char *const s_slowCpuWhenBehind[] = {
    "T40203N",
    "T15113N",
};

static const char *const s_noSlowCpuWhenBehind[] = {
    "T1205N",
};

#define DISC_USES_WINDOWS_CE 1u

static const char *const s_slowCpuClimb[] = {
    "T15113N",
};

static int slow_cpu_climb_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_slowCpuClimb) / sizeof(s_slowCpuClimb[0]); k++)
        if (!strcmp(product, s_slowCpuClimb[k])) return 1;
    return 0;
}

static int slow_cpu_when_behind_of(const DiscInfo *info)
{
    size_t k;
    for (k = 0; k < sizeof(s_noSlowCpuWhenBehind) / sizeof(s_noSlowCpuWhenBehind[0]); k++)
        if (!strcmp(info->product, s_noSlowCpuWhenBehind[k])) return 0;
    if (info->peripherals & DISC_USES_WINDOWS_CE) return 1;
    for (k = 0; k < sizeof(s_slowCpuWhenBehind) / sizeof(s_slowCpuWhenBehind[0]); k++)
        if (!strcmp(info->product, s_slowCpuWhenBehind[k])) return 1;
    return 0;
}

struct WideProfile
{
    const char *product;
    int mode;
};
static const WideProfile s_wideProfile[] = {
    { "", WIDE_OFF },
};

static int wide_mode_of(const Config *c, const DiscInfo *info)
{
    const WidePatch *patch = wide_patch_find(info);
    int mode = WIDE_OFF;
    size_t k;
    if (!c->gpuRender || c->aspect) mode = WIDE_OFF;
    else if (c->widescreen == CONFIG_WIDE_REVEAL) mode = WIDE_REVEAL;
    else if (c->widescreen == CONFIG_WIDE_ANAMORPHIC) mode = WIDE_ANAMORPHIC;
    else if (c->widescreen == CONFIG_WIDE_AUTO && patch) mode = WIDE_ANAMORPHIC;
    else if (c->widescreen == CONFIG_WIDE_AUTO && info->product[0])
        for (k = 0; k < sizeof(s_wideProfile) / sizeof(s_wideProfile[0]); k++)
            if (!strcmp(info->product, s_wideProfile[k].product)) mode = s_wideProfile[k].mode;
    wide_patch_set(mode == WIDE_ANAMORPHIC ? patch : NULL);
    return mode;
}

static int fb_readback_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_fbReadback) / sizeof(s_fbReadback[0]); k++)
        if (!strcmp(product, s_fbReadback[k])) return 1;
    return 0;
}

static int late_render_wait_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_lateRenderWait) / sizeof(s_lateRenderWait[0]); k++)
        if (!strcmp(product, s_lateRenderWait[k])) return 1;
    return 0;
}

static const char *const s_exactDepth[] = {
    "MK-5105950",
    "MK-51059",
    "HDR-0016",
};

static const char *const s_palShader[] = {
    "6108099",
    "T0002M",
};

static const struct
{
    const char *product;
    int samples;
} s_soundLag[] = {
    { "6108099", 64 },
};

static int sound_lag_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_soundLag) / sizeof(s_soundLag[0]); k++)
        if (!strcmp(product, s_soundLag[k].product)) return s_soundLag[k].samples;
    return 0;
}

static int pal_shader_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_palShader) / sizeof(s_palShader[0]); k++)
        if (!strcmp(product, s_palShader[k])) return 1;
    return 0;
}

static int exact_depth_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_exactDepth) / sizeof(s_exactDepth[0]); k++)
        if (!strcmp(product, s_exactDepth[k])) return 1;
    return 0;
}

extern int g_sh4_tmu_poll_skip;

static int no_tmu_poll_skip_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_noTmuPollSkip) / sizeof(s_noTmuPollSkip[0]); k++)
        if (!strcmp(product, s_noTmuPollSkip[k])) return 1;
    return 0;
}

static int arcade_conversion(const char *product)
{
    return strlen(product) == 6 && !strncmp(product, "T00", 3) && product[5] == 'M';
}

static const char *const s_mmuSqOnly[] = {
    "T38706M",
    "MK-51060",
};

static int mmu_sq_only_of(const char *product)
{
    size_t k;
    if (arcade_conversion(product)) return 1;
    for (k = 0; k < sizeof(s_mmuSqOnly) / sizeof(s_mmuSqOnly[0]); k++)
        if (!strcmp(product, s_mmuSqOnly[k])) return 1;
    return 0;
}

static int rotate_auto(const Config *c)
{
    return c->rotate != 0 && c->rotate != 90 && c->rotate != 270;
}

static const struct { const char *product; int hpos; } s_vgaCable[] = {
    { "T0025M", 224 },
};

static int vga_cable_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_vgaCable) / sizeof(s_vgaCable[0]); k++)
        if (!strcmp(product, s_vgaCable[k].product)) return s_vgaCable[k].hpos;
    return 0;
}

static u32 countdown_loop_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_countdownLoop) / sizeof(s_countdownLoop[0]); k++)
        if (!strcmp(product, s_countdownLoop[k].product)) return s_countdownLoop[k].pc;
    return 0;
}

static int trans_always_last_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_transAlwaysLast) / sizeof(s_transAlwaysLast[0]); k++)
        if (!strcmp(product, s_transAlwaysLast[k])) return 1;
    return 0;
}

static int gun_port_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_gunPortC) / sizeof(s_gunPortC[0]); k++)
        if (!strcmp(product, s_gunPortC[k])) return 2;
    return 0;
}

static void draw_gun_sight(void)
{
    float x, y, w, h, cx, cy;
    xb_display_rect(&x, &y, &w, &h);
    cx = x + (s_gunX + 0.5f) * w / 640.0f;
    cy = y + (s_gunY + 0.5f) * h / 480.0f;
    UiRoundRing(cx - 13, cy - 13, 26, 26, 13, 3, D3DCOLOR_ARGB(230, 255, 60, 60));
    UiRect(cx - 1.5f, cy - 20, 3, 12, D3DCOLOR_ARGB(230, 255, 255, 255));
    UiRect(cx - 1.5f, cy + 8, 3, 12, D3DCOLOR_ARGB(230, 255, 255, 255));
    UiRect(cx - 20, cy - 1.5f, 12, 3, D3DCOLOR_ARGB(230, 255, 255, 255));
    UiRect(cx + 8, cy - 1.5f, 12, 3, D3DCOLOR_ARGB(230, 255, 255, 255));
}

static DWORD s_rumbleStop[4];
static int s_rumbleOn[4];

static void rumble_motors(int port, float power)
{
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed = (WORD)(power * 65535.0f);
    v.wRightMotorSpeed = (WORD)(power * 32767.0f);
    XInputSetState((DWORD)port, &v);
    s_rumbleOn[port] = power > 0;
}

static void rumble_update(const Config *cfg)
{
    int port;
    for (port = 0; port < 4; port++)
    {
        float power;
        u32 ms;
        if (maple_take_rumble(port, &power, &ms))
        {
            static int logged;
            if (logged < 8)
            {
                logged++;
                LOG_INFO("[pad] vibration, controller %d: %d%% for %u ms%s", port + 1, (int)(power * 100), ms, cfg->rumble ? "" : " (off)");
            }
            if (cfg->rumble && power > 0 && ms)
            {
                rumble_motors(port, power);
                s_rumbleStop[port] = GetTickCount() + ms;
            }
            else if (s_rumbleOn[port])
                rumble_motors(port, 0);
        }
        else if (s_rumbleOn[port] && (int)(GetTickCount() - s_rumbleStop[port]) >= 0)
            rumble_motors(port, 0);
    }
}

static void rumble_stop_all(void)
{
    int port;
    for (port = 0; port < 4; port++)
        if (s_rumbleOn[port]) rumble_motors(port, 0);
}

static IDirect3DTexture9 *upload(IDirect3DDevice9 *dev, int w, int h)
{
    int i = s_texNext, y;
    D3DLOCKED_RECT lr;
    s_texNext ^= 1;
    if (!s_tex[i] || s_texW[i] != w || s_texH[i] != h)
    {
        if (s_tex[i]) UiImageRelease(dev, s_tex[i]);
        s_tex[i] = NULL;
        if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_X8R8G8B8, D3DPOOL_DEFAULT, &s_tex[i], NULL))) return NULL;
        s_texW[i] = w;
        s_texH[i] = h;
    }
    if (FAILED(s_tex[i]->LockRect(0, &lr, NULL, 0))) return NULL;
    for (y = 0; y < h; y++) memcpy((u8 *)lr.pBits + y * lr.Pitch, s_fb + y * w, w * 4);
    s_tex[i]->UnlockRect(0);
    return s_tex[i];
}

static void shown_resolution(char *out, int size, IDirect3DTexture9 *tex, const Config *cfg)
{
    int w = 0, h = 0, k;
    f32 scale = 1.0f;
    char sc[16];
    int gpu = xb_pvr_display_size(tex, &w, &h, &scale);
    if (!gpu)
    {
        u32 ctrl = pvr_reg(0x005F8044), fbSize = pvr_reg(0x005F805C), depth = (ctrl >> 2) & 3;
        u32 bpp = depth == 2 ? 3 : depth == 3 ? 4 : 2, lines = ((fbSize >> 10) & 0x3FF) + 1;
        if ((pvr_reg(0x005F80D0) >> 4) & 1) lines *= 2;
        w = (int)(((fbSize & 0x3FF) + 1) * 4 / bpp);
        h = (int)lines;
        scale = 1.0f;
    }
    _snprintf(sc, sizeof(sc) - 1, "%.2f", scale);
    sc[sizeof(sc) - 1] = 0;
    for (k = (int)strlen(sc) - 1; k > 0 && sc[k] == '0'; k--) sc[k] = 0;
    if (k > 0 && sc[k] == '.') sc[k] = 0;
    _snprintf(out, size - 1, "%dx%d  " UI_CH_MIDDOT "  %sx%s", w, h, sc, gpu && g_xb_widescreen ? "  16:9" : "");
    out[size - 1] = 0;
}

static void draw_picture(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex, const Config *cfg)
{
    u32 ctrl = pvr_reg(0x005F8044), size = pvr_reg(0x005F805C), depth = (ctrl >> 2) & 3;
    u32 bpp = depth == 2 ? 3 : depth == 3 ? 4 : 2, xWords = (size & 0x3FF) + 1, lines = ((size >> 10) & 0x3FF) + 1;
    int interlaced = (pvr_reg(0x005F80D0) >> 4) & 1;
    f32 u0, u1;
    if (!tex) return;
    xb_display_view(xb_pvr_display_view(tex, &u0, &u1), u0, u1);
    xb_display_draw(dev, tex, (int)(xWords * 4 / bpp), (int)(interlaced ? lines * 2 : lines), cfg);
}

static IDirect3DDevice9 *s_dev;

static void frozen_game(void *user)
{
    const Screen *s = (const Screen *)user;
    draw_picture(s_dev, s->tex, s->cfg);
}

static void message(IDirect3DDevice9 *dev, const char *title, const char *text)
{
    UiInput in;
    xb_pvr_sync();
    UiInputInit(&in);
    for (;;)
    {
        static char wrapped[1024];
        WORD down = UiInputPoll(&in);
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_BACK)) break;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        UiBrand(UI_SAFE_X, 72, 34);
        UiPanel(UI_SAFE_X, 140, UI_WIDTH - 2 * UI_SAFE_X, 300, 18);
        UiText(UI_BOLD, 28, UI_SAFE_X + 40, 190, UI_COL_TEXT, title, UI_LEFT);
        UiTextWrap(UI_REGULAR, 20, UI_WIDTH - 2 * UI_SAFE_X - 80, 8, text, wrapped, sizeof(wrapped));
        UiText(UI_REGULAR, 20, UI_SAFE_X + 40, 250, UI_COL_TEXT2, wrapped, UI_LEFT);
        UiScreenFooter(Tr(T_FOOT_BACK));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
    }
}

enum { DEBUGGER_BACK = -1, DEBUGGER_KEEP = 0, DEBUGGER_NO_FASTMEM = 1 };

static int debugger_screen(IDirect3DDevice9 *dev)
{
    UiInput in;
    xb_pvr_sync();
    UiInputInit(&in);
    for (;;)
    {
        static char wrapped[1024];
        WORD down = UiInputPoll(&in);
        if (down & XINPUT_GAMEPAD_A) return DEBUGGER_NO_FASTMEM;
        if (down & XINPUT_GAMEPAD_X) return DEBUGGER_KEEP;
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return DEBUGGER_BACK;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        UiBrand(UI_SAFE_X, 72, 34);
        UiPanel(UI_SAFE_X, 140, UI_WIDTH - 2 * UI_SAFE_X, 300, 18);
        UiText(UI_BOLD, 28, UI_SAFE_X + 40, 190, UI_COL_TEXT, Tr(T_DEBUGGER), UI_LEFT);
        UiTextWrap(UI_REGULAR, 20, UI_WIDTH - 2 * UI_SAFE_X - 80, 8, Tr(T_DEBUGGER_TEXT), wrapped, sizeof(wrapped));
        UiText(UI_REGULAR, 20, UI_SAFE_X + 40, 250, UI_COL_TEXT2, wrapped, UI_LEFT);
        UiScreenFooter(Tr(T_FOOT_DEBUGGER));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
    }
}

static int run_frame_guarded(char *crash, int crashSize)
{
    __try
    {
        return dc_run_frame();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        _snprintf(crash, crashSize, "Exception %08X at SH-4 pc %08X", GetExceptionCode(), g_sh4.pc);
        crash[crashSize - 1] = 0;
        return -2;
    }
}

static char s_vmuPath[96];

static void vmu_choose_card(const char *gameFolder, int shared)
{
    if (shared || !gameFolder || !gameFolder[0])
        xb_fs_data_path(s_vmuPath, sizeof(s_vmuPath), "saves", "vmu_a1.bin");
    else
    {
        char name[48], d[64];
        _snprintf(name, sizeof(name), "%.38s.bin", gameFolder);
        name[sizeof(name) - 1] = 0;
        xb_fs_data_dir(d, sizeof(d), "saves");
        xb_fs_data_path(s_vmuPath, sizeof(s_vmuPath), "saves\\vmu", name);
    }
}

static int vmu_file_size(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size;
    if (h == INVALID_HANDLE_VALUE) return -1;
    size = GetFileSize(h, NULL);
    CloseHandle(h);
    return (int)size;
}

static void vmu_load(const char *gameFolder, int shared)
{
    static u8 data[VMU_SIZE];
    char tmp[sizeof(s_vmuPath) + 4];
    DWORD got = 0;
    int size;
    vmu_choose_card(gameFolder, shared);
    _snprintf(tmp, sizeof(tmp), "%s.tmp", s_vmuPath);
    tmp[sizeof(tmp) - 1] = 0;
    size = vmu_file_size(s_vmuPath);
    if (size < 0 && vmu_file_size(tmp) == VMU_SIZE)
    {
        MoveFileA(tmp, s_vmuPath);
        size = vmu_file_size(s_vmuPath);
    }
    if (size >= 0 && size != VMU_SIZE)
    {
        char bad[sizeof(s_vmuPath) + 4];
        _snprintf(bad, sizeof(bad), "%s.bad", s_vmuPath);
        bad[sizeof(bad) - 1] = 0;
        DeleteFileA(bad);
        MoveFileA(s_vmuPath, bad);
        LOG_WARN("[emu] VMU: %s is %d bytes, not %u: renamed %s", s_vmuPath, size, VMU_SIZE, bad);
    }
    else if (size == VMU_SIZE)
    {
        HANDLE h = CreateFileA(s_vmuPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            ReadFile(h, data, VMU_SIZE, &got, NULL);
            CloseHandle(h);
        }
    }
    maple_vmu_load(got == VMU_SIZE ? data : NULL);
    LOG_INFO("[emu] VMU: %s%s", s_vmuPath, got == VMU_SIZE ? "" : " (new, formatted)");
}

static void vmu_save(void)
{
    char tmp[sizeof(s_vmuPath) + 4];
    HANDLE h;
    DWORD put = 0;
    if (!s_vmuPath[0]) return;
    _snprintf(tmp, sizeof(tmp), "%s.tmp", s_vmuPath);
    tmp[sizeof(tmp) - 1] = 0;
    h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("[emu] cannot write %s", tmp);
        return;
    }
    WriteFile(h, maple_vmu_image(), VMU_SIZE, &put, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (put != VMU_SIZE)
    {
        DeleteFileA(tmp);
        LOG_ERROR("[emu] VMU: %u of %u bytes written (disk full?), %s kept", (unsigned)put, VMU_SIZE, s_vmuPath);
        return;
    }
    DeleteFileA(s_vmuPath);
    if (!MoveFileA(tmp, s_vmuPath))
    {
        LOG_ERROR("[emu] VMU: cannot rename %s", tmp);
        return;
    }
    LOG_INFO("[emu] VMU saved: %s", s_vmuPath);
}

#define STATE_SLOTS 4
#define STATE_FILE_MAGIC 0x53544154u

struct StateFileHeader
{
    u32 magic;
    u32 disc;
    u32 bytes;
    u32 reserved;
};

static int s_lastSlot = 1;

static void state_path(const char *gameFolder, int slot, char *out, int size)
{
    char name[64];
    const char *g = gameFolder && gameFolder[0] ? gameFolder : "_";
    if (slot <= 1) _snprintf(name, sizeof(name), "%s.dcs", g);
    else _snprintf(name, sizeof(name), "%s_%d.dcs", g, slot);
    name[sizeof(name) - 1] = 0;
    xb_fs_data_path(out, size, "saves", name);
}

static int state_exists(const char *gameFolder, int slot)
{
    char path[96];
    state_path(gameFolder, slot, path, sizeof(path));
    return GetFileAttributesA(path) != (DWORD)-1;
}

static int file_io(HANDLE h, u8 *p, u32 bytes, int write)
{
    while (bytes)
    {
        DWORD n = bytes > (1u << 20) ? (1u << 20) : bytes, done = 0;
        BOOL ok = write ? WriteFile(h, p, n, &done, NULL) : ReadFile(h, p, n, &done, NULL);
        if (!ok || done != n) return -1;
        p += n;
        bytes -= n;
    }
    return 0;
}

static int state_save(const char *gameFolder, int slot, int disc, char *error, int errorSize)
{
    char path[96];
    StateFileHeader hd;
    u32 size = dc_state_size();
    u8 *buf = (u8 *)malloc(size);
    HANDLE h;
    int r = -1;
    state_path(gameFolder, slot, path, sizeof(path));
    if (!buf)
    {
        _snprintf(error, errorSize, Tr(T_STATE_NO_MEMORY), size >> 10);
        error[errorSize - 1] = 0;
        return -1;
    }
    if (dc_state_save(buf, size) == 0)
    {
        h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            hd.magic = STATE_FILE_MAGIC;
            hd.disc = (u32)disc;
            hd.bytes = size;
            hd.reserved = 0;
            r = file_io(h, (u8 *)&hd, sizeof(hd), 1) == 0 && file_io(h, buf, size, 1) == 0 ? 0 : -1;
            CloseHandle(h);
            if (r != 0) DeleteFileA(path);
        }
    }
    free(buf);
    if (r != 0)
    {
        _snprintf(error, errorSize, Tr(T_STATE_CANNOT_WRITE), path);
        error[errorSize - 1] = 0;
    }
    else
    {
        LOG_INFO("[emu] state saved: %s (%u KB)", path, size >> 10);
        XbRaStateSaved(path);
        XbAchUnlock(XB_ACH_SAVE_STATE);
    }
    return r;
}

static int state_load(const char *gameFolder, int slot, int disc, char *error, int errorSize)
{
    char path[96];
    StateFileHeader hd;
    u32 size;
    u8 *buf;
    HANDLE h;
    int r = -1;
    state_path(gameFolder, slot, path, sizeof(path));
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        _snprintf(error, errorSize, Tr(T_STATE_NONE), slot);
        error[errorSize - 1] = 0;
        return -1;
    }
    if (file_io(h, (u8 *)&hd, sizeof(hd), 0) != 0 || hd.magic != STATE_FILE_MAGIC ||
        (size = hd.bytes, GetFileSize(h, NULL) != sizeof(hd) + size))
    {
        CloseHandle(h);
        _snprintf(error, errorSize, "%s", Tr(T_STATE_OTHER_VERSION));
        error[errorSize - 1] = 0;
        return -1;
    }
    if ((int)hd.disc != disc)
    {
        CloseHandle(h);
        _snprintf(error, errorSize, Tr(T_STATE_OTHER_DISC), hd.disc + 1);
        error[errorSize - 1] = 0;
        return -1;
    }
    buf = (u8 *)malloc(size);
    if (buf && file_io(h, buf, size, 0) == 0) r = dc_state_load(buf, size);
    CloseHandle(h);
    free(buf);
    if (r != 0)
    {
        _snprintf(error, errorSize, "%s", Tr(T_STATE_DAMAGED));
        error[errorSize - 1] = 0;
    }
    else
    {
        LOG_INFO("[emu] state loaded: %s", path);
        XbRaStateLoaded(path);
    }
    return r;
}

#define OSD_MS 2500
static char s_osd[128];
static DWORD s_osdAt;

static void osd(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(s_osd, sizeof(s_osd), fmt, ap);
    va_end(ap);
    s_osd[sizeof(s_osd) - 1] = 0;
    s_osdAt = GetTickCount();
}

static CheatList *s_cheats;
static int s_achOpen;

static char *read_whole(const char *path, u32 *len)
{
    FILE *f = fopen(path, "rb");
    char *data;
    long n;
    *len = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = n > 0 && n < (8 << 20) ? (char *)malloc((size_t)n) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n)
    {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) *len = (u32)n;
    return data;
}

static void cheats_load(const Config *c, const char *product, const char *gameFolder)
{
    char path[160], name[80];
    char *text;
    u32 len;
    int db = 0, own = 0;
    cheat_free(s_cheats);
    s_cheats = cheat_new();
    if (!s_cheats) return;
    xb_fs_data_path(path, sizeof(path), "cheats", "dreamcast_cheats.txt");
    text = read_whole(path, &len);
    if (text)
    {
        db = cheat_load_db(s_cheats, text, len, product);
        free(text);
    }
    if (gameFolder[0])
    {
        _snprintf(name, sizeof(name), "%s.cht", gameFolder);
        name[sizeof(name) - 1] = 0;
        xb_fs_data_path(path, sizeof(path), "cheats", name);
        text = read_whole(path, &len);
        if (text)
        {
            own += cheat_load_cht(s_cheats, text, len);
            free(text);
        }
        _snprintf(name, sizeof(name), "%s.txt", gameFolder);
        name[sizeof(name) - 1] = 0;
        xb_fs_data_path(path, sizeof(path), "cheats", name);
        text = read_whole(path, &len);
        if (text)
        {
            own += cheat_load_codes(s_cheats, text, len);
            free(text);
        }
    }
    cheat_states_read(s_cheats, c->cheats);
    if (c->cheatOn >= 0) cheat_enable(s_cheats, c->cheatOn, 1);
    LOG_INFO("[cheat] %d cheats from the database, %d from the game's files (%s.cht/.txt), %d on", db, own, gameFolder,
             cheat_active(s_cheats));
}

#define CHEAT_ROWS 10

static void cheats_menu(IDirect3DDevice9 *dev, Config *cfg, const char *gameFolder, Screen *screen)
{
    UiMenu m;
    UiInput in;
    int n = cheat_count(s_cheats), sel = 0, changed = 0;
    if (!n)
    {
        message(dev, Tr(T_CHEATS), Trf(T_CHEATS_NONE, gameFolder[0] ? gameFolder : "-"));
        return;
    }
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        int top = sel / CHEAT_ROWS * CHEAT_ROWS, i;
        char title[96];
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) break;
        if (down & XINPUT_GAMEPAD_DPAD_UP) sel = (sel + n - 1) % n;
        if (down & XINPUT_GAMEPAD_DPAD_DOWN) sel = (sel + 1) % n;
        if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) sel = sel >= CHEAT_ROWS ? sel - CHEAT_ROWS : 0;
        if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) sel = sel + CHEAT_ROWS < n ? sel + CHEAT_ROWS : n - 1;
        if ((down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) && cheat_supported(s_cheats, sel))
        {
            cheat_enable(s_cheats, sel, !cheat_enabled(s_cheats, sel));
            changed = 1;
        }
        top = sel / CHEAT_ROWS * CHEAT_ROWS;
        _snprintf(title, sizeof(title), "%s  %d / %d", Tr(T_CHEATS), sel + 1, n);
        title[sizeof(title) - 1] = 0;
        UiMenuClear(&m, title, Tr(T_FOOT_CHEATS));
        for (i = top; i < n && i < top + CHEAT_ROWS; i++)
        {
            char value[64], fit[96];
            const char *v;
            if (!cheat_supported(s_cheats, i)) v = Tr(T_CHEAT_UNSUPPORTED);
            else
            {
                _snprintf(value, sizeof(value), "< %s >", Tr(cheat_enabled(s_cheats, i) ? T_ON : T_OFF));
                value[sizeof(value) - 1] = 0;
                v = value;
            }
            UiTextFit(UI_BOLD, 22, 660 - 88 - 30 - UiTextWidth(UI_BOLD, 21, v) - 60, cheat_name(s_cheats, i), fit, sizeof(fit));
            UiMenuAdd(&m, i, fit, v);
        }
        m.sel = sel - top;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        UiBegin(dev);
        frozen_game(screen);
        UiMenuDraw(&m);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
    }
    if (changed)
    {
        char game[CONFIG_PATH_MAX];
        cheat_states_write(s_cheats, cfg->cheats, sizeof(cfg->cheats));
        if (gameFolder[0])
        {
            SettingsPaths(gameFolder, NULL, game, sizeof(game));
            if (!SettingsSaveKey(game, "cheats", cfg->cheats)) LOG_WARN("[cheat] cannot write %s", game);
        }
        LOG_INFO("[cheat] %d on: %s", cheat_active(s_cheats), cfg->cheats);
        if (cheat_active(s_cheats)) XbAchUnlock(XB_ACH_CHEAT);
    }
}

static char s_achBadge[UI_MENU_MAX][24];
static IDirect3DTexture9 *ach_icon(int row) { return row >= 0 && row < UI_MENU_MAX ? XbRaBadge(s_achBadge[row]) : NULL; }

static void ach_menu(IDirect3DDevice9 *dev, const Config *cfg, Screen *screen)
{
    UiMenu m;
    UiInput in;
    int n = XbRaListBegin(), sel = 0;
    if (!n)
    {
        XbRaListEnd();
        UiNotify(Tr(!cfg->achievements ? T_RA_OFF_HINT : XbRaState() == XB_RA_IN ? T_RA_NONE : T_RA_NOT_CONNECTED), 4000, NULL);
        return;
    }
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        int top, i;
        char title[96];
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) break;
        if (down & XINPUT_GAMEPAD_DPAD_UP) sel = (sel + n - 1) % n;
        if (down & XINPUT_GAMEPAD_DPAD_DOWN) sel = (sel + 1) % n;
        if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) sel = sel >= CHEAT_ROWS ? sel - CHEAT_ROWS : 0;
        if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) sel = sel + CHEAT_ROWS < n ? sel + CHEAT_ROWS : n - 1;
        if (down & XINPUT_GAMEPAD_A)
        {
            XbRaItem it;
            XbRaListItem(sel, &it);
            if (it.description[0]) UiNotify(it.description, 5000, it.badge);
        }
        top = sel / CHEAT_ROWS * CHEAT_ROWS;
        _snprintf(title, sizeof(title), "%s  %d / %d", Tr(T_ACHIEVEMENTS), sel + 1, n);
        title[sizeof(title) - 1] = 0;
        UiMenuClear(&m, title, Tr(T_FOOT_ACH));
        m.icon = ach_icon;
        for (i = top; i < n && i < top + CHEAT_ROWS; i++)
        {
            XbRaItem it;
            char value[32], fit[96];
            XbRaListItem(i, &it);
            strcpy(s_achBadge[i - top], it.badge);
            if (it.unlocked) strcpy(value, Tr(T_RA_DONE));
            else sprintf(value, "%d", it.points);
            UiTextFit(UI_BOLD, 22, 660 - 88 - 30 - 48 - (UiTextWidth(UI_REGULAR, 21, value) + 16), it.title, fit, sizeof(fit));
            UiMenuAdd(&m, i, fit, value);
        }
        m.sel = sel - top;
        UiServicePump(dev);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        UiBegin(dev);
        frozen_game(screen);
        UiMenuDraw(&m);
        UiNotifyDraw();
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
    }
    XbRaListEnd();
}

enum { P_RESUME = 1, P_SAVE_STATE, P_LOAD_STATE, P_SLOT, P_DISC, P_CHEATS, P_ACH, P_SETTINGS, P_RESET, P_LIST, P_QUIT };
enum { PAUSE_RESUME, PAUSE_RESET, PAUSE_LIST, PAUSE_QUIT, PAUSE_LOADED };

static int disc_menu(IDirect3DDevice9 *dev, DiscSet *set, Screen *screen)
{
    UiMenu m;
    UiInput in;
    char label[96], image[GAME_PATH_MAX], error[400];
    int i;
    UiMenuClear(&m, Tr(T_CHANGE_DISC), Tr(T_FOOT_INSERT));
    for (i = 0; i < set->count; i++)
    {
        if (set->number[i]) _snprintf(label, sizeof(label), Tr(T_DISC_N), set->number[i]);
        else _snprintf(label, sizeof(label), Tr(T_DISC_N), i + 1);
        label[sizeof(label) - 1] = 0;
        UiMenuAdd(&m, i, label, i == set->current ? Tr(T_IN_DRIVE) : NULL);
    }
    m.sel = set->current;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        UiMenuNavigate(&m, down);
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return 0;
        if (down & XINPUT_GAMEPAD_A)
        {
            int chosen = UiMenuSelected(&m), r;
            Disc *d;
            if (chosen < 0 || chosen == set->current) return 0;
            r = ArchivePrepare(dev, set->path[chosen], image, sizeof(image), error, sizeof(error), frozen_game, screen);
            if (r == 0) return 0;
            d = r > 0 ? disc_open(image) : NULL;
            if (!d)
            {
                if (r > 0) _snprintf(error, sizeof(error), "%s", Trf(T_CANNOT_OPEN_DISC, image));
                error[sizeof(error) - 1] = 0;
                message(dev, Tr(T_DISC_ERROR), error);
                return 0;
            }
            gdrom_swap_disc(d);
            set->current = chosen;
            LOG_INFO("[emu] disc %d inserted: %s", chosen + 1, image);
            return 1;
        }
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        UiBegin(dev);
        frozen_game(screen);
        UiMenuDraw(&m);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
    }
}

static void pause_build(UiMenu *m, const char *gameFolder, const DiscSet *discs, int sel)
{
    char value[96];
    UiMenuClear(m, Tr(T_PAUSED), Tr(T_FOOT_PAUSED));
    UiMenuAdd(m, P_RESUME, Tr(T_RESUME), NULL);
    UiMenuAdd(m, P_SAVE_STATE, Tr(T_SAVE_STATE), NULL);
    UiMenuAdd(m, P_LOAD_STATE, Tr(T_LOAD_STATE), state_exists(gameFolder, s_lastSlot) ? NULL : Tr(T_EMPTY));
    _snprintf(value, sizeof(value), "< %d \xC2\xB7 %s >", s_lastSlot, Tr(state_exists(gameFolder, s_lastSlot) ? T_SAVED : T_EMPTY));
    value[sizeof(value) - 1] = 0;
    UiMenuAdd(m, P_SLOT, Tr(T_STATE_SLOT), value);
    if (discs->count > 1)
    {
        _snprintf(value, sizeof(value), Tr(T_DISC_OF), discs->current + 1, discs->count);
        value[sizeof(value) - 1] = 0;
        UiMenuAdd(m, P_DISC, Tr(T_CHANGE_DISC), value);
    }
    if (cheat_active(s_cheats))
    {
        _snprintf(value, sizeof(value), Tr(T_CHEATS_ON), cheat_active(s_cheats));
        value[sizeof(value) - 1] = 0;
    }
    UiMenuAdd(m, P_CHEATS, Tr(T_CHEATS), cheat_active(s_cheats) ? value : NULL);
    {
        int done, total;
        XbRaProgress(&done, &total);
        if (total)
        {
            _snprintf(value, sizeof(value), "%d / %d", done, total);
            value[sizeof(value) - 1] = 0;
        }
        UiMenuAdd(m, P_ACH, Tr(T_ACHIEVEMENTS), total ? value : NULL);
    }
    UiMenuAdd(m, P_SETTINGS, Tr(T_SETTINGS), NULL);
    UiMenuAdd(m, P_RESET, Tr(T_RESET_GAME), NULL);
    UiMenuAdd(m, P_LIST, Tr(T_BACK_TO_LIST), NULL);
    UiMenuAdd(m, P_QUIT, Tr(T_QUIT_DASHBOARD), NULL);
    m->sel = sel < m->count ? sel : 0;
}

static int pause_menu(IDirect3DDevice9 *dev, Config *cfg, const char *gameFolder, Screen *screen, DiscSet *discs)
{
    UiMenu m;
    UiInput in;
    pause_build(&m, gameFolder, discs, 0);
    UiInputInit(&in);
    if (s_achOpen)
    {
        s_achOpen = 0;
        ach_menu(dev, cfg, screen);
        UiInputInit(&in);
    }
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        UiMenuNavigate(&m, down);
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) return PAUSE_RESUME;
        if (down & XINPUT_GAMEPAD_X)
        {
            AboutScreen(dev, frozen_game, screen);
            UiInputInit(&in);
        }
        else if ((down & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_A)) && UiMenuSelected(&m) == P_SLOT)
        {
            s_lastSlot += (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 1;
            if (s_lastSlot < 1) s_lastSlot = STATE_SLOTS;
            if (s_lastSlot > STATE_SLOTS) s_lastSlot = 1;
            pause_build(&m, gameFolder, discs, m.sel);
        }
        else if (down & XINPUT_GAMEPAD_A)
        {
            switch (UiMenuSelected(&m))
            {
            case P_RESUME: return PAUSE_RESUME;
            case P_RESET: return PAUSE_RESET;
            case P_SAVE_STATE:
            case P_LOAD_STATE:
            {
                char error[200];
                int disc = discs->count > 1 ? discs->current : 0, slot = s_lastSlot;
                if (UiMenuSelected(&m) == P_SAVE_STATE)
                {
                    if (state_save(gameFolder, slot, disc, error, sizeof(error)) == 0)
                    {
                        osd(Tr(T_OSD_SAVED), slot);
                        return PAUSE_RESUME;
                    }
                    message(dev, Tr(T_SAVE_STATE), error);
                }
                else if (state_exists(gameFolder, slot))
                {
                    if (state_load(gameFolder, slot, disc, error, sizeof(error)) == 0)
                    {
                        osd(Tr(T_OSD_LOADED), slot);
                        return PAUSE_LOADED;
                    }
                    message(dev, Tr(T_LOAD_STATE), error);
                }
                break;
            }
            case P_DISC:
                if (disc_menu(dev, discs, screen)) return PAUSE_RESUME;
                break;
            case P_CHEATS: cheats_menu(dev, cfg, gameFolder, screen); break;
            case P_ACH: ach_menu(dev, cfg, screen); break;
            case P_SETTINGS:
                SettingsScreen(dev, cfg, gameFolder, frozen_game, screen);
                xb_audio_set_target_ms(cfg->audioMs > 0 ? cfg->audioMs : 120);
                g_cdda_realtime = cfg->cdRealtime;
                break;
            case P_LIST: return PAUSE_LIST;
            case P_QUIT:
                if (ConfirmScreen(dev, Tr(T_QUIT_ASK), Tr(T_QUIT), frozen_game, screen)) return PAUSE_QUIT;
                break;
            }
            pause_build(&m, gameFolder, discs, m.sel);
            UiInputInit(&in);
        }
        UiServicePump(dev);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        UiBegin(dev);
        frozen_game(screen);
        UiMenuDraw(&m);
        UiNotifyDraw();
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
    }
}

int xb_emu_run(IDirect3DDevice9 *dev, const Config *baseCfg, const char *entry)
{
    char boot[300], flash[300], error[400], crash[128], gameFolder[64], gamePath[CONFIG_PATH_MAX];
    char image[GAME_PATH_MAX];
    const char *disc = entry;
    DiscSet discs;
    LARGE_INTEGER freq, t0, now;
    s64 nextField;
    int vsync = baseCfg->vsync != 0, fastSeconds = 0, vsyncWait = 3, vsyncOnSeconds = 0;
    double cpuLoad = 0;
    u64 cyclesAtT0;
    u32 frames = 0, fpsFrames = 0, seconds = 0;
    int skipPresent = 0, skippedLast = 0;
    int audioThread = 0;
    int slowCpuWhenBehind = 0;
    int slowCpuClimb = 0;
    int gunPort = 0;
    int rotateConv = 0, rotatePending = 0, rotateStreak = 0;
    double fps = 0, speed = 0;
    int result, useReal, outcome = XB_EMU_TO_LIST, quit = 0;
    DWORD backDown = 0;
    WORD prevButtons = 0;
    int backUsed = 0;
    Config cfg = *baseCfg;
    Screen screen;
    DiscInfo info;
    int gameSoundLag = 0;

    s_dev = dev;
    gameFolder[0] = 0;
    memset(&discs, 0, sizeof(discs));
    memset(&info, 0, sizeof(info));
    if (entry && entry[0])
    {
        int r = ArchivePrepare(dev, entry, image, sizeof(image), error, sizeof(error), NULL, NULL);
        if (r == 0) return XB_EMU_TO_LIST;
        if (r < 0)
        {
            message(dev, Tr(T_CANNOT_UNPACK), error);
            return XB_EMU_ERROR;
        }
        disc = image;
        game_disc_set(entry, &discs);
        if (discs.count == 1 && game_is_archive(entry))
        {
            DiscSet inside;
            if (game_cache_images(XB_CACHE_DIR, entry, &inside) > 1) discs = inside;
        }
        if (discs.count > 1) LOG_INFO("[emu] %d discs, disc %d first", discs.count, discs.current + 1);
    }
    sh4_mmr_set_cable(CABLE_COMPOSITE);
    if (cfg.gunX > 0) s_gunX = (float)cfg.gunX;
    if (cfg.gunY > 0) s_gunY = (float)cfg.gunY;
    {
        extern int g_pvr_gun_hpos;
        g_pvr_gun_hpos = cfg.gunHpos > 0 ? cfg.gunHpos : 286;
    }
    g_sh4_tmu_poll_skip = 1;
    g_sh4_jit_wce_msg_poll = 0;
    g_mmu_sq_only_ok = cfg.mmuSq == 1;
    {
        extern int g_xb_exact_depth;
        g_xb_exact_depth = cfg.exactDepth == 1;
    }
    {
        extern int g_xb_pvr_late_wait;
        g_xb_pvr_late_wait = cfg.renderWait == 1;
    }
    {
        extern int g_xb_fb_readback;
        g_xb_fb_readback = cfg.fbReadback == 1;
    }
    if (disc && disc[0] && disc_info_read_file(disc, &info) == 0)
    {
        XbGameIdentity(disc, &info);
        disc_info_folder(&info, gameFolder, sizeof(gameFolder));
        SettingsPaths(gameFolder, NULL, gamePath, sizeof(gamePath));
        SettingsLoad(&cfg, gamePath);
        LOG_INFO("[emu] %s (%s %s)", info.title, info.product, info.version);
        gunPort = gun_port_of(info.product);
        gameSoundLag = sound_lag_of(info.product);
        g_ta_trans_always_last = trans_always_last_of(info.product);
        g_sh4_jit_countdown_skip = cfg.countdown != 2;
        g_sh4_jit_countdown_pc = countdown_loop_of(info.product);
        g_sh4_tmu_poll_skip = !no_tmu_poll_skip_of(info.product);
        g_sh4_jit_wce_msg_poll = wce_msg_poll_of(info.product);
        slowCpuWhenBehind = slow_cpu_when_behind_of(&info);
        slowCpuClimb = slow_cpu_climb_of(info.product);
        {
            extern int g_xb_exact_depth;
            g_xb_exact_depth = cfg.exactDepth ? cfg.exactDepth == 1 : exact_depth_of(info.product);
        }
        {
            extern int g_xb_pal_shader;
            g_xb_pal_shader = cfg.palShader ? cfg.palShader == 1 : pal_shader_of(info.product);
        }
        {
            extern int g_xb_pvr_late_wait;
            g_xb_pvr_late_wait = cfg.renderWait ? cfg.renderWait == 1 : late_render_wait_of(info.product);
        }
        {
            extern int g_xb_fb_readback;
            g_xb_fb_readback = cfg.fbReadback ? cfg.fbReadback == 1 : fb_readback_of(info.product);
        }
        {
            int hpos = vga_cable_of(info.product);
            if (cfg.cable) sh4_mmr_set_cable(cfg.cable == 1 ? CABLE_VGA : CABLE_RGB);
            else if (hpos) sh4_mmr_set_cable(CABLE_VGA);
            if (hpos && cfg.gunHpos <= 0)
            {
                extern int g_pvr_gun_hpos;
                g_pvr_gun_hpos = hpos;
            }
        }
        rotateConv = arcade_conversion(info.product);
        g_mmu_sq_only_ok = cfg.mmuSq ? cfg.mmuSq == 1 : mmu_sq_only_of(info.product);
        if (gunPort)
        {
            cfg.lightGun = 1;
            LOG_INFO("[emu] light gun on port %c", 'A' + gunPort);
        }
    }
    if (!s_initDone)
    {
        if (dc_init() != 0)
        {
            message(dev, Tr(T_OUT_OF_MEMORY), Tr(T_OUT_OF_MEMORY_TEXT));
            return XB_EMU_ERROR;
        }
        if (!cfg.gpuRender || xb_pvr_init(dev) != 0) soft_render_install();
        if (sh4_jit_init(xb_codemem_base() + 1024, XB_CODEMEM_BYTES - 4096 - ARM_CODE_BYTES, xb_codemem_flush) != 0)
            LOG_ERROR("[emu] recompiler unavailable");
        if (arm7_jit_init(xb_codemem_base() + (XB_CODEMEM_BYTES - ARM_CODE_BYTES) / 4, ARM_CODE_BYTES, xb_codemem_flush) != 0)
            LOG_ERROR("[emu] ARM7 recompiler unavailable");
        if (cfg.soundThread) sound_thread_start(2);
        g_pvr_ta_skip = cfg.taSkip;
        if (cfg.sampler)
        {
            xb_sampler_start();
            g_sh4_jit_dispatch_hook = dispatch_count;
            { extern int g_sh4_jit_exit_why; g_sh4_jit_exit_why = 1; }
            { extern int g_sh4_jit_count_calls; g_sh4_jit_count_calls = 1; }
            { extern int g_aica_measure, g_xb_audio_measure; g_aica_measure = g_xb_audio_measure = 1; }
        }
        if (cfg.fpuBench) xb_fpu_bench();
        if (cfg.wavDump > 0)
        {
            extern s16 *g_aica_dump;
            extern u32 g_aica_dump_cap, g_aica_dump_n, g_aica_dump_skip;
            g_aica_dump_cap = (u32)cfg.wavDump * 44100u;
            g_aica_dump = (s16 *)malloc(g_aica_dump_cap * 4);
            g_aica_dump_n = 0;
            g_aica_dump_skip = (u32)(cfg.wavDumpAt > 0 ? cfg.wavDumpAt : 0) * 44100u;
            if (!g_aica_dump) g_aica_dump_cap = 0;
        }
        s_initDone = 1;
    }
    g_sh4_jit_fast_fpu = cfg.fastFpu;
    g_sh4_jit_split = cfg.jitSplit;
    g_sh4_jit_skips = cfg.jitSkips;
    g_sh4_jit_fastmem = cfg.fastMem;
    if (cfg.useJit && cfg.fastMem && debugger_takes_faults())
    {
        int choice = debugger_screen(dev);
        LOG_WARN("[emu] a debugger is attached (xbdm): %s", choice == DEBUGGER_NO_FASTMEM ? "fast memory access off for this game"
                                                            : choice == DEBUGGER_KEEP ? "fast memory access kept (the game may freeze)"
                                                                                      : "back to the list");
        if (choice == DEBUGGER_BACK) return XB_EMU_TO_LIST;
        if (choice == DEBUGGER_NO_FASTMEM) g_sh4_jit_fastmem = 0;
    }
    g_sh4_jit_idle_skip = cfg.idleSkip;
    if (cfg.jitTrail && !cfg.sampler)
    {
        extern void (*g_sh4_illegal_hook)(u32 pc);
        g_sh4_jit_dispatch_hook = trail_record;
        g_sh4_illegal_hook = trail_dump;
    }
    g_xb_auto_rotate = 0;
    {
        extern int g_xb_orient;
        g_xb_orient = rotateConv && rotate_auto(&cfg);
    }
    dc_set_cpu_percent(cfg.sh4Clock > 0 ? cfg.sh4Clock : 100);
    {
        extern int g_xb_trans_sort, g_xb_mod_vols, g_xb_trans_depth, g_xb_fog;
        g_xb_trans_sort = cfg.transSort;
        g_xb_mod_vols = cfg.modVols;
        g_xb_trans_depth = cfg.transDepth;
        { extern int g_xb_huge_depth; g_xb_huge_depth = cfg.hugeDepth; }
        g_xb_fog = cfg.fog;
        g_xb_widescreen = wide_mode_of(&cfg, &info);
        if (cfg.widescreen) LOG_INFO("[emu] 16:9: %s", g_xb_widescreen == WIDE_REVEAL ? "the sides beyond the 4:3 picture shown"
                                                        : g_xb_widescreen == WIDE_ANAMORPHIC ? (wide_patch_active() ? "the game's 16:9 patch, its picture stretched" : "the picture stretched (the game draws 16:9)")
                                                                                             : "4:3 picture, the sides filled");
        { extern int g_xb_tex_filter; g_xb_tex_filter = cfg.texFilter; } { extern int g_xb_render_1x; g_xb_render_1x = cfg.renderScale == 1; }
        { extern int g_aica_trace; g_aica_trace = cfg.aicaTrace; }
        {
            extern u64 g_sound_lag_max;
            int lag = cfg.soundLag > 0 ? cfg.soundLag : gameSoundLag;
            g_sound_lag_max = lag > 0 ? (u64)lag * 4535u : (u64)SH4_CLOCK_HZ / 60u;
            if (lag > 0) LOG_INFO("[sound] the sound side at most %d samples behind the SH-4", lag);
        }
    }
    sh4_jit_enable(cfg.useJit);
    xb_audio_set_target_ms(cfg.audioMs > 0 ? cfg.audioMs : 120);
    g_cdda_realtime = cfg.cdRealtime;
    audioThread = xb_audio_start_thread(aica_audio_read, aica_audio_available, cfg.audioStretch);
    dc_set_slice(cfg.slice);
    arm7_jit_enable(cfg.armJit);
    xb_fs_join(boot, sizeof(boot), cfg.biosDir, "dc_boot.bin");
    xb_fs_join(flash, sizeof(flash), cfg.biosDir, "dc_flash.bin");
    useReal = cfg.biosMode == BIOS_REAL || (cfg.biosMode == BIOS_AUTO && xb_fs_exists(boot) && xb_fs_exists(flash));
    if (useReal && dc_load_bios(boot, flash, error, sizeof(error)) != 0)
    {
        LOG_ERROR("[emu] %s", error);
        message(dev, Tr(T_BIOS_MISSING), error);
        return XB_EMU_ERROR;
    }
    if (useReal && disc && disc[0])
    {
        int region = cfg.region;
        if (region < 0)
        {
            DiscInfo di;
            region = disc_info_read_file(disc, &di) == 0 ? disc_info_console_region(&di) : -1;
        }
        flash_set_region(region);
        LOG_INFO("[emu] Sega BIOS, console region %d", region);
    }
    if (!useReal)
    {
        if (!disc || !disc[0])
        {
            message(dev, Tr(T_NO_DISC), Tr(T_NO_DISC_TEXT));
            return XB_EMU_ERROR;
        }
        int region = cfg.region, language = HLE_LANG_ENGLISH;
        const char *ui = LangCode(LangGet());
        if (region < 0)
        {
            DiscInfo di;
            region = disc_info_read_file(disc, &di) == 0 ? disc_info_console_region(&di) : HLE_REGION_USA;
        }
        if (region == HLE_REGION_JAPAN) language = HLE_LANG_JAPANESE;
        else if (!strcmp(ui, "de")) language = HLE_LANG_GERMAN;
        else if (!strcmp(ui, "fr")) language = HLE_LANG_FRENCH;
        else if (!strcmp(ui, "es")) language = HLE_LANG_SPANISH;
        dc_use_hle_bios(region, language);
    }
    disc_set_read_ahead(3);
    if (dc_insert_disc(disc) != 0)
    {
        disc_set_read_ahead(-2);
        _snprintf(error, sizeof(error), "%s", Trf(T_CANNOT_OPEN_DISC, disc));
        error[sizeof(error) - 1] = 0;
        message(dev, Tr(T_DISC_ERROR), error);
        return XB_EMU_ERROR;
    }
    dc_reset();
    xb_pvr_reset();
    vmu_load(gameFolder, cfg.vmuShared);
    cheats_load(&cfg, gameFolder[0] ? info.product : "", gameFolder);
    XbRaGameStart(disc);
    if (cfg.loadState)
    {
        if (state_load(gameFolder, cfg.loadState, discs.count > 1 ? discs.current : 0, error, sizeof(error)) != 0)
            LOG_ERROR("[emu] %s", error);
    }
    LOG_INFO("[emu] start, disc: %s", disc && disc[0] ? disc : "none");
    xb_log_flush();

    screen.tex = NULL;
    screen.cfg = &cfg;
    QueryPerformanceFrequency(&freq);
    prof_set_clock(qpc_now, (u64)freq.QuadPart);
    prof_reset();
    QueryPerformanceCounter(&t0);
    nextField = t0.QuadPart;
    cyclesAtT0 = sched_now();
    XbPadGameStarts();
    maple_set_rumble_pack(cfg.rumble);
    if (!vsync) dev->SetRenderState(D3DRS_PRESENTINTERVAL, D3DPRESENT_INTERVAL_IMMEDIATE);
    while (!quit)
    {
        XINPUT_STATE pad;
        MaplePad mp;
        IDirect3DTexture9 *tex = NULL;
        int w = 0, h = 0, openMenu = 0;

        ZeroMemory(&pad, sizeof(pad));
        if (XbPadGet(0, &pad) == ERROR_SUCCESS)
        {
            WORD b = pad.Gamepad.wButtons, pressed = (WORD)(b & ~prevButtons);
            prevButtons = b;
            if (b & XINPUT_GAMEPAD_BACK)
            {
                if (!backDown) backDown = GetTickCount();
                if (!backUsed && GetTickCount() - backDown >= BACK_MENU_MS && !(s_osdAt && GetTickCount() - s_osdAt < OSD_MS))
                    osd("%s", Tr(T_KEEP_HOLDING));
                if (!backUsed && GetTickCount() - backDown >= BACK_LIST_MS)
                {
                    outcome = XB_EMU_TO_LIST;
                    quit = 1;
                    backUsed = 1;
                }
                if (pressed & (XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_DPAD_LEFT |
                               XINPUT_GAMEPAD_DPAD_RIGHT))
                {
                    char error[200];
                    int discNow = discs.count > 1 ? discs.current : 0;
                    backUsed = 1;
                    if (!gameFolder[0])
                        osd("%s", Tr(T_OSD_NO_STATES));
                    else if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)
                    {
                        xb_pvr_sync();
                        if (state_save(gameFolder, s_lastSlot, discNow, error, sizeof(error)) == 0)
                            osd(Tr(T_OSD_SAVED), s_lastSlot);
                        else
                            osd("%s", error);
                    }
                    else if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)
                    {
                        xb_pvr_sync();
                        if (state_load(gameFolder, s_lastSlot, discNow, error, sizeof(error)) == 0)
                        {
                            xb_pvr_reset();
                            screen.tex = NULL;
                            osd(Tr(T_OSD_LOADED), s_lastSlot);
                        }
                        else
                            osd("%s", error);
                    }
                    else
                    {
                        s_lastSlot += (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : -1;
                        if (s_lastSlot < 1) s_lastSlot = STATE_SLOTS;
                        if (s_lastSlot > STATE_SLOTS) s_lastSlot = 1;
                        osd(Tr(state_exists(gameFolder, s_lastSlot) ? T_OSD_SLOT_SAVED : T_OSD_SLOT_EMPTY), s_lastSlot);
                    }
                }
                ZeroMemory(&pad.Gamepad, sizeof(pad.Gamepad));
            }
            else if (backDown)
            {
                openMenu = !backUsed && GetTickCount() - backDown < BACK_MENU_MS;
                backDown = 0;
                backUsed = 0;
            }
            pad.Gamepad.wButtons &= ~XINPUT_GAMEPAD_BACK;
            if (cfg.lightGun && gunPort)
            {
                gun_from_xinput(&pad.Gamepad, &mp);
                maple_set_pad(gunPort, &mp);
                pad_from_xinput(&pad.Gamepad, &mp, &cfg, 0);
            }
            else if (cfg.lightGun)
                gun_from_xinput(&pad.Gamepad, &mp);
            else
                pad_from_xinput(&pad.Gamepad, &mp, &cfg, 0);
            maple_set_pad(0, &mp);
        }
        {
            DWORD port;
            for (port = 1; port < 4; port++)
            {
                XINPUT_STATE other;
                if (cfg.lightGun && (int)port == gunPort) continue;
                if (cfg.lightGun && gunPort == 2 && port == 3)
                {
                    ZeroMemory(&mp, sizeof(mp));
                    mp.connected = 1;
                    mp.gun = 1;
                    mp.gunOff = 1;
                    mp.jx = mp.jy = 128;
                    maple_set_pad(3, &mp);
                    continue;
                }
                ZeroMemory(&other, sizeof(other));
                if (XbPadGet(port, &other) == ERROR_SUCCESS)
                {
                    other.Gamepad.wButtons &= ~XINPUT_GAMEPAD_BACK;
                    pad_from_xinput(&other.Gamepad, &mp, &cfg, (int)port);
                }
                else
                    ZeroMemory(&mp, sizeof(mp));
                maple_set_pad((int)port, &mp);
            }
        }
        rumble_update(&cfg);
        if (s_achOpen) openMenu = 1;
        if (openMenu)
        {
            int r;
            rumble_stop_all();
            prof_reset();
            xb_pvr_sync();
            if (!vsync) dev->SetRenderState(D3DRS_PRESENTINTERVAL, D3DPRESENT_INTERVAL_ONE);
            r = pause_menu(dev, &cfg, gameFolder, &screen, &discs);
            vsync = cfg.vsync != 0;
            if (!vsync) dev->SetRenderState(D3DRS_PRESENTINTERVAL, D3DPRESENT_INTERVAL_IMMEDIATE);
            { extern int g_xb_tex_filter; g_xb_tex_filter = cfg.texFilter; } { extern int g_xb_render_1x; g_xb_render_1x = cfg.renderScale == 1; }
            maple_set_rumble_pack(cfg.rumble);
            g_xb_widescreen = wide_mode_of(&cfg, &info);
            if (r == PAUSE_RESET)
            {
                dc_reset();
                xb_pvr_reset();
                screen.tex = NULL;
                XbRaReset(cheat_active(s_cheats));
                LOG_INFO("[emu] reset");
            }
            else if (r == PAUSE_LOADED)
            {
                xb_pvr_reset();
                screen.tex = NULL;
            }
            else if (r == PAUSE_LIST) break;
            else if (r == PAUSE_QUIT)
            {
                outcome = XB_EMU_QUIT;
                break;
            }
            QueryPerformanceCounter(&t0);
            fpsFrames = 0;
            cyclesAtT0 = sched_now();
            continue;
        }

#ifdef EMU_TRACE
        if (cfg.traceField > 0 && frames + 1 == (u32)cfg.traceField)
        {
            const char *file = trace_start();
            result = run_frame_guarded(crash, sizeof(crash));
            if (file) trace_stop();
            LOG_INFO("[emu] field %u traced: %s", frames + 1, file ? file : "no trace");
        }
        else
#endif
        sh4_refresh_rounding();
        result = run_frame_guarded(crash, sizeof(crash));
        sh4_host_rounding();
        if (result == -2)
        {
            LOG_ERROR("[emu] %s", crash);
            xb_log_flush();
            message(dev, Tr(T_CRASHED), crash);
            break;
        }
        frames++;
        fpsFrames++;
        cheat_apply(s_cheats);
        wide_patch_apply();
        XbRaFrame(cheat_active(s_cheats));
        if (cfg.raToast > 0 && frames == (u32)cfg.raToast) XbRaTestToast();
        if (cfg.achAt > 0 && frames == (u32)cfg.achAt) s_achOpen = 1;
        if (cfg.gpuDump > 0 && frames == (u32)cfg.gpuDump) xb_pvr_dump_frames(cfg.gpuDumpCount > 0 ? cfg.gpuDumpCount : 1);
        if (cfg.jitMap > 0 && frames % (u32)cfg.jitMap == 0)
        {
            char path[96];
            FILE *f;
            xb_fs_data_path(path, sizeof(path), "saves", "jitmap.txt");
            f = fopen(path, "w");
            if (f)
            {
                sh4_jit_block_map(jitmap_line, f);
                fclose(f);
            }
            xb_fs_data_path(path, sizeof(path), "saves", "jitram.bin");
            f = fopen(path, "wb");
            if (f)
            {
                fwrite(g_ram, 1, RAM_SIZE, f);
                fclose(f);
            }
        }
        if (cfg.dumpBlocks[0] && frames == (u32)(cfg.dumpBlocksAt > 0 ? cfg.dumpBlocksAt : 600))
        {
            const char *p = cfg.dumpBlocks;
            while (*p)
            {
                char *end, path[96], name[32];
                u32 pc = (u32)strtoul(p, &end, 16), size;
                const u32 *code;
                if (end == p) break;
                size = sh4_jit_block_code(pc, &code);
                if (size)
                {
                    HANDLE h;
                    DWORD done;
                    sprintf(name, "blk_%08X.bin", pc);
                    xb_fs_data_path(path, sizeof(path), "saves", name);
                    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (h != INVALID_HANDLE_VALUE)
                    {
                        void *copy = malloc(size);
                        if (copy)
                        {
                            memcpy(copy, code, size);
                            WriteFile(h, copy, size, &done, NULL);
                            free(copy);
                        }
                        CloseHandle(h);
                    }
                    LOG_INFO("[jit] block %08X: %u bytes at %p written to %s", pc, size, (const void *)code, path);
                }
                else
                    LOG_INFO("[jit] block %08X: not compiled", pc);
                p = end;
                while (*p == ',' || *p == ' ') p++;
            }
        }
        if (cfg.sceneDump > 0 && frames >= (u32)cfg.sceneDump &&
            (frames - (u32)cfg.sceneDump) % (u32)(cfg.sceneDumpEvery > 0 ? cfg.sceneDumpEvery : 1) == 0 &&
            (frames - (u32)cfg.sceneDump) / (u32)(cfg.sceneDumpEvery > 0 ? cfg.sceneDumpEvery : 1) <
                (u32)(cfg.sceneDumpCount > 0 ? cfg.sceneDumpCount : 1))
        {
            char path[96], name[32];
            if (cfg.sceneDumpCount > 1) sprintf(name, "scene_%u.bin", frames);
            else strcpy(name, "scene.bin");
            xb_fs_data_path(path, sizeof(path), "saves", name);
            xb_pvr_dump_next_scene(path);
        }
        if (cfg.saveStateAt > 0 && frames == (u32)cfg.saveStateAt &&
            state_save(gameFolder, 1, discs.count > 1 ? discs.current : 0, error, sizeof(error)) != 0)
            LOG_ERROR("[emu] %s", error);

        if (frames == 1 && !useReal && hle_bios_boot_status() < 0)
        {
            message(dev, Tr(T_BOOT_FAILED), Tr(T_BOOT_FAILED_TEXT));
            break;
        }
        prof_begin(PROF_PRESENT);
        skipPresent = !skippedLast && cfg.gpuDisplay && xb_pvr_busy();
        skippedLast = skipPresent;
        if (!skipPresent)
        {
            xb_pvr_sync();
            tex = cfg.gpuDisplay ? xb_pvr_display() : NULL;
            if (!s_fb) s_fb = (u32 *)malloc(FB_MAX_W * FB_MAX_H * sizeof(u32));
            if (!tex && s_fb && pvr_read_framebuffer(s_fb, &w, &h) && w > 0 && h > 0) tex = upload(dev, w, h);
            screen.tex = tex;
        }

        QueryPerformanceCounter(&now);
        if (now.QuadPart - t0.QuadPart >= freq.QuadPart)
        {
            double secs = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
            double mips = (double)(sched_now() - cyclesAtT0) / secs / 1e6;
            f64 ms[PROF_COUNT];
            fps = fpsFrames / secs;
            {
                u64 cyclesNow = sched_now();
                speed = cyclesNow > cyclesAtT0 ? (double)(cyclesNow - cyclesAtT0) / secs / (double)SH4_CLOCK_HZ * 100.0 : 0.0;
            }
            {
                if (vsync) vsyncOnSeconds++;
                if (cfg.vsync == 1 && vsync && fps < 57.0)
                {
                    dev->SetRenderState(D3DRS_PRESENTINTERVAL, D3DPRESENT_INTERVAL_IMMEDIATE);
                    vsync = 0;
                    fastSeconds = 0;
                    if (vsyncOnSeconds <= 2) vsyncWait = vsyncWait * 2 > 96 ? 96 : vsyncWait * 2;
                }
                else if (cfg.vsync == 1 && !vsync && (fps >= 59.0 ? ++fastSeconds : (fastSeconds = 0)) >= vsyncWait)
                {
                    dev->SetRenderState(D3DRS_PRESENTINTERVAL, D3DPRESENT_INTERVAL_ONE);
                    vsync = 1;
                    vsyncOnSeconds = 0;
                }
                if (vsync && vsyncOnSeconds >= 30) vsyncWait = 3;
            }
            prof_report(fpsFrames, ms);
            {
                extern int g_xb_orient;
                g_xb_orient = rotateConv && rotate_auto(&cfg) && !g_xb_auto_rotate;
            }
            if (rotateConv && rotate_auto(&cfg) && !g_xb_auto_rotate)
            {
                f32 a, d, u, t;
                int want = 0;
                xb_pvr_orientation(&a, &d, &u);
                t = a + d + u;
                if (t > 120.0f)
                {
                    if (d > 0.8f * t) want = 270;
                    else if (u > 0.8f * t) want = 90;
                }
                if (!want) rotateStreak = 0;
                else if (want != rotatePending)
                {
                    rotatePending = want;
                    rotateStreak = 1;
                }
                else if (++rotateStreak >= 2)
                {
                    g_xb_auto_rotate = want;
                    {
                        extern int g_xb_orient;
                        g_xb_orient = 0;
                    }
                    LOG_INFO("[emu] picture turned %d degrees (the game draws for an upright screen)", want);
                }
            }
            if (cfg.sh4Clock < 0)
            {
                static u32 lastYuv;
                u32 yuv = pvr_yuv_blocks();
                int pct = dc_cpu_percent(), was = pct;
                int behind = cfg.slowCpu == 1 ? 1 : cfg.slowCpu == 2 ? 0 : slowCpuWhenBehind;
                if (yuv - lastYuv >= 60 || behind)
                {
                    if (speed < 96.7)
                    {
                        pct = (int)(pct * speed / 100.0);
                        if (pct < (behind ? 40 : 50)) pct = behind ? 40 : 50;
                    }
                    else if (pct < 100 && (behind && slowCpuClimb ? ms[PROF_SH4] * (pct + 5) / pct < 14.5 : ms[PROF_SH4] < 10.0))
                        pct = pct + 5 > 100 ? 100 : pct + 5;
                }
                else pct = 100;
                lastYuv = yuv;
                if (pct != was)
                {
                    dc_set_cpu_percent(pct);
                    LOG_INFO("[emu] SH-4 at %d%% of its speed (%s)", pct, pct < 100 ? (behind ? "behind" : "movie") : (behind ? "caught up" : "movie over"));
                }
            }
            cpuLoad = (ms[PROF_SH4] + ms[PROF_ARM7] + ms[PROF_AICA]) / (1000.0 / 59.94);
            Sh4JitStats js;
            sh4_jit_stats(&js);
            LOG_INFO("[emu] %.1f fields/s, %.1f SH-4 MIPS, pc %08X%s; ms/field: sh4 %.1f arm7 %.1f aica %.1f render %.1f present %.1f "
                     "sound wait %.1f render wait %.1f; jit %u compiled, %u invalidated, %u flushes",
                     fps, mips, g_sh4.pc, sh4_jit_active() ? " (jit)" : "", ms[PROF_SH4], ms[PROF_ARM7], ms[PROF_AICA], ms[PROF_RENDER],
                     ms[PROF_PRESENT], ms[PROF_SOUND_WAIT], ms[PROF_RENDER_WAIT], (u32)js.blocksCompiled, (u32)js.invalidations,
                     (u32)js.flushes);
            {
                u32 decodes;
                f64 decodeMs, uploadMs;
                xb_pvr_tex_stats(&decodes, &decodeMs, &uploadMs);
                LOG_INFO("[xenos] per field: %.1f textures decoded, %.2f ms decoding, %.2f ms creating/filling",
                         decodes / (double)fpsFrames, decodeMs / fpsFrames, uploadMs / fpsFrames);
                {
                    u32 bands;
                    f64 fbMs, fbWaitMs;
                    xb_pvr_fb_stats(&bands, &fbMs, &fbWaitMs);
                    if (bands)
                        LOG_INFO("[xenos] frames read back per field: %.1f bands of lines, %.2f ms (%.2f ms waiting for the frame)",
                                 bands / (double)fpsFrames, fbMs / fpsFrames, fbWaitMs / fpsFrames);
                }
                {
                    f64 dms, oms, tms;
                    u32 rf;
                    xb_pvr_render_stats(&dms, &oms, &tms, &rf);
                    if (rf)
                        LOG_INFO("[xenos] per frame drawn: TA decoding %.2f ms, opaque+punch-through %.2f ms, translucent %.2f ms",
                                 dms / rf, oms / rf, tms / rf);
                }
            }
            {
                static u64 lastSlices, lastRun, lastSkips;
                char events[256];
                sched_report(events, sizeof(events));
                LOG_INFO("[emu] per field: %.0f slices, %.0f dispatches, %.0f idle skips; events: %s",
                         (js.slices - lastSlices) / (double)fpsFrames, (js.blocksRun - lastRun) / (double)fpsFrames,
                         (js.idleSkips - lastSkips) / (double)fpsFrames, events);
                {
                    extern u32 g_sh4_jit_poll_stats[4];
                    if (g_sh4_jit_poll_stats[0])
                        LOG_INFO("[emu] polled library calls per field: %.0f, %.0f quick, %.0f waits, %.0f kept by an interrupt",
                                 g_sh4_jit_poll_stats[0] / (double)fpsFrames, g_sh4_jit_poll_stats[1] / (double)fpsFrames,
                                 g_sh4_jit_poll_stats[2] / (double)fpsFrames, g_sh4_jit_poll_stats[3] / (double)fpsFrames);
                    memset(g_sh4_jit_poll_stats, 0, sizeof(g_sh4_jit_poll_stats));
                }
                {
                    extern u32 g_sh4_jit_mmu_slow[6];
                    u32 *m = g_sh4_jit_mmu_slow;
                    if (m[0] | m[1] | m[2] | m[3] | m[4] | m[5])
                        LOG_INFO("[emu] MMU slow path per field: RAM %.0f read %.0f write, other %.0f read %.0f write, pairs %.0f, store queues %.0f",
                                 m[0] / (double)fpsFrames, m[1] / (double)fpsFrames, m[2] / (double)fpsFrames, m[3] / (double)fpsFrames,
                                 m[4] / (double)fpsFrames, m[5] / (double)fpsFrames);
                    memset(g_sh4_jit_mmu_slow, 0, sizeof(g_sh4_jit_mmu_slow));
                }
                if (cfg.sampler)
                {
                    extern u32 g_sh4_jit_lookup_why[8];
                    u32 *w = g_sh4_jit_lookup_why;
                    dispatch_report(fpsFrames);
                    LOG_INFO("[emu] dispatcher entries with the MMU, why per field: no page tag %u, no block %u, another address %u, "
                             "no FPSCR version %u, there %u; chains stopped %u, irq ready %u; code pages refilled %u", w[0] / fpsFrames, w[1] / fpsFrames,
                             w[2] / fpsFrames, w[3] / fpsFrames, w[4] / fpsFrames, w[5] / fpsFrames, w[6] / fpsFrames, w[7] / fpsFrames);
                    memset(g_sh4_jit_lookup_why, 0, sizeof(g_sh4_jit_lookup_why));
                    {
                        extern u32 g_sh4_jit_call_stats[16];
                        u32 *k = g_sh4_jit_call_stats;
                        LOG_INFO("[emu] per field: %u blocks entered, %u calls (%u to leaves <= 16, %u to leaves 17-32, %u through an unknown register), %u rts",
                                 k[0] / fpsFrames, k[1] / fpsFrames, k[2] / fpsFrames, k[3] / fpsFrames, k[5] / fpsFrames, k[4] / fpsFrames);
                        LOG_INFO("[emu] calls through an unknown register per field, its literal: none in the block %u, other page %u, "
                                 "outside the block's parts %u, after a store %u, written %u, computed %u",
                                 k[8] / fpsFrames, k[9] / fpsFrames, k[10] / fpsFrames, k[11] / fpsFrames, k[12] / fpsFrames, k[13] / fpsFrames);
                        memset(g_sh4_jit_call_stats, 0, 16 * sizeof(u32));
                    }
                    {
                        extern u32 g_sh4_jit_probe[16];
                        extern u32 g_sh4_trap_counts[256][2];
                        extern u32 g_sh4_trap_total;
                        u32 *p = g_sh4_jit_probe, f = fpsFrames, k, i;
                        char line[256];
                        int len;
                        LOG_INFO("[probe] per field: %u ops in unknown FPSCR mode (%u FPU interpreted), %u FPU interpreted with PR=1, "
                                 "%u FPSCR writes, %u exits unchained for it; %u div1, %u ftrv, %u fipr; %u exits, %u regs written back, %u reloaded",
                                 p[0] / f, p[1] / f, p[2] / f, p[3] / f, p[4] / f, p[5] / f, p[6] / f, p[7] / f, p[10] / f, p[8] / f, p[9] / f);
                        memset(g_sh4_jit_probe, 0, sizeof(g_sh4_jit_probe));
                        len = _snprintf(line, sizeof(line), "[probe] system calls per field: %u;", g_sh4_trap_total / f);
                        for (k = 0; k < 6 && len > 0 && len < (int)sizeof(line); k++)
                        {
                            int best = -1;
                            for (i = 0; i < 256; i++)
                                if (g_sh4_trap_counts[i][1] && (best < 0 || g_sh4_trap_counts[i][1] > g_sh4_trap_counts[best][1])) best = (int)i;
                            if (best < 0) break;
                            len += _snprintf(line + len, sizeof(line) - len, " %08X %u", g_sh4_trap_counts[best][0], g_sh4_trap_counts[best][1] / f);
                            g_sh4_trap_counts[best][1] = 0;
                        }
                        line[sizeof(line) - 1] = 0;
                        LOG_INFO("%s", line);
                        memset(g_sh4_trap_counts, 0, sizeof(g_sh4_trap_counts));
                        g_sh4_trap_total = 0;
                    }
                    {
                        extern u32 g_mmu_stats[16];
                        u32 *m = g_mmu_stats;
                        LOG_INFO("[mmu] per field: %u refills (%u same as before, %u changed, %u new), %u by exception, %u ldtlb, "
                                 "%u ASID changes, %u TLB flushes, %u TLB array writes, %u translation cache fills, %u page table fills, %u emptied",
                                 m[0] / fpsFrames, m[5] / fpsFrames, m[6] / fpsFrames, m[7] / fpsFrames, m[8] / fpsFrames, m[1] / fpsFrames,
                                 m[2] / fpsFrames, m[3] / fpsFrames, m[4] / fpsFrames, m[9] / fpsFrames, m[11] / fpsFrames, m[12] / fpsFrames);
                        memset(g_mmu_stats, 0, sizeof(g_mmu_stats));
                    }
                    {
                        extern u32 g_sh4_jit_exit_fail[11];
                        u32 *f = g_sh4_jit_exit_fail;
                        LOG_INFO("[emu] indirect exits that missed, per field, by check: 2 %u, 3 %u, 4 %u, 5 %u, 6 %u, 7 %u, 9 %u",
                                 f[3] / fpsFrames, f[4] / fpsFrames, f[5] / fpsFrames, f[6] / fpsFrames, f[7] / fpsFrames, f[8] / fpsFrames,
                                 f[10] / fpsFrames);
                        memset(g_sh4_jit_exit_fail, 0, sizeof(g_sh4_jit_exit_fail));
                    }
                }
                lastSlices = js.slices;
                lastRun = js.blocksRun;
                lastSkips = js.idleSkips;
            }
            {
                static u32 lastExc[64];
                char line[256];
                int n = 0, k;
                for (k = 0; k < 64 && n < 200; k++)
                    if (g_sh4_exc_counts[k] != lastExc[k])
                    {
                        n += _snprintf(line + n, sizeof(line) - n, " %03X %.0f", k << 5,
                                       (g_sh4_exc_counts[k] - lastExc[k]) / (double)fpsFrames);
                        lastExc[k] = g_sh4_exc_counts[k];
                    }
                if (n > 0) LOG_INFO("[emu] exceptions per field:%s", line);
            }
            {
                XbAudioStats as;
                xb_audio_stats(&as);
                LOG_INFO("[audio] queue %u ms, rate %u.%03u, %u underruns, %u resyncs", as.fillMs, as.ratioPermille / 1000,
                         as.ratioPermille % 1000, as.underruns, as.resyncs);
                if (cfg.sampler)
                {
                    extern u32 g_aica_rms[16], g_aica_rms_count, g_xb_audio_rms[32], g_xb_audio_rms_count;
                    char line[400];
                    int n = _snprintf(line, sizeof(line), "[audio] rms aica:");
                    u32 k, c = g_aica_rms_count, o = g_xb_audio_rms_count;
                    for (k = c > 10 ? c - 10 : 0; k < c && n > 0 && n < (int)sizeof(line); k++) n += _snprintf(line + n, sizeof(line) - n, " %u", g_aica_rms[k & 15]);
                    if (n > 0 && n < (int)sizeof(line)) n += _snprintf(line + n, sizeof(line) - n, " | out:");
                    for (k = o > 24 ? o - 24 : 0; k < o && n > 0 && n < (int)sizeof(line); k++) n += _snprintf(line + n, sizeof(line) - n, " %u", g_xb_audio_rms[k & 31]);
                    line[sizeof(line) - 1] = 0;
                    LOG_INFO("%s", line);
                    {
                        extern void aica_debug_levels(char *out, int size);
                        char lv[700];
                        aica_debug_levels(lv, sizeof(lv));
                        LOG_INFO("[audio] %s", lv);
                    }
                }
            }
            {
                SoundWaitStats sw;
                sound_thread_wait_stats(&sw);
                if (sw.reads || sw.writes || sw.queueFull || sw.ahead || sw.fastReads)
                    LOG_INFO("[sound] SH-4 waits per field: %.1f reads, %.1f writes, %.1f full queue, %.1f ahead; %.1f reads without waiting",
                             sw.reads / (double)fpsFrames, sw.writes / (double)fpsFrames, sw.queueFull / (double)fpsFrames,
                             sw.ahead / (double)fpsFrames, sw.fastReads / (double)fpsFrames);
            }
            prof_reset();
            fpsFrames = 0;
            t0 = now;
            cyclesAtT0 = sched_now();
            if (maple_vmu_take_dirty()) vmu_save();
            if (cfg.sampler && ++seconds % 5 == 0) xb_sampler_report();
        }

        if (!skipPresent)
        {
        XbRaBadgesPump(dev);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        UiBegin(dev);
        if (tex) draw_picture(dev, tex, &cfg);
        if (tex && cfg.lightGun) draw_gun_sight();
        if (cfg.showFps)
        {
            char badge[192];
            int n = _snprintf(badge, sizeof(badge), "%.1f FPS", fps);
            if (cfg.showFps >= 2 && n > 0)
            {
                char sp[48], res[48];
                _snprintf(sp, sizeof(sp) - 1, Tr(T_SPEED_PCT), speed > 0 ? (int)(speed + 0.5) : 0);
                sp[sizeof(sp) - 1] = 0;
                shown_resolution(res, sizeof(res), screen.tex, &cfg);
                n += _snprintf(badge + n, sizeof(badge) - n, "\n%s\n%s", sp, res);
            }
            if (cfg.showFps >= 3 && n > 0 && n < (int)sizeof(badge))
            {
                MEMORYSTATUS mem;
                GlobalMemoryStatus(&mem);
                _snprintf(badge + n, sizeof(badge) - n, "\nCPU %d%%\nRAM %u MB", (int)(cpuLoad * 100 + 0.5),
                          (unsigned)((mem.dwTotalPhys - mem.dwAvailPhys) >> 20));
            }
            badge[sizeof(badge) - 1] = 0;
            if (cfg.showFps >= 2) UiPerfPanel(badge);
            else UiFpsBadge(badge);
        }
        if (UiNotifyDraw()) {}
        else if (s_osdAt && GetTickCount() - s_osdAt < OSD_MS)
            UiToast(s_osd, (float)(GetTickCount() - s_osdAt), (float)OSD_MS);
        else if (frames < 300)
        {
            UiToast(Tr(T_HINT), (float)frames * 16.7f, 5000.0f);
        }
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        }
        prof_end(PROF_PRESENT);
        if (!audioThread) xb_audio_feed(aica_audio_read, aica_audio_available);
        {
            static int skippedBefore;
            static u64 cyclesBefore;
            u64 cyclesNow = sched_now(), fieldCycles = cyclesNow - cyclesBefore;
            double fieldTime = fieldCycles >= SH4_CLOCK_HZ / 75 && fieldCycles <= SH4_CLOCK_HZ / 45 ? (double)fieldCycles / (double)SH4_CLOCK_HZ
                                                                                                    : 1.0 / 59.94;
            s64 period = (s64)((double)freq.QuadPart * fieldTime);
            LARGE_INTEGER t;
            cyclesBefore = cyclesNow;
            QueryPerformanceCounter(&t);
            nextField += period;
            if (vsync && !skipPresent && !skippedBefore)
            {
                s64 soonest = nextField - period + (s64)((double)period * (59.94 / 60.3));
                nextField = t.QuadPart > soonest ? t.QuadPart : soonest;
            }
            else if (t.QuadPart - nextField > 3 * period) nextField = t.QuadPart;
            skippedBefore = skipPresent;
            while (t.QuadPart < nextField)
            {
                if (nextField - t.QuadPart > (s64)freq.QuadPart / 500) Sleep(1);
                QueryPerformanceCounter(&t);
            }
        }

        if (cfg.exitAfter > 0 && frames >= (u32)cfg.exitAfter)
        {
            outcome = XB_EMU_QUIT;
            quit = 1;
        }
    }
    rumble_stop_all();
    if (maple_vmu_take_dirty()) vmu_save();
    xb_pvr_sync();
    if (!vsync) dev->SetRenderState(D3DRS_PRESENTINTERVAL, D3DPRESENT_INTERVAL_ONE);
    LOG_INFO("[emu] stop after %u fields, pc %08X", frames, g_sh4.pc);
    {
        extern s16 *g_aica_dump;
        extern u32 g_aica_dump_n;
        if (g_aica_dump && g_aica_dump_n)
        {
            char path[96];
            FILE *f;
            u32 hd[11], i, bytes = g_aica_dump_n * 4;
            hd[0] = 0x46464952u; hd[1] = 36 + bytes; hd[2] = 0x45564157u; hd[3] = 0x20746D66u; hd[4] = 16;
            hd[5] = 1u | (2u << 16); hd[6] = 44100; hd[7] = 44100 * 4; hd[8] = 4u | (16u << 16); hd[9] = 0x61746164u; hd[10] = bytes;
            for (i = 0; i < 11; i++) hd[i] = _byteswap_ulong(hd[i]);
            for (i = 0; i < g_aica_dump_n * 2; i++) g_aica_dump[i] = (s16)_byteswap_ushort((u16)g_aica_dump[i]);
            xb_fs_data_path(path, sizeof(path), "saves", "aica.wav");
            f = fopen(path, "wb");
            if (f)
            {
                fwrite(hd, 4, 11, f);
                fwrite(g_aica_dump, 4, g_aica_dump_n, f);
                fclose(f);
                LOG_INFO("[emu] %u samples of the AICA's output written to %s", g_aica_dump_n, path);
            }
            g_aica_dump_n = 0;
        }
    }
    xb_log_flush();
    dc_insert_disc(NULL);
    disc_set_read_ahead(-2);
    XbRaGameEnd();
    wide_patch_set(NULL);
    cheat_free(s_cheats);
    s_cheats = NULL;
    return outcome;
}
