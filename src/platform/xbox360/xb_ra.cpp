// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_ra.h"

#include <xtl.h>
#include <xbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc_client.h"

#include "../../core/log.h"
#include "../../core/types.h"
#include "../../disc/disc.h"
#include "../../hw/ra_dc.h"
#include "xb_fs.h"
#include "xb_lang.h"
#include "xb_menu.h"
#include "xb_net.h"
#include "xb_png.h"

struct RaCall
{
    char *url, *post, *type;
    rc_client_server_callback_t callback;
    void *data;
    u8 *body;
    u32 len;
    int status;
    RaCall *next;
};
static RaCall *s_todo, *s_todoTail, *s_done, *s_doneTail;
static CRITICAL_SECTION s_qLock, s_pumpLock;
static HANDLE s_wake, s_thread;

static rc_client_t *s_client;
static int s_enabled, s_state = XB_RA_OFF;
static char s_iniPath[96], s_logPath[96];
static char s_user[64], s_token[96];
static void (*s_toast)(const char *text, int ms, const char *badge);
static void (*s_onUnlock)(void);
#define RA_WARNING_ID 101000001u
static int s_running, s_gameAsked, s_cheated;
static char s_hash[33];
static u32 s_frames;

static char *copy_text(const char *s)
{
    size_t n;
    char *d;
    if (!s) return NULL;
    n = strlen(s) + 1;
    d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void call_run(RaCall *c)
{
    char host[128], agent[96];
    const char *p = c->url, *slash;
    size_t hl;
    c->status = 0;
    if (!p || strncmp(p, "https://", 8)) return;
    p += 8;
    slash = strchr(p, '/');
    hl = slash ? (size_t)(slash - p) : strlen(p);
    if (hl >= sizeof(host)) return;
    memcpy(host, p, hl);
    host[hl] = 0;
    if (!NetStart(0)) return;
    agent[0] = 0;
    rc_client_get_user_agent_clause(s_client, agent, sizeof(agent));
    if (c->post) c->status = NetPost(host, slash ? slash : "/", c->post, c->type, agent, &c->body, &c->len, 8 << 20);
    else c->status = NetGet(host, slash ? slash : "/", 1, &c->body, &c->len, 8 << 20, NULL, 0);
}

static DWORD WINAPI ra_thread(LPVOID)
{
    for (;;)
    {
        RaCall *c;
        WaitForSingleObject(s_wake, INFINITE);
        for (;;)
        {
            EnterCriticalSection(&s_qLock);
            c = s_todo;
            if (c)
            {
                s_todo = c->next;
                if (!s_todo) s_todoTail = NULL;
            }
            LeaveCriticalSection(&s_qLock);
            if (!c) break;
            call_run(c);
            c->next = NULL;
            EnterCriticalSection(&s_qLock);
            if (s_doneTail) s_doneTail->next = c;
            else s_done = c;
            s_doneTail = c;
            LeaveCriticalSection(&s_qLock);
        }
    }
}

static void RC_CCONV server_call(const rc_api_request_t *request, rc_client_server_callback_t callback, void *data, rc_client_t *)
{
    RaCall *c = (RaCall *)calloc(1, sizeof(RaCall));
    if (!c)
    {
        rc_api_server_response_t r;
        memset(&r, 0, sizeof(r));
        r.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
        callback(&r, data);
        return;
    }
    c->url = copy_text(request->url);
    c->post = copy_text(request->post_data);
    c->type = copy_text(request->content_type);
    c->callback = callback;
    c->data = data;
    EnterCriticalSection(&s_qLock);
    if (s_todoTail) s_todoTail->next = c;
    else s_todo = c;
    s_todoTail = c;
    LeaveCriticalSection(&s_qLock);
    SetEvent(s_wake);
}

static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t *buffer, uint32_t count, rc_client_t *)
{
    return s_running ? ra_dc_read(address, buffer, count) : 0;
}

static rc_clock_t RC_CCONV now_ms(const rc_client_t *)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (rc_clock_t)(t.QuadPart * 1000 / freq.QuadPart);
}

static void RC_CCONV log_message(const char *message, const rc_client_t *)
{
    LOG_INFO("[ra] %s", message);
}

static void toast(const char *text, int ms)
{
    if (s_toast) s_toast(text, ms, NULL);
}

