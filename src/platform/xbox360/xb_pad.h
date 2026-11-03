// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_PAD_H
#define XB_PAD_H

#include <xtl.h>

#include "../../core/types.h"

int XbPadScript(const char *text);
void XbPadGameStarts(void);
DWORD XbPadGet(DWORD user, XINPUT_STATE *state);

int XbPadSource(const XINPUT_GAMEPAD *g, int src);
int XbPadSources(const XINPUT_GAMEPAD *g, u32 mask);
u32 XbPadPressed(const XINPUT_GAMEPAD *g);

#endif
