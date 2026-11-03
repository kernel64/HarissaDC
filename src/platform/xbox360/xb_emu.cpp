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
#include "../../hw/dreamcast.h"
#include "../../hw/gdrom.h"
#include "../../hw/maple.h"
#include "../../hw/pvr.h"
#include "../../mem/mem.h"
#include "../../hw/sound_thread.h"
#include "../../render/soft_render.h"
#include "../../render/ta_scene.h"
#include "../../sh4/sh4.h"
#include "../../sh4/sh4_jit.h"
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
#include "xb_sampler.h"
#include "xb_ui.h"
#ifdef EMU_TRACE
extern "C" LONG NTAPI XexGetModuleHandle(PCSTR name, PHANDLE handle);
extern "C" LONG NTAPI XexGetProcedureAddress(HANDLE module, DWORD ordinal, PVOID *address);
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
    SHORT sx = cfg->padStick[player] ? g->sThumbRX : g->sThumbLX, sy = cfg->padStick[player] ? g->sThumbRY : g->sThumbLY;
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

static const char *const s_noCountdownSkip[] = {
    "T15113N",
};

static int no_countdown_skip_of(const char *product)
{
    size_t k;
    for (k = 0; k < sizeof(s_noCountdownSkip) / sizeof(s_noCountdownSkip[0]); k++)
        if (!strcmp(product, s_noCountdownSkip[k])) return 1;
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

static void draw_picture(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex, const Config *cfg)
{
    u32 ctrl = pvr_reg(0x005F8044), size = pvr_reg(0x005F805C), depth = (ctrl >> 2) & 3;
    u32 bpp = depth == 2 ? 3 : depth == 3 ? 4 : 2, xWords = (size & 0x3FF) + 1, lines = ((size >> 10) & 0x3FF) + 1;
    int interlaced = (pvr_reg(0x005F80D0) >> 4) & 1;
    if (!tex) return;
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

static const char *vmu_path(void)
{
    static char path[64];
    return xb_fs_data_path(path, sizeof(path), "saves", "vmu_a1.bin");
}
#define VMU_PATH vmu_path()

static void vmu_load(void)
{
    static u8 data[VMU_SIZE];
    HANDLE h = CreateFileA(VMU_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0;
    if (h != INVALID_HANDLE_VALUE)
    {
        ReadFile(h, data, VMU_SIZE, &got, NULL);
        CloseHandle(h);
    }
    maple_vmu_load(got == VMU_SIZE ? data : NULL);
    LOG_INFO("[emu] VMU: %s", got == VMU_SIZE ? VMU_PATH : "new, formatted");
}

static void vmu_save(void)
{
    HANDLE h;
    DWORD put = 0;
    h = CreateFileA(VMU_PATH, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("[emu] cannot write %s", VMU_PATH);
        return;
    }
    WriteFile(h, maple_vmu_image(), VMU_SIZE, &put, NULL);
    CloseHandle(h);
    LOG_INFO("[emu] VMU saved");
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

static int state_any(const char *gameFolder)
{
    int s;
    for (s = 1; s <= STATE_SLOTS; s++)
        if (state_exists(gameFolder, s)) return 1;
    return 0;
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
    else LOG_INFO("[emu] state saved: %s (%u KB)", path, size >> 10);
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
    else LOG_INFO("[emu] state loaded: %s", path);
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

enum { P_RESUME = 1, P_RESET, P_SAVE_STATE, P_LOAD_STATE, P_DISC, P_SETTINGS, P_ABOUT, P_LIST, P_QUIT };
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

static int slot_menu(IDirect3DDevice9 *dev, const char *gameFolder, int save, Screen *screen)
{
    UiMenu m;
    UiInput in;
    char label[96];
    int s;
    UiMenuClear(&m, Tr(save ? T_SAVE_STATE : T_LOAD_STATE), Tr(save ? T_FOOT_SAVE : T_FOOT_LOAD));
    for (s = 1; s <= STATE_SLOTS; s++)
    {
        _snprintf(label, sizeof(label), Tr(T_SLOT_N), s);
        label[sizeof(label) - 1] = 0;
        UiMenuAdd(&m, s, label, Tr(state_exists(gameFolder, s) ? T_SAVED : T_EMPTY));
    }
    m.sel = s_lastSlot - 1;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        UiMenuNavigate(&m, down);
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return 0;
        if (down & XINPUT_GAMEPAD_A) return UiMenuSelected(&m);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        UiBegin(dev);
        frozen_game(screen);
        UiMenuDraw(&m);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
    }
}

static int pause_menu(IDirect3DDevice9 *dev, Config *cfg, const char *gameFolder, Screen *screen, DiscSet *discs)
{
    UiMenu m;
    UiInput in;
    UiMenuClear(&m, Tr(T_PAUSED), Tr(T_FOOT_PAUSED));
    UiMenuAdd(&m, P_RESUME, Tr(T_RESUME), NULL);
    UiMenuAdd(&m, P_RESET, Tr(T_RESET_GAME), NULL);
    UiMenuAdd(&m, P_SAVE_STATE, Tr(T_SAVE_STATE), NULL);
    if (state_any(gameFolder)) UiMenuAdd(&m, P_LOAD_STATE, Tr(T_LOAD_STATE), NULL);
    if (discs->count > 1)
    {
        char value[96];
        _snprintf(value, sizeof(value), Tr(T_DISC_OF), discs->current + 1, discs->count);
        value[sizeof(value) - 1] = 0;
        UiMenuAdd(&m, P_DISC, Tr(T_CHANGE_DISC), value);
    }
    UiMenuAdd(&m, P_SETTINGS, Tr(T_SETTINGS), NULL);
    UiMenuAdd(&m, P_ABOUT, Tr(T_ABOUT), NULL);
    UiMenuAdd(&m, P_LIST, Tr(T_BACK_TO_LIST), NULL);
    UiMenuAdd(&m, P_QUIT, Tr(T_QUIT_DASHBOARD), NULL);
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        UiMenuNavigate(&m, down);
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) return PAUSE_RESUME;
        if (down & XINPUT_GAMEPAD_A)
        {
            switch (UiMenuSelected(&m))
            {
            case P_RESUME: return PAUSE_RESUME;
            case P_RESET: return PAUSE_RESET;
            case P_SAVE_STATE:
            case P_LOAD_STATE:
            {
                char error[200];
                int disc = discs->count > 1 ? discs->current : 0, save = UiMenuSelected(&m) == P_SAVE_STATE;
                int slot = slot_menu(dev, gameFolder, save, screen);
                if (!slot) break;
                s_lastSlot = slot;
                if (save)
                {
                    if (state_save(gameFolder, slot, disc, error, sizeof(error)) == 0) return PAUSE_RESUME;
                    message(dev, Tr(T_SAVE_STATE), error);
                }
                else
                {
                    if (state_load(gameFolder, slot, disc, error, sizeof(error)) == 0) return PAUSE_LOADED;
                    message(dev, Tr(T_LOAD_STATE), error);
                }
                break;
            }
            case P_DISC:
                if (disc_menu(dev, discs, screen)) return PAUSE_RESUME;
                break;
            case P_SETTINGS: SettingsScreen(dev, cfg, gameFolder, frozen_game, screen); break;
            case P_ABOUT: AboutScreen(dev, frozen_game, screen); break;
            case P_LIST: return PAUSE_LIST;
            case P_QUIT:
                if (ConfirmScreen(dev, Tr(T_QUIT_ASK), Tr(T_QUIT), frozen_game, screen)) return PAUSE_QUIT;
                break;
            }
            UiInputInit(&in);
        }
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        UiBegin(dev);
        frozen_game(screen);
        UiMenuDraw(&m);
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
    int vsync = baseCfg->vsync != 0, fastSeconds = 0;
    double cpuLoad = 0;
    u64 cyclesAtT0;
    u32 frames = 0, fpsFrames = 0, seconds = 0;
    int skipPresent = 0, skippedLast = 0;
    int audioThread = 0;
    int gunPort = 0;
    int rotateAuto, rotatePending = 0, rotateStreak = 0;
    double fps = 0;
    int result, useReal, outcome = XB_EMU_TO_LIST, quit = 0;
    DWORD backDown = 0;
    WORD prevButtons = 0;
    int backUsed = 0;
    Config cfg = *baseCfg;
    Screen screen;
    DiscInfo info;

    s_dev = dev;
    gameFolder[0] = 0;
    memset(&discs, 0, sizeof(discs));
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
    if (disc && disc[0] && disc_info_read_file(disc, &info) == 0)
    {
        disc_info_folder(&info, gameFolder, sizeof(gameFolder));
        SettingsPaths(gameFolder, NULL, gamePath, sizeof(gamePath));
        SettingsLoad(&cfg, gamePath);
        LOG_INFO("[emu] %s (%s %s)", info.title, info.product, info.version);
        gunPort = gun_port_of(info.product);
        g_ta_trans_always_last = trans_always_last_of(info.product);
        g_sh4_jit_countdown_skip = !no_countdown_skip_of(info.product);
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
        }
        if (cfg.fpuBench) xb_fpu_bench();
        s_initDone = 1;
    }
    g_sh4_jit_fast_fpu = cfg.fastFpu;
    g_sh4_jit_split = cfg.jitSplit;
    g_sh4_jit_skips = cfg.jitSkips;
    g_sh4_jit_fastmem = cfg.fastMem;
    g_sh4_jit_idle_skip = cfg.idleSkip;
    rotateAuto = cfg.rotate != 0 && cfg.rotate != 90 && cfg.rotate != 270;
    if (rotateAuto) cfg.rotate = 0;
    {
        extern int g_xb_orient;
        g_xb_orient = rotateAuto;
    }
    dc_set_cpu_percent(cfg.sh4Clock > 0 ? cfg.sh4Clock : 100);
    {
        extern int g_xb_trans_sort, g_xb_mod_vols, g_xb_trans_depth, g_xb_fog;
        g_xb_trans_sort = cfg.transSort;
        g_xb_mod_vols = cfg.modVols;
        g_xb_trans_depth = cfg.transDepth;
        g_xb_fog = cfg.fog;
        { extern int g_xb_widescreen; g_xb_widescreen = cfg.widescreen; }
        { extern int g_xb_tex_filter; g_xb_tex_filter = cfg.texFilter; }
        { extern int g_aica_trace; g_aica_trace = cfg.aicaTrace; }
        if (cfg.soundLag > 0) { extern u64 g_sound_lag_max; g_sound_lag_max = (u64)cfg.soundLag * 4535u; }
    }
    sh4_jit_enable(cfg.useJit);
    xb_audio_set_target_ms(cfg.audioMs > 0 ? cfg.audioMs : 500);
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
    vmu_load();
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
            { extern int g_xb_tex_filter; g_xb_tex_filter = cfg.texFilter; }
            maple_set_rumble_pack(cfg.rumble);
            { extern int g_xb_widescreen; g_xb_widescreen = cfg.widescreen; }
            if (r == PAUSE_RESET)
            {
                dc_reset();
                xb_pvr_reset();
                screen.tex = NULL;
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
        result = run_frame_guarded(crash, sizeof(crash));
        if (result == -2)
        {
            LOG_ERROR("[emu] %s", crash);
            xb_log_flush();
            message(dev, Tr(T_CRASHED), crash);
            break;
        }
        frames++;
        fpsFrames++;
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
            if (cfg.vsync == 1 && vsync && fps < 57.0)
            {
                dev->SetRenderState(D3DRS_PRESENTINTERVAL, D3DPRESENT_INTERVAL_IMMEDIATE);
                vsync = 0;
                fastSeconds = 0;
            }
            else if (cfg.vsync == 1 && !vsync && (fps >= 59.0 ? ++fastSeconds : (fastSeconds = 0)) >= 3)
            {
                dev->SetRenderState(D3DRS_PRESENTINTERVAL, D3DPRESENT_INTERVAL_ONE);
                vsync = 1;
            }
            prof_report(fpsFrames, ms);
            if (rotateAuto && !cfg.rotate)
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
                    cfg.rotate = want;
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
                if (yuv - lastYuv >= 60)
                {
                    if (fps < 58.0)
                    {
                        pct = (int)(pct * fps / 60.0);
                        if (pct < 50) pct = 50;
                    }
                    else if (ms[PROF_SH4] < 10.0 && pct < 100)
                        pct = pct + 5 > 100 ? 100 : pct + 5;
                }
                else pct = 100;
                lastYuv = yuv;
                if (pct != was)
                {
                    dc_set_cpu_percent(pct);
                    LOG_INFO("[emu] SH-4 at %d%% of its speed (%s)", pct, pct < 100 ? "movie" : "movie over");
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
                LOG_INFO("[audio] queue %u ms, rate %u.%03u, %u underruns", as.fillMs, as.ratioPermille / 1000,
                         as.ratioPermille % 1000, as.underruns);
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
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        UiBegin(dev);
        if (tex) draw_picture(dev, tex, &cfg);
        if (tex && cfg.lightGun) draw_gun_sight();
        if (cfg.showFps)
        {
            char badge[96];
            int n = _snprintf(badge, sizeof(badge), "%.1f FPS", fps);
            if (cfg.showFps >= 2 && n > 0)
            {
                MEMORYSTATUS mem;
                GlobalMemoryStatus(&mem);
                n += _snprintf(badge + n, sizeof(badge) - n, "  " UI_CH_MIDDOT "  CPU %d%%  " UI_CH_MIDDOT "  RAM %u MB", (int)(cpuLoad * 100 + 0.5),
                               (unsigned)((mem.dwTotalPhys - mem.dwAvailPhys) >> 20));
            }
            if (cfg.showFps >= 3 && n > 0 && n < (int)sizeof(badge))
            {
                u32 ctrl = pvr_reg(0x005F8044), size = pvr_reg(0x005F805C), depth = (ctrl >> 2) & 3;
                u32 bpp = depth == 2 ? 3 : depth == 3 ? 4 : 2, lines = ((size >> 10) & 0x3FF) + 1;
                if ((pvr_reg(0x005F80D0) >> 4) & 1) lines *= 2;
                _snprintf(badge + n, sizeof(badge) - n, "  " UI_CH_MIDDOT "  %ux%u%s", ((size & 0x3FF) + 1) * 4 / bpp, lines,
                          cfg.widescreen && cfg.gpuDisplay && cfg.gpuRender ? " 16:9" : "");
            }
            badge[sizeof(badge) - 1] = 0;
            UiFpsBadge(badge);
        }
        if (s_osdAt && GetTickCount() - s_osdAt < OSD_MS)
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
            s64 period = (s64)((double)freq.QuadPart / 59.94);
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            nextField += period;
            if (vsync || t.QuadPart - nextField > 3 * period) nextField = t.QuadPart;
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
    xb_log_flush();
    dc_insert_disc(NULL);
    disc_set_read_ahead(-2);
    return outcome;
}
