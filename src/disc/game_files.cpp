// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "game_files.h"
#include "archivezip.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_XBOX)
#include <xtl.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

#define MARKER_NAME "extracted.txt"

static void copy_str(char *out, int size, const char *in)
{
    if (size <= 0) return;
    strncpy(out, in, size - 1);
    out[size - 1] = 0;
}

static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot : "";
}

int game_is_archive(const char *name)
{
    return _stricmp(ext_of(name), ".7z") == 0 || _stricmp(ext_of(name), ".zip") == 0;
}

static int is_image(const char *name)
{
    const char *e = ext_of(name);
    return !_stricmp(e, ".gdi") || !_stricmp(e, ".cue") || !_stricmp(e, ".cdi") || !_stricmp(e, ".iso") || !_stricmp(e, ".chd");
}

int game_is_listed(const char *name)
{
    return is_image(name) || game_is_archive(name);
}

static int find_token(const char *name, int len, int *start, int *end)
{
    static const char *words[] = { "disc", "disk", "cd" };
    int i, w;
    for (i = 0; i < len; i++)
    {
        if (i > 0 && isalnum((unsigned char)name[i - 1])) continue;
        for (w = 0; w < 3; w++)
        {
            int wl = (int)strlen(words[w]), j = i + wl, num = 0, digits = 0, s = i, e;
            if (i + wl > len || _strnicmp(name + i, words[w], wl) != 0) continue;
            while (j < len && (name[j] == ' ' || name[j] == '_' || name[j] == '-' || name[j] == '.')) j++;
            while (j < len && isdigit((unsigned char)name[j]) && digits < 3) num = num * 10 + (name[j++] - '0'), digits++;
            if (!digits || num == 0 || (j < len && isalnum((unsigned char)name[j]))) continue;
            e = j;
            {
                int k = e;
                while (k < len && name[k] == ' ') k++;
                if (k + 2 < len && !_strnicmp(name + k, "of", 2))
                {
                    k += 2;
                    while (k < len && name[k] == ' ') k++;
                    if (k < len && isdigit((unsigned char)name[k]))
                    {
                        while (k < len && isdigit((unsigned char)name[k])) k++;
                        e = k;
                    }
                }
            }
            {
                int a = s - 1, b = e;
                while (a >= 0 && name[a] == ' ') a--;
                while (b < len && name[b] == ' ') b++;
                if (a >= 0 && b < len && ((name[a] == '(' && name[b] == ')') || (name[a] == '[' && name[b] == ']')))
                {
                    s = a;
                    e = b + 1;
                }
            }
            *start = s;
            *end = e;
            return num;
        }
    }
    return 0;
}

static int stem_len(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? (int)(dot - name) : (int)strlen(name);
}

int game_disc_number(const char *name)
{
    int s, e;
    return find_token(name, stem_len(name), &s, &e);
}

static void strip_token(const char *name, char *out, int size)
{
    int len = stem_len(name), s = len, e = len, n = 0, i;
    if (!find_token(name, len, &s, &e)) s = e = len;
    for (i = 0; i < len && n < size - 1; i++)
    {
        if (i >= s && i < e) continue;
        if (name[i] == ' ' && (n == 0 || out[n - 1] == ' ')) continue;
        out[n++] = name[i];
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '-' || out[n - 1] == '_' || out[n - 1] == '.')) n--;
    out[n] = 0;
}

