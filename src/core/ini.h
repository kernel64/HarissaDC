// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_INI_H
#define EMU_INI_H

#include "types.h"

typedef int (*IniCallback)(void *user, const char *section, const char *key, const char *value, int line);

int ini_parse(const char *text, IniCallback cb, void *user);

int ini_key_is(const char *key, const char *name);
int ini_bool(const char *value, int fallback);
int ini_int(const char *value, int fallback);

#endif
