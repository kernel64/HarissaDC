// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "archive7z.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_XBOX)
#include <xtl.h>
#elif defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "../third_party/lzma/7z.h"
#include "../third_party/lzma/7zCrc.h"
#include "../third_party/lzma/Lzma2Dec.h"
#include "../third_party/lzma/LzmaDec.h"

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

#define METHOD_COPY 0x00
#define METHOD_LZMA2 0x21
#define METHOD_LZMA 0x30101
#define LOOK_SIZE (1u << 16)
#define IN_SIZE (1u << 20)
#define OUT_SIZE (1u << 20)

static void *sz_alloc(ISzAllocPtr, size_t size) { return size ? malloc(size) : NULL; }
static void sz_free(ISzAllocPtr, void *p) { free(p); }
static const ISzAlloc s_alloc = { sz_alloc, sz_free };

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

static void make_dir(const char *path)
{
#if defined(_XBOX)
    CreateDirectoryA(path, NULL);
#elif defined(_WIN32)
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

void archive7z_make_dirs(const char *path)
{
    char buf[ARCHIVE7Z_NAME_MAX * 2];
    size_t i, n = strlen(path);
    if (n >= sizeof(buf)) return;
    memcpy(buf, path, n + 1);
    for (i = 1; i < n; i++)
        if (buf[i] == PATH_SEP)
        {
            buf[i] = 0;
            make_dir(buf);
            buf[i] = PATH_SEP;
        }
    make_dir(buf);
}

struct FileStream
{
    ISeekInStream vt;
    FILE *f;
};

static SRes fs_read(const ISeekInStream *p, void *buf, size_t *size)
{
    FILE *f = ((const FileStream *)p)->f;
    *size = fread(buf, 1, *size, f);
    return ferror(f) ? SZ_ERROR_READ : SZ_OK;
}

static SRes fs_seek(const ISeekInStream *p, Int64 *pos, ESzSeek origin)
{
    FILE *f = ((const FileStream *)p)->f;
    int whence = origin == SZ_SEEK_SET ? SEEK_SET : origin == SZ_SEEK_CUR ? SEEK_CUR : SEEK_END;
    if (seek64(f, *pos, whence) != 0) return SZ_ERROR_READ;
    *pos = tell64(f);
    return SZ_OK;
}

struct Archive
{
    FileStream file;
    CLookToRead2 look;
    CSzArEx db;
    int dbOpen;
};

static void archive_close(Archive *a)
{
    if (a->dbOpen) SzArEx_Free(&a->db, &s_alloc);
    free(a->look.buf);
    if (a->file.f) fclose(a->file.f);
    memset(a, 0, sizeof(*a));
}

static int archive_open(Archive *a, const char *path, char *error, int errorSize)
{
    static int crcReady;
    SRes res;
    if (!crcReady)
    {
        CrcGenerateTable();
        crcReady = 1;
    }
    memset(a, 0, sizeof(*a));
    a->file.f = fopen(path, "rb");
    if (!a->file.f)
    {
        set_error(error, errorSize, "cannot open the archive");
        return -1;
    }
    a->file.vt.Read = fs_read;
    a->file.vt.Seek = fs_seek;
    LookToRead2_CreateVTable(&a->look, False);
    a->look.buf = (Byte *)malloc(LOOK_SIZE);
    a->look.bufSize = LOOK_SIZE;
    a->look.realStream = &a->file.vt;
    LookToRead2_Init(&a->look);
    if (!a->look.buf)
    {
        archive_close(a);
        set_error(error, errorSize, "out of memory");
        return -1;
    }
    SzArEx_Init(&a->db);
    res = SzArEx_Open(&a->db, &a->look.vt, &s_alloc, &s_alloc);
    if (res != SZ_OK)
    {
        archive_close(a);
        set_error(error, errorSize, res == SZ_ERROR_NO_ARCHIVE ? "not a 7z archive"
                                    : res == SZ_ERROR_UNSUPPORTED ? "unsupported 7z archive"
                                    : res == SZ_ERROR_MEM ? "out of memory"
                                                          : "damaged 7z archive");
        return -1;
    }
    a->dbOpen = 1;
    return 0;
}

static void entry_name(const CSzArEx *db, u32 i, char *out, int size)
{
    UInt16 name[ARCHIVE7Z_NAME_MAX];
    size_t n = SzArEx_GetFileNameUtf16(db, i, NULL), k;
    if (n > ARCHIVE7Z_NAME_MAX)
    {
        set_error(out, size, "name_too_long");
        return;
    }
    SzArEx_GetFileNameUtf16(db, i, name);
    for (k = 0; k + 1 < n && (int)k < size - 1; k++)
    {
        UInt16 c = name[k];
        out[k] = c == '\\' ? '/' : (c >= 0x20 && c < 0x80) ? (char)c : '_';
    }
    out[k] = 0;
}

int archive7z_list(const char *path, Archive7zEntry *out, int max, char *error, int errorSize)
{
    Archive a;
    u32 i;
    if (archive_open(&a, path, error, errorSize) != 0) return -1;
    for (i = 0; i < a.db.NumFiles && (int)i < max; i++)
    {
        entry_name(&a.db, i, out[i].name, sizeof(out[i].name));
        out[i].size = SzArEx_GetFileSize(&a.db, i);
        out[i].isDir = SzArEx_IsDir(&a.db, i);
    }
    i = a.db.NumFiles;
    archive_close(&a);
    return (int)i;
}

static int fatx_char(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    return c != 0 && strchr(" !#$%&'()-.@[]^_`{}~", c) != NULL;
}

static void safe_name_with(const char *in, const char *suffix, int maxLen, char *out, int outSize)
{
    char buf[ARCHIVE7Z_NAME_MAX];
    int n = 0, len;
    const char *p;
    for (p = in; *p && n < (int)sizeof(buf) - 1; p++) buf[n++] = fatx_char(*p) ? *p : '_';
    buf[n] = 0;
    len = n;
    if (len > maxLen)
    {
        const char *dot = strrchr(buf, '.');
        int ext = dot && len - (int)(dot - buf) <= 8 ? len - (int)(dot - buf) : 0, keep;
        keep = maxLen - ext - (int)strlen(suffix);
        if (keep < 1) keep = 1;
        memmove(buf + keep + strlen(suffix), buf + len - ext, ext + 1);
        memcpy(buf + keep, suffix, strlen(suffix));
    }
    set_error(out, outSize, buf);
}

void archive7z_safe_name(const char *in, int index, int maxLen, char *out, int outSize)
{
    char suffix[16];
    sprintf(suffix, "~%d", index);
    safe_name_with(in, suffix, maxLen, out, outSize);
}

void archive7z_out_path(const char *outDir, const char *name, u32 index, char *out, int size)
{
    char part[ARCHIVE7Z_NAME_MAX], safe[ARCHIVE7Z_NAME_MAX];
    int n = _snprintf(out, size, "%s", outDir);
    const char *p = name;
    while (*p && n < size - 1)
    {
        const char *slash = strchr(p, '/');
        int len = slash ? (int)(slash - p) : (int)strlen(p);
        if (len >= (int)sizeof(part)) len = sizeof(part) - 1;
        if (len > 0)
        {
            memcpy(part, p, len);
            part[len] = 0;
            if (slash)
            {
                u32 h = 2166136261u;
                const char *q;
                char suffix[16];
                for (q = name; q < p + len; q++) h = (h ^ (u8)*q) * 16777619u;
                sprintf(suffix, "~%06X", (unsigned)(h & 0xFFFFFFu));
                safe_name_with(part, suffix, ARCHIVE7Z_SAFE_NAME_MAX, safe, sizeof(safe));
            }
            else
                archive7z_safe_name(part, (int)index, ARCHIVE7Z_SAFE_NAME_MAX, safe, sizeof(safe));
            n += _snprintf(out + n, size - n, "%c%s", PATH_SEP, safe);
        }
        p = slash ? slash + 1 : p + strlen(p);
    }
    out[size - 1] = 0;
}

struct Sink
{
    Archive *a;
    const char *outDir;
    u32 file, end;
    FILE *out;
    u64 left;
    u32 crc;
    u64 done, total;
    Archive7zProgress progress;
    void *user;
    char *error;
    int errorSize;
};

static int sink_open_next(Sink *s)
{
    char name[ARCHIVE7Z_NAME_MAX], path[ARCHIVE7Z_NAME_MAX * 2];
    while (s->file < s->end && (SzArEx_IsDir(&s->a->db, s->file) || SzArEx_GetFileSize(&s->a->db, s->file) == 0)) s->file++;
    if (s->file >= s->end) return 0;
    entry_name(&s->a->db, s->file, name, sizeof(name));
    archive7z_out_path(s->outDir, name, s->file, path, sizeof(path));
    {
        char *sep = strrchr(path, PATH_SEP);
        if (sep)
        {
            *sep = 0;
            archive7z_make_dirs(path);
            *sep = PATH_SEP;
        }
    }
    s->out = fopen(path, "wb");
    if (!s->out)
    {
        set_error(s->error, s->errorSize, "cannot write the extracted file");
        return -1;
    }
    s->left = SzArEx_GetFileSize(&s->a->db, s->file);
    s->crc = CRC_INIT_VAL;
    return 0;
}

static int sink_close(Sink *s)
{
    int ok = 1;
    if (!s->out) return 0;
    if (fclose(s->out) != 0) ok = 0;
    s->out = NULL;
    if (!ok)
    {
        set_error(s->error, s->errorSize, "cannot write the extracted file (disk full?)");
        return -1;
    }
    if (SzBitWithVals_Check(&s->a->db.CRCs, s->file) && CRC_GET_DIGEST(s->crc) != s->a->db.CRCs.Vals[s->file])
    {
        set_error(s->error, s->errorSize, "CRC error: the archive is damaged");
        return -1;
    }
    s->file++;
    return 0;
}

static int sink_write(Sink *s, const u8 *data, size_t len)
{
    while (len)
    {
        size_t n;
        if (!s->out)
        {
            if (sink_open_next(s) != 0) return -1;
            if (!s->out) return 0;
        }
        n = len < s->left ? len : (size_t)s->left;
        if (fwrite(data, 1, n, s->out) != n)
        {
            set_error(s->error, s->errorSize, "cannot write the extracted file (disk full?)");
            return -1;
        }
        s->crc = CrcUpdate(s->crc, data, n);
        s->left -= n;
        s->done += n;
        data += n;
        len -= n;
        if (!s->left && sink_close(s) != 0) return -1;
    }
    if (s->progress && !s->progress(s->user, s->done, s->total))
    {
        set_error(s->error, s->errorSize, "cancelled");
        return -1;
    }
    return 0;
}

static int stream_block(Sink *s, u32 method, const Byte *props, unsigned propsSize, u64 packPos, u64 packSize, u64 unpackSize)
{
    FILE *f = s->a->file.f;
    u8 *in = (u8 *)malloc(IN_SIZE), *out = (u8 *)malloc(OUT_SIZE);
    CLzmaDec lzma;
    CLzma2Dec lzma2;
    size_t inLen = 0, inPos = 0;
    int r = -1;
    SRes res = SZ_OK;
    LzmaDec_Construct(&lzma);
    Lzma2Dec_Construct(&lzma2);
    if (!in || !out) goto fail_mem;
    if (seek64(f, (s64)packPos, SEEK_SET) != 0) goto fail_read;
    if (method == METHOD_LZMA) res = LzmaDec_Allocate(&lzma, props, propsSize, &s_alloc);
    else if (method == METHOD_LZMA2) res = propsSize == 1 ? Lzma2Dec_Allocate(&lzma2, props[0], &s_alloc) : SZ_ERROR_UNSUPPORTED;
    if (res == SZ_ERROR_MEM) goto fail_dict;
    if (res != SZ_OK) goto fail_data;
    if (method == METHOD_LZMA) LzmaDec_Init(&lzma);
    if (method == METHOD_LZMA2) Lzma2Dec_Init(&lzma2);
    while (unpackSize)
    {
        SizeT outLen, inProc;
        ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
        if (inPos == inLen && packSize)
        {
            size_t want = packSize < IN_SIZE ? (size_t)packSize : IN_SIZE;
            inLen = fread(in, 1, want, f);
            inPos = 0;
            if (inLen == 0) goto fail_read;
            packSize -= inLen;
        }
        outLen = unpackSize < OUT_SIZE ? (SizeT)unpackSize : OUT_SIZE;
        inProc = inLen - inPos;
        if (method == METHOD_COPY)
        {
            outLen = inProc < outLen ? inProc : outLen;
            memcpy(out, in + inPos, outLen);
            inProc = outLen;
        }
        else if (method == METHOD_LZMA)
            res = LzmaDec_DecodeToBuf(&lzma, out, &outLen, in + inPos, &inProc, LZMA_FINISH_ANY, &status);
        else
            res = Lzma2Dec_DecodeToBuf(&lzma2, out, &outLen, in + inPos, &inProc, LZMA_FINISH_ANY, &status);
        if (res != SZ_OK) goto fail_data;
        inPos += inProc;
        unpackSize -= outLen;
        if (sink_write(s, out, outLen) != 0) goto done;
        if (outLen == 0 && inProc == 0 && (inPos == inLen && !packSize)) goto fail_data;
        if (outLen == 0 && inProc == 0 && status == LZMA_STATUS_FINISHED_WITH_MARK) goto fail_data;
    }
    r = 0;
    goto done;
fail_mem:
    set_error(s->error, s->errorSize, "out of memory");
    goto done;
fail_dict:
    set_error(s->error, s->errorSize, "the archive's dictionary is too large for this console's memory");
    goto done;
fail_read:
    set_error(s->error, s->errorSize, "the archive is truncated");
    goto done;
fail_data:
    set_error(s->error, s->errorSize, "data error: the archive is damaged");
done:
    LzmaDec_Free(&lzma, &s_alloc);
    Lzma2Dec_Free(&lzma2, &s_alloc);
    free(in);
    free(out);
    return r;
}

static int extract_block(Sink *s, u32 f)
{
    const CSzAr *ar = &s->a->db.db;
    const Byte *coders = ar->CodersData + ar->FoCodersOffsets[f];
    u64 unpack = SzAr_GetFolderUnpackSize(ar, f);
    u32 si = ar->FoStartPackStreamIndex[f];
    CSzFolder fo;
    CSzData sd;
    sd.Data = coders;
    sd.Size = ar->FoCodersOffsets[f + 1] - ar->FoCodersOffsets[f];
    if (SzGetNextFolderItem(&fo, &sd) != SZ_OK)
    {
        set_error(s->error, s->errorSize, "damaged 7z archive");
        return -1;
    }
    if (fo.NumCoders == 1 && fo.NumPackStreams == 1)
    {
        u32 m = fo.Coders[0].MethodID;
        if (m == METHOD_COPY || m == METHOD_LZMA || m == METHOD_LZMA2)
            return stream_block(s, m, coders + fo.Coders[0].PropsOffset, fo.Coders[0].PropsSize,
                                s->a->db.dataPos + ar->PackPositions[si], ar->PackPositions[si + 1] - ar->PackPositions[si], unpack);
    }
    {
        u8 *buf;
        SRes res;
        int r;
        if (unpack > ARCHIVE7Z_MEMORY_BLOCK)
        {
            set_error(s->error, s->errorSize, "unsupported compression for a file this large (use LZMA or LZMA2)");
            return -1;
        }
        buf = (u8 *)malloc((size_t)unpack ? (size_t)unpack : 1);
        if (!buf)
        {
            set_error(s->error, s->errorSize, "out of memory");
            return -1;
        }
        res = SzAr_DecodeFolder(ar, f, &s->a->look.vt, s->a->db.dataPos, buf, (size_t)unpack, &s_alloc);
        if (res != SZ_OK)
        {
            free(buf);
            set_error(s->error, s->errorSize, res == SZ_ERROR_UNSUPPORTED ? "unsupported compression method"
                                              : res == SZ_ERROR_MEM   ? "out of memory"
                                                                      : "data error: the archive is damaged");
            return -1;
        }
        r = sink_write(s, buf, (size_t)unpack);
        free(buf);
        return r;
    }
}

static char *read_small(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    char *t;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (*len < 0 || *len > (1 << 20) || !(t = (char *)malloc(*len + 1)))
    {
        fclose(f);
        return NULL;
    }
    *len = (long)fread(t, 1, *len, f);
    t[*len] = 0;
    fclose(f);
    return t;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

void archive7z_patch_sheets(const char *outDir, u32 count, Archive7zEntryFn get, void *ctx)
{
    u32 i, j;
    for (i = 0; i < count; i++)
    {
        char name[ARCHIVE7Z_NAME_MAX], path[ARCHIVE7Z_NAME_MAX * 2], *text;
        const char *dot;
        long len;
        int changed = 0;
        if (get(ctx, i, name, sizeof(name))) continue;
        dot = strrchr(name, '.');
        if (!dot || (_stricmp(dot, ".cue") && _stricmp(dot, ".gdi"))) continue;
        archive7z_out_path(outDir, name, i, path, sizeof(path));
        text = read_small(path, &len);
        if (!text) continue;
        for (j = 0; j < count; j++)
        {
            char other[ARCHIVE7Z_NAME_MAX], safe[ARCHIVE7Z_NAME_MAX];
            const char *b;
            char *hit;
            if (j == i || get(ctx, j, other, sizeof(other))) continue;
            b = base_name(other);
            if (!b[0]) continue;
            archive7z_safe_name(b, (int)j, ARCHIVE7Z_SAFE_NAME_MAX, safe, sizeof(safe));
            if (!strcmp(b, safe)) continue;
            while ((hit = strstr(text, b)) != NULL)
            {
                size_t bl = strlen(b), sl = strlen(safe);
                memcpy(hit, safe, sl);
                memmove(hit + sl, hit + bl, strlen(hit + bl) + 1);
                changed = 1;
            }
        }
        if (changed)
        {
            FILE *f = fopen(path, "wb");
            if (f)
            {
                fwrite(text, 1, strlen(text), f);
                fclose(f);
            }
        }
        free(text);
    }
}

static int sz_entry(void *ctx, u32 i, char *name, int size)
{
    const CSzArEx *db = (const CSzArEx *)ctx;
    entry_name(db, i, name, size);
    return SzArEx_IsDir(db, i) ? 1 : 0;
}

int archive7z_extract(const char *path, const char *outDir, Archive7zProgress progress, void *user, char *error,
                      int errorSize)
{
    Archive a;
    Sink s;
    u32 i, f;
    if (archive_open(&a, path, error, errorSize) != 0) return -1;
    archive7z_make_dirs(outDir);
    memset(&s, 0, sizeof(s));
    s.a = &a;
    s.outDir = outDir;
    s.progress = progress;
    s.user = user;
    s.error = error;
    s.errorSize = errorSize;
    for (i = 0; i < a.db.NumFiles; i++) s.total += SzArEx_GetFileSize(&a.db, i);
    for (i = 0; i < a.db.NumFiles; i++)
    {
        char name[ARCHIVE7Z_NAME_MAX], p[ARCHIVE7Z_NAME_MAX * 2];
        if (!SzArEx_IsDir(&a.db, i) && SzArEx_GetFileSize(&a.db, i) != 0) continue;
        entry_name(&a.db, i, name, sizeof(name));
        archive7z_out_path(outDir, name, i, p, sizeof(p));
        if (SzArEx_IsDir(&a.db, i)) archive7z_make_dirs(p);
        else
        {
            FILE *e = fopen(p, "wb");
            if (e) fclose(e);
        }
    }
    for (f = 0; f < a.db.db.NumFolders; f++)
    {
        s.file = a.db.FolderToFile[f];
        s.end = a.db.FolderToFile[f + 1];
        int r = extract_block(&s, f);
        while (r == 0 && s.file < s.end && (SzArEx_IsDir(&a.db, s.file) || SzArEx_GetFileSize(&a.db, s.file) == 0)) s.file++;
        if (r == 0 && (s.out || s.file < s.end))
        {
            set_error(error, errorSize, "the archive is damaged (block shorter than its files)");
            r = -1;
        }
        if (r != 0)
        {
            if (s.out) fclose(s.out);
            archive_close(&a);
            return -1;
        }
    }
    archive7z_patch_sheets(outDir, a.db.NumFiles, sz_entry, &a.db);
    archive_close(&a);
    return 0;
}
