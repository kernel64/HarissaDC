// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_ARCHIVEZIP_H
#define EMU_ARCHIVEZIP_H

#include "archive7z.h"

int archivezip_extract(const char *path, const char *outDir, Archive7zProgress progress, void *user, char *error,
                       int errorSize);

#endif
