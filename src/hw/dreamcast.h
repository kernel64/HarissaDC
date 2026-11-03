// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_DREAMCAST_H
#define EMU_DREAMCAST_H

#include "../core/types.h"

int dc_init(void);
void dc_shutdown(void);

int dc_load_bios(const char *bootPath, const char *flashPath, char *error, int errorSize);
void dc_set_bios(const u8 *boot, u32 bootSize, const u8 *flash, u32 flashSize);

void dc_use_hle_bios(int region, int language);
void dc_set_slice(int cycles);
void dc_set_cpu_percent(int percent);
int dc_cpu_percent(void);

int dc_insert_disc(const char *path);

void dc_reset(void);

u32 dc_state_size(void);
int dc_state_save(u8 *buf, u32 size);
u32 dc_state_bytes(const u8 *buf, u32 size);
int dc_state_load(const u8 *buf, u32 size);
int dc_run_frame(void);

#endif
