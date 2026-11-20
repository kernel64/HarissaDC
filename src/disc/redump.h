// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_REDUMP_H
#define EMU_REDUMP_H

#include "../core/types.h"

struct RedumpIndex;

RedumpIndex *redump_index_parse(const char *text);
RedumpIndex *redump_index_load(const char *path);
void redump_index_free(RedumpIndex *ix);
u32 redump_index_count(const RedumpIndex *ix);

int redump_ip_borrowed(const RedumpIndex *ix, const char *product, const char *title);

#endif
