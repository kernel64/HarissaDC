// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_RA_H
#define XB_RA_H

#include "../../core/types.h"
#include "xb_ui.h"

enum { XB_RA_OFF = 0, XB_RA_OUT, XB_RA_BUSY, XB_RA_IN };

void XbRaInit(int enabled, void (*toast)(const char *text, int ms, const char *badge));
void XbRaSetEnabled(int on);
int XbRaState(void);
const char *XbRaUser(void);
void XbRaAccount(void);
int XbRaBusy(void);
void XbRaPump(void);

void XbRaGameStart(const char *image);
void XbRaGameEnd(void);
void XbRaFrame(int cheats);
void XbRaReset(int cheats);
void XbRaStateSaved(const char *statePath);
void XbRaStateLoaded(const char *statePath);

struct XbRaItem
{
    const char *title, *description;
    int points, unlocked;
    char badge[24];
};
int XbRaListBegin(void);
void XbRaListItem(int index, XbRaItem *item);
void XbRaListEnd(void);
void XbRaProgress(int *unlocked, int *total);

void XbRaBadgesPump(IDirect3DDevice9 *dev);
IDirect3DTexture9 *XbRaBadge(const char *name);

struct XbRaTotals
{
    int unlocks, points, games, completed;
};
void XbRaTotalsGet(XbRaTotals *t);
void XbRaSetOnUnlock(void (*fn)(void));
int XbRaUnlocksTitled(const char *text);

void XbRaTestToast(void);
int XbRaSelfTest(void);

#endif