#define BADGE_MAX 48
#define BADGE_KEEP_FREE 12
#define BADGE_RETRY_MS 30000
enum { BG_FREE = 0, BG_WANTED, BG_LOADING, BG_PIXELS, BG_TEXTURE, BG_FAILED };
struct Badge
{
    char name[24], host[64], path[96];
    int state;
    u8 *px;
    int w, h;
    IDirect3DTexture9 *tex;
    DWORD used;
};
static Badge s_badge[BADGE_MAX];
static CRITICAL_SECTION s_badgeLock;
static HANDLE s_badgeWake, s_badgeThread;

static Badge *badge_find(const char *name)
{
    int i;
    for (i = 0; i < BADGE_MAX; i++)
        if (s_badge[i].state != BG_FREE && !strcmp(s_badge[i].name, name)) return &s_badge[i];
    return NULL;
}

static u8 *badge_load(const char *name, const char *host, const char *path, int *w, int *h)
{
    char file[160], dir[96], fname[40];
    u8 *data = NULL, *px;
    u32 len = 0;
    FILE *f;
    xb_fs_data_dir(dir, sizeof(dir), "art");
    xb_fs_data_dir(dir, sizeof(dir), "art\\badges");
    _snprintf(fname, sizeof(fname), "%s.png", name);
    fname[sizeof(fname) - 1] = 0;
    xb_fs_join(file, sizeof(file), dir, fname);
    f = fopen(file, "rb");
    if (f)
    {
        long size;
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0 && size < 1024 * 1024) data = (u8 *)malloc((size_t)size);
        if (data && fread(data, 1, (size_t)size, f) == (size_t)size) len = (u32)size;
        fclose(f);
    }
    if (!len)
    {
        free(data);
        data = NULL;
        if (NetTestMode() == 2 || !NetStart(0) || NetGet(host, path, 1, &data, &len, 1024 * 1024, NULL, 0) != 200 || !len)
        {
            free(data);
            return NULL;
        }
        f = fopen(file, "wb");
        if (f)
        {
            size_t wrote = fwrite(data, 1, len, f);
            fclose(f);
            if (wrote != len) DeleteFileA(file);
        }
    }
    px = png_decode(data, len, w, h, 512u * 512u);
    free(data);
    if (!px) DeleteFileA(file);
    return px;
}

static DWORD WINAPI badge_thread(LPVOID)
{
    for (;;)
    {
        WaitForSingleObject(s_badgeWake, INFINITE);
        for (;;)
        {
            char name[24], host[64], path[96];
            Badge *b = NULL;
            u8 *px;
            int i, w = 0, h = 0;
            EnterCriticalSection(&s_badgeLock);
            for (i = 0; i < BADGE_MAX; i++)
                if (s_badge[i].state == BG_WANTED && (!b || (LONG)(s_badge[i].used - b->used) > 0)) b = &s_badge[i];
            if (b)
            {
                b->state = BG_LOADING;
                strcpy(name, b->name);
                strcpy(host, b->host);
                strcpy(path, b->path);
            }
            LeaveCriticalSection(&s_badgeLock);
            if (!b) break;
            px = badge_load(name, host, path, &w, &h);
            EnterCriticalSection(&s_badgeLock);
            b->px = px;
            b->w = w;
            b->h = h;
            b->used = GetTickCount();
            b->state = px ? BG_PIXELS : BG_FAILED;
            LeaveCriticalSection(&s_badgeLock);
        }
    }
}

