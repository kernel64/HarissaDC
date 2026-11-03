// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_art.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../core/log.h"
#include "xb_fs.h"
#include "xb_lang.h"
#include "xb_net.h"
#include "xb_png.h"

#define ART_MAX_PNG (4u << 20)
#define ART_MAX_PIXELS (2048u * 2048u)
#define ART_MAX_DB (8u << 20)
#define ART_WANT_MAX 8
#define ART_MAX_RESULTS 24
#define ART_DB_MAX_AGE_DAYS 30
#define ART_NAMES 8

static const char s_dbHost[] = "raw.githubusercontent.com";
static const char s_dbPath[] = "/libretro/libretro-database/master/metadat/redump/Sega%20-%20Dreamcast.dat";
static const char s_artHost[] = "thumbnails.libretro.com";
static const char s_artPath[] = "/Sega%20-%20Dreamcast/Named_Boxarts/";

struct DbEntry
{
    std::string name;
    std::string title;
    std::string serial;
    char region;
    int disc;
};

static CRITICAL_SECTION s_lock;
static HANDLE s_event, s_thread;
static volatile LONG s_started, s_netAllowed, s_paused;
static char s_dir[64], s_coverDir[64];

static std::vector<ArtGame> s_wants;
static u32 s_wantGen;
static std::set<std::string> s_delivered;
static std::deque<ArtResult> s_results;
static std::vector<ArtGame> s_bulk;
static size_t s_bulkPos;
static ArtStatus s_status;
static int s_note = -1, s_noteA, s_noteB;

static std::vector<DbEntry> s_db;
static int s_dbTried;
static std::set<std::string> s_tried;
static std::map<std::string, DWORD> s_failAt;
#define ART_RETRY_MS 30000

static std::string alnum_upper(const char *s, size_t n)
{
    std::string out;
    size_t i;
    for (i = 0; i < n && s[i]; i++)
    {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += c;
    }
    return out;
}

static std::string norm_serial(const char *s)
{
    std::string n = alnum_upper(s, strlen(s));
    if (n.size() > 2 && n[0] == 'M' && n[1] == 'K') n = n.substr(2);
    return n;
}

static std::string game_key(const ArtGame *g)
{
    std::string k = alnum_upper(g->product, strlen(g->product));
    if (k.empty()) k = alnum_upper(g->file, strlen(g->file));
    if (k.size() > 40) k = k.substr(0, 40);
    return k;
}

static int file_read(const char *path, u8 **data, u32 *len, u32 maxLen)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, got = 0;
    u8 *buf;
    *data = NULL;
    *len = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > maxLen)
    {
        CloseHandle(h);
        return 0;
    }
    buf = (u8 *)malloc(size + 1);
    if (!buf || !ReadFile(h, buf, size, &got, NULL) || got != size)
    {
        free(buf);
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    buf[size] = 0;
    *data = buf;
    *len = size;
    return 1;
}

static int file_write(const char *path, const void *data, u32 len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, data, len, &put, NULL) && put == len;
    CloseHandle(h);
    if (!ok) DeleteFileA(path);
    return ok;
}

static int file_exists(const char *path) { return GetFileAttributesA(path) != (DWORD)0xFFFFFFFF; }

static int file_age_days(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILETIME now;
    ULARGE_INTEGER a, b;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 1 << 20;
    GetSystemTimeAsFileTime(&now);
    a.LowPart = fa.ftLastWriteTime.dwLowDateTime;
    a.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (int)((b.QuadPart - a.QuadPart) / (10000000ULL * 86400ULL)) : 0;
}

static void cover_file(const std::string &key, const char *ext, char *out, size_t size)
{
    _snprintf(out, size - 1, "%s\\%s.%s", s_coverDir, key.c_str(), ext);
    out[size - 1] = 0;
}

struct ArtInfo
{
    int have;
    int found;
    int cover;
};

static void info_load(const std::string &key, ArtInfo *info)
{
    char path[160];
    u8 *data;
    u32 len;
    const char *p;
    memset(info, 0, sizeof(*info));
    info->cover = -1;
    cover_file(key, "txt", path, sizeof(path));
    if (!file_read(path, &data, &len, 4096)) return;
    info->have = 1;
    if ((p = strstr((const char *)data, "found=")) != NULL) info->found = atoi(p + 6);
    if ((p = strstr((const char *)data, "cover=")) != NULL) info->cover = atoi(p + 6);
    free(data);
}

