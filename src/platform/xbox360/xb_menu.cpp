// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_menu.h"
#include "xb_art.h"
#include "xb_pad.h"
#include "xb_update.h"
#include "xb_display.h"
#include "xb_lang.h"
#include "xb_flags.h"
#include "xb_png.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/log.h"
#include "../../core/version.h"
#include "../../disc/disc_info.h"
#include "../../disc/game_files.h"
#include "xb_fs.h"
#include "xb_ui.h"

struct UiAnim
{
    float value, last;
    int live;
};

static float anim_step(UiAnim *a, float target, int restart)
{
    float now = UiTime(), dt = now - a->last;
    a->last = now;
    if (restart || !a->live || dt > 0.3f || dt < 0.0f)
    {
        a->value = target;
        a->live = 1;
        return target;
    }
    {
        float k = dt * 16.0f;
        if (k > 1.0f) k = 1.0f;
        a->value += (target - a->value) * k;
        if (fabsf(target - a->value) < 0.25f) a->value = target;
    }
    return a->value;
}

static float smooth01(float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return t * t * (3.0f - 2.0f * t);
}

static void selection_bar_side(float x, float y, float w, float h, int right)
{
    UiRoundRect(x, y, w, h, 12, D3DCOLOR_ARGB(78, 240, 112, 32), D3DCOLOR_ARGB(54, 240, 112, 32));
    UiRoundRing(x, y, w, h, 12, 1.0f, D3DCOLOR_ARGB(60, 255, 150, 70));
    UiRoundRect(right ? x + w - 11 : x + 7, y + h * 0.25f, 4, h * 0.5f, 2, UI_COL_ACCENT_HI, UI_COL_ACCENT);
}

static void selection_bar(float x, float y, float w, float h) { selection_bar_side(x, y, w, h, 0); }

static float paragraph(int font, float size, float x, float y, float w, int maxLines, D3DCOLOR color, const char *text)
{
    char buf[1024];
    int lines = UiTextWrap(font, size, w, maxLines, text, buf, sizeof(buf));
    float lineH = size * 1.35f;
    UiText(font, size, x, y + lineH * 0.5f, color, buf, UI_LEFT);
    return lines * lineH;
}

static void heading(float x, float cy, const char *text)
{
    UiText(UI_BOLD, 15, x, cy, UI_COL_ACCENT_HI, text, UI_LEFT);
}

void UiMenuClear(UiMenu *m, const char *title, const char *footer)
{
    memset(m, 0, sizeof(*m));
    strncpy(m->title, title, sizeof(m->title) - 1);
    strncpy(m->footer, footer, sizeof(m->footer) - 1);
}

int UiMenuAdd(UiMenu *m, int id, const char *label, const char *value)
{
    if (m->count >= UI_MENU_MAX) return -1;
    m->id[m->count] = id;
    strncpy(m->label[m->count], label, sizeof(m->label[0]) - 1);
    strncpy(m->value[m->count], value ? value : "", sizeof(m->value[0]) - 1);
    return m->count++;
}

int UiMenuSelected(const UiMenu *m)
{
    return m->count ? m->id[m->sel] : -1;
}

static float draw_value(float right, float cy, const char *value, int selected)
{
    size_t n = strlen(value);
    D3DCOLOR text = selected ? UI_COL_TEXT : UI_COL_TEXT2;
    if (n >= 4 && value[0] == '<' && value[1] == ' ' && value[n - 1] == '>' && value[n - 2] == ' ')
    {
        char inner[64];
        float cw = UiTextWidth(UI_BOLD, 30, UI_CH_RCHEVRON), x = right;
        D3DCOLOR chev = selected ? UI_COL_ACCENT_HI : UI_COL_TEXT3;
        size_t k = n - 4 < sizeof(inner) - 1 ? n - 4 : sizeof(inner) - 1;
        memcpy(inner, value + 2, k);
        inner[k] = 0;
        UiText(UI_BOLD, 30, x, cy - 1, chev, UI_CH_RCHEVRON, UI_RIGHT);
        x -= cw + 10;
        x -= UiText(selected ? UI_BOLD : UI_REGULAR, 21, x, cy, text, inner, UI_RIGHT);
        x -= 10;
        UiText(UI_BOLD, 30, x, cy - 1, chev, UI_CH_LCHEVRON, UI_RIGHT);
        return x - cw;
    }
    return right - UiText(UI_REGULAR, 21, right, cy, text, value, UI_RIGHT);
}

static float value_width(const char *value, int selected)
{
    size_t n = strlen(value);
    if (n >= 4 && value[0] == '<' && value[1] == ' ' && value[n - 1] == '>' && value[n - 2] == ' ')
    {
        char inner[64];
        size_t k = n - 4 < sizeof(inner) - 1 ? n - 4 : sizeof(inner) - 1;
        memcpy(inner, value + 2, k);
        inner[k] = 0;
        return 2 * (UiTextWidth(UI_BOLD, 30, UI_CH_RCHEVRON) + 10) + UiTextWidth(selected ? UI_BOLD : UI_REGULAR, 21, inner);
    }
    return UiTextWidth(UI_REGULAR, 21, value);
}

void UiMenuDraw(const UiMenu *m)
{
    static UiAnim sel;
    static char lastTitle[128];
    static int lastCount;
    const float w = 660, head = 100, foot = 78;
    float rowH = m->count > 0 && (UI_HEIGHT - 40 - head - 18 - foot) / m->count < 48 ? (UI_HEIGHT - 40 - head - 18 - foot) / m->count : 48;
    float h = head + m->count * rowH + 18 + foot, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f, sy;
    int i, restart = strcmp(lastTitle, m->title) != 0 || lastCount != m->count, rtl = LangRtl();
    strncpy(lastTitle, m->title, sizeof(lastTitle) - 1);
    lastCount = m->count;
    UiDim();
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 30, rtl ? x + w - 40 : x + 40, y + 50, UI_COL_TEXT, m->title, rtl ? UI_RIGHT : UI_LEFT);
    UiRoundRect(rtl ? x + w - 70 : x + 40, y + 74, 30, 4, 2, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    sy = anim_step(&sel, y + head + m->sel * rowH, restart);
    if (m->count) selection_bar_side(x + 18, sy + 3, w - 36, rowH - 6, rtl);
    for (i = 0; i < m->count; i++)
    {
        float cy = y + head + i * rowH + rowH * 0.5f;
        int s = i == m->sel;
        if (rtl)
        {
            UiText(s ? UI_BOLD : UI_REGULAR, 22, x + w - 44, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, m->label[i], UI_RIGHT);
            if (m->value[i][0]) draw_value(x + 40 + value_width(m->value[i], s), cy, m->value[i], s);
            continue;
        }
        UiText(s ? UI_BOLD : UI_REGULAR, 22, x + 44, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, m->label[i], UI_LEFT);
        if (m->value[i][0]) draw_value(x + w - 40, cy, m->value[i], s);
    }
    UiRect(x + 24, y + h - foot, w - 48, 1, UI_COL_LINE);
    UiFooter(rtl ? x + w - 40 - UiFooterWidth(m->footer) : x + 40, y + h - foot * 0.5f, m->footer);
}

void UiMenuNavigate(UiMenu *m, WORD down)
{
    if (!m->count) return;
    if (down & XINPUT_GAMEPAD_DPAD_UP) m->sel = (m->sel + m->count - 1) % m->count;
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) m->sel = (m->sel + 1) % m->count;
}

static void begin_frame(IDirect3DDevice9 *dev, UiBackgroundFn background, void *user)
{
    dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
    UiBegin(dev);
    if (background) background(user);
    else UiBackdrop();
}

static void end_frame(IDirect3DDevice9 *dev)
{
    UiEnd();
    dev->Present(NULL, NULL, NULL, NULL);
}

static char *read_text(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, got = 0;
    char *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(h, NULL);
    buf = (char *)malloc(size + 1);
    if (buf && !ReadFile(h, buf, size, &got, NULL)) got = 0;
    CloseHandle(h);
    if (buf) buf[got] = 0;
    return buf;
}

static int write_text(const char *path, const char *text, int len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    WriteFile(h, text, (DWORD)len, &put, NULL);
    CloseHandle(h);
    return put == (DWORD)len ? 0 : -1;
}

void SettingsPaths(const char *gameFolder, char *all, char *game, int size)
{
    char name[64];
    if (all) xb_fs_data_path(all, size, "config", "settings.ini");
    if (game)
    {
        _snprintf(name, sizeof(name), "%s.ini", gameFolder && gameFolder[0] ? gameFolder : "_");
        name[sizeof(name) - 1] = 0;
        xb_fs_data_path(game, size, "config", name);
    }
}

void SettingsLoad(Config *c, const char *path)
{
    char *text = read_text(path);
    if (!text) return;
    config_parse_text(c, text);
    free(text);
}

int SettingsSave(const Config *c, const char *path, int withLastGame)
{
    char text[1024];
    int n = config_write(c, withLastGame, text, sizeof(text));
    int r = write_text(path, text, n);
    if (r != 0) LOG_ERROR("[menu] cannot write %s", path);
    return r;
}

int SettingsSaveLang(const char *path, const char *code)
{
    char *old = read_text(path), *out, *p;
    int n = 0, r, size = (old ? (int)strlen(old) : 0) + 64;
    out = (char *)malloc(size);
    if (!out)
    {
        free(old);
        return 0;
    }
    for (p = old; p && *p;)
    {
        char *e = p + strcspn(p, "\r\n");
        int len = (int)(e - p);
        if (len && _strnicmp(p, "lang=", 5) != 0)
        {
            memcpy(out + n, p, len);
            n += len;
            out[n++] = '\r';
            out[n++] = '\n';
        }
        p = e + strspn(e, "\r\n");
    }
    n += _snprintf(out + n, size - n, "lang=%s\r\n", code);
    r = write_text(path, out, n);
    free(out);
    free(old);
    if (r != 0) LOG_ERROR("[menu] cannot write %s", path);
    return r == 0;
}

void SettingsApplyLanguage(const Config *c)
{
    int l = c->uiLang[0] ? LangFromCode(c->uiLang) : c->lang[0] ? LangFromCode(c->lang) : -1;
    LangSet(l >= 0 ? l : LangFromConsole());
}

enum
{
    S_CPU = 1, S_RENDERER, S_FPS, S_GRAPHICS, S_CONTROLS, S_LANGUAGE, S_ART_DL, S_ART_ALL, S_SAVE_ALL, S_SAVE_GAME, S_BACK,
    G_PRESET = 100, G_FORMAT, G_ZOOM, G_POSX, G_POSY, G_BRIGHT, G_VSYNC, G_TEXFILTER, G_SMOOTH, G_FXAA, G_SHARPEN, G_BLUR, G_SCREEN, G_BACK
};

static const char *chev(int id)
{
    static char bufs[8][64];
    static int next;
    char *b = bufs[next];
    next = (next + 1) & 7;
    _snprintf(b, 64, "< %s >", Tr(id));
    b[63] = 0;
    return b;
}

static const char *onoff(int v) { return chev(v ? T_ON : T_OFF); }

