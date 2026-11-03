// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/config.h"
#include "../../core/log.h"
#include "../../core/types.h"
#include "../../core/version.h"
#include "../../sh4/sh4_jit.h"
#include "xb_audio.h"
#include "xb_codemem.h"
#include "xb_diag.h"
#include "xb_emu.h"
#include "xb_fs.h"
#include "xb_log.h"
#include "xb_lang.h"
#include "xb_menu.h"
#include "xb_pad.h"
#include "xb_ui.h"

static void load_settings(Config *c)
{
    char all[CONFIG_PATH_MAX];
    config_defaults(c);
    SettingsLoad(c, "game:\\" EMU_SHORTNAME ".ini");
    SettingsPaths(NULL, all, NULL, sizeof(all));
    SettingsLoad(c, all);
    if (c->logLevel >= LOG_LEVEL_ERROR && c->logLevel <= LOG_LEVEL_DEBUG) log_set_level(c->logLevel);
    SettingsApplyLanguage(c);
}

static void diag_screen(IDirect3DDevice9 *dev, int level)
{
    DiagResult results[DIAG_MAX_RESULTS];
    int count = xb_diag_run(level, results, DIAG_MAX_RESULTS), i;
    UiInput in;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        float x = UI_SAFE_X + 28, y = 150;
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A)) return;
        if (down & XINPUT_GAMEPAD_X) count = xb_diag_run(1, results, DIAG_MAX_RESULTS);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        UiBrand(UI_SAFE_X, 72, 34);
        UiPanel(UI_SAFE_X, 118, UI_WIDTH - 2 * UI_SAFE_X, 520, 18);
        for (i = 0; i < count; i++)
        {
            const DiagResult *r = &results[i];
            char cut[200];
            D3DCOLOR dot = r->ok > 0 ? D3DCOLOR_XRGB(90, 200, 110) : r->ok == 0 ? D3DCOLOR_XRGB(230, 70, 60) : UI_COL_TEXT3;
            UiCircle(x + 6, y + i * 56, 6, dot);
            UiText(UI_REGULAR, 19, x + 22, y + i * 56, UI_COL_TEXT2, r->name, UI_LEFT);
            UiText(UI_REGULAR, 19, x + 340, y + i * 56, UI_COL_TEXT, r->ok > 0 ? "OK" : r->ok == 0 ? "FAILED" : "not run", UI_LEFT);
            UiTextFit(UI_REGULAR, 16, UI_WIDTH - 2 * UI_SAFE_X - 80, r->detail, cut, sizeof(cut));
            UiText(UI_REGULAR, 16, x + 22, y + i * 56 + 24, UI_COL_TEXT3, cut, UI_LEFT);
        }
        UiScreenFooter("X:Run safe tests|B:Continue");
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
    }
}

int __cdecl main()
{
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    Config cfg;
    int quit = 0;
    char path[CONFIG_PATH_MAX];

    xb_fs_set_data_drive(xb_log_open());
    LOG_INFO("[main] %s %s (Xbox 360), %s-endian, %d-bit pointers", EMU_NAME, EMU_VERSION,
             EMU_HOST_BIG_ENDIAN ? "big" : "little", (int)(sizeof(void *) * 8));
    load_settings(&cfg);
    if (cfg.xinput[0]) XbPadScript(cfg.xinput);

    if (cfg.mountDrives) xb_fs_mount_drives();

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = UI_WIDTH;
    pp.BackBufferHeight = UI_HEIGHT;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.EnableAutoDepthStencil = FALSE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    {
        LOG_ERROR("[main] CreateDevice failed");
        xb_log_close();
        XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
        return 1;
    }
    LOG_INFO("[main] D3D device ready, %dx%d", UI_WIDTH, UI_HEIGHT);
    if (xb_audio_init() != 0) LOG_ERROR("[main] XAudio2 failed");

    if (cfg.diag > 0) diag_screen(dev, cfg.diag);

    if (cfg.autoBoot)
    {
        int r = xb_emu_run(dev, &cfg, cfg.disc);
        quit = r == XB_EMU_QUIT || cfg.exitAfter > 0;
        load_settings(&cfg);
    }
    if (!quit && !cfg.lang[0] && !cfg.uiLang[0])
    {
        int lang = LanguageScreen(dev, LangFromConsole(), 1);
        char all[CONFIG_PATH_MAX];
        SettingsPaths(NULL, all, NULL, sizeof(all));
        SettingsSaveLang(all, LangCode(lang));
        load_settings(&cfg);
    }
    while (!quit)
    {
        if (!GameList(dev, &cfg, path, sizeof(path))) break;
        quit = xb_emu_run(dev, &cfg, path) == XB_EMU_QUIT;
        load_settings(&cfg);
    }

    LOG_INFO("[main] quitting");
    xb_audio_shutdown();
    xb_log_close();
    XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
    return 0;
}