static void info_save(const std::string &key, const ArtInfo *info, const char *name)
{
    char path[160], text[400];
    int n;
    cover_file(key, "txt", path, sizeof(path));
    n = _snprintf(text, sizeof(text) - 1, "found=%d\r\ncover=%d\r\nname=%s\r\n", info->found, info->cover, name ? name : "");
    if (n < 0) n = (int)sizeof(text) - 1;
    text[sizeof(text) - 1] = 0;
    if (!file_write(path, text, (u32)n)) LOG_WARN("[art] cannot write %s", path);
}

static int field(const char *line, const char *key, std::string *out)
{
    size_t k = strlen(key);
    const char *p = line, *e;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, key, k) || p[k] != ' ' || p[k + 1] != '"') return 0;
    p += k + 2;
    e = strchr(p, '"');
    if (!e) return 0;
    out->assign(p, e - p);
    return 1;
}

static void db_parse(const char *text)
{
    const char *p = text;
    DbEntry cur;
    int in = 0;
    s_db.clear();
    while (*p)
    {
        const char *e = p + strcspn(p, "\r\n");
        std::string line(p, e - p), v;
        p = e + strspn(e, "\r\n");
        if (!line.compare(0, 6, "game (")) { in = 1; cur = DbEntry(); cur.disc = 1; continue; }
        if (!in) continue;
        if (line == ")")
        {
            if (!cur.name.empty()) s_db.push_back(cur);
            in = 0;
            continue;
        }
        if (field(line.c_str(), "name", &v))
        {
            size_t paren = v.find(" (");
            const char *d = strstr(v.c_str(), "(Disc ");
            cur.name = v;
            cur.title = alnum_upper(v.c_str(), paren == std::string::npos ? v.size() : paren);
            if (d) cur.disc = atoi(d + 6);
        }
        else if (field(line.c_str(), "region", &v))
            cur.region = strstr(v.c_str(), "USA") ? 'U' : strstr(v.c_str(), "Europe") ? 'E' : strstr(v.c_str(), "Japan") ? 'J' : 0;
        else if (field(line.c_str(), "serial", &v))
            cur.serial = norm_serial(v.c_str());
    }
    LOG_INFO("[art] %u discs in the Redump list", (u32)s_db.size());
}

static int db_ready(int refresh)
{
    char path[96];
    u8 *data = NULL;
    u32 len = 0;
    int stale;
    if (!s_db.empty() && !refresh) return 1;
    _snprintf(path, sizeof(path), "%s\\dreamcast.dat", s_dir);
    path[sizeof(path) - 1] = 0;
    stale = refresh && file_age_days(path) >= ART_DB_MAX_AGE_DAYS;
    if (!s_db.empty() && !stale) return 1;
    if ((stale || !file_read(path, &data, &len, ART_MAX_DB)) && s_netAllowed && !s_dbTried)
    {
        int st = NetStart(0) ? NetGet(s_dbHost, s_dbPath, 1, &data, &len, ART_MAX_DB, NULL, 0) : 0;
        if (st == 200 && data && len >= 1000)
        {
            file_write(path, data, len);
            LOG_INFO("[art] Redump list downloaded (%u KB)", len >> 10);
        }
        else
        {
            free(data);
            data = NULL;
            LOG_INFO("[art] cannot download the Redump list (status %d)", st);
            if (st) s_dbTried = 1;
            else
            {
                EnterCriticalSection(&s_lock);
                s_status.offline = 1;
                LeaveCriticalSection(&s_lock);
            }
            if (!s_db.empty()) return 1;
            file_read(path, &data, &len, ART_MAX_DB);
        }
    }
    if (!data) return !s_db.empty();
    db_parse((const char *)data);
    free(data);
    return !s_db.empty();
}

static int name_penalty(const std::string &n)
{
    static const char *const bad[] = { "(Demo", "(Beta", "(Proto", "(Sample", "(Kiosk", "(Trial", "(Taikenban", "(Preview" };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (n.find(bad[i]) != std::string::npos) return 6;
    return 0;
}

struct Candidate
{
    const DbEntry *e;
    int score;
};

static bool candidate_better(const Candidate &a, const Candidate &b)
{
    return a.score > b.score;
}