static const char *preset_name(int p)
{
    static const int ids[DISPLAY_PRESETS] = { T_PRESET_ORIGINAL, T_PRESET_ENHANCED, T_PRESET_SMOOTH, T_PRESET_CRT };
    return Tr(p >= 0 && p < DISPLAY_PRESETS ? ids[p] : T_CUSTOM);
}

static const char *source_name(int src)
{
    if (src == PADSRC_NONE) return Tr(T_SRC_NONE);
    if (src == PADSRC_LS) return Tr(T_SRC_LS);
    if (src == PADSRC_RS) return Tr(T_SRC_RS);
    if (src >= PADSRC_DUP && src <= PADSRC_DRIGHT) return Tr(T_SRC_DUP + (src - PADSRC_DUP));
    return config_pad_source_name(src);
}

static const char *input_name(int in)
{
    if (in == PADIN_START) return Tr(T_DC_START);
    if (in == PADIN_L) return Tr(T_DC_LTRIG);
    if (in == PADIN_R) return Tr(T_DC_RTRIG);
    if (in >= PADIN_UP && in <= PADIN_RIGHT) return Tr(T_DPAD_UP + (in - PADIN_UP));
    return config_pad_input_name(in);
}

static const char *arrows(char *buf, int size, const char *fmt, int v)
{
    char inner[32];
    _snprintf(inner, sizeof(inner), fmt, v);
    inner[sizeof(inner) - 1] = 0;
    _snprintf(buf, size, "< %s >", inner);
    buf[size - 1] = 0;
    return buf;
}

static int s_fromList, s_artAll;

static void settings_build(UiMenu *m, const Config *c, int perGame, int sel)
{
    char v[64];
    UiMenuClear(m, Tr(T_SETTINGS), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, S_CPU, Tr(T_CPU_NEXT), chev(c->useJit ? T_RECOMPILER : T_INTERPRETER));
    UiMenuAdd(m, S_RENDERER, Tr(T_RENDERER_NEXT), chev(c->gpuRender ? T_GPU : T_SOFTWARE));
    {
        static const int fpsLabels[4] = { T_OFF, T_FPS_ONLY, T_FPS_CPU, T_FPS_ALL };
        UiMenuAdd(m, S_FPS, Tr(T_SHOW_FPS), chev(fpsLabels[c->showFps & 3]));
    }
    _snprintf(v, sizeof(v), LangRtl() ? "<  %s" : "%s  >", preset_name(xb_display_preset_of(c)));
    v[sizeof(v) - 1] = 0;
    UiMenuAdd(m, S_GRAPHICS, Tr(T_GRAPHICS), v);
    {
        u32 def[PADIN_COUNT];
        int stick, in, p, custom = 0;
        config_pad_defaults(def, &stick);
        for (p = 0; p < PAD_PLAYERS; p++)
        {
            if (stick != c->padStick[p]) custom = 1;
            for (in = 0; in < PADIN_COUNT; in++)
                if (def[in] != c->padMap[p][in]) custom = 1;
        }
        _snprintf(v, sizeof(v), LangRtl() ? "<  %s" : "%s  >", Tr(custom ? T_CUSTOM : T_DEFAULT));
        v[sizeof(v) - 1] = 0;
        UiMenuAdd(m, S_CONTROLS, Tr(T_CONTROLS), v);
    }
    _snprintf(v, sizeof(v), LangRtl() ? "<  %s" : "%s  >", Tr(T_LANG_NAME));
    v[sizeof(v) - 1] = 0;
    UiMenuAdd(m, S_LANGUAGE, Tr(T_LANGUAGE), v);
    if (s_fromList)
    {
        UiMenuAdd(m, S_ART_DL, Tr(T_ART_DOWNLOADS), onoff(c->artDownload));
        UiMenuAdd(m, S_ART_ALL, Tr(T_ART_ALL), NULL);
    }
    UiMenuAdd(m, S_SAVE_ALL, Tr(T_SAVE_ALL), NULL);
    if (perGame) UiMenuAdd(m, S_SAVE_GAME, Tr(T_SAVE_GAME), NULL);
    UiMenuAdd(m, S_BACK, Tr(T_BACK), NULL);
    m->sel = sel < m->count ? sel : 0;
}

static int format_of(const Config *c)
{
    if (c->widescreen && c->gpuRender) return 1;
    return c->aspect || c->widescreen ? 2 : 0;
}

static void format_set(Config *c, int f)
{
    c->widescreen = f == 1;
    c->aspect = f == 2;
}

static void graphics_build(UiMenu *m, const Config *c, int sel)
{
    static const int levels[3] = { T_OFF, T_LOW, T_HIGH };
    static const int blurs[3] = { T_OFF, T_SOFT, T_STRONG };
    static const int bright[3] = { T_NORMAL, T_LIGHTER, T_MUCH_LIGHTER };
    static const int screens[6] = { T_SCREEN_NONE, T_SCANLINES, T_CRT, T_CRT_CURVED, T_LCD_GRID, T_LIGHT_SCANLINES };
    char v[32], pr[72];
    UiMenuClear(m, Tr(T_GRAPHICS), Tr(T_FOOT_SELECT));
    _snprintf(pr, sizeof(pr), "< %s >", preset_name(xb_display_preset_of(c)));
    pr[sizeof(pr) - 1] = 0;
    UiMenuAdd(m, G_PRESET, Tr(T_PRESET), pr);
    {
        static const int formats[3] = { T_ASPECT_43, T_ASPECT_WIDE, T_ASPECT_STRETCH };
        UiMenuAdd(m, G_FORMAT, Tr(T_ASPECT), chev(formats[format_of(c)]));
    }
    UiMenuAdd(m, G_ZOOM, Tr(T_PIC_SIZE), arrows(v, sizeof(v), "%d%%", c->zoom));
    UiMenuAdd(m, G_POSX, Tr(T_PIC_LR), arrows(v, sizeof(v), "%d", c->posX));
    UiMenuAdd(m, G_POSY, Tr(T_PIC_UD), arrows(v, sizeof(v), "%d", c->posY));
    UiMenuAdd(m, G_BRIGHT, Tr(T_BRIGHTNESS), chev(bright[c->brightness >= 0 && c->brightness <= 2 ? c->brightness : 0]));
    UiMenuAdd(m, G_VSYNC, Tr(T_VSYNC), c->vsync == 2 ? chev(T_VSYNC_STRICT) : onoff(c->vsync));
    if (c->gpuRender)
    {
        static const int filters[3] = { T_FILTER_GAME, T_FILTER_NEAREST, T_FILTER_BILINEAR };
        UiMenuAdd(m, G_TEXFILTER, Tr(T_TEXFILTER), chev(filters[c->texFilter % 3]));
    }
    UiMenuAdd(m, G_SMOOTH, Tr(T_SMOOTH_SCALING), onoff(c->smooth));
    UiMenuAdd(m, G_FXAA, Tr(T_FXAA), onoff(c->fxaa));
    UiMenuAdd(m, G_SHARPEN, Tr(T_SHARPEN), chev(levels[c->sharpen <= 2 ? c->sharpen : 0]));
    UiMenuAdd(m, G_BLUR, Tr(T_BLUR), chev(blurs[c->blur <= 2 ? c->blur : 0]));
    UiMenuAdd(m, G_SCREEN, Tr(T_SCREEN_EFFECT), chev(screens[c->screen >= 0 && c->screen <= 5 ? c->screen : 0]));
    UiMenuAdd(m, G_BACK, Tr(T_BACK), NULL);
    m->sel = sel < m->count ? sel : 0;
}

enum { C_STICK = PADIN_COUNT, C_DEAD, C_RANGE, C_INVERT, C_RUMBLE, C_RESET, C_BACK, C_COUNT };
#define CAPTURE_MS 5000
#define CTL_X 70.0f
#define CTL_Y 40.0f
#define CTL_W 1140.0f
#define CTL_H 640.0f
#define CTL_ROW 34.0f
#define CTL_TOP 124.0f
#define CTL_LISTW 520.0f

struct ControlsState
{
    Config *c;
    int player;
    int capturing;
    int capItem, capAdd, capReleased;
    DWORD capStart;
    char msg[192];
    DWORD msgAt;
};
static ControlsState s_ctl;

static const char *ctl_item_name(int item)
{
    static const int labels[] = { T_ANALOG_STICK, T_DEAD_ZONE, T_SENSITIVITY, T_INVERT, T_RUMBLE, T_DEFAULT_CONTROLS, T_BACK };
    if (item >= PADIN_UP && item < PADIN_COUNT) return input_name(item);
    if (item < PADIN_COUNT) return Trf(T_DC_BUTTON, input_name(item));
    return Tr(labels[item - C_STICK]);
}

#define CTL_AREA_X (CTL_X + 24 + CTL_LISTW + 40)
#define CTL_AREA_W (CTL_X + CTL_W - 40 - CTL_AREA_X)
#define CTL_PAD_S (CTL_AREA_W * 0.44f / 100.0f)
#define CTL_OPT_ROW 31.0f
#define CTL_OPT_TOP (CTL_Y + CTL_TOP + 78 * CTL_PAD_S + 44)

static void ctl_place(int item, float *x, float *y, float *w)
{
    if (item < PADIN_COUNT)
    {
        *x = CTL_X + 24;
        *w = CTL_LISTW;
        *y = CTL_Y + CTL_TOP + item * CTL_ROW + (item >= PADIN_UP ? 40 : 0);
    }
    else
    {
        *x = CTL_AREA_X;
        *w = CTL_AREA_W;
        *y = CTL_OPT_TOP + (item - C_STICK) * CTL_OPT_ROW;
    }
}

static void ctl_note(const char *text)
{
    strncpy(s_ctl.msg, text, sizeof(s_ctl.msg) - 1);
    s_ctl.msg[sizeof(s_ctl.msg) - 1] = 0;
    s_ctl.msgAt = GetTickCount();
}

static int controls_capture(void)
{
    XINPUT_STATE st;
    u32 src = 0, *map = s_ctl.c->padMap[s_ctl.player];
    int i;
    ZeroMemory(&st, sizeof(st));
    if (XbPadGet(0, &st) == ERROR_SUCCESS)
    {
        if (st.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)
        {
            s_ctl.capturing = 0;
            ctl_note(Tr(T_CANCELLED));
            return 0;
        }
        src = XbPadPressed(&st.Gamepad);
    }
    if (!s_ctl.capReleased)
    {
        if (!src)
        {
            s_ctl.capReleased = 1;
            s_ctl.capStart = GetTickCount();
        }
        return 0;
    }
    if (GetTickCount() - s_ctl.capStart >= CAPTURE_MS)
    {
        s_ctl.capturing = 0;
        ctl_note(Tr(T_NO_INPUT));
        return 0;
    }
    if (!src) return 0;
    for (i = 1; i < PADSRC_COUNT; i++)
        if (src & (1u << i)) break;
    if (s_ctl.capAdd) map[s_ctl.capItem] |= 1u << i;
    else map[s_ctl.capItem] = 1u << i;
    s_ctl.capturing = 0;
    ctl_note(Trf(s_ctl.capAdd ? T_INPUT_ADDED : T_INPUT_SET, ctl_item_name(s_ctl.capItem), source_name(i)));
    return 1;
}

