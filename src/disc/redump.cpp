// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "redump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RD_SERIAL 24
#define RD_TITLE 96

struct RedumpEntry
{
    char serial[RD_SERIAL];
    char title[RD_TITLE];
};

struct RedumpIndex
{
    RedumpEntry *e;
    u32 count, cap;
};

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int alnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

static void squash(const char *s, size_t n, char *out, size_t size)
{
    size_t i, k = 0;
    for (i = 0; i < n && s[i] && k + 1 < size; i++)
        if (alnum(s[i])) out[k++] = up(s[i]);
    out[k] = 0;
}

static void norm_serial(const char *s, size_t n, char *out, size_t size)
{
    squash(s, n, out, size);
    if (out[0] == 'M' && out[1] == 'K' && out[2]) memmove(out, out + 2, strlen(out + 2) + 1);
}

static void add(RedumpIndex *ix, const char *serial, const char *title)
{
    RedumpEntry *e;
    if (!serial[0] || !title[0]) return;
    if (ix->count == ix->cap)
    {
        u32 cap = ix->cap ? ix->cap * 2 : 1024;
        RedumpEntry *n = (RedumpEntry *)realloc(ix->e, cap * sizeof(RedumpEntry));
        if (!n) return;
        ix->e = n;
        ix->cap = cap;
    }
    e = &ix->e[ix->count++];
    strncpy(e->serial, serial, RD_SERIAL - 1);
    e->serial[RD_SERIAL - 1] = 0;
    strncpy(e->title, title, RD_TITLE - 1);
    e->title[RD_TITLE - 1] = 0;
}

static const char *field(const char *line, const char *end, const char *key, size_t *len)
{
    size_t k = strlen(key);
    const char *q;
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if ((size_t)(end - line) < k + 2 || strncmp(line, key, k) || line[k] != ' ' || line[k + 1] != '"') return NULL;
    line += k + 2;
    for (q = line; q < end && *q != '"'; q++) {}
    *len = (size_t)(q - line);
    return line;
}

RedumpIndex *redump_index_parse(const char *text)
{
    RedumpIndex *ix = (RedumpIndex *)calloc(1, sizeof(RedumpIndex));
    const char *p = text;
    char title[RD_TITLE] = "", serials[160] = "";
    int in = 0;
    if (!ix) return NULL;
    while (*p)
    {
        const char *end = p + strcspn(p, "\r\n"), *v;
        size_t n;
        if (!strncmp(p, "game (", 6))
        {
            in = 1;
            title[0] = serials[0] = 0;
        }
        else if (in && end - p == 1 && *p == ')')
        {
            const char *s = serials;
            while (*s)
            {
                size_t k = strcspn(s, ",/");
                char one[RD_SERIAL];
                norm_serial(s, k, one, sizeof(one));
                add(ix, one, title);
                s += k;
                if (*s) s++;
            }
            in = 0;
        }
        else if (in && !title[0] && (v = field(p, end, "name", &n)) != NULL)
        {
            size_t k;
            for (k = 0; k + 1 < n && !(v[k] == ' ' && v[k + 1] == '('); k++) {}
            if (k + 1 >= n) k = n;
            if (k >= sizeof(title)) k = sizeof(title) - 1;
            memcpy(title, v, k);
            title[k] = 0;
        }
        else if (in && (v = field(p, end, "serial", &n)) != NULL)
        {
            if (n >= sizeof(serials)) n = sizeof(serials) - 1;
            memcpy(serials, v, n);
            serials[n] = 0;
        }
        p = end + strspn(end, "\r\n");
    }
    if (!ix->count)
    {
        redump_index_free(ix);
        return NULL;
    }
    return ix;
}

RedumpIndex *redump_index_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    RedumpIndex *ix = NULL;
    char *text;
    long size;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size < (32l << 20) && (text = (char *)malloc((size_t)size + 1)) != NULL)
    {
        size_t got = fread(text, 1, (size_t)size, f);
        text[got] = 0;
        ix = redump_index_parse(text);
        free(text);
    }
    fclose(f);
    return ix;
}

void redump_index_free(RedumpIndex *ix)
{
    if (!ix) return;
    free(ix->e);
    free(ix);
}

u32 redump_index_count(const RedumpIndex *ix) { return ix ? ix->count : 0; }

static int next_word(const char **p, char *out, size_t size)
{
    static const char *generic[] = { "SEGA", "DREAMCAST", "GAME", "GAMES", "EDITION", "THE", "VERSION", "DISC", "FOR", "AND", "VOL" };
    for (;;)
    {
        size_t k = 0, i;
        int skip = 0;
        while (**p && !alnum(**p)) (*p)++;
        if (!**p) return 0;
        while (alnum(**p))
        {
            if (k + 1 < size) out[k++] = up(**p);
            (*p)++;
        }
        out[k] = 0;
        if (k < 3) continue;
        for (i = 0; i < sizeof(generic) / sizeof(generic[0]); i++)
            if (!strcmp(out, generic[i])) skip = 1;
        if (!skip) return 1;
    }
}

static int titles_match(const char *header, const char *redump)
{
    char a[160], b[RD_TITLE], wa[64], wb[64];
    size_t la, lb, k = 0;
    const char *p = header;
    squash(header, strlen(header), a, sizeof(a));
    squash(redump, strlen(redump), b, sizeof(b));
    la = strlen(a);
    lb = strlen(b);
    if (la >= 3 && lb >= 3 && (strstr(a, b) || strstr(b, a))) return 1;
    while (k < la && k < lb && a[k] == b[k]) k++;
    if (k >= 3 && (k >= 6 || k == la || k == lb)) return 1;
    while (next_word(&p, wa, sizeof(wa)))
    {
        const char *q = redump;
        while (next_word(&q, wb, sizeof(wb)))
            if (!strcmp(wa, wb)) return 1;
    }
    return 0;
}

int redump_ip_borrowed(const RedumpIndex *ix, const char *product, const char *title)
{
    char serial[RD_SERIAL];
    u32 i;
    int known = 0;
    if (!ix || !product || !title) return 0;
    norm_serial(product, strlen(product), serial, sizeof(serial));
    if (!serial[0]) return 0;
    for (i = 0; i < ix->count; i++)
    {
        if (strcmp(ix->e[i].serial, serial)) continue;
        known = 1;
        if (titles_match(title, ix->e[i].title)) return 0;
    }
    return known;
}
