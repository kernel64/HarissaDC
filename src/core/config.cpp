// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"

#include <stdio.h>
#include <string.h>

#include "ini.h"
#include "log.h"

void config_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->logLevel = LOG_LEVEL_INFO;
    strcpy(c->biosDir, "game:\\bios");
    strcpy(c->gamesDir, "game:\\games");
    c->mountDrives = 1;
    c->showLog = 1;
    c->toneHz = 0;
    c->biosMode = BIOS_HLE;
    c->useJit = 1;
    c->gpuRender = 1;
    c->soundThread = 1;
    c->audioStretch = 1;
    c->armJit = 1;
    c->fastFpu = 1;
    c->jitSplit = 1;
    c->jitSkips = 1;
    {
        int p;
        for (p = 0; p < PAD_PLAYERS; p++)
        {
            config_pad_defaults(c->padMap[p], &c->padStick[p]);
            c->padDead[p] = 24;
            c->padRange[p] = 100;
        }
    }
    c->zoom = 100;
    c->artDownload = 1;
    c->rumble = 1;
    c->vsync = 1;
    c->updateCheck = 1;
    c->fastMem = 1;
    c->idleSkip = 1;
    c->sh4Clock = -1;
    c->rotate = -1;
    c->transSort = 1;
    c->modVols = 1;
    c->transDepth = 1;
    c->fog = 1;
    c->gpuDisplay = 1;
    c->region = -1;
    c->smooth = 1;
}

static const char *const s_padSrcKey[PADSRC_COUNT] = { "none", "a", "b", "x", "y", "lb", "rb", "lt", "rt", "start", "ls", "rs", "dup", "ddown", "dleft", "dright" };
static const char *const s_padSrcName[PADSRC_COUNT] = { "None", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START",
                                                        "Left stick click", "Right stick click", "D-pad up", "D-pad down",
                                                        "D-pad left", "D-pad right" };
static const char *const s_padInKey[PADIN_COUNT] = { "map_a", "map_b", "map_x", "map_y", "map_start", "map_l", "map_r", "map_up", "map_down", "map_left", "map_right" };
static const char *const s_padInName[PADIN_COUNT] = { "A", "B", "X", "Y", "Start", "L trigger", "R trigger", "D-pad up", "D-pad down", "D-pad left", "D-pad right" };

const char *config_pad_source_key(int src) { return src >= 0 && src < PADSRC_COUNT ? s_padSrcKey[src] : "none"; }
const char *config_pad_source_name(int src) { return src >= 0 && src < PADSRC_COUNT ? s_padSrcName[src] : "None"; }
const char *config_pad_input_name(int in) { return in >= 0 && in < PADIN_COUNT ? s_padInName[in] : "?"; }

void config_pad_defaults(u32 map[PADIN_COUNT], int *stick)
{
    map[PADIN_A] = 1u << PADSRC_A;
    map[PADIN_B] = 1u << PADSRC_B;
    map[PADIN_X] = 1u << PADSRC_X;
    map[PADIN_Y] = 1u << PADSRC_Y;
    map[PADIN_START] = 1u << PADSRC_START;
    map[PADIN_L] = 1u << PADSRC_LT;
    map[PADIN_R] = 1u << PADSRC_RT;
    map[PADIN_UP] = 1u << PADSRC_DUP;
    map[PADIN_DOWN] = 1u << PADSRC_DDOWN;
    map[PADIN_LEFT] = 1u << PADSRC_DLEFT;
    map[PADIN_RIGHT] = 1u << PADSRC_DRIGHT;
    *stick = 0;
}

static u32 pad_sources(const char *v)
{
    u32 mask = 0;
    while (*v)
    {
        char name[16];
        int n = 0, src;
        while (*v == ' ' || *v == '+' || *v == ',') v++;
        while (*v && *v != '+' && *v != ',' && *v != ' ' && n < 15) name[n++] = *v++;
        name[n] = 0;
        for (src = 1; src < PADSRC_COUNT; src++)
            if (n && ini_key_is(name, s_padSrcKey[src])) mask |= 1u << src;
        while (*v && *v != '+' && *v != ',') v++;
    }
    return mask;
}