static void ctl_hints(u32 mask, char *out, size_t size)
{
    static const char *const glyph[PADSRC_COUNT] = { "", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "LS", "RS",
                                                     "DPAD", "DPAD", "DPAD", "DPAD" };
    int pass, i;
    out[0] = 0;
    for (pass = 0; pass < 2; pass++)
        for (i = 1; i < PADSRC_COUNT; i++)
        {
            char item[64];
            size_t len = strlen(out);
            int click = i == PADSRC_LS || i == PADSRC_RS || (i >= PADSRC_DUP && i <= PADSRC_DRIGHT);
            if (!(mask & (1u << i)) || click != pass) continue;
            if (click) _snprintf(item, sizeof(item) - 1, "%s:%s", glyph[i], Tr(i >= PADSRC_DUP ? T_DIR_UP + (i - PADSRC_DUP) : T_CLICK));
            else strcpy(item, glyph[i]);
            item[sizeof(item) - 1] = 0;
            if (len + strlen(item) + 2 > size) return;
            if (len) strcat(out, pass ? "|" : "/");
            strcat(out, item);
        }
}

static D3DCOLOR pad_rgb(int r, int g, int b) { return D3DCOLOR_ARGB(255, r, g, b); }

static int dc_shape(int item, float *x, float *y, float *w, float *h, float *r)
{
    static const float circles[][4] = {
        { PADIN_A, 78.0f, 33.0f, 3.7f }, { PADIN_B, 85.0f, 26.0f, 3.7f }, { PADIN_X, 71.0f, 26.0f, 3.7f },
        { PADIN_Y, 78.0f, 19.0f, 3.7f }, { PADIN_START, 50.0f, 45.0f, 2.8f }, { C_STICK, 24.0f, 21.0f, 7.5f },
        { C_DEAD, 24.0f, 21.0f, 7.5f }, { C_RANGE, 24.0f, 21.0f, 7.5f }, { C_INVERT, 24.0f, 21.0f, 7.5f },
    };
    static const float boxes[][6] = {
        { PADIN_L, 12.0f, -6.0f, 24.0f, 11.0f, 3.0f }, { PADIN_R, 64.0f, -6.0f, 24.0f, 11.0f, 3.0f },
        { PADIN_UP, 21.8f, 37.5f, 4.4f, 4.6f, 1.0f }, { PADIN_DOWN, 21.8f, 45.9f, 4.4f, 4.6f, 1.0f },
        { PADIN_LEFT, 17.5f, 41.8f, 4.6f, 4.4f, 1.0f }, { PADIN_RIGHT, 25.9f, 41.8f, 4.6f, 4.4f, 1.0f },
    };
    int i;
    for (i = 0; i < (int)(sizeof(circles) / sizeof(circles[0])); i++)
        if ((int)circles[i][0] == item)
        {
            *x = circles[i][1] - circles[i][3];
            *y = circles[i][2] - circles[i][3];
            *w = *h = circles[i][3] * 2;
            *r = circles[i][3];
            return 1;
        }
    for (i = 0; i < (int)(sizeof(boxes) / sizeof(boxes[0])); i++)
        if ((int)boxes[i][0] == item)
        {
            *x = boxes[i][1];
            *y = boxes[i][2];
            *w = boxes[i][3];
            *h = boxes[i][4];
            *r = boxes[i][5];
            return 1;
        }
    return 0;
}

static void draw_dc_pad(float ox, float oy, float s, int hiItem, int blink)
{
    const D3DCOLOR shell = pad_rgb(232, 232, 236), shell2 = pad_rgb(196, 197, 205), dark = pad_rgb(62, 64, 72);
    static const struct { int item; int r, g, b; const char *name; } face[4] = {
        { PADIN_A, 214, 52, 48, "A" }, { PADIN_B, 52, 98, 214, "B" }, { PADIN_X, 236, 190, 40, "X" }, { PADIN_Y, 44, 160, 76, "Y" },
    };
    float x, y, w, h, r;
    int i;
#define PU(v) ((v) * s)
    UiRoundRect(ox + PU(12), oy + PU(-6), PU(24), PU(13), PU(3), pad_rgb(150, 152, 162), pad_rgb(120, 122, 132));
    UiRoundRect(ox + PU(64), oy + PU(-6), PU(24), PU(13), PU(3), pad_rgb(150, 152, 162), pad_rgb(120, 122, 132));
    UiText(UI_BOLD, PU(4.2f), ox + PU(24), oy + PU(-0.6f), pad_rgb(60, 62, 70), "L", UI_CENTER);
    UiText(UI_BOLD, PU(4.2f), ox + PU(76), oy + PU(-0.6f), pad_rgb(60, 62, 70), "R", UI_CENTER);
    UiShadow(ox + PU(2), oy + PU(6), PU(96), PU(64), PU(16), 22, UI_COL_SHADOW);
    UiRoundRect(ox + PU(5), oy + PU(28), PU(32), PU(44), PU(16), shell, shell2);
    UiRoundRect(ox + PU(63), oy + PU(28), PU(32), PU(44), PU(16), shell, shell2);
    UiRoundRect(ox + PU(26), oy + PU(28), PU(48), PU(30), PU(12), shell, shell2);
    UiRoundRect(ox + PU(2), oy + PU(5), PU(96), PU(42), PU(18), shell, pad_rgb(214, 214, 220));
    UiRoundRect(ox + PU(39), oy + PU(9), PU(22), PU(25), PU(3), pad_rgb(88, 90, 100), pad_rgb(70, 72, 82));
    UiRoundRect(ox + PU(42), oy + PU(12), PU(16), PU(11), PU(1.5f), pad_rgb(120, 140, 120), pad_rgb(96, 116, 98));
    UiCircle(ox + PU(24), oy + PU(21), PU(7.5f), pad_rgb(150, 152, 160));
    UiCircle(ox + PU(24), oy + PU(21), PU(5.5f), dark);
    UiCircle(ox + PU(24), oy + PU(21), PU(4.0f), pad_rgb(84, 86, 96));
    UiRoundRect(ox + PU(17.5f), oy + PU(41.8f), PU(13), PU(4.4f), PU(1), dark, dark);
    UiRoundRect(ox + PU(21.8f), oy + PU(37.5f), PU(4.4f), PU(13), PU(1), dark, dark);
    UiCircle(ox + PU(50), oy + PU(45), PU(2.8f), dark);
    for (i = 0; i < 4; i++)
        if (dc_shape(face[i].item, &x, &y, &w, &h, &r))
        {
            UiCircle(ox + PU(x + r), oy + PU(y + r), PU(r), pad_rgb(face[i].r, face[i].g, face[i].b));
            UiText(UI_BOLD, PU(4.0f), ox + PU(x + r), oy + PU(y + r), UI_COL_TEXT, face[i].name, UI_CENTER);
        }
    if (dc_shape(hiItem, &x, &y, &w, &h, &r))
    {
        float on = blink ? 0.5f + 0.5f * sinf(UiTime() * 12.566f) : 1.0f, pad = 3.0f;
        int a = (int)(255 * on);
        x = ox + PU(x);
        y = oy + PU(y);
        w = PU(w);
        h = PU(h);
        r = PU(r);
        if (blink)
        {
            UiShadow(x - 2, y - 2, w + 4, h + 4, r + 2, 16, D3DCOLOR_ARGB((int)(200 * on), 255, 150, 70));
            UiRoundRect(x, y, w, h, r, D3DCOLOR_ARGB((int)(190 * on), 255, 150, 70), D3DCOLOR_ARGB((int)(190 * on), 240, 112, 32));
        }
        UiRoundRing(x - pad, y - pad, w + pad * 2, h + pad * 2, r + pad, 2.5f, D3DCOLOR_ARGB(a, 255, 150, 70));
    }
#undef PU
}

