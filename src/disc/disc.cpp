// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "disc.h"

#include <stdlib.h>
#include <string.h>

#include "../core/log.h"
#include "chd.h"

static int has_ext(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    return n >= e && _stricmp(path + n - e, ext) == 0;
}

static u64 file_size(FILE *f)
{
    long pos = ftell(f), end;
    fseek(f, 0, SEEK_END);
    end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return end < 0 ? 0 : (u64)end;
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    u64 size;
    if (!f) return NULL;
    size = file_size(f);
    if (size > (1u << 20)) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)size + 1);
    if (buf)
    {
        size_t got = fread(buf, 1, (size_t)size, f);
        buf[got] = 0;
    }
    fclose(f);
    return buf;
}

static void join_path(char *out, size_t size, const char *dir, const char *name)
{
    if (!dir || !dir[0]) _snprintf(out, size, "%s", name);
    else if (strchr(dir, '/') && !strchr(dir, '\\')) _snprintf(out, size, "%s/%s", dir, name);
    else _snprintf(out, size, "%s\\%s", dir, name);
    out[size - 1] = 0;
}

static const char *next_token(const char *p, char *out, size_t outSize)
{
    size_t n = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"')
    {
        p++;
        while (*p && *p != '"' && *p != '\n')
        {
            if (n + 1 < outSize) out[n++] = *p;
            p++;
        }
        if (*p == '"') p++;
    }
    else
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
        {
            if (n + 1 < outSize) out[n++] = *p;
            p++;
        }
    out[n] = 0;
    return p;
}

int disc_parse_gdi(Disc *d, const char *text, const char *dir)
{
    char tok[DISC_PATH_MAX];
    const char *p = text;
    u32 count, i;
    memset(d, 0, sizeof(*d));
    p = next_token(p, tok, sizeof(tok));
    count = (u32)atoi(tok);
    if (count == 0 || count > DISC_MAX_TRACKS) return -1;
    for (i = 0; i < count; i++)
    {
        DiscTrack *t = &d->tracks[i];
        u32 lba;
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') p++;
        p = next_token(p, tok, sizeof(tok)); t->number = (u32)atoi(tok);
        p = next_token(p, tok, sizeof(tok)); lba = (u32)atoi(tok);
        p = next_token(p, tok, sizeof(tok)); t->ctrl = (u32)atoi(tok);
        p = next_token(p, tok, sizeof(tok)); t->sectorSize = (u32)atoi(tok);
        p = next_token(p, tok, sizeof(tok));
        join_path(t->path, sizeof(t->path), dir, tok);
        t->path[sizeof(t->path) - 1] = 0;
        p = next_token(p, tok, sizeof(tok));
        t->fileOffset = (u64)atoi(tok);
        if (t->number != i + 1 || (t->sectorSize != 2352 && t->sectorSize != 2048)) return -1;
        t->startFad = lba + 150;
        t->endFad = t->startFad;
        while (*p && *p != '\n') p++;
    }
    d->trackCount = count;
    d->type = DISC_GDROM;
    return 0;
}

static Disc *open_gdi(const char *path)
{
    char dir[DISC_PATH_MAX], *slash, *text;
    Disc *d;
    u32 i;
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else dir[0] = 0;
    text = read_text(path);
    if (!text)
    {
        LOG_ERROR("[disc] cannot read %s", path);
        return NULL;
    }
    d = (Disc *)calloc(1, sizeof(Disc));
    if (!d || disc_parse_gdi(d, text, dir) != 0)
    {
        LOG_ERROR("[disc] %s: not a valid GDI", path);
        free(text);
        free(d);
        return NULL;
    }
    free(text);
    for (i = 0; i < d->trackCount; i++)
    {
        DiscTrack *t = &d->tracks[i];
        u64 size;
        t->file = fopen(t->path, "rb");
        if (!t->file)
        {
            LOG_ERROR("[disc] cannot open track %u: %s", t->number, t->path);
            disc_close(d);
            return NULL;
        }
        size = file_size(t->file);
        t->endFad = t->startFad + (u32)(size / t->sectorSize) - 1;
        LOG_INFO("[disc] track %u: FAD %u-%u, %s, %u-byte sectors", t->number, t->startFad, t->endFad,
                 t->ctrl & 4 ? "data" : "audio", t->sectorSize);
    }
    d->leadOutFad = d->tracks[d->trackCount - 1].endFad + 1;
    return d;
}

