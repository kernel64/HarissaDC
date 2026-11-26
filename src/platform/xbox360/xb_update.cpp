// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/log.h"
#include "../../disc/archivezip.h"
#include "xb_fs.h"
#include "xb_net.h"
#include "xb_update.h"

#define UPDATE_HOST "github.com"
#define UPDATE_PATH "/kernel64/HarissaDC/releases/latest"
#define UPDATE_API_HOST "api.github.com"
#define UPDATE_API_PATH "/repos/kernel64/HarissaDC/releases/latest"

#define UPDATE_DIR "game:\\update"
#define UPDATE_FILES "game:\\update\\files"
#define INSTALL_XEX "game:\\default.xex"
#define PREVIOUS_XEX "game:\\default_previous.xex"
#define NEW_XEX UPDATE_FILES "\\HarissaDC\\default.xex"
#define NEW_CHEATS UPDATE_FILES "\\HarissaDC\\cheats\\dreamcast_cheats.txt"

static volatile LONG s_started, s_found;
static char s_current[32], s_latest[32];
static HANDLE s_thread;

static char s_url[384], s_sha[65];
static u32 s_size;
static int s_installable;

static void parse_version(const char *s, int v[3])
{
    int i;
    if (*s == 'v' || *s == 'V') s++;
    for (i = 0; i < 3; i++)
    {
        v[i] = atoi(s);
        while (*s >= '0' && *s <= '9') s++;
        if (*s == '.') s++;
    }
}