static void controls_draw(int sel)
{
    static UiAnim selX, selY;
    const float x = CTL_X, y = CTL_Y, rowX = x + 24, foot = y + CTL_H - 78;
    const u32 *map = s_ctl.c->padMap[s_ctl.player];
    int i;
    char buf[320];

    UiDim();
    UiPanel(x, y, CTL_W, CTL_H, 22);
    UiText(UI_BOLD, 30, x + 40, y + 50, UI_COL_TEXT, Tr(T_CONTROLS), UI_LEFT);
    UiRoundRect(x + 40, y + 74, 30, 4, 2, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    {
        float r = x + CTL_W - 40, w;
        _snprintf(buf, sizeof(buf) - 1, Tr(T_PLAYER), s_ctl.player + 1);
        buf[sizeof(buf) - 1] = 0;
        r -= UiFooterWidth("RB");
        UiFooter(r, y + 50, "RB");
        w = UiTextWidth(UI_BOLD, 22, buf);
        UiText(UI_BOLD, 22, r - 16, y + 50, UI_COL_TEXT, buf, UI_RIGHT);
        r -= 16 + w + 16 + UiFooterWidth("LB");
        UiFooter(r, y + 50, "LB");
        if (s_ctl.player)
            UiText(UI_REGULAR, 16, x + CTL_W - 40, y + 82, UI_COL_TEXT3, Trf(T_USED_WHEN, s_ctl.player + 1), UI_RIGHT);
    }

    heading(rowX + 20, y + CTL_TOP - 14, Tr(T_HEAD_BUTTONS));
    {
        float hx, hy, hw;
        ctl_place(PADIN_UP, &hx, &hy, &hw);
        heading(hx + 20, hy - 14, Tr(T_HEAD_DPAD));
        ctl_place(C_STICK, &hx, &hy, &hw);
        heading(hx + 20, hy - 14, Tr(T_HEAD_MORE));
    }
    {
        static int lastRight = -1;
        float bx, by, bw;
        int right = sel >= PADIN_COUNT;
        ctl_place(sel, &bx, &by, &bw);
        selection_bar(anim_step(&selX, bx, right != lastRight), anim_step(&selY, by, right != lastRight) + 2, bw, CTL_ROW - 4);
        lastRight = right;
    }
    for (i = 0; i < C_COUNT; i++)
    {
        int s = i == sel;
        float rx, ry, rw, cy, right;
        ctl_place(i, &rx, &ry, &rw);
        cy = ry + CTL_ROW * 0.5f;
        right = rx + rw - 18;
        UiText(s ? UI_BOLD : UI_REGULAR, 20, rx + 24, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, ctl_item_name(i), UI_LEFT);
        if (i < PADIN_COUNT)
        {
            if (s_ctl.capturing && i == s_ctl.capItem)
                UiText(UI_BOLD, 18, right, cy, UI_COL_ACCENT_HI, Tr(T_PRESS_INPUT), UI_RIGHT);
            else
            {
                ctl_hints(map[i], buf, sizeof(buf));
                if (!buf[0]) UiText(UI_REGULAR, 18, right, cy, UI_COL_TEXT3, Tr(T_NO_INPUT_SET), UI_RIGHT);
                else
                {
                    float w = UiFooterWidth(buf);
                    if (w <= CTL_LISTW - 230) UiFooter(right - w, cy, buf);
                    else
                    {
                        int k, cnt = 0;
                        for (k = 1; k < PADSRC_COUNT; k++) cnt += (map[i] >> k) & 1;
                        UiText(UI_REGULAR, 18, right, cy, UI_COL_TEXT2, Trf(T_N_INPUTS, cnt), UI_RIGHT);
                    }
                }
            }
        }
        else if (i == C_STICK)
            draw_value(right, cy, chev(s_ctl.c->padStick[s_ctl.player] ? T_STICK_RIGHT : T_STICK_LEFT), s);
        else if (i == C_RUMBLE)
            draw_value(right, cy, onoff(s_ctl.c->rumble), s);
        else if (i == C_DEAD || i == C_RANGE)
        {
            char v[32];
            draw_value(right, cy, arrows(v, sizeof(v), "%d%%", i == C_DEAD ? s_ctl.c->padDead[s_ctl.player] : s_ctl.c->padRange[s_ctl.player]), s);
        }
        else if (i == C_INVERT)
        {
            static const char *axes[4] = { NULL, "X", "Y", NULL };
            int inv = s_ctl.c->padInvert[s_ctl.player] & 3;
            char v[64];
            _snprintf(v, sizeof(v), "< %s >", inv == 0 ? Tr(T_OFF) : inv == 3 ? Tr(T_X_AND_Y) : axes[inv]);
            v[sizeof(v) - 1] = 0;
            draw_value(right, cy, v, s);
        }
    }

    {
        const float s = CTL_PAD_S, padY = y + CTL_TOP + 6 * s, padX = CTL_AREA_X + (CTL_AREA_W - 100 * s) * 0.5f;
        draw_dc_pad(padX, padY, s, s_ctl.capturing ? s_ctl.capItem : sel, s_ctl.capturing);
    }

    UiRect(x + 24, foot, CTL_W - 48, 1, UI_COL_LINE);
    if (s_ctl.capturing)
    {
        DWORD age = s_ctl.capReleased ? GetTickCount() - s_ctl.capStart : 0;
        float left = age >= CAPTURE_MS ? 0.0f : (CAPTURE_MS - age) / (float)CAPTURE_MS, bw = 220, cancelW;
        const char *colon = strchr(Tr(T_FOOT_CANCEL), ':');
        char cancel[64];
        _snprintf(cancel, sizeof(cancel) - 1, "BACK:%s", colon ? colon + 1 : "");
        cancel[sizeof(cancel) - 1] = 0;
        cancelW = UiFooterWidth(cancel);
        UiText(UI_BOLD, 20, x + 40, foot + 39, UI_COL_TEXT, Trf(T_PRESS_FOR, ctl_item_name(s_ctl.capItem)), UI_LEFT);
        UiFooter(x + CTL_W - 40 - cancelW, foot + 39, cancel);
        UiRoundRect(x + CTL_W - 80 - cancelW - bw, foot + 36, bw, 6, 3, D3DCOLOR_ARGB(30, 255, 255, 255), D3DCOLOR_ARGB(30, 255, 255, 255));
        if (left > 0) UiRoundRect(x + CTL_W - 80 - cancelW - bw, foot + 36, bw * left, 6, 3, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    }
    else
    {
        DWORD age = GetTickCount() - s_ctl.msgAt;
        UiFooter(x + 40, foot + 39, Tr(sel < PADIN_COUNT ? T_FOOT_CTL_BUTTON : T_FOOT_CTL_OTHER));
        if (s_ctl.msg[0] && age < 4000) UiText(UI_REGULAR, 18, x + CTL_W - 40, foot + 39, UI_COL_ACCENT_HI, s_ctl.msg, UI_RIGHT);
    }
}

static int controls_page(IDirect3DDevice9 *dev, Config *c, UiBackgroundFn background, void *user)
{
    UiInput in;
    int sel = 0, changed = 0;
    s_ctl.c = c;
    s_ctl.capturing = 0;
    s_ctl.msg[0] = 0;
    if (s_ctl.player < 0 || s_ctl.player >= PAD_PLAYERS) s_ctl.player = 0;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        u32 *map = c->padMap[s_ctl.player];
        if (s_ctl.capturing)
            changed |= controls_capture();
        else
        {
            int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
            if (down & XINPUT_GAMEPAD_DPAD_UP) sel = (sel + C_COUNT - 1) % C_COUNT;
            if (down & XINPUT_GAMEPAD_DPAD_DOWN) sel = (sel + 1) % C_COUNT;
            if ((down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) || (sel == C_BACK && (down & XINPUT_GAMEPAD_A))) break;
            if (down & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))
                s_ctl.player = (s_ctl.player + ((down & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : PAD_PLAYERS - 1)) % PAD_PLAYERS;
            else if (sel < PADIN_COUNT)
            {
                if (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_Y))
                {
                    s_ctl.capturing = 1;
                    s_ctl.capItem = sel;
                    s_ctl.capAdd = (down & XINPUT_GAMEPAD_A) ? 0 : 1;
                    s_ctl.capReleased = 0;
                    s_ctl.capStart = GetTickCount();
                }
                else if (down & XINPUT_GAMEPAD_X)
                {
                    map[sel] = 0;
                    changed = 1;
                }
            }
            else if (sel == C_STICK && (dir || (down & XINPUT_GAMEPAD_A)))
            {
                c->padStick[s_ctl.player] = !c->padStick[s_ctl.player];
                changed = 1;
            }
            else if ((sel == C_DEAD || sel == C_RANGE || sel == C_INVERT) && (dir || (down & XINPUT_GAMEPAD_A)))
            {
                int p = s_ctl.player, d = dir ? dir : 1;
                if (sel == C_DEAD) c->padDead[p] = max(0, min(60, c->padDead[p] + d * 2));
                else if (sel == C_RANGE) c->padRange[p] = max(50, min(150, c->padRange[p] + d * 5));
                else c->padInvert[p] = (c->padInvert[p] + d + 4) & 3;
                changed = 1;
            }
            else if (sel == C_RUMBLE && (dir || (down & XINPUT_GAMEPAD_A)))
            {
                c->rumble = !c->rumble;
                changed = 1;
            }
            else if (sel == C_RESET && (down & XINPUT_GAMEPAD_A))
            {
                config_pad_defaults(map, &c->padStick[s_ctl.player]);
                c->padDead[s_ctl.player] = 24;
                c->padRange[s_ctl.player] = 100;
                c->padInvert[s_ctl.player] = 0;
                ctl_note(Tr(T_DEFAULT_CONTROLS));
                changed = 1;
            }
        }
        begin_frame(dev, background, user);
        controls_draw(sel);
        end_frame(dev);
    }
    return changed;
}

static int cycle(int v, int n, int dir) { return (v + n + dir) % n; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int settings_change(Config *c, int id, int dir)
{
    switch (id)
    {
    case S_CPU: c->useJit = !c->useJit; return 1;
    case S_RENDERER:
        c->gpuRender = !c->gpuRender;
        if (!c->gpuRender && c->widescreen) format_set(c, 2);
        return 1;
    case S_FPS: c->showFps = cycle(c->showFps, 4, dir); return 1;
    case S_ART_DL: c->artDownload = !c->artDownload; return 1;
    case G_PRESET:
    {
        int p = xb_display_preset_of(c);
        xb_display_preset(c, p < 0 ? 0 : cycle(p, DISPLAY_PRESETS, dir));
        return 1;
    }
    case G_FORMAT:
    {
        int f = cycle(format_of(c), 3, dir);
        if (f == 1 && !c->gpuRender) f = cycle(f, 3, dir);
        format_set(c, f);
        return 1;
    }
    case G_VSYNC: c->vsync = cycle(c->vsync, 3, dir); return 1;
    case G_TEXFILTER: c->texFilter = cycle(c->texFilter, 3, dir); return 1;
    case G_ZOOM: c->zoom = clampi(c->zoom + dir * 2, 80, 110); return 1;
    case G_POSX: c->posX = clampi(c->posX + dir * 4, -96, 96); return 1;
    case G_POSY: c->posY = clampi(c->posY + dir * 4, -64, 64); return 1;
    case G_BRIGHT: c->brightness = cycle(c->brightness, 3, dir); return 1;
    case G_SMOOTH: c->smooth = !c->smooth; return 1;
    case G_FXAA: c->fxaa = !c->fxaa; return 1;
    case G_SHARPEN: c->sharpen = cycle(c->sharpen, 3, dir); if (c->sharpen) c->blur = 0; return 1;
    case G_BLUR: c->blur = cycle(c->blur, 3, dir); if (c->blur) c->sharpen = 0; return 1;
    case G_SCREEN: c->screen = cycle(c->screen, 6, dir); return 1;
    }
    return 0;
}

int SettingsScreen(IDirect3DDevice9 *dev, Config *c, const char *gameFolder, UiBackgroundFn background, void *user)
{
    UiMenu m;
    UiInput in;
    char all[CONFIG_PATH_MAX], game[CONFIG_PATH_MAX], note[192];
    int changed = 0, perGame = gameFolder && gameFolder[0], page = 0, mainSel = 0;
    DWORD noteAt = 0;
    note[0] = 0;
    SettingsPaths(gameFolder, all, game, sizeof(all));
    settings_build(&m, c, perGame, 0);
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        int id = UiMenuSelected(&m);
        UiMenuNavigate(&m, down);
        if (page && ((down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) || ((down & XINPUT_GAMEPAD_A) && id == G_BACK)))
        {
            page = 0;
            settings_build(&m, c, perGame, mainSel);
            continue;
        }
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) break;
        if ((down & XINPUT_GAMEPAD_A) && id == S_GRAPHICS)
        {
            page = 1;
            mainSel = m.sel;
            graphics_build(&m, c, 0);
            continue;
        }
        if ((down & XINPUT_GAMEPAD_A) && id == S_CONTROLS)
        {
            if (controls_page(dev, c, background, user)) changed = 1;
            settings_build(&m, c, perGame, m.sel);
            UiInputInit(&in);
            continue;
        }
        if ((down & XINPUT_GAMEPAD_A) && id == S_LANGUAGE)
        {
            int lang = LanguageScreen(dev, LangGet(), 0);
            if (lang >= 0)
            {
                LangSet(lang);
                strncpy(c->lang, LangCode(lang), sizeof(c->lang) - 1);
                c->lang[sizeof(c->lang) - 1] = 0;
                SettingsSaveLang(all, c->lang);
            }
            settings_build(&m, c, perGame, m.sel);
            UiInputInit(&in);
            continue;
        }
        if ((down & XINPUT_GAMEPAD_A) && id == S_ART_ALL)
        {
            s_artAll = 1;
            break;
        }
        if (down & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_A))
        {
            int dir = (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 1;
            if (settings_change(c, id, dir)) changed = 1;
            else if ((down & XINPUT_GAMEPAD_A) && id == S_BACK) break;
            else if ((down & XINPUT_GAMEPAD_A) && (id == S_SAVE_ALL || id == S_SAVE_GAME))
            {
                int ok = SettingsSave(c, id == S_SAVE_ALL ? all : game, id == S_SAVE_ALL) == 0;
                strncpy(note, Tr(ok ? (id == S_SAVE_ALL ? T_SAVED_ALL : T_SAVED_GAME) : T_SETTINGS_WRITE_FAILED), sizeof(note) - 1);
                note[sizeof(note) - 1] = 0;
                noteAt = GetTickCount();
            }
            if (page == 1) graphics_build(&m, c, m.sel);
            else settings_build(&m, c, perGame, m.sel);
        }
        begin_frame(dev, background, user);
        UiMenuDraw(&m);
        if (note[0])
        {
            DWORD age = GetTickCount() - noteAt;
            if (age < 2500) UiToast(note, (float)age, 2500.0f);
            else note[0] = 0;
        }
        end_frame(dev);
    }
    return changed;
}

