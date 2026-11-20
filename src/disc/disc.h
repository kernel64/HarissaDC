// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_DISC_H
#define EMU_DISC_H

#include <stdio.h>

#include "../core/types.h"

#define DISC_MAX_TRACKS 99
#define DISC_PATH_MAX 260
#define DISC_MAX_SESSIONS 99

enum DiscType { DISC_NONE = 0, DISC_CDDA = 0x00, DISC_CDROM = 0x10, DISC_CDROM_XA = 0x20, DISC_GDROM = 0x80 };

struct DiscTrack
{
    u32 number;
    u32 startFad;
    u32 endFad;
    u32 ctrl;
    u32 sectorSize;
    u32 sectorStride;
    u64 fileOffset;
    char path[DISC_PATH_MAX];
    FILE *file;
    s32 chdOffset;
    int chdSwap;
};

struct ChdFile;

struct Disc
{
    int type;
    u32 trackCount;
    DiscTrack tracks[DISC_MAX_TRACKS];
    u32 leadOutFad;
    ChdFile *chd;
    u32 sessionCount;
    u8 sessionFirstTrack[DISC_MAX_SESSIONS];
};

Disc *disc_open(const char *path);
void disc_set_read_ahead(int hwThread);
void disc_close(Disc *d);

u32 disc_read(Disc *d, u32 fad, u32 count, u8 *out, u32 format);

DiscTrack *disc_track_at(Disc *d, u32 fad);

int disc_parse_gdi(Disc *d, const char *text, const char *dir);

int disc_parse_cdi(Disc *d, FILE *f, u64 size, const char *path);

#endif
