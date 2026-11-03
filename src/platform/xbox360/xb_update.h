// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_UPDATE_H
#define XB_UPDATE_H

#include <stddef.h>

void UpdateCheckStart(const char *current);

int UpdateCheckTake(char *version, size_t size);

#endif
