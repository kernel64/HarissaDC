// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "archivezip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive7z.h"
#include "../third_party/zlib/zlib.h"

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

#define BUF_SIZE (1u << 18)
#define SIG_END 0x06054B50u
#define SIG_CENTRAL 0x02014B50u
#define SIG_LOCAL 0x04034B50u

static void set_error(char *error, int size, const char *text)
{
    if (error && size > 0)
    {
        strncpy(error, text, size - 1);
        error[size - 1] = 0;
    }
}

static int seek64(FILE *f, s64 pos, int whence)
{
#ifdef _MSC_VER
    return _fseeki64(f, pos, whence);
#else
    return fseeko(f, (off_t)pos, whence);
#endif
}

static s64 tell64(FILE *f)
{
#ifdef _MSC_VER
    return _ftelli64(f);
#else
    return (s64)ftello(f);
#endif
}

static u32 le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static voidpf z_alloc(voidpf, uInt n, uInt size) { return malloc((size_t)n * size); }
static void z_free(voidpf, voidpf p) { free(p); }

static int find_directory(FILE *f, u32 *start, u32 *size, u32 *count)
{
    u8 *buf;
    s64 len, from;
    int n, i;
    if (seek64(f, 0, SEEK_END) != 0) return -1;
    len = tell64(f);
    if (len < 22) return -1;
    from = len > 65557 ? len - 65557 : 0;
    n = (int)(len - from);
    buf = (u8 *)malloc(n);
    if (!buf) return -1;
    if (seek64(f, from, SEEK_SET) != 0 || fread(buf, 1, n, f) != (size_t)n)
    {
        free(buf);
        return -1;
    }
    for (i = n - 22; i >= 0; i--)
        if (le32(buf + i) == SIG_END)
        {
            *count = le16(buf + i + 10);
            *size = le32(buf + i + 12);
            *start = le32(buf + i + 16);
            free(buf);
            return *start == 0xFFFFFFFFu ? -1 : 0;
        }
    free(buf);
    return -1;
}

struct ZipEntry
{
    char name[ARCHIVE7Z_NAME_MAX];
    u32 method, packed, size, local;
};

static int read_entries(FILE *f, ZipEntry **out, char *error, int errorSize)
{
    u32 start, size, count, i, pos = 0;
    u8 *dir;
    ZipEntry *e;
    *out = NULL;
    if (find_directory(f, &start, &size, &count) != 0)
    {
        set_error(error, errorSize, "not a zip archive (or a ZIP64 one)");
        return -1;
    }
    dir = (u8 *)malloc(size ? size : 1);
    e = (ZipEntry *)calloc(count ? count : 1, sizeof(ZipEntry));
    if (!dir || !e || seek64(f, start, SEEK_SET) != 0 || fread(dir, 1, size, f) != size)
    {
        free(dir);
        free(e);
        set_error(error, errorSize, "cannot read the zip's directory");
        return -1;
    }
    for (i = 0; i < count; i++)
    {
        u32 nameLen, extra, comment;
        if (pos + 46 > size || le32(dir + pos) != SIG_CENTRAL) break;
        e[i].method = le16(dir + pos + 10);
        e[i].packed = le32(dir + pos + 20);
        e[i].size = le32(dir + pos + 24);
        nameLen = le16(dir + pos + 28);
        extra = le16(dir + pos + 30);
        comment = le16(dir + pos + 32);
        e[i].local = le32(dir + pos + 42);
        if (pos + 46 + nameLen > size) break;
        if (nameLen >= ARCHIVE7Z_NAME_MAX) nameLen = ARCHIVE7Z_NAME_MAX - 1;
        memcpy(e[i].name, dir + pos + 46, nameLen);
        e[i].name[nameLen] = 0;
        pos += 46 + le16(dir + pos + 28) + extra + comment;
    }
    free(dir);
    *out = e;
    return (int)i;
}

static int zip_entry(void *ctx, u32 i, char *name, int size)
{
    const ZipEntry *e = (const ZipEntry *)ctx + i;
    size_t len = strlen(e->name);
    _snprintf(name, size, "%s", e->name);
    name[size - 1] = 0;
    return len && e->name[len - 1] == '/';
}