static void badge_want(const char *name, const char *url)
{
    const char *host = url, *slash;
    Badge *b;
    int i;
    if (!name[0] || strlen(name) >= sizeof(s_badge[0].name)) return;
    if (strncmp(url, "https://", 8)) return;
    host += 8;
    slash = strchr(host, '/');
    if (!slash || (size_t)(slash - host) >= sizeof(s_badge[0].host) || strlen(slash) >= sizeof(s_badge[0].path)) return;
    if (!s_badgeThread)
    {
        InitializeCriticalSection(&s_badgeLock);
        s_badgeWake = CreateEvent(NULL, FALSE, FALSE, NULL);
        s_badgeThread = CreateThread(NULL, 128 * 1024, badge_thread, NULL, CREATE_SUSPENDED, NULL);
        if (!s_badgeThread) return;
        XSetThreadProcessor(s_badgeThread, 3);
        SetThreadPriority(s_badgeThread, THREAD_PRIORITY_BELOW_NORMAL);
        ResumeThread(s_badgeThread);
    }
    EnterCriticalSection(&s_badgeLock);
    b = badge_find(name);
    if (b)
    {
        if (b->state == BG_FAILED && GetTickCount() - b->used > BADGE_RETRY_MS)
        {
            b->state = BG_WANTED;
            SetEvent(s_badgeWake);
        }
        if (b->state != BG_FAILED) b->used = GetTickCount();
    }
    else
    {
        for (i = 0; i < BADGE_MAX && s_badge[i].state != BG_FREE; i++) {}
        if (i < BADGE_MAX)
        {
            b = &s_badge[i];
            memset(b, 0, sizeof(*b));
            strcpy(b->name, name);
            memcpy(b->host, host, (size_t)(slash - host));
            strcpy(b->path, slash);
            b->used = GetTickCount();
            b->state = BG_WANTED;
            SetEvent(s_badgeWake);
        }
    }
    LeaveCriticalSection(&s_badgeLock);
}

void XbRaBadgesPump(IDirect3DDevice9 *dev)
{
    int i, freeSlots = 0;
    if (!s_badgeThread) return;
    EnterCriticalSection(&s_badgeLock);
    for (i = 0; i < BADGE_MAX; i++)
    {
        Badge *b = &s_badge[i];
        if (b->state == BG_PIXELS)
        {
            b->tex = UiImageCreate(dev, b->px, b->w, b->h);
            free(b->px);
            b->px = NULL;
            b->used = GetTickCount();
            b->state = b->tex ? BG_TEXTURE : BG_FAILED;
        }
        if (b->state == BG_FREE) freeSlots++;
    }
    while (freeSlots < BADGE_KEEP_FREE)
    {
        Badge *old = NULL;
        DWORD now = GetTickCount();
        for (i = 0; i < BADGE_MAX; i++)
        {
            Badge *b = &s_badge[i];
            if ((b->state == BG_TEXTURE || b->state == BG_FAILED) && now - b->used > 2000 && (!old || (LONG)(old->used - b->used) > 0))
                old = b;
        }
        if (!old) break;
        if (old->tex) UiImageRelease(dev, old->tex);
        memset(old, 0, sizeof(*old));
        freeSlots++;
    }
    LeaveCriticalSection(&s_badgeLock);
}

IDirect3DTexture9 *XbRaBadge(const char *name)
{
    IDirect3DTexture9 *tex = NULL;
    Badge *b;
    if (!s_badgeThread || !name || !name[0]) return NULL;
    EnterCriticalSection(&s_badgeLock);
    b = badge_find(name);
    if (b && b->state == BG_TEXTURE)
    {
        b->used = GetTickCount();
        tex = b->tex;
    }
    LeaveCriticalSection(&s_badgeLock);
    return tex;
}

static void badge_of(const rc_client_achievement_t *a, int unlocked, char *name)
{
    char url[256];
    name[0] = 0;
    if (!a->badge_name[0] || strlen(a->badge_name) > 16) return;
    if (rc_client_achievement_get_image_url(a, unlocked ? RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED : RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE, url,
                                            sizeof(url)) != RC_OK)
        return;
    sprintf(name, unlocked ? "%s" : "%s_lock", a->badge_name);
    badge_want(name, url);
}

#define RA_MAX_UNLOCKS 8192
#define RA_MAX_GAMES 512
static u32 s_unlockId[RA_MAX_UNLOCKS], s_unlockGame[RA_MAX_UNLOCKS];
static u32 s_gameId[RA_MAX_GAMES], s_masterId[RA_MAX_GAMES];
static char s_gameTitle[RA_MAX_GAMES][48];
static int s_unlocks, s_games, s_masters, s_points;

