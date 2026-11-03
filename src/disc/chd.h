// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_CHD_H
#define EMU_CHD_H

#include "../core/types.h"

#define CHD_FRAME_BYTES 2448

struct ChdFile;

ChdFile *chd_open(const char *path);
void chd_close(ChdFile *c);

u32 chd_hunk_bytes(const ChdFile *c);
u32 chd_hunk_count(const ChdFile *c);
int chd_read_hunk(ChdFile *c, u32 n, u8 *out);
int chd_read_frame(ChdFile *c, u32 frame, u8 *out);

int chd_start_read_ahead(ChdFile *c, int hwThread);

int chd_metadata(ChdFile *c, const char *tag, int index, char *out, int size);

void chd_stats(const ChdFile *c, u32 perCodec[4], u32 *none, u32 *self);

int chd_flac_decode(const u8 *src, u32 len, u8 *dst, u32 samples);

void chd_cd_ecc_generate(u8 *sector);

#endif
