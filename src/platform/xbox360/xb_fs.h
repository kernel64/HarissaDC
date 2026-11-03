// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_FS_H
#define XB_FS_H

#include "../../core/types.h"

void xb_fs_mount_drives(void);

#define XB_FS_MAX_DRIVES 8
int xb_fs_list_drives(char names[][8], int max);

struct XbDirEntry
{
    char name[64];
    int isDir;
    u64 size;
};

int xb_fs_list_dir(const char *dir, XbDirEntry *entries, int max);

void xb_fs_join(char *out, int outSize, const char *dir, const char *name);
int xb_fs_parent(char *path);
int xb_fs_exists(const char *path);

void xb_fs_set_data_drive(const char *drive);
const char *xb_fs_data_dir(char *out, int outSize, const char *dir);
const char *xb_fs_data_path(char *out, int outSize, const char *dir, const char *name);

#endif