static int unlock_count(u32 game, u32 id, u32 points, const char *title)
{
    int i;
    if (id >= RA_WARNING_ID) return 0;
    for (i = 0; i < s_unlocks; i++)
        if (s_unlockId[i] == id) return 0;
    if (s_unlocks >= RA_MAX_UNLOCKS) return 0;
    s_unlockGame[s_unlocks] = game;
    s_unlockId[s_unlocks++] = id;
    s_points += (int)points;
    for (i = 0; i < s_games; i++)
        if (s_gameId[i] == game) break;
    if (i == s_games && s_games < RA_MAX_GAMES) s_gameId[s_games++] = game;
    if (i < s_games && title && title[0] && !s_gameTitle[i][0])
    {
        strncpy(s_gameTitle[i], title, sizeof(s_gameTitle[i]) - 1);
        s_gameTitle[i][sizeof(s_gameTitle[i]) - 1] = 0;
    }
    return 1;
}

static int master_count(u32 game)
{
    int i;
    for (i = 0; i < s_masters; i++)
        if (s_masterId[i] == game) return 0;
    if (s_masters >= RA_MAX_GAMES) return 0;
    s_masterId[s_masters++] = game;
    return 1;
}

void XbRaTotalsGet(XbRaTotals *t)
{
    t->unlocks = s_unlocks;
    t->points = s_points;
    t->games = s_games;
    t->completed = s_masters;
}

static int holds(const char *s, const char *text)
{
    size_t n = strlen(text);
    for (; *s; s++)
        if (!_strnicmp(s, text, n)) return 1;
    return 0;
}

int XbRaUnlocksTitled(const char *text)
{
    int i, g, n = 0;
    for (g = 0; g < s_games; g++)
    {
        if (!s_gameTitle[g][0] || !holds(s_gameTitle[g], text)) continue;
        for (i = 0; i < s_unlocks; i++) n += s_unlockGame[i] == s_gameId[g];
    }
    return n;
}

void XbRaSetOnUnlock(void (*fn)(void))
{
    s_onUnlock = fn;
    if (fn) fn();
}

static void unlocks_load(void)
{
    char line[160];
    FILE *f = fopen(s_logPath, "rb");
    if (!f) return;
    while (fgets(line, sizeof(line), f))
    {
        unsigned game = 0, id = 0, points = 0;
        int at = 0;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = 0;
        if (line[0] == 'A' && sscanf(line + 1, "%u %u %u %n", &game, &id, &points, &at) >= 3)
            unlock_count(game, id, points, at ? line + 1 + at : "");
        else if (line[0] == 'M' && sscanf(line + 1, "%u", &game) == 1) master_count(game);
    }
    fclose(f);
    LOG_INFO("[ra] unlocked on this console: %d achievements, %d points, %d games, %d completed", s_unlocks, s_points, s_games, s_masters);
}

static void unlocks_append(const char *line)
{
    FILE *f = fopen(s_logPath, "r+b");
    if (!f) f = fopen(s_logPath, "wb");
    if (!f)
    {
        LOG_WARN("[ra] cannot write %s", s_logPath);
        return;
    }
    fseek(f, 0, SEEK_END);
    fputs(line, f);
    fclose(f);
}

static void RC_CCONV on_event(const rc_client_event_t *e, rc_client_t *client)
{
    char text[200], line[128], badge[24];
    const rc_client_game_t *game = rc_client_get_game_info(client);
    u32 gameId = game ? game->id : 0;
    switch (e->type)
    {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        if (e->achievement->id >= RA_WARNING_ID)
        {
            LOG_INFO("[ra] server notice %u: %s", e->achievement->id, e->achievement->title);
            break;
        }
        _snprintf(text, sizeof(text) - 1, Tr(T_RA_UNLOCKED), e->achievement->title, (int)e->achievement->points);
        text[sizeof(text) - 1] = 0;
        badge_of(e->achievement, 1, badge);
        if (s_toast) s_toast(text, 6000, badge);
        LOG_INFO("[ra] unlocked: %u \"%s\" (%u points)", e->achievement->id, e->achievement->title, e->achievement->points);
        if (unlock_count(gameId, e->achievement->id, e->achievement->points, game ? game->title : ""))
        {
            _snprintf(line, sizeof(line) - 1, "A %u %u %u %.47s\r\n", gameId, e->achievement->id, e->achievement->points,
                      game && game->title ? game->title : "");
            line[sizeof(line) - 1] = 0;
            unlocks_append(line);
        }
        if (s_onUnlock) s_onUnlock();
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        toast(Tr(T_RA_MASTERED), 6000);
        LOG_INFO("[ra] game %u completed", gameId);
        if (master_count(gameId))
        {
            _snprintf(line, sizeof(line) - 1, "M %u\r\n", gameId);
            line[sizeof(line) - 1] = 0;
            unlocks_append(line);
        }
        if (s_onUnlock) s_onUnlock();
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        LOG_WARN("[ra] server error (%s): %s", e->server_error->api, e->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        LOG_WARN("[ra] the server cannot be reached: unlocks are kept and sent later");
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        LOG_INFO("[ra] the server is back: pending unlocks sent");
        break;
    default:
        break;
    }
}

static void ini_read(void)
{
    char line[200];
    FILE *f = fopen(s_iniPath, "rb");
    s_user[0] = s_token[0] = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f))
    {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = 0;
        if (!strncmp(line, "user=", 5))
        {
            strncpy(s_user, line + 5, sizeof(s_user) - 1);
            s_user[sizeof(s_user) - 1] = 0;
        }
        else if (!strncmp(line, "token=", 6))
        {
            strncpy(s_token, line + 6, sizeof(s_token) - 1);
            s_token[sizeof(s_token) - 1] = 0;
        }
    }
    fclose(f);
}

