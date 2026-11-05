// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static LogSink s_sink;
int g_log_level = LOG_LEVEL_INFO;
static char s_recent[LOG_RECENT_LINES][LOG_LINE_MAX];
static u32 s_recentNext;

void log_set_sink(LogSink sink) { s_sink = sink; }
LogSink log_get_sink(void) { return s_sink; }
void log_set_level(int maxLevel) { g_log_level = maxLevel; }
int log_level(void) { return g_log_level; }

void log_write(int level, const char *fmt, ...)
{
    static const char *const prefix[] = { "E ", "W ", "I ", "D " };
    char line[LOG_LINE_MAX];
    va_list ap;
    if (level > g_log_level) return;
    if (level < 0) level = 0;
    if (level > LOG_LEVEL_DEBUG) level = LOG_LEVEL_DEBUG;
    strcpy(line, prefix[level]);
    va_start(ap, fmt);
    vsnprintf(line + 2, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = 0;
    memcpy(s_recent[s_recentNext % LOG_RECENT_LINES], line, sizeof(line));
    s_recentNext++;
    if (s_sink) s_sink(level, line);
}

int log_recent(const char **lines, int max)
{
    u32 count = s_recentNext < LOG_RECENT_LINES ? s_recentNext : LOG_RECENT_LINES;
    u32 first = s_recentNext - count, i;
    if ((u32)max < count)
    {
        first += count - (u32)max;
        count = (u32)max;
    }
    for (i = 0; i < count; i++) lines[i] = s_recent[(first + i) % LOG_RECENT_LINES];
    return (int)count;
}