void AboutDraw(void)
{
    const float x = 150, y = 46, w = 980, h = 628, cx = x + 52, col2 = x + 560;
    float ty;
    UiDim();
    UiPanel(x, y, w, h, 24);
    UiBrand(cx, y + 72, 44);
    UiText(UI_REGULAR, 18, x + w - 52, y + 62, UI_COL_TEXT2, Trf(T_ABOUT_VERSION, EMU_VERSION), UI_RIGHT);
    UiText(UI_REGULAR, 16, x + w - 52, y + 88, UI_COL_TEXT3, Trf(T_ABOUT_BUILT, __DATE__), UI_RIGHT);
    UiText(UI_REGULAR, 24, cx, y + 146, UI_COL_TEXT, Tr(T_ABOUT_TAGLINE), UI_LEFT);
    {
        float a = UiText(UI_REGULAR, 20, cx, y + 184, UI_COL_TEXT2, Tr(T_ABOUT_CREATED), UI_LEFT);
        UiText(UI_BOLD, 20, cx + a + 6, y + 184, UI_COL_TEXT, "Mohamed Aymen (kernel64)", UI_LEFT);
    }
    UiRect(x + 32, y + 246, w - 64, 1, UI_COL_LINE);
    ty = y + 280;
    heading(cx, ty, Tr(T_HEAD_ENGINE));
    paragraph(UI_REGULAR, 19, cx, ty + 16, 460, 9, UI_COL_TEXT2,
              Tr(T_ENGINE_TEXT));
    heading(col2, ty, Tr(T_HEAD_THANKS));
    paragraph(UI_REGULAR, 19, col2, ty + 16, 368, 9, UI_COL_TEXT2,
              Tr(T_THANKS_TEXT));
    UiRect(x + 32, y + h - 72, w - 64, 1, UI_COL_LINE);
    UiFooter(cx, y + h - 36, Tr(T_FOOT_BACK));
}

void AboutScreen(IDirect3DDevice9 *dev, UiBackgroundFn background, void *user)
{
    UiInput in;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_BACK)) return;
        begin_frame(dev, background, user);
        AboutDraw();
        end_frame(dev);
    }
}

int ConfirmScreen(IDirect3DDevice9 *dev, const char *question, const char *yes, UiBackgroundFn background, void *user)
{
    UiInput in;
    char hints[128];
    _snprintf(hints, sizeof(hints), Tr(T_FOOT_CONFIRM), yes);
    hints[sizeof(hints) - 1] = 0;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        const float w = 640, h = 220, x = (UI_WIDTH - w) / 2, y = (UI_HEIGHT - h) / 2;
        if (down & XINPUT_GAMEPAD_A) return 1;
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return 0;
        begin_frame(dev, background, user);
        UiDim();
        UiPanel(x, y, w, h, 22);
        UiText(UI_BOLD, 30, x + 48, y + 70, UI_COL_TEXT, question, UI_LEFT);
        UiFooter(x + 48, y + h - 50, hints);
        end_frame(dev);
    }
}

#define LANG_CARD_W 220.0f
#define LANG_CARD_H 196.0f
#define LANG_GAP 24.0f

static void lang_card_pos(int i, float *x, float *y)
{
    int row = i < 4 ? 0 : 1, n = row ? LANG_COUNT - 4 : 4, col = row ? i - 4 : i;
    float total = n * LANG_CARD_W + (n - 1) * LANG_GAP;
    *x = (UI_WIDTH - total) * 0.5f + col * (LANG_CARD_W + LANG_GAP);
    *y = row ? 408.0f : 188.0f;
}

static int lang_other_row(int i)
{
    float x, y, bx, by, best = 1e9f;
    int k, pick = i;
    lang_card_pos(i, &x, &y);
    for (k = 0; k < LANG_COUNT; k++)
    {
        float d;
        if ((k < 4) == (i < 4)) continue;
        lang_card_pos(k, &bx, &by);
        d = fabsf(bx - x);
        if (d < best) { best = d; pick = k; }
    }
    return pick;
}

int LanguageScreen(IDirect3DDevice9 *dev, int initial, int firstStart)
{
    IDirect3DTexture9 *flags[LANG_COUNT];
    UiInput in;
    UiAnim ax, ay;
    int sel = initial >= 0 && initial < LANG_COUNT ? initial : LANG_EN, i, result = -2;
    memset(&ax, 0, sizeof(ax));
    memset(&ay, 0, sizeof(ay));
    for (i = 0; i < LANG_COUNT; i++)
    {
        int w = 0, h = 0;
        u8 *px = png_decode(s_flagPng[i], s_flagPngSize[i], &w, &h, FLAG_W * FLAG_H);
        flags[i] = px ? UiImageCreate(dev, px, w, h) : NULL;
        free(px);
    }
    UiInputInit(&in);
    LOG_INFO("[lang] language list (%s)", firstStart ? "first start" : "settings");
    while (result == -2)
    {
        WORD down = UiInputPoll(&in);
        float cx, cy, now = UiTime();
        if (down & XINPUT_GAMEPAD_DPAD_LEFT) sel = (sel + LANG_COUNT - 1) % LANG_COUNT;
        if (down & XINPUT_GAMEPAD_DPAD_RIGHT) sel = (sel + 1) % LANG_COUNT;
        if (down & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN)) sel = lang_other_row(sel);
        if (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START)) result = sel;
        else if (!firstStart && (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))) result = -1;

        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        UiBrand(UI_SAFE_X, 72, 34);
        UiText(UI_BOLD, 34, UI_WIDTH * 0.5f, 134, UI_COL_TEXT, TrIn(sel, T_LANG_TITLE), UI_CENTER);
        lang_card_pos(sel, &cx, &cy);
        cx = anim_step(&ax, cx, 0);
        cy = anim_step(&ay, cy, 0);
        UiShadow(cx - 4, cy - 4, LANG_CARD_W + 8, LANG_CARD_H + 8, 24, 26, D3DCOLOR_ARGB(150, 240, 112, 32));
        for (i = 0; i < LANG_COUNT; i++)
        {
            float x, y;
            int s = i == sel;
            lang_card_pos(i, &x, &y);
            UiShadow(x, y + 8, LANG_CARD_W, LANG_CARD_H, 20, 24, UI_COL_SHADOW);
            UiRoundRect(x, y, LANG_CARD_W, LANG_CARD_H, 20, s ? D3DCOLOR_ARGB(255, 52, 42, 38) : UI_COL_PANEL_TOP,
                        s ? D3DCOLOR_ARGB(255, 40, 33, 30) : UI_COL_PANEL);
            UiRoundRing(x, y, LANG_CARD_W, LANG_CARD_H, 20, 1.0f, UI_COL_LINE);
            if (flags[i])
            {
                const float fw = 176, fh = 117, fx = x + (LANG_CARD_W - fw) * 0.5f, fy = y + 20;
                UiShadow(fx, fy + 4, fw, fh, 8, 12, D3DCOLOR_ARGB(140, 0, 0, 0));
                UiImage(flags[i], fx, fy, fw, fh, 8, s ? D3DCOLOR_ARGB(255, 255, 255, 255) : D3DCOLOR_ARGB(255, 200, 200, 200));
                UiRoundRing(fx, fy, fw, fh, 8, 1.0f, D3DCOLOR_ARGB(50, 255, 255, 255));
            }
            UiText(s ? UI_BOLD : UI_REGULAR, 22, x + LANG_CARD_W * 0.5f, y + 166, s ? UI_COL_TEXT : UI_COL_TEXT2, TrIn(i, T_LANG_NAME), UI_CENTER);
        }
        {
            float on = 0.75f + 0.25f * sinf(now * 4.0f);
            UiRoundRing(cx - 3, cy - 3, LANG_CARD_W + 6, LANG_CARD_H + 6, 23, 3.0f, D3DCOLOR_ARGB((int)(255 * on), 255, 150, 70));
        }
        UiScreenFooter(TrIn(sel, firstStart ? T_FOOT_LANG_FIRST : T_FOOT_LANG));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
    }
    for (i = 0; i < LANG_COUNT; i++)
        if (flags[i]) UiImageRelease(dev, flags[i]);
    LOG_INFO("[lang] language list: %s", result >= 0 ? LangCode(result) : "left");
    return result;
}

struct PrepareUi
{
    IDirect3DDevice9 *dev;
    UiBackgroundFn background;
    void *user;
    const char *name;
    DWORD last;
    UiInput in;
};

static int prepare_progress(void *user, u64 done, u64 total)
{
    PrepareUi *p = (PrepareUi *)user;
    const float w = 760, h = 250, x = (UI_WIDTH - w) / 2, y = (UI_HEIGHT - h) / 2, bx = x + 48, bw = w - 96;
    DWORD now = GetTickCount();
    char buf[160], fit[160];
    float f = total ? (float)((double)done / (double)total) : 0.0f;
    if (now - p->last < 60 && done < total) return 1;
    p->last = now;
    if (UiInputPoll(&p->in) & XINPUT_GAMEPAD_B) return 0;
    begin_frame(p->dev, p->background, p->user);
    UiDim();
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 30, bx, y + 62, UI_COL_TEXT, Tr(T_UNPACKING), UI_LEFT);
    UiTextFit(UI_REGULAR, 19, bw, p->name, fit, sizeof(fit));
    UiText(UI_REGULAR, 19, bx, y + 100, UI_COL_TEXT2, fit, UI_LEFT);
    UiRoundRect(bx, y + 128, bw, 12, 6, D3DCOLOR_ARGB(40, 255, 255, 255), D3DCOLOR_ARGB(40, 255, 255, 255));
    if (f > 0) UiRoundRect(bx, y + 128, bw * f < 12 ? 12 : bw * f, 12, 6, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    _snprintf(buf, sizeof(buf), Tr(T_UNPACK_PROGRESS), (u32)(done >> 20), (u32)(total >> 20));
    buf[sizeof(buf) - 1] = 0;
    UiText(UI_REGULAR, 17, bx, y + 168, UI_COL_TEXT3, buf, UI_LEFT);
    UiFooter(bx, y + h - 42, Tr(T_FOOT_CANCEL));
    end_frame(p->dev);
    return 1;
}

int ArchivePrepare(IDirect3DDevice9 *dev, const char *path, char *image, int imageSize, char *error, int errorSize,
                   UiBackgroundFn background, void *user)
{
    PrepareUi p;
    const char *name = strrchr(path, '\\');
    p.dev = dev;
    p.background = background;
    p.user = user;
    p.name = name ? name + 1 : path;
    p.last = 0;
    UiInputInit(&p.in);
    if (game_resolve(XB_CACHE_DIR, path, prepare_progress, &p, image, imageSize, error, errorSize) == 0) return 1;
    if (!strcmp(error, "cancelled")) return 0;
    LOG_ERROR("[menu] %s: %s", path, error);
    return -1;
}