static int db_find(const ArtGame *g, const DbEntry **out, int max)
{
    std::string serial = norm_serial(g->product), title = alnum_upper(g->title, strlen(g->title));
    size_t pl = strlen(g->product), i;
    std::vector<Candidate> found;
    char want;
    int n;
    if (pl >= 2 && !strcmp(g->product + pl - 2, "50")) want = 'E';
    else if (!strcmp(g->areas, "J")) want = 'J';
    else if (strchr(g->areas, 'U')) want = 'U';
    else if (strchr(g->areas, 'E')) want = 'E';
    else want = 'J';
    for (i = 0; i < s_db.size(); i++)
    {
        const DbEntry *e = &s_db[i];
        Candidate c;
        int score = 0;
        if (!serial.empty() && e->serial == serial) score = 32;
        else if (title.size() >= 4 && e->title == title) score = 16;
        else if (title.size() >= 6 && !e->title.compare(0, title.size(), title)) score = 8;
        if (!score) continue;
        if (e->region == want) score += 4;
        else if (e->region == 'U') score += 2;
        else if (e->region == 'E') score += 1;
        if (e->disc == 1) score += 2;
        score -= name_penalty(e->name);
        c.e = e;
        c.score = score;
        found.push_back(c);
    }
    std::stable_sort(found.begin(), found.end(), candidate_better);
    for (n = 0; n < max && n < (int)found.size(); n++) out[n] = found[n].e;
    return n;
}

static std::string thumb_path(const std::string &name)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out = s_artPath;
    size_t i;
    for (i = 0; i < name.size(); i++)
    {
        unsigned char c = (unsigned char)name[i];
        if (strchr("&*/:`<>?\\|\"", c)) c = '_';
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.()~!',", c)) out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out + ".png";
}

static std::string first_group(const std::string &name)
{
    size_t a = name.find(" ("), b;
    if (a == std::string::npos) return name;
    b = name.find(')', a);
    return b == std::string::npos ? name : name.substr(0, b + 1);
}

static int fetch_cover(const std::vector<std::string> &names, const char *dest)
{
    size_t i;
    int netFail = 0;
    for (i = 0; i < names.size(); i++)
    {
        u8 *png = NULL;
        u32 len = 0;
        int st = NetGet(s_artHost, thumb_path(names[i]).c_str(), 1, &png, &len, ART_MAX_PNG, NULL, 0);
        if (st == 200 && png)
        {
            int w = 0, h = 0;
            u8 *px = png_decode(png, len, &w, &h, ART_MAX_PIXELS);
            if (px)
            {
                free(px);
                if (file_write(dest, png, len))
                {
                    LOG_INFO("[art] %s: box art (%dx%d, %u bytes)", names[i].c_str(), w, h, len);
                    free(png);
                    return 1;
                }
            }
            else
                LOG_INFO("[art] %s: not a usable PNG (%u bytes)", names[i].c_str(), len);
        }
        else if (!st)
        {
            netFail = 1;
            free(png);
            break;
        }
        free(png);
    }
    return netFail ? -1 : 0;
}

static void set_busy(const char *path)
{
    EnterCriticalSection(&s_lock);
    strncpy(s_status.busyPath, path, sizeof(s_status.busyPath) - 1);
    s_status.busyPath[sizeof(s_status.busyPath) - 1] = 0;
    LeaveCriticalSection(&s_lock);
}

static int download_missing(const ArtGame *g, int bulk)
{
    std::string key = game_key(g);
    const DbEntry *cands[ART_NAMES];
    std::vector<std::string> names;
    char png[160];
    ArtInfo info;
    int nc, i, r;
    cover_file(key, "png", png, sizeof(png));
    if (file_exists(png)) return 0;
    info_load(key, &info);
    if (info.have && !bulk && (!info.found || info.cover == 0)) return 0;
    if (!NetStart(0))
    {
        EnterCriticalSection(&s_lock);
        s_status.offline = 1;
        LeaveCriticalSection(&s_lock);
        return -1;
    }
    set_busy(g->path);
    if (!db_ready(0))
    {
        set_busy("");
        return s_dbTried ? 0 : -1;
    }
    nc = db_find(g, cands, ART_NAMES);
    if (!nc)
    {
        LOG_INFO("[art] %s (%s): not in the Redump list", g->title, g->product);
        info.found = 0;
        info.cover = 0;
        info_save(key, &info, NULL);
        set_busy("");
        return 0;
    }
    for (i = 0; i < nc && (int)names.size() < ART_NAMES; i++)
    {
        std::string a = cands[i]->name, b = first_group(a);
        if (std::find(names.begin(), names.end(), a) == names.end()) names.push_back(a);
        if (b != a && (int)names.size() < ART_NAMES && std::find(names.begin(), names.end(), b) == names.end()) names.push_back(b);
    }
    r = fetch_cover(names, png);
    if (r < 0)
    {
        EnterCriticalSection(&s_lock);
        s_status.offline = 1;
        LeaveCriticalSection(&s_lock);
    }
    else
    {
        if (!r) LOG_INFO("[art] %s: no box art under %d name(s)", cands[0]->name.c_str(), (int)names.size());
        info.found = 1;
        info.cover = r;
        info_save(key, &info, cands[0]->name.c_str());
    }
    set_busy("");
    return r;
}