static int extract_entry(FILE *f, const ZipEntry *e, FILE *dst, u8 *in, u8 *outBuf, Archive7zProgress progress,
                         void *user, u64 *done, u64 total)
{
    u8 head[30];
    u32 left = e->packed;
    z_stream z;
    int zr = Z_OK;
    if (seek64(f, e->local, SEEK_SET) != 0 || fread(head, 1, 30, f) != 30 || le32(head) != SIG_LOCAL) return -1;
    if (seek64(f, (s64)e->local + 30 + le16(head + 26) + le16(head + 28), SEEK_SET) != 0) return -1;
    if (e->method == 0)
    {
        while (left)
        {
            u32 n = left < BUF_SIZE ? left : BUF_SIZE;
            if (fread(in, 1, n, f) != n || fwrite(in, 1, n, dst) != n) return -1;
            left -= n;
            *done += n;
            if (progress && !progress(user, *done, total)) return -2;
        }
        return 0;
    }
    if (e->method != 8) return -1;
    memset(&z, 0, sizeof(z));
    z.zalloc = z_alloc;
    z.zfree = z_free;
    if (inflateInit2(&z, -15) != Z_OK) return -1;
    while (zr != Z_STREAM_END)
    {
        if (z.avail_in == 0 && left)
        {
            u32 n = left < BUF_SIZE ? left : BUF_SIZE;
            if (fread(in, 1, n, f) != n) break;
            left -= n;
            z.next_in = in;
            z.avail_in = n;
        }
        z.next_out = outBuf;
        z.avail_out = BUF_SIZE;
        zr = inflate(&z, Z_NO_FLUSH);
        if (zr != Z_OK && zr != Z_STREAM_END) break;
        {
            u32 got = BUF_SIZE - z.avail_out;
            if (got && fwrite(outBuf, 1, got, dst) != got) break;
            *done += got;
            if (progress && !progress(user, *done, total))
            {
                inflateEnd(&z);
                return -2;
            }
        }
        if (z.avail_in == 0 && !left && zr != Z_STREAM_END && z.avail_out != 0) break;
    }
    inflateEnd(&z);
    return zr == Z_STREAM_END ? 0 : -1;
}

int archivezip_extract(const char *path, const char *outDir, Archive7zProgress progress, void *user, char *error,
                       int errorSize)
{
    FILE *f = fopen(path, "rb");
    ZipEntry *e = NULL;
    u8 *in = NULL, *outBuf = NULL;
    u64 total = 0, done = 0;
    int count, i, r = 0;
    if (!f)
    {
        set_error(error, errorSize, "cannot open the archive");
        return -1;
    }
    count = read_entries(f, &e, error, errorSize);
    in = (u8 *)malloc(BUF_SIZE);
    outBuf = (u8 *)malloc(BUF_SIZE);
    if (count < 0 || !in || !outBuf)
    {
        if (count >= 0) set_error(error, errorSize, "out of memory");
        r = -1;
    }
    for (i = 0; i < count; i++) total += e[i].size;
    archive7z_make_dirs(outDir);
    for (i = 0; r == 0 && i < count; i++)
    {
        char dst[ARCHIVE7Z_NAME_MAX * 2];
        size_t len = strlen(e[i].name);
        FILE *out;
        int x;
        archive7z_out_path(outDir, e[i].name, (u32)i, dst, sizeof(dst));
        if (len && e[i].name[len - 1] == '/')
        {
            archive7z_make_dirs(dst);
            continue;
        }
        {
            char folder[ARCHIVE7Z_NAME_MAX * 2];
            char *sep;
            strcpy(folder, dst);
            sep = strrchr(folder, PATH_SEP);
            if (sep)
            {
                *sep = 0;
                archive7z_make_dirs(folder);
            }
        }
        out = fopen(dst, "wb");
        if (!out)
        {
            set_error(error, errorSize, "cannot write an extracted file");
            r = -1;
            break;
        }
        x = extract_entry(f, &e[i], out, in, outBuf, progress, user, &done, total);
        fclose(out);
        if (x == -2)
        {
            set_error(error, errorSize, "cancelled");
            r = -1;
        }
        else if (x != 0)
        {
            set_error(error, errorSize, e[i].method != 0 && e[i].method != 8 ? "unsupported zip compression (only stored and deflate)"
                                                                             : "damaged zip archive");
            r = -1;
        }
    }
    if (r == 0 && count > 0) archive7z_patch_sheets(outDir, (u32)count, zip_entry, e);
    free(in);
    free(outBuf);
    free(e);
    fclose(f);
    return r;
}