static int pad_key(Config *c, const char *key, const char *value)
{
    int p = 0, in;
    if ((key[0] == 'p' || key[0] == 'P') && key[1] >= '2' && key[1] <= '0' + PAD_PLAYERS && key[2] == '_')
    {
        p = key[1] - '1';
        key += 3;
    }
    if (ini_key_is(key, "stick"))
    {
        c->padStick[p] = ini_key_is(value, "right") ? 1 : 0;
        return 1;
    }
    if (ini_key_is(key, "deadzone"))
    {
        int v = ini_int(value, c->padDead[p]);
        c->padDead[p] = v < 0 ? 0 : v > 60 ? 60 : v;
        return 1;
    }
    if (ini_key_is(key, "sensitivity"))
    {
        int v = ini_int(value, c->padRange[p]);
        c->padRange[p] = v < 50 ? 50 : v > 150 ? 150 : v;
        return 1;
    }
    if (ini_key_is(key, "invert"))
    {
        c->padInvert[p] = ini_int(value, c->padInvert[p]) & 3;
        return 1;
    }
    for (in = 0; in < PADIN_COUNT; in++)
        if (ini_key_is(key, s_padInKey[in]))
        {
            c->padMap[p][in] = pad_sources(value);
            return 1;
        }
    return 0;
}

static void copy_path(char *dst, const char *src)
{
    strncpy(dst, src, CONFIG_PATH_MAX - 1);
    dst[CONFIG_PATH_MAX - 1] = 0;
}

struct ConfigParse
{
    Config *c;
    int unknown;
};