static void ini_write(void)
{
    if (!SettingsSaveKey(s_iniPath, "user", s_user) || !SettingsSaveKey(s_iniPath, "token", s_token))
        LOG_WARN("[ra] cannot write %s", s_iniPath);
}

static void RC_CCONV on_game(int result, const char *error, rc_client_t *client, void *)
{
    char text[160];
    int unlocked, total;
    const rc_client_game_t *game = rc_client_get_game_info(client);
    if (result != RC_OK)
    {
        LOG_WARN("[ra] game not loaded (%d): %s", result, error ? error : "");
        if (result == RC_NO_GAME_LOADED) toast(Tr(T_RA_NONE), 4000);
        return;
    }
    XbRaProgress(&unlocked, &total);
    LOG_INFO("[ra] game %u \"%s\": %d achievements, %d unlocked", game ? game->id : 0, game ? game->title : "", total, unlocked);
    if (!total)
    {
        toast(Tr(T_RA_NONE), 4000);
        return;
    }
    _snprintf(text, sizeof(text) - 1, Tr(T_RA_GAME), unlocked, total);
    text[sizeof(text) - 1] = 0;
    toast(text, 5000);
}

static void game_load(void)
{
    if (!s_client || !s_enabled || !s_running || s_gameAsked || s_state == XB_RA_OUT || !s_hash[0]) return;
    s_gameAsked = 1;
    LOG_INFO("[ra] game hash %s", s_hash);
    rc_client_begin_load_game(s_client, s_hash, on_game, NULL);
}

static void RC_CCONV on_login(int result, const char *error, rc_client_t *client, void *)
{
    char text[160];
    const rc_client_user_t *u = result == RC_OK ? rc_client_get_user_info(client) : NULL;
    if (!u)
    {
        LOG_WARN("[ra] connection failed (%d): %s", result, error ? error : "");
        s_state = XB_RA_OUT;
        if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN)
        {
            s_token[0] = 0;
            ini_write();
        }
        toast(Tr(T_RA_LOGIN_FAILED), 4000);
        return;
    }
    strncpy(s_user, u->username, sizeof(s_user) - 1);
    s_user[sizeof(s_user) - 1] = 0;
    strncpy(s_token, u->token, sizeof(s_token) - 1);
    s_token[sizeof(s_token) - 1] = 0;
    ini_write();
    s_state = XB_RA_IN;
    LOG_INFO("[ra] connected as %s", u->display_name);
    _snprintf(text, sizeof(text) - 1, Tr(T_RA_CONNECTED), u->display_name);
    text[sizeof(text) - 1] = 0;
    toast(text, 4000);
    game_load();
}

static void token_login(void)
{
    if (!s_client || !s_enabled || s_state != XB_RA_OUT || !s_user[0] || !s_token[0]) return;
    s_state = XB_RA_BUSY;
    rc_client_begin_login_with_token(s_client, s_user, s_token, on_login, NULL);
}