static int newer(const char *latest, const char *current)
{
    int a[3], b[3], i;
    parse_version(latest, a);
    parse_version(current, b);
    for (i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}

static int fetch_latest(char *version, size_t size)
{
    char loc[512];
    const char *p, *e;
    int status = NetGet(UPDATE_HOST, UPDATE_PATH, 0, NULL, NULL, 0, loc, sizeof(loc));
    if (!status) return 0;
    p = strstr(loc, "/releases/tag/");
    if (p)
    {
        p += 14;
        for (e = p; *e && *e != '"' && *e != '/'; e++) ;
        if (e > p && (size_t)(e - p) < size)
        {
            memcpy(version, p, e - p);
            version[e - p] = 0;
            if (*version == 'v' || *version == 'V') memmove(version, version + 1, strlen(version));
            if (version[0] >= '0' && version[0] <= '9') return 1;
        }
    }
    LOG_INFO("[update] no release in the answer (status %d, %.60s)", status, loc);
    return 0;
}

static int json_str(const char *from, const char *end, const char *key, char *out, size_t size)
{
    char pat[48];
    const char *p, *q;
    _snprintf(pat, sizeof(pat) - 1, "\"%s\":", key);
    pat[sizeof(pat) - 1] = 0;
    p = strstr(from, pat);
    if (!p || (end && p >= end)) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    q = strchr(++p, '"');
    if (!q || (size_t)(q - p) >= size) return 0;
    memcpy(out, p, q - p);
    out[q - p] = 0;
    return 1;
}

static int json_num_before(const char *after, const char *before, const char *key, u32 *out)
{
    char pat[48];
    const char *p, *last = NULL;
    _snprintf(pat, sizeof(pat) - 1, "\"%s\":", key);
    pat[sizeof(pat) - 1] = 0;
    for (p = strstr(after, pat); p && p < before; p = strstr(p + 1, pat)) last = p;
    if (!last) return 0;
    *out = (u32)strtoul(last + strlen(pat), NULL, 10);
    return 1;
}

static int json_str_before(const char *after, const char *before, const char *key, char *out, size_t size)
{
    char pat[48];
    const char *p, *last = NULL;
    _snprintf(pat, sizeof(pat) - 1, "\"%s\":", key);
    pat[sizeof(pat) - 1] = 0;
    for (p = strstr(after, pat); p && p < before; p = strstr(p + 1, pat)) last = p;
    return last && json_str(last, before, key, out, size);
}

static int fetch_api(char *version, size_t size)
{
    u8 *body = NULL;
    u32 len = 0;
    const char *text, *assets, *p;
    int status = NetGet(UPDATE_API_HOST, UPDATE_API_PATH, 1, &body, &len, 1u << 20, NULL, 0);
    if (status != 200 || !body)
    {
        LOG_INFO("[update] the release API did not answer (status %d)", status);
        free(body);
        return 0;
    }
    text = (const char *)body;
    if (!json_str(text, NULL, "tag_name", version, size))
    {
        free(body);
        return 0;
    }
    if (*version == 'v' || *version == 'V') memmove(version, version + 1, strlen(version));
    assets = strstr(text, "\"assets\":");
    for (p = assets ? strstr(assets, "\"browser_download_url\":") : NULL; p; p = strstr(p + 1, "\"browser_download_url\":"))
    {
        char url[sizeof(s_url)], digest[80];
        size_t n;
        if (!json_str(p, NULL, "browser_download_url", url, sizeof(url))) continue;
        n = strlen(url);
        if (n < 4 || _stricmp(url + n - 4, ".zip") || strncmp(url, "https://github.com/", 19)) continue;
        if (!json_num_before(assets, p, "size", &s_size) || !json_str_before(assets, p, "digest", digest, sizeof(digest)) ||
            strncmp(digest, "sha256:", 7) || strlen(digest + 7) != 64)
        {
            LOG_INFO("[update] %s: no size or SHA-256 given: not installable from here", url);
            break;
        }
        strcpy(s_url, url);
        strcpy(s_sha, digest + 7);
        _strlwr(s_sha);
        s_installable = 1;
        break;
    }
    free(body);
    return 1;
}

static DWORD WINAPI update_thread(LPVOID)
{
    char latest[32];
    DWORD t0 = GetTickCount();
    if (!NetStart(0)) return 0;
    if (!fetch_api(latest, sizeof(latest)) && !fetch_latest(latest, sizeof(latest))) return 0;
    LOG_INFO("[update] latest release %s, running %s (%u ms)%s", latest, s_current, (unsigned)(GetTickCount() - t0),
             s_installable ? ", installable" : "");
    if (newer(latest, s_current))
    {
        strncpy(s_latest, latest, sizeof(s_latest) - 1);
        InterlockedExchange(&s_found, 1);
    }
    return 0;
}

void UpdateCheckStart(const char *current)
{
    if (InterlockedExchange(&s_started, 1)) return;
    strncpy(s_current, current, sizeof(s_current) - 1);
    s_thread = CreateThread(NULL, 256 * 1024, update_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!s_thread) return;
    XSetThreadProcessor(s_thread, 3);
    SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(s_thread);
}

int UpdateCheckTake(char *version, size_t size, int *installable)
{
    if (!InterlockedExchange(&s_found, 0)) return 0;
    strncpy(version, s_latest, size - 1);
    version[size - 1] = 0;
    *installable = s_installable;
    return 1;
}

static volatile LONG s_stage, s_cancel;
static volatile u64 s_done, s_total;
static char s_error[128];
static HANDLE s_install;

static void set_stage(int stage, u64 total)
{
    s_done = 0;
    s_total = total;
    InterlockedExchange(&s_stage, stage);
}

static int fail(const char *why)
{
    strncpy(s_error, why, sizeof(s_error) - 1);
    LOG_ERROR("[update] %s", why);
    InterlockedExchange(&s_stage, UPDATE_FAILED);
    return 0;
}

static int net_progress(void *, u32 done, u32 total)
{
    s_done = done;
    if (total) s_total = total;
    return s_cancel ? 1 : 0;
}

static int zip_progress(void *, u64 done, u64 total)
{
    s_done = done;
    s_total = total;
    return 1;
}

static void delete_tree(const char *dir)
{
    static XbDirEntry entries[64];
    char path[256];
    int n = xb_fs_list_dir(dir, entries, 64), i;
    XbDirEntry *own;
    if (n < 0) return;
    own = (XbDirEntry *)malloc(sizeof(XbDirEntry) * (n ? n : 1));
    if (!own) return;
    memcpy(own, entries, sizeof(XbDirEntry) * n);
    for (i = 0; i < n; i++)
    {
        xb_fs_join(path, sizeof(path), dir, own[i].name);
        if (own[i].isDir) delete_tree(path);
        else DeleteFileA(path);
    }
    free(own);
    RemoveDirectoryA(dir);
}

static u64 file_size(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((u64)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

static int looks_like_xex(const char *path)
{
    FILE *f = fopen(path, "rb");
    char magic[4];
    int ok = f && fread(magic, 1, 4, f) == 4 && !memcmp(magic, "XEX2", 4);
    if (f) fclose(f);
    return ok && file_size(path) >= (1u << 20);
}

static DWORD WINAPI install_thread(LPVOID)
{
    char zip[96], hex[65], err[128];
    int r;
    _snprintf(zip, sizeof(zip) - 1, UPDATE_DIR "\\HarissaDC-%s.zip", s_latest);
    zip[sizeof(zip) - 1] = 0;
    CreateDirectoryA(UPDATE_DIR, NULL);
    delete_tree(UPDATE_FILES);
    if (!xb_fs_exists(INSTALL_XEX)) return fail("game:\\default.xex not found: the running copy has another name");

    set_stage(UPDATE_DOWNLOAD, s_size);
    LOG_INFO("[update] downloading %s (%u bytes)", s_url, s_size);
    r = NetDownload(s_url, zip, s_size, net_progress, NULL);
    if (r < 0 || s_cancel)
    {
        InterlockedExchange(&s_stage, UPDATE_CANCELLED);
        return 0;
    }
    if (r == 0) return fail("the download failed");
    if (file_size(zip) != s_size)
    {
        DeleteFileA(zip);
        return fail("the download has the wrong size");
    }

    set_stage(UPDATE_VERIFY, s_size);
    if (NetSha256File(zip, hex, net_progress, NULL) != 1) return fail("cannot read the download");
    if (strcmp(hex, s_sha))
    {
        LOG_ERROR("[update] SHA-256 %s, expected %s", hex, s_sha);
        DeleteFileA(zip);
        return fail("the download is damaged (SHA-256)");
    }

    set_stage(UPDATE_EXTRACT, s_size);
    if (archivezip_extract(zip, UPDATE_FILES, zip_progress, NULL, err, sizeof(err)) != 0)
    {
        delete_tree(UPDATE_FILES);
        return fail(err);
    }
    if (!looks_like_xex(NEW_XEX))
    {
        delete_tree(UPDATE_FILES);
        return fail("no HarissaDC\\default.xex in the release");
    }

    set_stage(UPDATE_INSTALL, 0);
    DeleteFileA(PREVIOUS_XEX);
    if (!MoveFileA(INSTALL_XEX, PREVIOUS_XEX))
    {
        _snprintf(err, sizeof(err) - 1, "cannot set the running default.xex aside (error %lu)", GetLastError());
        err[sizeof(err) - 1] = 0;
        delete_tree(UPDATE_FILES);
        return fail(err);
    }
    if (!MoveFileA(NEW_XEX, INSTALL_XEX))
    {
        _snprintf(err, sizeof(err) - 1, "cannot put the new default.xex in place (error %lu)", GetLastError());
        err[sizeof(err) - 1] = 0;
        MoveFileA(PREVIOUS_XEX, INSTALL_XEX);
        delete_tree(UPDATE_FILES);
        return fail(err);
    }
    if (xb_fs_exists(NEW_CHEATS))
    {
        char cheats[160];
        xb_fs_data_path(cheats, sizeof(cheats), "cheats", "dreamcast_cheats.txt");
        if (!CopyFileA(NEW_CHEATS, cheats, FALSE)) LOG_WARN("[update] the cheat database was not replaced (error %lu)", GetLastError());
    }
    delete_tree(UPDATE_DIR);
    LOG_INFO("[update] HarissaDC %s installed (the previous version kept as default_previous.xex)", s_latest);
    InterlockedExchange(&s_stage, UPDATE_DONE);
    return 0;
}

void UpdateInstallStart(void)
{
    if (!s_installable || s_install) return;
    s_cancel = 0;
    s_error[0] = 0;
    set_stage(UPDATE_DOWNLOAD, s_size);
    s_install = CreateThread(NULL, 256 * 1024, install_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!s_install)
    {
        fail("cannot start the installer");
        return;
    }
    XSetThreadProcessor(s_install, 3);
    ResumeThread(s_install);
}

void UpdateInstallState(UpdateProgress *p)
{
    p->stage = (int)s_stage;
    p->done = s_done;
    p->total = s_total;
    strncpy(p->error, s_error, sizeof(p->error) - 1);
    p->error[sizeof(p->error) - 1] = 0;
}

void UpdateInstallCancel(void)
{
    InterlockedExchange(&s_cancel, 1);
}

void UpdateRestart(void)
{
    LOG_INFO("[update] restarting into %s", INSTALL_XEX);
    XLaunchNewImage(INSTALL_XEX, 0);
}