static int on_key(void *user, const char *section, const char *key, const char *value, int line)
{
    ConfigParse *p = (ConfigParse *)user;
    Config *c = p->c;
    (void)section;
    if (ini_key_is(key, "loglevel")) c->logLevel = ini_int(value, c->logLevel);
    else if (ini_key_is(key, "biosdir")) copy_path(c->biosDir, value);
    else if (ini_key_is(key, "bios"))
        c->biosMode = ini_key_is(value, "real") ? BIOS_REAL : ini_key_is(value, "auto") ? BIOS_AUTO : BIOS_HLE;
    else if (ini_key_is(key, "forceregion"))
        c->region = ini_key_is(value, "jp") ? 0 : ini_key_is(value, "us") ? 1 : ini_key_is(value, "eu") ? 2 : -1;
    else if (ini_key_is(key, "region")) {}
    else if (ini_key_is(key, "gamesdir")) copy_path(c->gamesDir, value);
    else if (ini_key_is(key, "mountdrives")) c->mountDrives = ini_bool(value, c->mountDrives);
    else if (ini_key_is(key, "showlog")) c->showLog = ini_bool(value, c->showLog);
    else if (ini_key_is(key, "diag")) c->diag = ini_int(value, c->diag);
    else if (ini_key_is(key, "exitafter")) c->exitAfter = ini_int(value, c->exitAfter);
    else if (ini_key_is(key, "savestateat")) c->saveStateAt = ini_int(value, c->saveStateAt);
    else if (ini_key_is(key, "loadstate")) c->loadState = ini_int(value, c->loadState);
    else if (ini_key_is(key, "tracefield")) c->traceField = ini_int(value, c->traceField);
    else if (ini_key_is(key, "sampler")) c->sampler = ini_bool(value, c->sampler);
    else if (ini_key_is(key, "taskip")) c->taSkip = ini_bool(value, c->taSkip);
    else if (ini_key_is(key, "audioms")) c->audioMs = ini_int(value, c->audioMs);
    else if (ini_key_is(key, "audiostretch")) c->audioStretch = ini_bool(value, c->audioStretch);
    else if (ini_key_is(key, "slice")) c->slice = ini_int(value, c->slice);
    else if (ini_key_is(key, "fastfpu")) c->fastFpu = ini_bool(value, c->fastFpu);
    else if (ini_key_is(key, "jitsplit")) c->jitSplit = ini_bool(value, c->jitSplit);
    else if (ini_key_is(key, "jitskips")) c->jitSkips = ini_bool(value, c->jitSkips);
    else if (pad_key(c, key, value)) {}
    else if (ini_key_is(key, "fxaa")) c->fxaa = ini_bool(value, c->fxaa);
    else if (ini_key_is(key, "sharpen")) c->sharpen = ini_int(value, c->sharpen) & 3;
    else if (ini_key_is(key, "blur")) c->blur = ini_int(value, c->blur) & 3;
    else if (ini_key_is(key, "screen")) c->screen = ini_int(value, c->screen);
    else if (ini_key_is(key, "brightness")) c->brightness = ini_int(value, c->brightness);
    else if (ini_key_is(key, "zoom")) c->zoom = ini_int(value, c->zoom);
    else if (ini_key_is(key, "posx")) c->posX = ini_int(value, c->posX);
    else if (ini_key_is(key, "posy")) c->posY = ini_int(value, c->posY);
    else if (ini_key_is(key, "fpubench")) c->fpuBench = ini_bool(value, c->fpuBench);
    else if (ini_key_is(key, "lang"))
    {
        strncpy(c->lang, value, sizeof(c->lang) - 1);
        c->lang[sizeof(c->lang) - 1] = 0;
    }
    else if (ini_key_is(key, "uilang"))
    {
        strncpy(c->uiLang, value, sizeof(c->uiLang) - 1);
        c->uiLang[sizeof(c->uiLang) - 1] = 0;
    }
    else if (ini_key_is(key, "xinput"))
    {
        strncpy(c->xinput, value, sizeof(c->xinput) - 1);
        c->xinput[sizeof(c->xinput) - 1] = 0;
    }
    else if (ini_key_is(key, "fastmem")) c->fastMem = ini_bool(value, c->fastMem);
    else if (ini_key_is(key, "sh4clock")) c->sh4Clock = ini_key_is(value, "auto") ? -1 : ini_int(value, c->sh4Clock);
    else if (ini_key_is(key, "idleskip")) c->idleSkip = ini_bool(value, c->idleSkip);
    else if (ini_key_is(key, "transsort")) c->transSort = ini_bool(value, c->transSort);
    else if (ini_key_is(key, "modvols")) c->modVols = ini_bool(value, c->modVols);
    else if (ini_key_is(key, "transdepth")) c->transDepth = ini_bool(value, c->transDepth);
    else if (ini_key_is(key, "fog")) c->fog = ini_bool(value, c->fog);
    else if (ini_key_is(key, "aicatrace")) c->aicaTrace = ini_bool(value, c->aicaTrace);
    else if (ini_key_is(key, "soundlag")) c->soundLag = ini_int(value, c->soundLag);
    else if (ini_key_is(key, "scenedump")) c->sceneDump = ini_int(value, c->sceneDump);
    else if (ini_key_is(key, "scenedumpcount")) c->sceneDumpCount = ini_int(value, c->sceneDumpCount);
    else if (ini_key_is(key, "scenedumpevery")) c->sceneDumpEvery = ini_int(value, c->sceneDumpEvery);
    else if (ini_key_is(key, "selftest")) c->selfTest = ini_bool(value, c->selfTest);
    else if (ini_key_is(key, "soundthread")) c->soundThread = ini_bool(value, c->soundThread);
    else if (ini_key_is(key, "armjit")) c->armJit = ini_bool(value, c->armJit);
    else if (ini_key_is(key, "tonehz")) c->toneHz = ini_int(value, c->toneHz);
    else if (ini_key_is(key, "cpu")) c->useJit = !ini_key_is(value, "interp");
    else if (ini_key_is(key, "renderer")) c->gpuRender = !ini_key_is(value, "soft");
    else if (ini_key_is(key, "gpudisplay")) c->gpuDisplay = ini_int(value, c->gpuDisplay) != 0;
    else if (ini_key_is(key, "showfps")) c->showFps = ini_int(value, c->showFps) & 3;
    else if (ini_key_is(key, "vsync")) c->vsync = ini_int(value, c->vsync) % 3;
    else if (ini_key_is(key, "texfilter")) c->texFilter = ini_int(value, c->texFilter) % 3;
    else if (ini_key_is(key, "artall")) c->artAll = ini_int(value, c->artAll);
    else if (ini_key_is(key, "updatecheck")) c->updateCheck = ini_int(value, c->updateCheck);
    else if (ini_key_is(key, "rumble")) c->rumble = ini_bool(value, c->rumble);
    else if (ini_key_is(key, "lightgun")) c->lightGun = ini_bool(value, c->lightGun);
    else if (ini_key_is(key, "rotate")) c->rotate = ini_key_is(value, "auto") ? -1 : ini_int(value, c->rotate);
    else if (ini_key_is(key, "artdownload")) c->artDownload = ini_int(value, c->artDownload);
    else if (ini_key_is(key, "aspect")) c->aspect = ini_key_is(value, "stretch") ? 1 : 0;
    else if (ini_key_is(key, "widescreen")) c->widescreen = ini_bool(value, c->widescreen);
    else if (ini_key_is(key, "smooth")) c->smooth = ini_bool(value, c->smooth);
    else if (ini_key_is(key, "lastgame")) copy_path(c->lastGame, value);
    else if (ini_key_is(key, "autoboot")) c->autoBoot = ini_bool(value, c->autoBoot);
    else if (ini_key_is(key, "disc")) copy_path(c->disc, value);
    else if (ini_key_is(key, "dumpblocks")) copy_path(c->dumpBlocks, value);
    else if (ini_key_is(key, "dumpblocksat")) c->dumpBlocksAt = ini_int(value, c->dumpBlocksAt);
    else if (ini_key_is(key, "jitmap")) c->jitMap = ini_int(value, c->jitMap);
    else
    {
        LOG_WARN("[config] line %d: unknown key '%s'", line, key);
        p->unknown++;
    }
    return 1;
}