static void client_make(void)
{
    if (s_client) return;
    InitializeCriticalSection(&s_qLock);
    InitializeCriticalSection(&s_pumpLock);
    s_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    s_client = rc_client_create(read_memory, server_call);
    if (!s_client) return;
    rc_client_enable_logging(s_client, RC_CLIENT_LOG_LEVEL_INFO, log_message);
    rc_client_set_get_time_millisecs_function(s_client, now_ms);
    rc_client_set_event_handler(s_client, on_event);
    rc_client_set_hardcore_enabled(s_client, 0);
    s_thread = CreateThread(NULL, 256 * 1024, ra_thread, NULL, CREATE_SUSPENDED, NULL);
    if (s_thread)
    {
        XSetThreadProcessor(s_thread, 3);
        ResumeThread(s_thread);
    }
    unlocks_load();
    if (s_onUnlock) s_onUnlock();
}

void XbRaInit(int enabled, void (*toastFn)(const char *text, int ms, const char *badge))
{
    xb_fs_data_path(s_iniPath, sizeof(s_iniPath), "config", "ra.ini");
    xb_fs_data_path(s_logPath, sizeof(s_logPath), "config", "ra_unlocks.txt");
    s_toast = toastFn;
    XbRaSetEnabled(enabled);
}

void XbRaSetEnabled(int on)
{
    on = on != 0;
    if (on == s_enabled) return;
    if (on)
    {
        client_make();
        if (!s_client) return;
        s_enabled = 1;
        if (s_state == XB_RA_OFF)
        {
            s_state = XB_RA_OUT;
            ini_read();
        }
        token_login();
        game_load();
    }
    else
    {
        if (s_client && s_gameAsked) rc_client_unload_game(s_client);
        s_gameAsked = 0;
        s_enabled = 0;
    }
}

int XbRaState(void) { return s_enabled ? s_state : XB_RA_OFF; }
const char *XbRaUser(void) { return s_enabled && s_state == XB_RA_IN ? s_user : ""; }

static int s_kb;
static DWORD s_kbSince;
static WCHAR s_kbText[128], s_kbTitle[96], s_kbDesc[160];
static XOVERLAPPED s_kbOv;
static char s_kbUser[64];

static void kb_open(int which)
{
    DWORD r;
    memset(&s_kbOv, 0, sizeof(s_kbOv));
    memset(s_kbText, 0, sizeof(s_kbText));
    MultiByteToWideChar(CP_UTF8, 0, Tr(which == 1 ? T_RA_USER_TITLE : T_RA_PASS_TITLE), -1, s_kbTitle, 95);
    MultiByteToWideChar(CP_UTF8, 0, Tr(T_RA_KB_DESC), -1, s_kbDesc, 159);
    r = XShowKeyboardUI(0, which == 1 ? VKBD_DEFAULT : VKBD_LATIN_PASSWORD, L"", s_kbTitle, s_kbDesc, s_kbText, 127, &s_kbOv);
    if (r == ERROR_IO_PENDING)
    {
        s_kb = which;
        return;
    }
    if (which == 2 && r == ERROR_ACCESS_DENIED && (s_kb != 3 || GetTickCount() - s_kbSince < 8000))
    {
        if (s_kb != 3) s_kbSince = GetTickCount();
        s_kb = 3;
        return;
    }
    s_kb = 0;
    LOG_WARN("[ra] the keyboard did not open (%lu)", r);
}

static void kb_pump(void)
{
    DWORD r;
    char text[256];
    if (s_kb == 3)
    {
        kb_open(2);
        return;
    }
    if (!s_kb || !XHasOverlappedIoCompleted(&s_kbOv)) return;
    r = XGetOverlappedResult(&s_kbOv, NULL, TRUE);
    text[0] = 0;
    if (r == ERROR_SUCCESS) WideCharToMultiByte(CP_UTF8, 0, s_kbText, -1, text, sizeof(text) - 1, NULL, NULL);
    text[sizeof(text) - 1] = 0;
    memset(s_kbText, 0, sizeof(s_kbText));
    if (!text[0])
    {
        s_kb = 0;
        return;
    }
    if (s_kb == 1)
    {
        strncpy(s_kbUser, text, sizeof(s_kbUser) - 1);
        s_kbUser[sizeof(s_kbUser) - 1] = 0;
        kb_open(2);
        return;
    }
    s_kb = 0;
    s_state = XB_RA_BUSY;
    LOG_INFO("[ra] connecting as %s", s_kbUser);
    rc_client_begin_login_with_password(s_client, s_kbUser, text, on_login, NULL);
    memset(text, 0, sizeof(text));
}

