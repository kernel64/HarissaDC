// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "isofs.h"

#include <ctype.h>
#include <string.h>

#include "disc.h"
#include "../core/log.h"

static u32 le32(const u8 *p) { return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

u32 iso_base_fad(Disc *d)
{
    int i;
    if (!d || d->trackCount == 0) return 0;
    if (d->type == DISC_GDROM && d->trackCount >= 3) return d->tracks[2].startFad;
    for (i = (int)d->trackCount - 1; i >= 0; i--)
        if (d->tracks[i].ctrl & 4) return d->tracks[i].startFad;
    return 0;
}

static int name_matches(const u8 *isoName, u32 len, const char *name)
{
    u32 i, n = (u32)strlen(name);
    for (i = 0; i < len; i++)
        if (isoName[i] == ';') { len = i; break; }
    if (len > 0 && isoName[len - 1] == '.') len--;
    if (len != n) return 0;
    for (i = 0; i < n; i++)
        if (toupper(isoName[i]) != toupper((u8)name[i])) return 0;
    return 1;
}

int iso_find_root_file(Disc *d, u32 baseFad, const char *name, u32 *fadOut, u32 *sizeOut)
{
    u8 sector[2048];
    u32 rootLba, rootSize, done;
    if (!d) return -1;
    disc_read(d, baseFad + 16, 1, sector, 2048);
    if (sector[0] != 1 || memcmp(sector + 1, "CD001", 5) != 0)
    {
        LOG_ERROR("[iso] no primary volume descriptor at FAD %u", baseFad + 16);
        return -1;
    }
    rootLba = le32(sector + 156 + 2);
    rootSize = le32(sector + 156 + 10);
    for (done = 0; done < rootSize; done += 2048)
    {
        u32 pos = 0;
        disc_read(d, rootLba + 150 + done / 2048, 1, sector, 2048);
        while (pos < 2048)
        {
            const u8 *rec = sector + pos;
            u32 len = rec[0];
            if (len == 0) break;
            if (len >= 34 && !(rec[25] & 2) && name_matches(rec + 33, rec[32], name))
            {
                *fadOut = le32(rec + 2) + 150;
                *sizeOut = le32(rec + 10);
                return 0;
            }
            pos += len;
        }
    }
    LOG_ERROR("[iso] %s not found in the root directory", name);
    return -1;
}

void iso_list_root(Disc *d, u32 baseFad, void (*fn)(const char *name, u32 fad, u32 size, int dir))
{
    u8 sector[2048];
    u32 rootLba, rootSize, done;
    if (!d) return;
    disc_read(d, baseFad + 16, 1, sector, 2048);
    if (sector[0] != 1 || memcmp(sector + 1, "CD001", 5) != 0) return;
    rootLba = le32(sector + 156 + 2);
    rootSize = le32(sector + 156 + 10);
    for (done = 0; done < rootSize; done += 2048)
    {
        u32 pos = 0;
        disc_read(d, rootLba + 150 + done / 2048, 1, sector, 2048);
        while (pos < 2048)
        {
            const u8 *rec = sector + pos;
            u32 len = rec[0], n;
            char name[64];
            if (len == 0) break;
            n = rec[32] < 63 ? rec[32] : 63;
            memcpy(name, rec + 33, n);
            name[n] = 0;
            if (len >= 34) fn(name, le32(rec + 2) + 150, le32(rec + 10), (rec[25] & 2) != 0);
            pos += len;
        }
    }
}