static Disc *open_iso(const char *path)
{
    Disc *d = (Disc *)calloc(1, sizeof(Disc));
    DiscTrack *t;
    u64 size;
    if (!d) return NULL;
    t = &d->tracks[0];
    t->file = fopen(path, "rb");
    if (!t->file)
    {
        LOG_ERROR("[disc] cannot open %s", path);
        free(d);
        return NULL;
    }
    size = file_size(t->file);
    t->number = 1;
    t->ctrl = 4;
    t->sectorSize = 2048;
    t->startFad = 150;
    t->endFad = 150 + (u32)(size / 2048) - 1;
    strncpy(t->path, path, sizeof(t->path) - 1);
    d->trackCount = 1;
    d->type = DISC_CDROM_XA;
    d->leadOutFad = t->endFad + 1;
    LOG_INFO("[disc] ISO image, %u sectors", (u32)(size / 2048));
    return d;
}

static u32 msf_frames(const char *msf)
{
    int m = 0, s = 0, f = 0;
    sscanf(msf, "%d:%d:%d", &m, &s, &f);
    return (u32)((m * 60 + s) * 75 + f);
}

static int open_cue_tracks(Disc *d, const char *text, const char *dir)
{
    char line[512], tok[DISC_PATH_MAX], file[DISC_PATH_MAX];
    const char *p = text;
    u32 cursor = 0, fileStart = 0, fileSectors = 0, firstTrackOfFile = 0, sectorSize = 2352;
    u64 fileSize = 0;
    int highDensity = 0, pendingHd = 0, haveFile = 0;
    FILE *f;
    file[0] = 0;
    while (*p)
    {
        const char *eol = p, *q;
        size_t n;
        while (*eol && *eol != '\n') eol++;
        n = (size_t)(eol - p) < sizeof(line) - 1 ? (size_t)(eol - p) : sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p = *eol ? eol + 1 : eol;
        q = next_token(line, tok, sizeof(tok));
        if (!_stricmp(tok, "REM"))
        {
            next_token(q, tok, sizeof(tok));
            if (!_stricmp(tok, "HIGH-DENSITY")) pendingHd = highDensity = 1;
        }
        else if (!_stricmp(tok, "FILE"))
        {
            next_token(q, tok, sizeof(tok));
            join_path(file, sizeof(file), dir, tok);
            file[sizeof(file) - 1] = 0;
            f = fopen(file, "rb");
            if (!f)
            {
                LOG_ERROR("[disc] cannot open %s", file);
                return -1;
            }
            fileSize = file_size(f);
            fclose(f);
            if (pendingHd) { cursor = 45000; pendingHd = 0; }
            fileStart = cursor;
            firstTrackOfFile = d->trackCount;
            haveFile = 1;
            fileSectors = (u32)(fileSize / 2352);
            cursor = fileStart + fileSectors;
        }
        else if (!_stricmp(tok, "TRACK") && haveFile && d->trackCount < DISC_MAX_TRACKS)
        {
            DiscTrack *t = &d->tracks[d->trackCount++];
            q = next_token(q, tok, sizeof(tok));
            t->number = (u32)atoi(tok);
            next_token(q, tok, sizeof(tok));
            t->ctrl = _stricmp(tok, "AUDIO") ? 4 : 0;
            sectorSize = (strstr(tok, "2048") != NULL) ? 2048 : 2352;
            t->sectorSize = sectorSize;
            fileSectors = (u32)(fileSize / sectorSize);
            cursor = fileStart + fileSectors;
            strncpy(t->path, file, sizeof(t->path) - 1);
            t->startFad = fileStart + 150;
            t->fileOffset = 0;
            t->endFad = fileStart + fileSectors - 1 + 150;
            if (strstr(tok, "MODE2")) d->type = DISC_CDROM_XA;
        }
        else if (!_stricmp(tok, "INDEX") && d->trackCount > 0)
        {
            DiscTrack *t = &d->tracks[d->trackCount - 1];
            q = next_token(q, tok, sizeof(tok));
            if (atoi(tok) == 1)
            {
                u32 off;
                next_token(q, tok, sizeof(tok));
                off = msf_frames(tok);
                t->startFad = fileStart + off + 150;
                t->fileOffset = (u64)off * t->sectorSize;
                if (d->trackCount - 1 > firstTrackOfFile) d->tracks[d->trackCount - 2].endFad = t->startFad - 1;
            }
            else if (atoi(tok) == 0 && d->trackCount - 1 > firstTrackOfFile)
            {
                next_token(q, tok, sizeof(tok));
                d->tracks[d->trackCount - 2].endFad = fileStart + msf_frames(tok) + 150 - 1;
            }
        }
    }
    if (d->trackCount == 0) return -1;
    if (highDensity) d->type = DISC_GDROM;
    else if (d->type != DISC_CDROM_XA) d->type = DISC_CDROM;
    return 0;
}