void XbRaAccount(void)
{
    if (!s_client || !s_enabled || s_kb || s_state == XB_RA_BUSY) return;
    if (s_state == XB_RA_IN)
    {
        if (s_gameAsked) rc_client_unload_game(s_client);
        rc_client_logout(s_client);
        s_gameAsked = 0;
        s_user[0] = s_token[0] = 0;
        ini_write();
        s_state = XB_RA_OUT;
        LOG_INFO("[ra] disconnected");
        toast(Tr(T_RA_LOGGED_OUT), 3000);
        return;
    }
    kb_open(1);
}

int XbRaBusy(void) { return s_kb != 0; }

void XbRaPump(void)
{
    if (!s_client) return;
    EnterCriticalSection(&s_pumpLock);
    kb_pump();
    for (;;)
    {
        RaCall *c;
        rc_api_server_response_t r;
        EnterCriticalSection(&s_qLock);
        c = s_done;
        if (c)
        {
            s_done = c->next;
            if (!s_done) s_doneTail = NULL;
        }
        LeaveCriticalSection(&s_qLock);
        if (!c) break;
        memset(&r, 0, sizeof(r));
        r.body = (const char *)c->body;
        r.body_length = c->len;
        r.http_status_code = c->status ? c->status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        c->callback(&r, c->data);
        free(c->body);
        free(c->url);
        free(c->post);
        free(c->type);
        free(c);
    }
    LeaveCriticalSection(&s_pumpLock);
}

void XbRaFrame(int cheats)
{
    if (!s_client || !s_enabled || !s_running) return;
    XbRaPump();
    if (!s_cheated && cheats)
    {
        s_cheated = 1;
        LOG_INFO("[ra] a cheat is on: achievements paused until the game restarts");
        toast(Tr(T_RA_CHEATS_OFF), 5000);
    }
    if (s_cheated) rc_client_idle(s_client);
    else rc_client_do_frame(s_client);
    if (++s_frames % 300 == 0)
    {
        static char s_presence[256];
        char now[256];
        now[0] = 0;
        rc_client_get_rich_presence_message(s_client, now, sizeof(now));
        if (strcmp(now, s_presence))
        {
            strcpy(s_presence, now);
            LOG_INFO("[ra] presence: %s", now);
        }
    }
}

void XbRaGameStart(const char *image)
{
    char why[96];
    s_running = 1;
    s_gameAsked = 0;
    s_cheated = 0;
    s_frames = 0;
    s_hash[0] = 0;
    if (!s_client || !s_enabled) return;
    if (image && image[0])
    {
        Disc *d = disc_open(image);
        if (!d || ra_dc_hash(d, s_hash, why, sizeof(why)) != 0)
        {
            LOG_WARN("[ra] no hash for %s: %s", image, d ? why : "cannot open the disc");
            s_hash[0] = 0;
        }
        else
            LOG_INFO("[ra] disc hash %s", s_hash);
        if (d) disc_close(d);
    }
    token_login();
    game_load();
}

void XbRaGameEnd(void)
{
    if (s_client && s_gameAsked) rc_client_unload_game(s_client);
    s_gameAsked = 0;
    s_running = 0;
}

void XbRaReset(int cheats)
{
    if (!s_client || !s_enabled || !s_running) return;
    rc_client_reset(s_client);
    if (s_cheated && !cheats) s_cheated = 0;
}

static void state_side(char *out, size_t size, const char *statePath)
{
    _snprintf(out, size, "%s.ra", statePath);
    out[size - 1] = 0;
}

void XbRaStateSaved(const char *statePath)
{
    char path[200];
    size_t size;
    u8 *buf;
    FILE *f;
    state_side(path, sizeof(path), statePath);
    if (!s_client || !s_enabled || !s_running || !s_gameAsked)
    {
        DeleteFileA(path);
        return;
    }
    size = rc_client_progress_size(s_client);
    buf = size ? (u8 *)malloc(size) : NULL;
    if (!buf || rc_client_serialize_progress_sized(s_client, buf, size) != RC_OK)
    {
        free(buf);
        DeleteFileA(path);
        return;
    }
    f = fopen(path, "wb");
    if (f)
    {
        if (fwrite(buf, 1, size, f) != size)
        {
            fclose(f);
            DeleteFileA(path);
        }
        else
            fclose(f);
    }
    free(buf);
}

