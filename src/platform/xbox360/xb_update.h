// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_UPDATE_H
#define XB_UPDATE_H

#include <stddef.h>

#include "../../core/types.h"

void UpdateCheckStart(const char *current);

int UpdateCheckTake(char *version, size_t size, int *installable);

enum
{
    UPDATE_IDLE,
    UPDATE_DOWNLOAD,
    UPDATE_VERIFY,
    UPDATE_EXTRACT,
    UPDATE_INSTALL,
    UPDATE_DONE,
    UPDATE_FAILED,
    UPDATE_CANCELLED
};
struct UpdateProgress
{
    int stage;
    u64 done, total;
    char error[128];
};
void UpdateInstallStart(void);
void UpdateInstallState(UpdateProgress *p);
void UpdateInstallCancel(void);
void UpdateRestart(void);

#endif