static Disc *open_cue(const char *path)
{
    char dir[DISC_PATH_MAX], *slash, *text;
    Disc *d;
    u32 i;
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else dir[0] = 0;
    text = read_text(path);
    d = (Disc *)calloc(1, sizeof(Disc));
    if (!text || !d || open_cue_tracks(d, text, dir) != 0)
    {
        LOG_ERROR("[disc] %s: not a usable CUE sheet", path);
        free(text);
        free(d);
        return NULL;
    }
    free(text);
    for (i = 0; i < d->trackCount; i++)
    {
        DiscTrack *t = &d->tracks[i];
        t->file = fopen(t->path, "rb");
        if (!t->file)
        {
            disc_close(d);
            return NULL;
        }
        LOG_INFO("[disc] track %u: FAD %u-%u, %s, %u-byte sectors", t->number, t->startFad, t->endFad,
                 t->ctrl & 4 ? "data" : "audio", t->sectorSize);
    }
    d->leadOutFad = d->tracks[d->trackCount - 1].endFad + 1;
    LOG_INFO("[disc] %s, %u tracks", d->type == DISC_GDROM ? "GD-ROM" : "CD", d->trackCount);
    return d;
}

static u32 chd_sector_size(const char *type)
{
    if (!strcmp(type, "MODE1") || !strcmp(type, "MODE1/2048") || !strcmp(type, "MODE2_FORM1")) return 2048;
    if (!strcmp(type, "MODE2") || !strcmp(type, "MODE2/2336") || !strcmp(type, "MODE2_FORM_MIX")) return 2336;
    return 2352;
}

static int s_readAheadThread = -2;

void disc_set_read_ahead(int hwThread) { s_readAheadThread = hwThread; }

