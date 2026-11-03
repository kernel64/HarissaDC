// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_EMU_H
#define XB_EMU_H

#include <xtl.h>

struct Config;

enum { XB_EMU_TO_LIST = 0, XB_EMU_QUIT = 1, XB_EMU_ERROR = -1 };

int xb_emu_run(IDirect3DDevice9 *dev, const Config *cfg, const char *disc);

#endif
