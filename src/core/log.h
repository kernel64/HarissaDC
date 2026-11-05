// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_LOG_H
#define EMU_LOG_H

#include "types.h"

enum LogLevel
{
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
};

typedef void (*LogSink)(int level, const char *line);

void log_set_sink(LogSink sink);
LogSink log_get_sink(void);
void log_set_level(int maxLevel);
int log_level(void);
void log_write(int level, const char *fmt, ...);

#define LOG_RECENT_LINES 32
#define LOG_LINE_MAX 256
int log_recent(const char **lines, int max);

#define LOG_ERROR(...) log_write(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_INFO(...)  log_write(LOG_LEVEL_INFO, __VA_ARGS__)
extern int g_log_level;
#define LOG_DEBUG(...) do { if (g_log_level >= LOG_LEVEL_DEBUG) log_write(LOG_LEVEL_DEBUG, __VA_ARGS__); } while (0)

#endif
