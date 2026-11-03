// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_MENU_H
#define XB_MENU_H

#include <xtl.h>

#include "../../core/config.h"

#define UI_MENU_MAX 16

struct UiMenu
{
    char title[128];
    char label[UI_MENU_MAX][96];
    char value[UI_MENU_MAX][64];
    int id[UI_MENU_MAX];
    int count, sel;
    char footer[192];
};

void UiMenuClear(UiMenu *m, const char *title, const char *footer);
int UiMenuAdd(UiMenu *m, int id, const char *label, const char *value);
void UiMenuDraw(const UiMenu *m);
void UiMenuNavigate(UiMenu *m, WORD down);
int UiMenuSelected(const UiMenu *m);

void SettingsPaths(const char *gameFolder, char *all, char *game, int size);
void SettingsLoad(Config *c, const char *path);
int SettingsSave(const Config *c, const char *path, int withLastGame);

enum { SET_NONE = 0, SET_CHANGED, SET_BACK, SET_SAVED };
typedef void (*UiBackgroundFn)(void *user);
int SettingsScreen(IDirect3DDevice9 *dev, Config *c, const char *gameFolder, UiBackgroundFn background, void *user);

int SettingsSaveLang(const char *path, const char *code);
void SettingsApplyLanguage(const Config *c);

int LanguageScreen(IDirect3DDevice9 *dev, int initial, int firstStart);

void AboutDraw(void);
void AboutScreen(IDirect3DDevice9 *dev, UiBackgroundFn background, void *user);
int ConfirmScreen(IDirect3DDevice9 *dev, const char *question, const char *yes, UiBackgroundFn background, void *user);

const char *xb_cache_dir(void);
#define XB_CACHE_DIR xb_cache_dir()
int ArchivePrepare(IDirect3DDevice9 *dev, const char *path, char *image, int imageSize, char *error, int errorSize,
                   UiBackgroundFn background, void *user);

int GameList(IDirect3DDevice9 *dev, Config *c, char *path, int pathSize);

#endif