void game_disc_key(const char *name, char *out, int size)
{
    char *p;
    strip_token(name, out, size);
    for (p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

void game_display_name(const char *name, char *out, int size)
{
    strip_token(name, out, size);
}

typedef void (*DirFn)(void *user, const char *dir, const char *name, int isDir);

static void list_dir(const char *dir, DirFn fn, void *user)
{
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char pattern[GAME_PATH_MAX];
    HANDLE h;
    _snprintf(pattern, sizeof(pattern), "%s%c*", dir, PATH_SEP);
    pattern[sizeof(pattern) - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.cFileName[0] == '.') continue;
        fn(user, dir, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL)
    {
        char path[GAME_PATH_MAX];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        fn(user, dir, e->d_name, stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    }
    closedir(d);
#endif
}

static void join(char *out, int size, const char *dir, const char *name)
{
    if (dir[0]) _snprintf(out, size, "%s%c%s", dir, PATH_SEP, name);
    else _snprintf(out, size, "%s", name);
    out[size - 1] = 0;
}

static void split(const char *path, char *dir, int dirSize, const char **name)
{
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/'), *sep = a > b ? a : b;
    int n = sep ? (int)(sep - path) : 0;
    if (n > dirSize - 1) n = dirSize - 1;
    memcpy(dir, path, n);
    dir[n] = 0;
    *name = sep ? sep + 1 : path;
}

static void set_add(DiscSet *set, const char *path, int number)
{
    int i;
    if (set->count >= GAME_DISCS_MAX) return;
    for (i = set->count; i > 0 && set->number[i - 1] > number; i--)
    {
        set->number[i] = set->number[i - 1];
        memcpy(set->path[i], set->path[i - 1], GAME_PATH_MAX);
    }
    set->number[i] = number;
    copy_str(set->path[i], GAME_PATH_MAX, path);
    set->count++;
}

struct SiblingScan
{
    DiscSet *set;
    char key[GAME_PATH_MAX];
    int archives;
};

static void sibling_fn(void *user, const char *dir, const char *name, int isDir)
{
    SiblingScan *s = (SiblingScan *)user;
    char key[GAME_PATH_MAX], path[GAME_PATH_MAX];
    int n, i;
    if (isDir || !game_is_listed(name) || game_is_archive(name) != s->archives) return;
    n = game_disc_number(name);
    if (!n) return;
    game_disc_key(name, key, sizeof(key));
    if (strcmp(key, s->key)) return;
    for (i = 0; i < s->set->count; i++)
        if (s->set->number[i] == n) return;
    join(path, sizeof(path), dir, name);
    set_add(s->set, path, n);
}

void game_disc_set(const char *path, DiscSet *set)
{
    char dir[GAME_PATH_MAX];
    const char *name;
    int i, n;
    memset(set, 0, sizeof(*set));
    split(path, dir, sizeof(dir), &name);
    n = game_disc_number(name);
    if (n)
    {
        SiblingScan s;
        s.set = set;
        s.archives = game_is_archive(name);
        game_disc_key(name, s.key, sizeof(s.key));
        list_dir(dir, sibling_fn, &s);
    }
    for (i = 0; i < set->count; i++)
        if (set->number[i] == n) break;
    if (i < set->count && !_stricmp(set->path[i], path)) set->current = i;
    else if (i < set->count)
    {
        copy_str(set->path[i], GAME_PATH_MAX, path);
        set->current = i;
    }
    else
    {
        memset(set, 0, sizeof(*set));
        set_add(set, path, n);
    }
}

static u64 file_size64(const char *path)
{
    FILE *f = fopen(path, "rb");
    s64 n;
    if (!f) return 0;
#ifdef _MSC_VER
    _fseeki64(f, 0, SEEK_END);
    n = _ftelli64(f);
#else
    fseeko(f, 0, SEEK_END);
    n = (s64)ftello(f);
#endif
    fclose(f);
    return n > 0 ? (u64)n : 0;
}

void game_cache_dir(const char *cacheRoot, const char *path, char *out, int size)
{
    char dir[GAME_PATH_MAX], stem[GAME_PATH_MAX], safe[ARCHIVE7Z_NAME_MAX];
    const char *name, *p;
    u32 h = 2166136261u;
    int n;
    split(path, dir, sizeof(dir), &name);
    n = stem_len(name);
    if (n > (int)sizeof(stem) - 1) n = sizeof(stem) - 1;
    memcpy(stem, name, n);
    stem[n] = 0;
    for (p = name; *p; p++) h = (h ^ (u8)*p) * 16777619u;
    archive7z_safe_name(stem, (int)(h % 100000u), ARCHIVE7Z_SAFE_NAME_MAX, safe, sizeof(safe));
    join(out, size, cacheRoot, safe);
}

struct ImageScan
{
    DiscSet *set;
    int depth;
};

static void image_fn(void *user, const char *dir, const char *name, int isDir)
{
    ImageScan *s = (ImageScan *)user;
    char path[GAME_PATH_MAX];
    int n, i;
    join(path, sizeof(path), dir, name);
    if (isDir)
    {
        if (s->depth > 0)
        {
            ImageScan sub = { s->set, s->depth - 1 };
            list_dir(path, image_fn, &sub);
        }
        return;
    }
    if (!is_image(name)) return;
    n = game_disc_number(name);
    for (i = 0; i < s->set->count; i++)
        if (s->set->number[i] == n)
        {
            if (!_stricmp(ext_of(name), ".gdi")) copy_str(s->set->path[i], GAME_PATH_MAX, path);
            return;
        }
    set_add(s->set, path, n);
}

static int marker_ok(const char *dir, const char *archive)
{
    char marker[GAME_PATH_MAX], text[32] = "";
    FILE *f;
    join(marker, sizeof(marker), dir, MARKER_NAME);
    f = fopen(marker, "rb");
    if (!f) return 0;
    fgets(text, sizeof(text), f);
    fclose(f);
    return text[0] && _strtoui64(text, NULL, 10) == file_size64(archive);
}

int game_cache_images(const char *cacheRoot, const char *path, DiscSet *set)
{
    char dir[GAME_PATH_MAX];
    ImageScan s;
    memset(set, 0, sizeof(*set));
    game_cache_dir(cacheRoot, path, dir, sizeof(dir));
    if (!marker_ok(dir, path)) return 0;
    s.set = set;
    s.depth = 2;
    list_dir(dir, image_fn, &s);
    return set->count;
}

int game_resolve(const char *cacheRoot, const char *path, Archive7zProgress progress, void *user, char *image, int imageSize,
                 char *error, int errorSize)
{
    DiscSet set;
    char dir[GAME_PATH_MAX];
    if (error && errorSize > 0) error[0] = 0;
    if (!game_is_archive(path))
    {
        copy_str(image, imageSize, path);
        return 0;
    }
    if (!game_cache_images(cacheRoot, path, &set))
    {
        char marker[GAME_PATH_MAX];
        FILE *f;
        game_cache_dir(cacheRoot, path, dir, sizeof(dir));
        archive7z_make_dirs(dir);
        if ((_stricmp(ext_of(path), ".zip") == 0 ? archivezip_extract(path, dir, progress, user, error, errorSize)
                                                 : archive7z_extract(path, dir, progress, user, error, errorSize)) != 0)
            return -1;
        join(marker, sizeof(marker), dir, MARKER_NAME);
        f = fopen(marker, "wb");
        if (f)
        {
            fprintf(f, "%llu\n", (unsigned long long)file_size64(path));
            fclose(f);
        }
        if (!game_cache_images(cacheRoot, path, &set))
        {
            copy_str(error, errorSize, "the archive holds no disc image (.gdi, .cue, .cdi, .iso, .chd)");
            return -1;
        }
    }
    copy_str(image, imageSize, set.path[0]);
    return 0;
}