static void decode_cover(const char *file, ArtResult *r)
{
    u8 *png = NULL, *rgba;
    u32 len = 0;
    int w = 0, h = 0, fw, fh;
    if (!file_read(file, &png, &len, ART_MAX_PNG)) return;
    rgba = png_decode(png, len, &w, &h, ART_MAX_PIXELS);
    free(png);
    if (!rgba)
    {
        LOG_INFO("[art] cannot decode %s", file);
        return;
    }
    r->glow = image_scale(rgba, w, h, ART_GLOW_W, ART_GLOW_H);
    image_fit(w, h, ART_COVER_W, ART_COVER_H, &fw, &fh);
    if (fw != w || fh != h)
    {
        u8 *small = image_scale(rgba, w, h, fw, fh);
        free(rgba);
        rgba = small;
    }
    if (!rgba) return;
    r->cover = rgba;
    r->w = fw;
    r->h = fh;
}

static void deliver(const ArtGame *g)
{
    char file[160];
    ArtResult r;
    memset(&r, 0, sizeof(r));
    strncpy(r.path, g->path, sizeof(r.path) - 1);
    cover_file(game_key(g), "png", file, sizeof(file));
    if (file_exists(file)) decode_cover(file, &r);
    EnterCriticalSection(&s_lock);
    while (s_results.size() >= ART_MAX_RESULTS)
    {
        s_delivered.erase(s_results.front().path);
        free(s_results.front().cover);
        free(s_results.front().glow);
        s_results.pop_front();
    }
    s_results.push_back(r);
    s_delivered.insert(g->path);
    LeaveCriticalSection(&s_lock);
}

static int step_wants(void)
{
    std::vector<ArtGame> wants;
    u32 gen;
    size_t i;
    EnterCriticalSection(&s_lock);
    wants = s_wants;
    gen = s_wantGen;
    LeaveCriticalSection(&s_lock);
    for (i = 0; i < wants.size(); i++)
    {
        int sent;
        EnterCriticalSection(&s_lock);
        sent = s_delivered.count(wants[i].path) != 0;
        LeaveCriticalSection(&s_lock);
        if (!sent)
        {
            deliver(&wants[i]);
            return 1;
        }
    }
    if (!wants.empty() && s_netAllowed && !s_tried.count(game_key(&wants[0])))
    {
        std::string key = game_key(&wants[0]);
        std::map<std::string, DWORD>::iterator f = s_failAt.find(key);
        int r;
        if (f != s_failAt.end() && GetTickCount() - f->second < ART_RETRY_MS) return 0;
        s_tried.insert(key);
        r = download_missing(&wants[0], 0);
        if (r < 0)
        {
            s_tried.erase(key);
            s_failAt[key] = GetTickCount();
        }
        else
            s_failAt.erase(key);
        if (r > 0)
        {
            EnterCriticalSection(&s_lock);
            for (i = 0; i < s_wants.size(); i++)
                if (s_wantGen == gen || !strcmp(s_wants[i].path, wants[0].path))
                {
                    s_delivered.erase(wants[0].path);
                    break;
                }
            LeaveCriticalSection(&s_lock);
        }
        return 1;
    }
    return 0;
}

static int step_bulk(void)
{
    ArtGame g;
    size_t pos, total;
    int found;
    char png[160];
    EnterCriticalSection(&s_lock);
    if (!s_status.bulk || s_bulkPos >= s_bulk.size())
    {
        LeaveCriticalSection(&s_lock);
        return 0;
    }
    pos = s_bulkPos;
    total = s_bulk.size();
    g = s_bulk[pos];
    LeaveCriticalSection(&s_lock);
    if (pos == 0)
    {
        if (!NetStart(1))
        {
            EnterCriticalSection(&s_lock);
            s_status.bulk = 0;
            s_status.offline = 1;
            s_bulk.clear();
            s_bulkPos = 0;
            s_note = T_ART_NO_NETWORK;
            s_noteA = s_noteB = 0;
            LeaveCriticalSection(&s_lock);
            LOG_INFO("[art] get all: no network");
            return 1;
        }
        EnterCriticalSection(&s_lock);
        s_status.offline = 0;
        LeaveCriticalSection(&s_lock);
        db_ready(1);
    }
    if (download_missing(&g, 1) > 0)
    {
        EnterCriticalSection(&s_lock);
        s_delivered.erase(g.path);
        LeaveCriticalSection(&s_lock);
    }
    cover_file(game_key(&g), "png", png, sizeof(png));
    found = file_exists(png);
    EnterCriticalSection(&s_lock);
    s_bulkPos = pos + 1;
    s_status.done = (int)s_bulkPos;
    s_status.found += found;
    if (s_bulkPos >= total)
    {
        s_status.bulk = 0;
        s_note = s_status.offline ? T_ART_DONE_LOST : T_ART_DONE;
        s_noteA = s_status.found;
        s_noteB = s_status.total;
        s_bulk.clear();
        s_bulkPos = 0;
        LOG_INFO("[art] get all: %d games, %d with box art", (int)total, s_status.found);
    }
    LeaveCriticalSection(&s_lock);
    return 1;
}

