// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_fs.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/log.h"

struct XbKString
{
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
};
extern "C" LONG __stdcall ObCreateSymbolicLink(XbKString *linkName, XbKString *deviceName);
extern "C" LONG __stdcall ObDeleteSymbolicLink(XbKString *linkName);

static void kstring(XbKString *s, char *text)
{
    s->Length = (USHORT)strlen(text);
    s->MaximumLength = (USHORT)(s->Length + 1);
    s->Buffer = text;
}

struct DriveMap
{
    const char *drive;
    const char *device;
};

static const DriveMap s_drives[] = {
    { "Usb0", "\\Device\\Mass0" },
    { "Usb1", "\\Device\\Mass1" },
    { "Usb2", "\\Device\\Mass2" },
    { "Hdd1", "\\Device\\Harddisk0\\Partition1" },
    { "Mu0", "\\Device\\Mu0" },
    { "Mu1", "\\Device\\Mu1" },
};

void xb_fs_mount_drives(void)
{
    u32 i;
    for (i = 0; i < EMU_ARRAY_COUNT(s_drives); i++)
    {
        char link[32], device[64];
        XbKString linkName, deviceName;
        LONG status;
        _snprintf(link, sizeof(link), "\\??\\%s:", s_drives[i].drive);
        link[sizeof(link) - 1] = 0;
        strncpy(device, s_drives[i].device, sizeof(device) - 1);
        device[sizeof(device) - 1] = 0;
        kstring(&linkName, link);
        kstring(&deviceName, device);
        ObDeleteSymbolicLink(&linkName);
        status = ObCreateSymbolicLink(&linkName, &deviceName);
        LOG_INFO("[fs] link %s -> %s: status %08X", link, device, (u32)status);
    }
}

static int can_list(const char *root)
{
    WIN32_FIND_DATAA fd;
    char pattern[32];
    HANDLE h;
    _snprintf(pattern, sizeof(pattern), "%s\\*", root);
    pattern[sizeof(pattern) - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        FindClose(h);
        return 1;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

int xb_fs_list_drives(char names[][8], int max)
{
    int n = 0;
    u32 i;
    if (n < max && can_list("game:")) strcpy(names[n++], "game");
    for (i = 0; i < EMU_ARRAY_COUNT(s_drives) && n < max; i++)
    {
        char root[16];
        _snprintf(root, sizeof(root), "%s:", s_drives[i].drive);
        root[sizeof(root) - 1] = 0;
        if (can_list(root)) strcpy(names[n++], s_drives[i].drive);
    }
    return n;
}

static int compare_entries(const void *a, const void *b)
{
    const XbDirEntry *x = (const XbDirEntry *)a, *y = (const XbDirEntry *)b;
    if (x->isDir != y->isDir) return y->isDir - x->isDir;
    return _stricmp(x->name, y->name);
}

int xb_fs_list_dir(const char *dir, XbDirEntry *entries, int max)
{
    WIN32_FIND_DATAA fd;
    char pattern[300];
    HANDLE h;
    int n = 0;
    _snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    pattern[sizeof(pattern) - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    do
    {
        XbDirEntry *e;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (n >= max) break;
        e = &entries[n++];
        strncpy(e->name, fd.cFileName, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = 0;
        e->isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e->size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(entries, n, sizeof(entries[0]), compare_entries);
    return n;
}

void xb_fs_join(char *out, int outSize, const char *dir, const char *name)
{
    size_t len = strlen(dir);
    if (len && dir[len - 1] == '\\') _snprintf(out, outSize, "%s%s", dir, name);
    else _snprintf(out, outSize, "%s\\%s", dir, name);
    out[outSize - 1] = 0;
}

int xb_fs_parent(char *path)
{
    char *slash = strrchr(path, '\\');
    if (!slash) return 0;
    *slash = 0;
    return 1;
}

int xb_fs_exists(const char *path)
{
    return GetFileAttributesA(path) != 0xFFFFFFFF;
}

static const char *s_dataDrive = "game";

void xb_fs_set_data_drive(const char *drive)
{
    if (drive) s_dataDrive = drive;
}

const char *xb_fs_data_dir(char *out, int outSize, const char *dir)
{
    _snprintf(out, outSize, "%s:\\%s", s_dataDrive, dir);
    out[outSize - 1] = 0;
    CreateDirectoryA(out, NULL);
    return out;
}

const char *xb_fs_data_path(char *out, int outSize, const char *dir, const char *name)
{
    char d[64];
    xb_fs_join(out, outSize, xb_fs_data_dir(d, sizeof(d), dir), name);
    return out;
}
