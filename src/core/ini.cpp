// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "ini.h"

#include <stdlib.h>
#include <string.h>

#define INI_LINE_MAX 512

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

int ini_key_is(const char *key, const char *name)
{
    while (*key && *name)
    {
        if (lower((unsigned char)*key) != lower((unsigned char)*name)) return 0;
        key++;
        name++;
    }
    return *key == 0 && *name == 0;
}

int ini_bool(const char *v, int fallback)
{
    if (ini_key_is(v, "1") || ini_key_is(v, "on") || ini_key_is(v, "yes") || ini_key_is(v, "true")) return 1;
    if (ini_key_is(v, "0") || ini_key_is(v, "off") || ini_key_is(v, "no") || ini_key_is(v, "false")) return 0;
    return fallback;
}

int ini_int(const char *v, int fallback)
{
    char *end;
    long n;
    if (!*v) return fallback;
    n = strtol(v, &end, 0);
    return *end ? fallback : (int)n;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static int parse_section(const char *s, char *out, size_t outSize)
{
    size_t n = 0;
    while (*s == '[')
    {
        const char *close = strchr(s + 1, ']');
        size_t len;
        if (!close) return 0;
        len = (size_t)(close - (s + 1));
        if (n && n + 1 < outSize) out[n++] = '\n';
        if (n + len >= outSize) len = outSize - 1 - n;
        memcpy(out + n, s + 1, len);
        n += len;
        s = close + 1;
        while (*s == ' ' || *s == '\t') s++;
    }
    out[n] = 0;
    return n > 0 && (*s == 0 || *s == ';' || *s == '#');
}

int ini_parse(const char *text, IniCallback cb, void *user)
{
    char line[INI_LINE_MAX], section[INI_LINE_MAX];
    int lineNo = 0, bad = 0, haveSection = 0;
    while (*text)
    {
        const char *eol = text;
        size_t len;
        char *s, *eq, *key, *value, *c;
        while (*eol && *eol != '\n') eol++;
        len = (size_t)(eol - text);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, text, len);
        line[len] = 0;
        text = *eol ? eol + 1 : eol;
        lineNo++;

        s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[')
        {
            if (parse_section(s, section, sizeof(section))) haveSection = 1;
            else bad++;
            continue;
        }
        eq = strchr(s, '=');
        if (!eq || eq == s) { bad++; continue; }
        *eq = 0;
        key = trim(s);
        value = eq + 1;
        for (c = value; *c; c++)
            if ((*c == ';' || *c == '#') && c > value && (c[-1] == ' ' || c[-1] == '\t')) { *c = 0; break; }
        value = trim(value);
        if (!cb(user, haveSection ? section : NULL, key, value, lineNo)) break;
    }
    return bad;
}
