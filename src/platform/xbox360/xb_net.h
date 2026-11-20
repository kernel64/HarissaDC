// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_NET_H
#define XB_NET_H

#include "../../core/types.h"

int NetStart(int retry);

void NetTestOff(int mode);
int NetTestMode(void);

int NetGet(const char *host, const char *path, int follow, u8 **body, u32 *len, u32 maxLen, char *location, int locSize);

int NetPost(const char *host, const char *path, const char *data, const char *type, const char *agent, u8 **body, u32 *len, u32 maxLen);
int NetDownload(const char *url, const char *file, u32 expected, int (*progress)(void *user, u32 done, u32 total), void *user);
int NetSha256File(const char *file, char hex[65], int (*progress)(void *user, u32 done, u32 total), void *user);

#endif
