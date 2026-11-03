// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/log.h"
#include "xb_net.h"
#include "xb_update.h"

#define UPDATE_HOST "github.com"
#define UPDATE_PATH "/kernel64/HarissaDC/releases/latest"

static volatile LONG s_started, s_found;
static char s_current[32], s_latest[32];
static HANDLE s_thread;

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

static DWORD WINAPI update_thread(LPVOID)
{
    char latest[32];
    DWORD t0 = GetTickCount();
    if (!NetStart(0)) return 0;
    if (!fetch_latest(latest, sizeof(latest))) return 0;
    LOG_INFO("[update] latest release %s, running %s (%u ms)", latest, s_current, (unsigned)(GetTickCount() - t0));
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

int UpdateCheckTake(char *version, size_t size)
{
    if (!InterlockedExchange(&s_found, 0)) return 0;
    strncpy(version, s_latest, size - 1);
    version[size - 1] = 0;
    return 1;
}
