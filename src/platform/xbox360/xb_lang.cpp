// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#define XB_LANG_DEFINE_TABLES
#include "xb_lang.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <xtl.h>

static int s_lang = LANG_EN;

const char *TrIn(int lang, int id)
{
    if (lang < 0 || lang >= LANG_COUNT) lang = LANG_EN;
    if (id < 0 || id >= T_COUNT) return "";
    return s_langText[lang][id];
}

const char *Tr(int id) { return TrIn(s_lang, id); }
int LangGet(void) { return s_lang; }
void LangSet(int lang) { s_lang = lang >= 0 && lang < LANG_COUNT ? lang : LANG_EN; }
int LangRtl(void) { return s_lang == LANG_AR; }
const char *LangCode(int lang) { return s_langCode[lang >= 0 && lang < LANG_COUNT ? lang : LANG_EN]; }

int LangFromCode(const char *code)
{
    int i;
    for (i = 0; i < LANG_COUNT; i++)
        if (!_stricmp(code, s_langCode[i])) return i;
    return -1;
}

int LangFromConsole(void)
{
    switch (XGetLanguage())
    {
    case XC_LANGUAGE_SPANISH: return LANG_ES;
    case XC_LANGUAGE_GERMAN: return LANG_DE;
    case XC_LANGUAGE_FRENCH: return LANG_FR;
    case XC_LANGUAGE_PORTUGUESE: return LANG_PT;
    case XC_LANGUAGE_RUSSIAN: return LANG_RU;
    }
    return LANG_EN;
}

const char *Trf(int id, ...)
{
    static char bufs[4][256];
    static int next;
    char *b = bufs[next];
    va_list ap;
    next = (next + 1) & 3;
    va_start(ap, id);
    _vsnprintf(b, sizeof(bufs[0]), Tr(id), ap);
    va_end(ap);
    b[sizeof(bufs[0]) - 1] = 0;
    return b;
}