int config_parse_text(Config *c, const char *text)
{
    ConfigParse p;
    int bad;
    p.c = c;
    p.unknown = 0;
    bad = ini_parse(text, on_key, &p);
    if (bad) LOG_WARN("[config] %d malformed line(s)", bad);
    return bad + p.unknown;
}

int config_write(const Config *c, int withLastGame, char *out, int size)
{
    int n = _snprintf(out, size, "cpu=%s\r\nrenderer=%s\r\nshowfps=%d\r\naspect=%s\r\nwidescreen=%d\r\nsmooth=%d\r\n",
                      c->useJit ? "jit" : "interp", c->gpuRender ? "gpu" : "soft", c->showFps, c->aspect ? "stretch" : "4:3",
                      c->widescreen, c->smooth);
    if (n < 0 || n >= size) n = size - 1;
    {
        int r = _snprintf(out + n, size - n, "fxaa=%d\r\nsharpen=%d\r\nblur=%d\r\nscreen=%d\r\nbrightness=%d\r\nzoom=%d\r\nposx=%d\r\nposy=%d\r\n",
                          c->fxaa, c->sharpen, c->blur, c->screen, c->brightness, c->zoom, c->posX, c->posY);
        if (r > 0 && n + r < size) n += r;
    }
    {
        int p, in, src;
        for (p = 0; p < PAD_PLAYERS; p++)
        {
            char prefix[4] = "";
            int r;
            if (p) _snprintf(prefix, sizeof(prefix), "p%d_", p + 1);
            for (in = 0; in < PADIN_COUNT; in++)
            {
                char list[96];
                int len = 0;
                list[0] = 0;
                for (src = 1; src < PADSRC_COUNT; src++)
                    if (c->padMap[p][in] & (1u << src))
                    {
                        r = _snprintf(list + len, sizeof(list) - len, "%s%s", len ? "+" : "", s_padSrcKey[src]);
                        if (r > 0 && len + r < (int)sizeof(list)) len += r;
                    }
                r = _snprintf(out + n, size - n, "%s%s=%s\r\n", prefix, s_padInKey[in], len ? list : "none");
                if (r > 0 && n + r < size) n += r;
            }
            r = _snprintf(out + n, size - n, "%sstick=%s\r\n%sdeadzone=%d\r\n%ssensitivity=%d\r\n%sinvert=%d\r\n", prefix,
                          c->padStick[p] ? "right" : "left", prefix, c->padDead[p], prefix, c->padRange[p], prefix, c->padInvert[p]);
            if (r > 0 && n + r < size) n += r;
        }
        {
            int r = _snprintf(out + n, size - n, "rumble=%d\r\nvsync=%d\r\ntexfilter=%d\r\n", c->rumble, c->vsync, c->texFilter);
            if (r > 0 && n + r < size) n += r;
        }
    }
    if (withLastGame)
    {
        int r = _snprintf(out + n, size - n, "artdownload=%d\r\n", c->artDownload);
        if (r > 0 && n + r < size) n += r;
    }
    if (withLastGame && c->lang[0])
    {
        int r = _snprintf(out + n, size - n, "lang=%s\r\n", c->lang);
        if (r > 0 && n + r < size) n += r;
    }
    if (withLastGame && c->lastGame[0])
    {
        int r = _snprintf(out + n, size - n, "lastgame=%s\r\n", c->lastGame);
        if (r > 0 && n + r < size) n += r;
    }
    out[n] = 0;
    return n;
}