static DWORD WINAPI worker(LPVOID)
{
    for (;;)
    {
        WaitForSingleObject(s_event, 100);
        while (!s_paused && (step_wants() || step_bulk())) {}
    }
}

void ArtStart(void)
{
    if (InterlockedExchange(&s_started, 1)) return;
    InitializeCriticalSection(&s_lock);
    xb_fs_data_dir(s_dir, sizeof(s_dir), "art");
    xb_fs_data_dir(s_coverDir, sizeof(s_coverDir), "art\\covers");
    s_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    s_thread = CreateThread(NULL, 256 * 1024, worker, NULL, CREATE_SUSPENDED, NULL);
    if (s_thread)
    {
        XSetThreadProcessor(s_thread, 3);
        SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
        ResumeThread(s_thread);
    }
}

void ArtSetNetwork(int allowed) { InterlockedExchange(&s_netAllowed, allowed ? 1 : 0); }

void ArtPause(int paused)
{
    InterlockedExchange(&s_paused, paused ? 1 : 0);
    if (!paused && s_event) SetEvent(s_event);
}

void ArtWant(const ArtGame *games, int count)
{
    int i, same;
    if (!s_started) return;
    if (count > ART_WANT_MAX) count = ART_WANT_MAX;
    EnterCriticalSection(&s_lock);
    same = (int)s_wants.size() == count;
    for (i = 0; same && i < count; i++)
        if (strcmp(s_wants[i].path, games[i].path)) same = 0;
    if (!same)
    {
        s_wants.assign(games, games + count);
        s_wantGen++;
        SetEvent(s_event);
    }
    LeaveCriticalSection(&s_lock);
}

int ArtTake(ArtResult *r)
{
    int got = 0;
    if (!s_started) return 0;
    EnterCriticalSection(&s_lock);
    if (!s_results.empty())
    {
        *r = s_results.front();
        s_results.pop_front();
        got = 1;
    }
    LeaveCriticalSection(&s_lock);
    return got;
}

void ArtResultFree(ArtResult *r)
{
    free(r->cover);
    free(r->glow);
    r->cover = r->glow = NULL;
}

void ArtForget(const char *path)
{
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    s_delivered.erase(path);
    LeaveCriticalSection(&s_lock);
    SetEvent(s_event);
}

void ArtForgetAll(void)
{
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    while (!s_results.empty())
    {
        free(s_results.front().cover);
        free(s_results.front().glow);
        s_results.pop_front();
    }
    s_delivered.clear();
    s_wants.clear();
    s_wantGen++;
    LeaveCriticalSection(&s_lock);
}

void ArtBulkStart(const ArtGame *games, int count)
{
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    if (!s_status.bulk && count > 0)
    {
        s_bulk.assign(games, games + count);
        s_bulkPos = 0;
        memset(&s_status, 0, sizeof(s_status));
        s_status.bulk = 1;
        s_status.total = count;
        s_tried.clear();
        s_failAt.clear();
        s_dbTried = 0;
        SetEvent(s_event);
    }
    LeaveCriticalSection(&s_lock);
}

void ArtGetStatus(ArtStatus *s)
{
    if (!s_started)
    {
        memset(s, 0, sizeof(*s));
        return;
    }
    EnterCriticalSection(&s_lock);
    *s = s_status;
    LeaveCriticalSection(&s_lock);
}

int ArtTakeNote(int *a, int *b)
{
    int n;
    if (!s_started) return -1;
    EnterCriticalSection(&s_lock);
    n = s_note;
    *a = s_noteA;
    *b = s_noteB;
    s_note = -1;
    LeaveCriticalSection(&s_lock);
    return n;
}
