// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_ART_H
#define XB_ART_H

#include "../../core/types.h"

#define ART_COVER_W 528
#define ART_COVER_H 240
#define ART_GLOW_W 16
#define ART_GLOW_H 10

struct ArtGame
{
    char path[256];
    char product[16];
    char title[129];
    char areas[9];
    char file[64];
};

struct ArtResult
{
    char path[256];
    u8 *cover;
    int w, h;
    u8 *glow;
};

struct ArtStatus
{
    int bulk;
    int done, total;
    int found;
    int offline;
    char busyPath[256];
};

void ArtStart(void);
void ArtSetNetwork(int allowed);
void ArtPause(int paused);
void ArtWant(const ArtGame *games, int count);
int ArtTake(ArtResult *r);
void ArtResultFree(ArtResult *r);
void ArtForget(const char *path);
void ArtForgetAll(void);
void ArtBulkStart(const ArtGame *games, int count);
void ArtGetStatus(ArtStatus *s);
int ArtTakeNote(int *a, int *b);

#endif
