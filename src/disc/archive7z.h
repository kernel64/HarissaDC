// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_ARCHIVE7Z_H
#define EMU_ARCHIVE7Z_H

#include "../core/types.h"

#define ARCHIVE7Z_NAME_MAX 260
#define ARCHIVE7Z_SAFE_NAME_MAX 42
#define ARCHIVE7Z_MEMORY_BLOCK (64u << 20)

struct Archive7zEntry
{
    char name[ARCHIVE7Z_NAME_MAX];
    u64 size;
    int isDir;
};

typedef int (*Archive7zProgress)(void *user, u64 done, u64 total);

int archive7z_list(const char *path, Archive7zEntry *out, int max, char *error, int errorSize);

int archive7z_extract(const char *path, const char *outDir, Archive7zProgress progress, void *user, char *error,
                      int errorSize);

void archive7z_make_dirs(const char *path);

void archive7z_safe_name(const char *in, int index, int maxLen, char *out, int outSize);

#endif
