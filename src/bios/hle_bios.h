// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_HLE_BIOS_H
#define EMU_HLE_BIOS_H

#include "../core/types.h"

enum HleRegion { HLE_REGION_JAPAN = 0, HLE_REGION_USA = 1, HLE_REGION_EUROPE = 2 };
enum HleLanguage { HLE_LANG_JAPANESE = 0, HLE_LANG_ENGLISH, HLE_LANG_GERMAN, HLE_LANG_FRENCH, HLE_LANG_SPANISH, HLE_LANG_ITALIAN };

void hle_bios_install(int region, int language);
void hle_bios_reset(void);
int hle_bios_boot_status(void);

void hle_descramble(const u8 *src, u8 *dst, u32 size);
void hle_scramble(const u8 *src, u8 *dst, u32 size);

struct StateIO;
void hle_bios_state(StateIO *io);

#endif
