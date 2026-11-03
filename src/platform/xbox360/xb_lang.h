// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_LANG_H
#define XB_LANG_H

#include "xb_lang_table.h"

enum { LANG_EN = 0, LANG_ES, LANG_DE, LANG_FR, LANG_AR, LANG_PT, LANG_RU };

const char *Tr(int id);
const char *TrIn(int lang, int id);

int LangGet(void);
void LangSet(int lang);
int LangRtl(void);
const char *LangCode(int lang);
int LangFromCode(const char *code);
int LangFromConsole(void);

const char *Trf(int id, ...);

#endif