#define GAMES_MAX 1024

struct GameEntry
{
    char path[CONFIG_PATH_MAX];
    char shown[129];
    char file[64];
    DiscInfo info;
    int infoOk;
    u32 size;
    int discs;
    int archive;
    int sub;
    int fav;
};

static GameEntry *s_games;
static int s_gameCount;

static void add_game(const char *dir, const char *name, u32 size, int sub)
{
    GameEntry *g;
    if (s_gameCount >= GAMES_MAX) return;
    g = &s_games[s_gameCount++];
    memset(g, 0, sizeof(*g));
    xb_fs_join(g->path, sizeof(g->path), dir, name);
    strncpy(g->file, name, sizeof(g->file) - 1);
    g->size = size;
    g->discs = 1;
    g->archive = game_is_archive(name);
    g->sub = sub;
}

static void scan_dir(const char *dir, int depth)
{
    WIN32_FIND_DATAA fd;
    char pattern[CONFIG_PATH_MAX];
    HANDLE f;
    xb_fs_join(pattern, sizeof(pattern), dir, "*");
    f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (depth > 0)
            {
                char sub[CONFIG_PATH_MAX];
                xb_fs_join(sub, sizeof(sub), dir, fd.cFileName);
                scan_dir(sub, depth - 1);
            }
        }
        else if (game_is_listed(fd.cFileName))
            add_game(dir, fd.cFileName, fd.nFileSizeLow, depth == 0);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

static const char *list_cache_path(void)
{
    static char path[64];
    return xb_fs_data_path(path, sizeof(path), "config", "gamelist.txt");
}
#define CACHE_PATH list_cache_path()

const char *xb_cache_dir(void)
{
    static char dir[64];
    return xb_fs_data_dir(dir, sizeof(dir), "cache");
}

static void split_field(char **p, char *out, int size)
{
    char *bar = strchr(*p, '|');
    int n = bar ? (int)(bar - *p) : (int)strlen(*p);
    if (n > size - 1) n = size - 1;
    memcpy(out, *p, n);
    out[n] = 0;
    *p = bar ? bar + 1 : *p + strlen(*p);
}

static void cache_load(void)
{
    char *text = read_text(CACHE_PATH), *line;
    if (!text) return;
    line = strtok(text, "\r\n");
    while (line)
    {
        char path[CONFIG_PATH_MAX], size[16];
        char *p = line;
        int i;
        split_field(&p, path, sizeof(path));
        split_field(&p, size, sizeof(size));
        for (i = 0; i < s_gameCount; i++)
        {
            GameEntry *g = &s_games[i];
            if (g->infoOk || _stricmp(g->path, path) || (u32)strtoul(size, NULL, 10) != g->size) continue;
            split_field(&p, g->info.title, sizeof(g->info.title));
            split_field(&p, g->info.product, sizeof(g->info.product));
            split_field(&p, g->info.version, sizeof(g->info.version));
            split_field(&p, g->info.date, sizeof(g->info.date));
            split_field(&p, g->info.areas, sizeof(g->info.areas));
            split_field(&p, g->info.maker, sizeof(g->info.maker));
            split_field(&p, g->info.device, sizeof(g->info.device));
            g->infoOk = g->info.title[0] != 0;
            break;
        }
        line = strtok(NULL, "\r\n");
    }
    free(text);
}

static void cache_save(void)
{
    char *text = (char *)malloc(GAMES_MAX * 512);
    int n = 0, i;
    if (!text) return;
    for (i = 0; i < s_gameCount; i++)
    {
        const GameEntry *g = &s_games[i];
        int r;
        if (!g->infoOk) continue;
        r = _snprintf(text + n, GAMES_MAX * 512 - n, "%s|%u|%s|%s|%s|%s|%s|%s|%s\r\n", g->path, g->size, g->info.title,
                      g->info.product, g->info.version, g->info.date, g->info.areas, g->info.maker, g->info.device);
        if (r < 0) break;
        n += r;
    }
    write_text(CACHE_PATH, text, n);
    free(text);
}

static const char *base_of(const char *path)
{
    const char *s = strrchr(path, '\\');
    return s ? s + 1 : path;
}

static int copy_rank(const char *name)
{
    static const char *order[] = { ".gdi", ".cue", ".chd", ".cdi", ".iso", ".7z", ".zip" };
    const char *dot = strrchr(name, '.');
    int i;
    for (i = 0; dot && i < 7; i++)
        if (!_stricmp(dot, order[i])) return i;
    return 7;
}

static void group_games(void)
{
    int i, j, n = 0;
    for (i = 0; i < s_gameCount; i++)
    {
        GameEntry *a = &s_games[i];
        char keyA[160], dirA[CONFIG_PATH_MAX];
        u32 numbers;
        int numA;
        size_t dirLen;
        if (!a->discs) continue;
        game_disc_key(base_of(a->path), keyA, sizeof(keyA));
        dirLen = base_of(a->path) - a->path;
        memcpy(dirA, a->path, dirLen);
        dirA[dirLen] = 0;
        numA = game_disc_number(base_of(a->path));
        numbers = numA < 32 ? 1u << numA : 0;
        for (j = i + 1; j < s_gameCount; j++)
        {
            GameEntry *b = &s_games[j];
            char keyB[160];
            int numB;
            if (!b->discs || _strnicmp(b->path, dirA, dirLen) || strchr(b->path + dirLen, '\\')) continue;
            game_disc_key(base_of(b->path), keyB, sizeof(keyB));
            numB = game_disc_number(base_of(b->path));
            if (strcmp(keyA, keyB))
            {
                if (a->sub && !numA && !numB) b->discs = 0;
                continue;
            }
            if (numB < 32) numbers |= 1u << numB;
            if (numB < numA || (numB == numA && copy_rank(b->file) < copy_rank(a->file)))
            {
                GameEntry t = *a;
                *a = *b;
                *b = t;
                numA = numB;
            }
            b->discs = 0;
        }
        a->discs = 0;
        while (numbers)
        {
            a->discs += (int)(numbers & 1);
            numbers >>= 1;
        }
        if (!a->discs) a->discs = 1;
    }
    for (i = 0; i < s_gameCount; i++)
        if (s_games[i].discs) s_games[n++] = s_games[i];
    s_gameCount = n;
}

static int read_info(GameEntry *g)
{
    if (g->archive)
    {
        DiscSet set;
        if (!game_cache_images(XB_CACHE_DIR, g->path, &set)) return 0;
        return disc_info_read_file(set.path[0], &g->info) == 0;
    }
    return disc_info_read_file(g->path, &g->info) == 0;
}

static int game_less(const void *a, const void *b)
{
    const GameEntry *x = (const GameEntry *)a, *y = (const GameEntry *)b;
    if (x->fav != y->fav) return y->fav - x->fav;
    return _stricmp(x->shown, y->shown);
}

static void fav_path(char *out, int size)
{
    char dir[64];
    xb_fs_data_dir(dir, sizeof(dir), "config");
    _snprintf(out, size - 1, "%s\\favorites.txt", dir);
    out[size - 1] = 0;
}

static void fav_load(void)
{
    char path[96], line[CONFIG_PATH_MAX + 4];
    FILE *f;
    int i;
    for (i = 0; i < s_gameCount; i++) s_games[i].fav = 0;
    fav_path(path, sizeof(path));
    f = fopen(path, "rb");
    if (!f) return;
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        for (i = 0; i < s_gameCount; i++)
            if (!_stricmp(s_games[i].path, line)) s_games[i].fav = 1;
    }
    fclose(f);
}

static void fav_save(void)
{
    char path[96];
    FILE *f;
    int i;
    fav_path(path, sizeof(path));
    f = fopen(path, "wb");
    if (!f)
    {
        LOG_WARN("[menu] cannot write %s", path);
        return;
    }
    for (i = 0; i < s_gameCount; i++)
        if (s_games[i].fav) fprintf(f, "%s\r\n", s_games[i].path);
    fclose(f);
}