static Disc *open_chd(const char *path)
{
    const u32 sessionGap = 6750 + 4500 + 150;
    Disc *d = (Disc *)calloc(1, sizeof(Disc));
    u32 total = 150, stored = 0, lastPregap = 0;
    int gd = 0;
    if (!d) return NULL;
    d->chd = chd_open(path);
    if (!d->chd)
    {
        free(d);
        return NULL;
    }
    if (chd_hunk_bytes(d->chd) % CHD_FRAME_BYTES)
    {
        LOG_ERROR("[disc] %s: not a CD or GD-ROM CHD", path);
        disc_close(d);
        return NULL;
    }
    d->type = DISC_CDROM;
    while (d->trackCount < DISC_MAX_TRACKS)
    {
        char meta[256], type[32], sub[32], pgtype[32], pgsub[32];
        int num = 0, frames = 0, pad = 0, pregap = 0, postgap = 0, kind;
        u32 inFile;
        DiscTrack *t = &d->tracks[d->trackCount];
        strcpy(pgtype, "NONE");
        type[0] = 0;
        if (chd_metadata(d->chd, "CHT2", (int)d->trackCount, meta, sizeof(meta)) >= 0)
        {
            kind = 2;
            sscanf(meta, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                   &num, type, sub, &frames, &pregap, pgtype, pgsub, &postgap);
        }
        else if (chd_metadata(d->chd, "CHTR", (int)d->trackCount, meta, sizeof(meta)) >= 0)
        {
            kind = 1;
            sscanf(meta, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d", &num, type, sub, &frames);
        }
        else if ((kind = 3, chd_metadata(d->chd, "CHGD", (int)d->trackCount, meta, sizeof(meta)) >= 0) ||
                 (kind = 4, chd_metadata(d->chd, "CHGT", (int)d->trackCount, meta, sizeof(meta)) >= 0))
        {
            gd = 1;
            sscanf(meta, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PAD:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                   &num, type, sub, &frames, &pad, &pregap, pgtype, pgsub, &postgap);
        }
        else
            break;
        if (num != (int)d->trackCount + 1 || frames <= 0 || pregap < 0)
        {
            LOG_ERROR("[disc] %s: bad track entry \"%s\"", path, meta);
            disc_close(d);
            return NULL;
        }
        inFile = 0;
        if (pgtype[0] == 'V') inFile = (u32)(pregap < frames ? pregap : frames);
        t->number = (u32)num;
        t->ctrl = strcmp(type, "AUDIO") ? 4 : 0;
        t->sectorSize = chd_sector_size(type);
        t->startFad = total + (u32)pregap;
        total = t->startFad + (u32)frames - inFile;
        t->endFad = total - 1 - (u32)pad;
        t->chdOffset = (s32)(stored + inFile) - (s32)t->startFad;
        t->chdSwap = t->ctrl == 0 && kind != 4;
        strncpy(t->path, path, sizeof(t->path) - 1);
        if (strstr(type, "MODE2") || strstr(type, "CDI")) d->type = DISC_CDROM_XA;
        stored += ((u32)frames + 3) / 4 * 4;
        lastPregap = (u32)pregap;
        d->trackCount++;
    }
    if (!d->trackCount)
    {
        LOG_ERROR("[disc] %s: no track in the CHD", path);
        disc_close(d);
        return NULL;
    }
    if (gd)
    {
        d->type = DISC_GDROM;
        if (d->trackCount < 3) LOG_WARN("[disc] %s: GD-ROM with %u tracks", path, d->trackCount);
    }
    else if (d->trackCount > 1 && (d->tracks[d->trackCount - 1].ctrl & 4))
    {
        DiscTrack *t = &d->tracks[d->trackCount - 1];
        u32 gap = sessionGap - (lastPregap < 150 ? lastPregap : 150);
        t->startFad += gap;
        t->endFad += gap;
        t->chdOffset -= (s32)gap;
        d->type = DISC_CDROM_XA;
    }
    {
        u32 i;
        for (i = 0; i < d->trackCount; i++)
        {
            DiscTrack *t = &d->tracks[i];
            LOG_INFO("[disc] track %u: FAD %u-%u, %s, %u-byte sectors", t->number, t->startFad, t->endFad,
                     t->ctrl & 4 ? "data" : "audio", t->sectorSize);
        }
    }
    d->leadOutFad = d->tracks[d->trackCount - 1].endFad + 1;
    LOG_INFO("[disc] CHD image, %s, %u tracks", d->type == DISC_GDROM ? "GD-ROM" : "CD", d->trackCount);
    if (s_readAheadThread != -2) chd_start_read_ahead(d->chd, s_readAheadThread);
    return d;
}

Disc *disc_open(const char *path)
{
    if (has_ext(path, ".gdi")) return open_gdi(path);
    if (has_ext(path, ".iso")) return open_iso(path);
    if (has_ext(path, ".cue")) return open_cue(path);
    if (has_ext(path, ".chd")) return open_chd(path);
    LOG_ERROR("[disc] unsupported image format: %s", path);
    return NULL;
}

void disc_close(Disc *d)
{
    u32 i;
    if (!d) return;
    for (i = 0; i < d->trackCount; i++)
        if (d->tracks[i].file) fclose(d->tracks[i].file);
    if (d->chd) chd_close(d->chd);
    free(d);
}

DiscTrack *disc_track_at(Disc *d, u32 fad)
{
    u32 i;
    if (!d) return NULL;
    for (i = 0; i < d->trackCount; i++)
        if (fad >= d->tracks[i].startFad && fad <= d->tracks[i].endFad) return &d->tracks[i];
    return NULL;
}

static u32 user_data_offset(const u8 *raw)
{
    return raw[15] == 2 ? 24 : 16;
}

u32 disc_read(Disc *d, u32 fad, u32 count, u8 *out, u32 format)
{
    u8 raw[2352];
    u32 i, done = 0;
    for (i = 0; i < count; i++, fad++)
    {
        DiscTrack *t = disc_track_at(d, fad);
        u8 *dst = out + i * format;
        if (!t || (!t->file && !d->chd))
        {
            memset(dst, 0, format);
            continue;
        }
        if (d->chd)
        {
            if (chd_read_frame(d->chd, (u32)((s32)fad + t->chdOffset), raw))
            {
                memset(dst, 0, format);
                continue;
            }
            if (t->chdSwap)
            {
                u32 k;
                for (k = 0; k < 2352; k += 2)
                {
                    u8 x = raw[k];
                    raw[k] = raw[k + 1];
                    raw[k + 1] = x;
                }
            }
        }
        else if (fseek(t->file, (long)(t->fileOffset + (u64)(fad - t->startFad) * t->sectorSize), SEEK_SET) != 0 ||
                 fread(raw, 1, t->sectorSize, t->file) != t->sectorSize)
        {
            memset(dst, 0, format);
            continue;
        }
        if (format == t->sectorSize)
            memcpy(dst, raw, format);
        else if (format == 2048 && t->sectorSize == 2352)
            memcpy(dst, raw + user_data_offset(raw), 2048);
        else if (format == 2048 && t->sectorSize == 2336)
            memcpy(dst, raw + 8, 2048);
        else
            memset(dst, 0, format);
        done++;
    }
    return done;
}
