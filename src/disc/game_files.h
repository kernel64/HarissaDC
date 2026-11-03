// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_GAME_FILES_H
#define EMU_GAME_FILES_H

#include "../core/types.h"
#include "archive7z.h"

#define GAME_PATH_MAX 300
#define GAME_DISCS_MAX 8

struct DiscSet
{
    int count;
    int current;
    int number[GAME_DISCS_MAX];
    char path[GAME_DISCS_MAX][GAME_PATH_MAX];
};

int game_is_listed(const char *name);
int game_is_archive(const char *name);
int game_disc_number(const char *name);
void game_disc_key(const char *name, char *out, int size);
void game_display_name(const char *name, char *out, int size);

void game_disc_set(const char *path, DiscSet *set);

void game_cache_dir(const char *cacheRoot, const char *path, char *out, int size);
int game_cache_images(const char *cacheRoot, const char *path, DiscSet *set);
int game_resolve(const char *cacheRoot, const char *path, Archive7zProgress progress, void *user, char *image, int imageSize,
                 char *error, int errorSize);

#endif