static int fav_toggle(int sel)
{
    char path[CONFIG_PATH_MAX];
    int i;
    strncpy(path, s_games[sel].path, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    s_games[sel].fav = !s_games[sel].fav;
    fav_save();
    qsort(s_games, s_gameCount, sizeof(GameEntry), game_less);
    for (i = 0; i < s_gameCount; i++)
        if (!_stricmp(s_games[i].path, path)) return i;
    return 0;
}

static char game_letter(const GameEntry *g)
{
    const char *p = g->shown;
    for (; *p; p++)
    {
        char c = *p;
        if (c >= 'a' && c <= 'z') return (char)(c - 32);
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return '#';
    }
    return '#';
}

static int letter_jump(int sel, int dir)
{
    int n = s_gameCount, i;
#define SAME(a, b) (s_games[a].fav == s_games[b].fav && game_letter(&s_games[a]) == game_letter(&s_games[b]))
    if (dir > 0)
    {
        for (i = sel + 1; i < n; i++)
            if (!SAME(i, sel)) return i;
        return n - 1;
    }
    i = sel;
    while (i > 0 && SAME(i - 1, sel)) i--;
    if (i < sel) return i;
    if (i == 0) return 0;
    i--;
    while (i > 0 && SAME(i - 1, i)) i--;
#undef SAME
    return i;
}

static char s_jumpLetter;
static float s_jumpAt;

static void scan_games(const Config *c)
{
    int i, read = 0;
    if (!s_games) s_games = (GameEntry *)malloc(GAMES_MAX * sizeof(GameEntry));
    s_gameCount = 0;
    if (!s_games) return;
    scan_dir(c->gamesDir, 1);
    group_games();
    cache_load();
    for (i = 0; i < s_gameCount; i++)
    {
        GameEntry *g = &s_games[i];
        if (!g->infoOk)
        {
            g->infoOk = read_info(g);
            read++;
        }
        if (g->infoOk) strncpy(g->shown, g->info.title, sizeof(g->shown) - 1);
        else
            game_display_name(base_of(g->path), g->shown, sizeof(g->shown));
    }
    if (read) cache_save();
    fav_load();
    qsort(s_games, s_gameCount, sizeof(GameEntry), game_less);
    LOG_INFO("[menu] %d game(s) in %s (%d header(s) read)", s_gameCount, c->gamesDir, read);
}

static void initials(const char *name, char *out)
{
    int n = 0, start = 1;
    for (; *name && n < 2; name++)
    {
        char c = *name;
        if (c == '(' || c == '[') break;
        if (start && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            out[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        start = c == ' ' || c == '-' || c == '_';
    }
    if (!n) out[n++] = '?';
    out[n] = 0;
}

static void name_colors(const char *name, D3DCOLOR *top, D3DCOLOR *bottom)
{
    static const D3DCOLOR tops[6] = { D3DCOLOR_ARGB(255, 240, 112, 32), D3DCOLOR_ARGB(255, 245, 150, 60), D3DCOLOR_ARGB(255, 150, 100, 235),
                                      D3DCOLOR_ARGB(255, 70, 140, 240), D3DCOLOR_ARGB(255, 40, 180, 170), D3DCOLOR_ARGB(255, 110, 190, 80) };
    static const D3DCOLOR bottoms[6] = { D3DCOLOR_ARGB(255, 150, 50, 20), D3DCOLOR_ARGB(255, 180, 70, 20), D3DCOLOR_ARGB(255, 80, 40, 150),
                                         D3DCOLOR_ARGB(255, 30, 70, 160), D3DCOLOR_ARGB(255, 16, 100, 110), D3DCOLOR_ARGB(255, 40, 110, 50) };
    u32 h = 2166136261u;
    const char *p;
    for (p = name; *p; p++) h = (h ^ (u8)*p) * 16777619u;
    *top = tops[h % 6];
    *bottom = bottoms[h % 6];
}

static float chip(float x, float cy, const char *text, float a)
{
    float w = UiTextWidth(UI_BOLD, 15, text) + 26, h = 28;
    UiRoundRect(x, cy - h * 0.5f, w, h, h * 0.5f, UiAlpha(D3DCOLOR_ARGB(255, 50, 53, 68), a), UiAlpha(D3DCOLOR_ARGB(255, 42, 45, 58), a));
    UiRoundRing(x, cy - h * 0.5f, w, h, h * 0.5f, 1.0f, UiAlpha(D3DCOLOR_ARGB(34, 255, 255, 255), a));
    UiText(UI_BOLD, 15, x + w * 0.5f, cy, UiAlpha(UI_COL_TEXT2, a), text, UI_CENTER);
    return w;
}

static void detail(float x, float y, float w, const char *label, const char *value, float a)
{
    char fit[96];
    UiText(UI_BOLD, 13, x, y, UiAlpha(UI_COL_TEXT3, a), label, UI_LEFT);
    UiTextFit(UI_REGULAR, 19, w, value[0] ? value : "-", fit, sizeof(fit));
    UiText(UI_REGULAR, 19, x, y + 25, UiAlpha(UI_COL_TEXT, a), fit, UI_LEFT);
}

#define LIST_W 556.0f
#define LIST_Y 128.0f
#define ROW_H 36.0f
#define CARD_X 644.0f
#define CARD_Y 44.0f
#define CARD_W ((float)UI_WIDTH - UI_SAFE_X - CARD_X)
#define CARD_B 620.0f
#define STAGE_H 240.0f
#define LIST_ROWS 13

#define ART_VIEWS 24
struct ArtView
{
    int used;
    char path[CONFIG_PATH_MAX];
    IDirect3DTexture9 *cover;
    IDirect3DTexture9 *glow;
    int coverW, coverH;
    float coverAt;
    u32 lastUse;
};
static ArtView s_views[ART_VIEWS];
static u32 s_viewClock;

static ArtView *view_find(const char *path)
{
    int i;
    for (i = 0; i < ART_VIEWS; i++)
        if (s_views[i].used && !_stricmp(s_views[i].path, path))
        {
            s_views[i].lastUse = ++s_viewClock;
            return &s_views[i];
        }
    return NULL;
}

static void view_free(IDirect3DDevice9 *dev, ArtView *v)
{
    if (v->cover) UiImageRelease(dev, v->cover);
    if (v->glow) UiImageRelease(dev, v->glow);
    memset(v, 0, sizeof(*v));
}

static void views_take(IDirect3DDevice9 *dev)
{
    ArtResult r;
    while (ArtTake(&r))
    {
        ArtView *v = view_find(r.path);
        float coverAt = UiTime();
        if (v)
        {
            if (v->cover && r.cover) coverAt = v->coverAt;
            view_free(dev, v);
        }
        else
        {
            int i, best = 0;
            for (i = 0; i < ART_VIEWS; i++)
            {
                if (!s_views[i].used) { best = i; break; }
                if (s_views[i].lastUse < s_views[best].lastUse) best = i;
            }
            v = &s_views[best];
            if (v->used)
            {
                ArtForget(v->path);
                view_free(dev, v);
            }
        }
        v->used = 1;
        strncpy(v->path, r.path, sizeof(v->path) - 1);
        if (r.cover)
        {
            v->cover = UiImageCreate(dev, r.cover, r.w, r.h);
            v->coverW = r.w;
            v->coverH = r.h;
            if (r.glow) v->glow = UiImageCreate(dev, r.glow, ART_GLOW_W, ART_GLOW_H);
        }
        v->coverAt = coverAt;
        v->lastUse = ++s_viewClock;
        ArtResultFree(&r);
    }
}

static void views_clear(IDirect3DDevice9 *dev)
{
    int i;
    for (i = 0; i < ART_VIEWS; i++)
        if (s_views[i].used) view_free(dev, &s_views[i]);
    ArtForgetAll();
}

static void art_game(const GameEntry *g, ArtGame *a)
{
    memset(a, 0, sizeof(*a));
    strncpy(a->path, g->path, sizeof(a->path) - 1);
    if (g->infoOk)
    {
        strncpy(a->product, g->info.product, sizeof(a->product) - 1);
        strncpy(a->title, g->info.title, sizeof(a->title) - 1);
        strncpy(a->areas, g->info.areas, sizeof(a->areas) - 1);
    }
    else
        strncpy(a->title, g->shown, sizeof(a->title) - 1);
    strncpy(a->file, g->file, sizeof(a->file) - 1);
}

static void card_draw(const GameEntry *g, const ArtView *v, int busy, float selAt)
{
    const float x = CARD_X, y = CARD_Y, w = CARD_W, h = CARD_B - CARD_Y, ix = x + 22, iw = w - 44;
    const float sx = ix, sy = y + 22, sw = iw, sh = STAGE_H;
    float now = UiTime(), a = 0.3f + 0.7f * smooth01((now - selAt) / 0.2f), slide = (1.0f - smooth01((now - selAt) / 0.2f)) * 10.0f;
    float ty, divY = CARD_B - 76, ca = 0;
    char buf[512], ini[4];
    D3DCOLOR top, bottom;

    UiPanel(x, y, w, h, 22);
    if (v && v->cover) ca = smooth01((now - (v->coverAt > selAt ? v->coverAt : selAt)) / 0.25f);
    UiRoundRect(sx, sy, sw, sh, 16, D3DCOLOR_ARGB(255, 22, 23, 31), D3DCOLOR_ARGB(255, 16, 17, 23));
    {
        float pa = a * (v ? 1.0f : smooth01((now - selAt - 0.3f) / 0.2f)) * (1.0f - ca);
        if (pa > 0.01f)
        {
            name_colors(g->shown, &top, &bottom);
            initials(g->shown, ini);
            UiRoundRect(sx, sy, sw, sh, 16, UiAlpha(top, pa * 0.85f), UiAlpha(bottom, pa * 0.85f));
            UiRoundRect(sx + 2, sy + 2, sw - 4, sh * 0.5f, 14, UiAlpha(D3DCOLOR_ARGB(30, 255, 255, 255), pa), D3DCOLOR_ARGB(0, 255, 255, 255));
            UiTextShadowed(UI_BOLD, 92, sx + sw * 0.5f, sy + sh * 0.5f, UiAlpha(D3DCOLOR_ARGB(235, 255, 255, 255), pa), ini, UI_CENTER);
        }
    }
    if (ca > 0)
    {
        float maxW = sw - 56, maxH = sh - 36, cw = (float)v->coverW, chh = (float)v->coverH, k, cx, cy;
        D3DCOLOR t2, b2;
        if (v->glow)
            UiImage(v->glow, sx, sy, sw, sh, 16, UiAlpha(D3DCOLOR_ARGB(255, 255, 255, 255), ca * a));
        else
        {
            name_colors(g->shown, &t2, &b2);
            UiRoundRect(sx, sy, sw, sh, 16, UiAlpha(t2, ca * a * 0.35f), UiAlpha(b2, ca * a * 0.35f));
        }
        UiRoundRect(sx, sy, sw, sh, 16, UiAlpha(D3DCOLOR_ARGB(120, 12, 13, 18), ca * a), UiAlpha(D3DCOLOR_ARGB(185, 12, 13, 18), ca * a));
        k = maxW / cw < maxH / chh ? maxW / cw : maxH / chh;
        if (k > 1.15f) k = 1.15f;
        cw *= k;
        chh *= k;
        cx = sx + (sw - cw) * 0.5f;
        cy = sy + (sh - chh) * 0.5f + (1.0f - ca) * 6.0f;
        UiShadow(cx, cy + 8, cw, chh, 8, 26, UiAlpha(D3DCOLOR_ARGB(170, 0, 0, 0), ca * a));
        UiImage(v->cover, cx, cy, cw, chh, 7, UiAlpha(D3DCOLOR_ARGB(255, 255, 255, 255), ca * a));
        UiRoundRing(cx, cy, cw, chh, 7, 1.0f, UiAlpha(D3DCOLOR_ARGB(40, 255, 255, 255), ca * a));
    }
    UiRoundRing(sx, sy, sw, sh, 16, 1.0f, UiAlpha(UI_COL_LINE, a));
    if (busy && ca <= 0)
    {
        float pw = UiTextWidth(UI_REGULAR, 15, Tr(T_DOWNLOADING)) + 64, ph = 30, px = sx + sw - pw - 12, py = sy + sh - ph - 12;
        int d;
        UiRoundRect(px, py, pw, ph, ph * 0.5f, D3DCOLOR_ARGB(200, 16, 17, 23), D3DCOLOR_ARGB(200, 16, 17, 23));
        UiText(UI_REGULAR, 15, px + 14, py + ph * 0.5f, UI_COL_TEXT2, Tr(T_DOWNLOADING), UI_LEFT);
        for (d = 0; d < 3; d++)
        {
            float on = 0.5f + 0.5f * sinf(now * 6.0f - d * 0.9f);
            UiCircle(px + pw - 38 + d * 11, py + ph * 0.5f, 3.0f, UiAlpha(UI_COL_ACCENT_HI, 0.35f + 0.65f * on));
        }
    }

    ty = sy + sh + 32 + slide;
    {
        int lines = UiTextWrap(UI_BOLD, 26, iw, 2, g->shown, buf, sizeof(buf));
        UiText(UI_BOLD, 26, ix, ty, UiAlpha(UI_COL_TEXT, a), buf, UI_LEFT);
        ty += (lines - 1) * 26 * 1.35f + 32;
    }
    if (g->infoOk)
    {
        char region[48], fit[256];
        _snprintf(buf, sizeof(buf), "%s%s%s", g->info.maker, g->info.date[0] ? "  " UI_CH_MIDDOT "  " : "", g->info.date);
        buf[sizeof(buf) - 1] = 0;
        UiTextFit(UI_REGULAR, 18, iw, buf, fit, sizeof(fit));
        UiText(UI_REGULAR, 18, ix, ty, UiAlpha(UI_COL_TEXT2, a), fit, UI_LEFT);
        ty += 36;
        disc_info_region(&g->info, region, sizeof(region));
        {
            float cx = ix;
            if (region[0]) cx += chip(cx, ty, region, a) + 8;
            if (g->discs > 1)
            {
                char d[64];
                _snprintf(d, sizeof(d), Tr(T_DISCS), g->discs);
                cx += chip(cx, ty, d, a) + 8;
            }
            if (g->archive) cx += chip(cx, ty, "7z", a) + 8;
            if (g->info.device[0]) cx += chip(cx, ty, g->info.device, a) + 8;
        }
        ty += 30;
    }
    else if (g->archive)
    {
        UiText(UI_REGULAR, 18, ix, ty, UiAlpha(UI_COL_TEXT2, a), Tr(T_ARCHIVE_NOTE), UI_LEFT);
        ty += 36;
        if (g->discs > 1)
        {
            char d[64];
            _snprintf(d, sizeof(d), Tr(T_DISCS), g->discs);
            chip(ix, ty, d, a);
        }
    }
    else
        UiText(UI_REGULAR, 18, ix, ty, UiAlpha(UI_COL_ACCENT_HI, a), Tr(T_NO_HEADER), UI_LEFT);

    UiRect(ix, divY, iw, 1, UI_COL_LINE);
    detail(ix, divY + 22, 130, Tr(T_PRODUCT), g->infoOk ? g->info.product : "", 1.0f);
    detail(ix + 148, divY + 22, 90, Tr(T_VERSION), g->infoOk ? g->info.version : "", 1.0f);
    detail(ix + 256, divY + 22, iw - 256, Tr(T_FILE), g->file, 1.0f);
}

static void list_draw(const Config *c, int sel, int top, float selAt, const ArtStatus *st)
{
    static UiAnim selAnim;
    const float listX = UI_SAFE_X, listY = LIST_Y, listW = LIST_W, rowH = ROW_H, pad = 12;
    const float listH = LIST_ROWS * rowH + 2 * pad;
    int n = s_gameCount, i;
    char buf[160];
    UiBackdrop();
    UiBrand(UI_SAFE_X, 72, 34);
    if (n)
    {
        sprintf(buf, "%d / %d", sel + 1, n);
        UiText(UI_REGULAR, 19, listX + listW, 64, UI_COL_TEXT2, buf, UI_RIGHT);
        if (st->bulk)
        {
            const float bw = 120, right = listX + listW, cy = 89;
            float frac = st->total ? (float)st->done / st->total : 0;
            if (st->done) _snprintf(buf, sizeof(buf) - 1, Tr(T_ART_PROGRESS), st->done, st->total);
            else strncpy(buf, Tr(T_ART_CONNECTING), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            UiText(UI_REGULAR, 15, right - bw - 12, cy, UI_COL_TEXT2, buf, UI_RIGHT);
            UiRoundRect(right - bw, cy - 3, bw, 6, 3, D3DCOLOR_ARGB(30, 255, 255, 255), D3DCOLOR_ARGB(30, 255, 255, 255));
            if (frac > 0) UiRoundRect(right - bw, cy - 3, bw * frac, 6, 3, UI_COL_ACCENT_HI, UI_COL_ACCENT);
        }
        else
            UiText(UI_REGULAR, 15, listX + listW, 88, UI_COL_TEXT3, c->gamesDir, UI_RIGHT);
    }
    UiPanel(listX, listY, listW, listH, 20);
    if (!n)
    {
        float cy = listY + listH * 0.5f;
        UiText(UI_BOLD, 30, listX + listW * 0.5f, cy - 50, UI_COL_TEXT, Tr(T_NO_GAME), UI_CENTER);
        UiText(UI_REGULAR, 21, listX + listW * 0.5f, cy, UI_COL_TEXT2, Tr(T_COPY_GAMES), UI_CENTER);
        _snprintf(buf, sizeof(buf), Tr(T_INTO_FOLDER), c->gamesDir);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_REGULAR, 21, listX + listW * 0.5f, cy + 32, UI_COL_TEXT2, buf, UI_CENTER);
    }
    else
    {
        float sy = anim_step(&selAnim, listY + pad + (sel - top) * rowH, 0), textW = listW - 64;
        selection_bar(listX + 10, sy + 1, listW - (n > LIST_ROWS ? 36 : 20), rowH - 2);
        for (i = 0; i < LIST_ROWS && top + i < n; i++)
        {
            float cy = listY + pad + i * rowH + rowH * 0.5f;
            int s = top + i == sel;
            UiTextFit(s ? UI_BOLD : UI_REGULAR, 21, textW, s_games[top + i].shown, buf, sizeof(buf));
            UiText(s ? UI_BOLD : UI_REGULAR, 21, listX + 30, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, buf, UI_LEFT);
            if (s_games[top + i].fav) UiCircle(listX + 19, cy, 4.0f, D3DCOLOR_ARGB(255, 252, 206, 66));
        }
        {
            float age = UiTime() - s_jumpAt, al = age < 0.45f ? 1.0f : 1.0f - (age - 0.45f) / 0.25f;
            if (s_jumpLetter && al > 0)
            {
                char l[2] = { s_jumpLetter, 0 };
                float bx = listX + listW * 0.5f - 60, by = listY + listH * 0.5f - 60;
                UiRoundRect(bx, by, 120, 120, 26, UiAlpha(D3DCOLOR_ARGB(225, 30, 32, 42), al), UiAlpha(D3DCOLOR_ARGB(225, 20, 21, 28), al));
                UiRoundRing(bx, by, 120, 120, 26, 1.0f, UiAlpha(UI_COL_LINE, al));
                UiText(UI_BOLD, 72, bx + 60, by + 60, UiAlpha(UI_COL_TEXT, al), l, UI_CENTER);
            }
        }
        if (n > LIST_ROWS)
        {
            float tx = listX + listW - 16, ty = listY + pad + 4, th = listH - 2 * pad - 8;
            float hh = th * LIST_ROWS / n, yy;
            if (hh < 28) hh = 28;
            yy = ty + (th - hh) * top / (float)(n - LIST_ROWS);
            UiRoundRect(tx, ty, 4, th, 2, D3DCOLOR_ARGB(20, 255, 255, 255), D3DCOLOR_ARGB(20, 255, 255, 255));
            UiRoundRect(tx, yy, 4, hh, 2, D3DCOLOR_ARGB(150, 255, 255, 255), D3DCOLOR_ARGB(110, 255, 255, 255));
        }
        card_draw(&s_games[sel], view_find(s_games[sel].path), !_stricmp(st->busyPath, s_games[sel].path), selAt);
    }
    UiScreenFooter(Tr(n ? T_FOOT_BROWSER : T_FOOT_BROWSER_EMPTY));
}

#define MSG_MS 6000

static void art_bulk(void)
{
    ArtGame *all = (ArtGame *)malloc(sizeof(ArtGame) * (s_gameCount ? s_gameCount : 1));
    int i, n = 0;
    if (!all) return;
    for (i = 0; i < s_gameCount; i++)
        if (s_games[i].discs) art_game(&s_games[i], &all[n++]);
    ArtBulkStart(all, n);
    free(all);
}

int GameList(IDirect3DDevice9 *dev, Config *c, char *path, int pathSize)
{
    UiInput in;
    int sel = 0, top = 0, i, lastSel = -1;
    float selAt = 0;
    char all[CONFIG_PATH_MAX], msg[256];
    DWORD msgAt = 0;
    msg[0] = 0;
    scan_games(c);
    ArtStart();
    ArtSetNetwork(c->artDownload);
    ArtPause(0);
    if (c->artAll && s_gameCount) art_bulk();
    if (c->updateCheck) UpdateCheckStart(c->updateCheck == 2 ? "0.0.0" : EMU_VERSION);
    for (i = 0; i < s_gameCount; i++)
        if (!_stricmp(s_games[i].path, c->lastGame)) sel = i;
    SettingsPaths(NULL, all, NULL, sizeof(all));
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        int n = s_gameCount, na, nb, note;
        ArtStatus st;
        char newVersion[32];
        if (UpdateCheckTake(newVersion, sizeof(newVersion)))
        {
            _snprintf(msg, sizeof(msg) - 1, Tr(T_UPDATE_AVAILABLE), newVersion);
            msg[sizeof(msg) - 1] = 0;
            msgAt = GetTickCount();
        }
        note = ArtTakeNote(&na, &nb);
        if (note >= 0)
        {
            _snprintf(msg, sizeof(msg) - 1, Tr(note), na, nb);
            msg[sizeof(msg) - 1] = 0;
            msgAt = GetTickCount();
            lastSel = -1;
        }
        if ((down & XINPUT_GAMEPAD_BACK) && ConfirmScreen(dev, Tr(T_QUIT_ASK), Tr(T_QUIT), NULL, NULL))
        {
            views_clear(dev);
            ArtPause(1);
            return 0;
        }
        if (n)
        {
            if (down & XINPUT_GAMEPAD_DPAD_UP) sel = (sel + n - 1) % n;
            if (down & XINPUT_GAMEPAD_DPAD_DOWN) sel = (sel + 1) % n;
            if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0;
            if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) sel = sel + LIST_ROWS < n ? sel + LIST_ROWS : n - 1;
            if (down & (UI_BTN_LT | UI_BTN_RT))
            {
                sel = letter_jump(sel, (down & UI_BTN_RT) ? 1 : -1);
                s_jumpLetter = game_letter(&s_games[sel]);
                s_jumpAt = UiTime();
            }
            if (down & XINPUT_GAMEPAD_B)
            {
                sel = fav_toggle(sel);
                strncpy(msg, Tr(s_games[sel].fav ? T_FAV_ADDED : T_FAV_REMOVED), sizeof(msg) - 1);
                msg[sizeof(msg) - 1] = 0;
                msgAt = GetTickCount();
                lastSel = -1;
            }
            if (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START))
            {
                strncpy(path, s_games[sel].path, pathSize - 1);
                path[pathSize - 1] = 0;
                strncpy(c->lastGame, path, sizeof(c->lastGame) - 1);
                SettingsSave(c, all, 1);
                views_clear(dev);
                ArtPause(1);
                return 1;
            }
            if (sel < top) top = sel;
            if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
            if (sel != lastSel)
            {
                static const int order[5] = { 0, 1, -1, 2, -2 };
                ArtGame want[5];
                int k, cnt = 0;
                for (k = 0; k < 5 && k < n; k++) art_game(&s_games[(sel + order[k] + n * 2) % n], &want[cnt++]);
                ArtWant(want, cnt);
                selAt = UiTime();
                lastSel = sel;
            }
        }
        if (down & XINPUT_GAMEPAD_Y)
        {
            s_fromList = 1;
            s_artAll = 0;
            SettingsScreen(dev, c, NULL, NULL, NULL);
            s_fromList = 0;
            ArtSetNetwork(c->artDownload);
            if (s_artAll && n)
            {
                art_bulk();
                strncpy(msg, Tr(T_GETTING_ART), sizeof(msg) - 1);
                msg[sizeof(msg) - 1] = 0;
                msgAt = GetTickCount();
            }
            UiInputInit(&in);
        }
        if (down & XINPUT_GAMEPAD_X)
        {
            AboutScreen(dev, NULL, NULL);
            UiInputInit(&in);
        }
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        views_take(dev);
        ArtGetStatus(&st);
        UiBegin(dev);
        list_draw(c, sel, top, selAt, &st);
        if (msg[0])
        {
            DWORD age = GetTickCount() - msgAt;
            if (age < MSG_MS) UiToast(msg, (float)age, (float)MSG_MS);
            else msg[0] = 0;
        }
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
    }
}
