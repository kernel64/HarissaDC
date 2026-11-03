// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_DISC_INFO_H
#define EMU_DISC_INFO_H

#include "../core/types.h"

struct Disc;

struct DiscInfo
{
    char title[129];
    char product[11];
    char version[7];
    char date[11];
    char areas[9];
    char maker[17];
    char device[17];
    u32 peripherals;
};

int disc_info_read(Disc *d, DiscInfo *out);
int disc_info_read_file(const char *path, DiscInfo *out);
void disc_info_region(const DiscInfo *info, char *out, int size);

int disc_info_console_region(const DiscInfo *info);
void disc_info_folder(const DiscInfo *info, char *out, int size);

#endif