void XbRaStateLoaded(const char *statePath)
{
    char path[200];
    u8 *buf = NULL;
    long size = 0;
    FILE *f;
    int r;
    if (!s_client || !s_enabled || !s_running) return;
    state_side(path, sizeof(path), statePath);
    f = fopen(path, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = size > 0 ? (u8 *)malloc((size_t)size) : NULL;
        if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size)
        {
            free(buf);
            buf = NULL;
        }
        fclose(f);
    }
    r = rc_client_deserialize_progress_sized(s_client, buf, buf ? (size_t)size : 0);
    LOG_INFO("[ra] state loaded: achievement progress %s (%d)", buf ? "restored" : "reset", r);
    free(buf);
}

static rc_client_achievement_list_t *s_list;
#define RA_LIST_MAX 1024
static const rc_client_achievement_t *s_listItem[RA_LIST_MAX];
static int s_listCount;

int XbRaListBegin(void)
{
    u32 b, i;
    XbRaListEnd();
    if (!s_client || !s_enabled || !s_running || !rc_client_get_game_info(s_client)) return 0;
    s_list = rc_client_create_achievement_list(s_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!s_list) return 0;
    for (int pass = 0; pass < 2; pass++)
        for (b = 0; b < s_list->num_buckets; b++)
            for (i = 0; i < s_list->buckets[b].num_achievements && s_listCount < RA_LIST_MAX; i++)
            {
                const rc_client_achievement_t *a = s_list->buckets[b].achievements[i];
                int unlocked = a->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE;
                if (a->id < RA_WARNING_ID && unlocked == (pass == 0)) s_listItem[s_listCount++] = a;
            }
    return s_listCount;
}

void XbRaListItem(int index, XbRaItem *item)
{
    const rc_client_achievement_t *a;
    memset(item, 0, sizeof(*item));
    item->title = item->description = "";
    if (index < 0 || index >= s_listCount) return;
    a = s_listItem[index];
    item->title = a->title ? a->title : "";
    item->description = a->description ? a->description : "";
    item->points = (int)a->points;
    item->unlocked = a->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE;
    badge_of(a, item->unlocked, item->badge);
}

void XbRaListEnd(void)
{
    if (s_list) rc_client_destroy_achievement_list(s_list);
    s_list = NULL;
    s_listCount = 0;
}

void XbRaProgress(int *unlocked, int *total)
{
    rc_client_achievement_list_t *list;
    u32 b, i;
    *unlocked = *total = 0;
    if (!s_client || !s_enabled || !s_running || !rc_client_get_game_info(s_client)) return;
    list = rc_client_create_achievement_list(s_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) return;
    for (b = 0; b < list->num_buckets; b++)
        for (i = 0; i < list->buckets[b].num_achievements; i++)
        {
            if (list->buckets[b].achievements[i]->id >= RA_WARNING_ID) continue;
            (*total)++;
            if (list->buckets[b].achievements[i]->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE) (*unlocked)++;
        }
    rc_client_destroy_achievement_list(list);
}

void XbRaTestToast(void)
{
    char text[200], badge[24];
    if (XbRaListBegin() && s_toast)
    {
        const rc_client_achievement_t *a = s_listItem[0];
        _snprintf(text, sizeof(text) - 1, Tr(T_RA_UNLOCKED), a->title, (int)a->points);
        text[sizeof(text) - 1] = 0;
        badge_of(a, 1, badge);
        s_toast(text, 6000, badge);
    }
    XbRaListEnd();
}

int XbRaSelfTest(void)
{
    u8 *body = NULL;
    u32 len = 0;
    int status, id = 0;
    const char *p;
    if (!NetStart(0))
    {
        LOG_WARN("[ra] self-test: no network");
        return 0;
    }
    status = NetPost("retroachievements.org", "/dorequest.php", "r=gameid&m=e0dc1587750ef747cc873230b96a50bb", NULL, "rcheevos", &body, &len, 65536);
    p = body ? strstr((const char *)body, "\"GameID\":") : NULL;
    if (p) id = atoi(p + 9);
    LOG_INFO("[ra] self-test: HTTP %d, %u bytes, game id %d: %.120s", status, len, id, body ? (const char *)body : "");
    free(body);
    return id;
}
