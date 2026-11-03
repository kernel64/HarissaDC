// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_log.h"

#include <xtl.h>
#include <stdio.h>
#include <string>

#include "../../core/log.h"
#include "../../core/version.h"

static FILE *s_file;
static CRITICAL_SECTION s_bufLock;
static CRITICAL_SECTION s_fileLock;
static std::string s_buf;
static HANDLE s_thread;
static volatile LONG s_quit;

void xb_log_flush(void)
{
    std::string out;
    if (!s_file) return;
    EnterCriticalSection(&s_fileLock);
    EnterCriticalSection(&s_bufLock);
    out.swap(s_buf);
    LeaveCriticalSection(&s_bufLock);
    if (!out.empty())
    {
        fwrite(out.data(), 1, out.size(), s_file);
        fflush(s_file);
    }
    LeaveCriticalSection(&s_fileLock);
}

static DWORD WINAPI log_thread(LPVOID)
{
    while (!s_quit)
    {
        Sleep(500);
        xb_log_flush();
    }
    return 0;
}

static void file_sink(int level, const char *line)
{
    if (!s_file) return;
    EnterCriticalSection(&s_bufLock);
    s_buf += line;
    s_buf += '\n';
    LeaveCriticalSection(&s_bufLock);
    if (level <= LOG_LEVEL_WARN || !s_thread) xb_log_flush();
}

const char *xb_log_open(void)
{
    const char *drive = "game";
    InitializeCriticalSection(&s_bufLock);
    InitializeCriticalSection(&s_fileLock);
    s_file = fopen("game:\\" EMU_SHORTNAME ".log", "w");
    if (!s_file)
    {
        s_file = fopen("cache:\\" EMU_SHORTNAME ".log", "w");
        drive = "cache";
    }
    log_set_sink(file_sink);
    s_thread = CreateThread(NULL, 64 * 1024, log_thread, NULL, CREATE_SUSPENDED, NULL);
    if (s_thread)
    {
        XSetThreadProcessor(s_thread, 5);
        SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
        ResumeThread(s_thread);
    }
    return s_file ? drive : NULL;
}

void xb_log_close(void)
{
    if (s_thread)
    {
        s_quit = 1;
        WaitForSingleObject(s_thread, INFINITE);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    xb_log_flush();
    if (s_file)
    {
        fclose(s_file);
        s_file = NULL;
    }
}
