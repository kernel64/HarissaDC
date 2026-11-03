// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "disc_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disc.h"
#include "isofs.h"

static void field(char *out, const u8 *p, int n)
{
    int i;
    for (i = 0; i < n; i++) out[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : ' ';
    out[n] = 0;
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

int disc_info_read(Disc *d, DiscInfo *out)
{
    u8 sector[2048];
    u32 base;
    char date[9];
    memset(out, 0, sizeof(*out));
    if (!d) return -1;
    base = iso_base_fad(d);
    if (!base || disc_read(d, base, 1, sector, 2048) != 1) return -1;
    if (memcmp(sector, "SEGA", 4) != 0) return -1;
    field(out->maker, sector + 0x10, 16);
    field(out->device, sector + 0x20, 16);
    {
        char *sp = strchr(out->device, ' ');
        if (sp) memmove(out->device, sp + 1, strlen(sp));
    }
    field(out->areas, sector + 0x30, 8);
    {
        char per[9];
        field(per, sector + 0x38, 8);
        out->peripherals = (u32)strtoul(per, NULL, 16);
    }
    field(out->product, sector + 0x40, 10);
    field(out->version, sector + 0x4A, 6);
    field(date, sector + 0x50, 8);
    if (strlen(date) == 8)
        sprintf(out->date, "%.4s-%.2s-%.2s", date, date + 4, date + 6);
    field(out->title, sector + 0x80, 128);
    if (!out->title[0]) strcpy(out->title, out->product);
    return 0;
}

int disc_info_read_file(const char *path, DiscInfo *out)
{
    Disc *d = disc_open(path);
    int r;
    if (!d)
    {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    r = disc_info_read(d, out);
    disc_close(d);
    return r;
}

void disc_info_region(const DiscInfo *info, char *out, int size)
{
    static const struct { char c; const char *name; } areas[] = { { 'U', "USA" }, { 'J', "Japan" }, { 'E', "Europe" } };
    int i, n = 0;
    out[0] = 0;
    for (i = 0; i < 3; i++)
        if (strchr(info->areas, areas[i].c))
        {
            int r = _snprintf(out + n, size - n, "%s%s", n ? ", " : "", areas[i].name);
            if (r < 0 || n + r >= size) break;
            n += r;
        }
    out[size - 1] = 0;
}

int disc_info_console_region(const DiscInfo *info)
{
    enum { JAPAN = 0, USA = 1, EUROPE = 2 };
    const char *p = info->product;
    size_t n = strlen(p);
    int allowed[3], home = -1;
    allowed[JAPAN] = strchr(info->areas, 'J') != NULL;
    allowed[USA] = strchr(info->areas, 'U') != NULL;
    allowed[EUROPE] = strchr(info->areas, 'E') != NULL;
    if (n >= 2 && p[n - 2] == '5' && p[n - 1] == '0') home = EUROPE;
    else if (!strncmp(p, "HDR", 3) || (n && p[n - 1] == 'M')) home = JAPAN;
    else if (!strncmp(p, "MK-51", 5) || (n && p[n - 1] == 'N')) home = USA;
    if (home >= 0 && allowed[home]) return home;
    if (allowed[USA]) return USA;
    if (allowed[JAPAN]) return JAPAN;
    if (allowed[EUROPE]) return EUROPE;
    return home >= 0 ? home : USA;
}

void disc_info_folder(const DiscInfo *info, char *out, int size)
{
    const char *src = info->product[0] ? info->product : info->title;
    int n = 0;
    for (; *src && n < size - 1; src++)
    {
        char c = *src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out[n++] = c;
        else if (c == ' ' && n && out[n - 1] != '_') out[n++] = '_';
    }
    if (!n) out[n++] = '_';
    out[n] = 0;
}
