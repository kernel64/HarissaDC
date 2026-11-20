// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_CHEAT_H
#define EMU_CHEAT_H

#include "../core/types.h"

struct CheatList;

struct CheatMem
{
    u32 (*read)(u32 offset, int size);
    void (*write)(u32 offset, u32 value, int size);
};

CheatList *cheat_new(void);
void cheat_free(CheatList *l);
void cheat_set_mem(CheatList *l, const CheatMem *m);

int cheat_load_cht(CheatList *l, const char *text, u32 len);
int cheat_load_codes(CheatList *l, const char *text, u32 len);
int cheat_load_db(CheatList *l, const char *text, u32 len, const char *product);

int cheat_count(const CheatList *l);
const char *cheat_name(const CheatList *l, int i);
int cheat_supported(const CheatList *l, int i);
int cheat_enabled(const CheatList *l, int i);
void cheat_enable(CheatList *l, int i, int on);
int cheat_active(const CheatList *l);

void cheat_apply(CheatList *l);

void cheat_states_write(const CheatList *l, char *out, int size);
void cheat_states_read(CheatList *l, const char *text);

#endif
