// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "state.h"

#include <string.h>

void state_io(StateIO *io, void *p, u32 bytes)
{
    if (io->error) return;
    if (io->buf)
    {
        if (io->pos + bytes > io->size)
        {
            io->error = 1;
            return;
        }
        if (io->loading) memcpy(p, io->buf + io->pos, bytes);
        else memcpy(io->buf + io->pos, p, bytes);
    }
    io->pos += bytes;
}

void state_tag(StateIO *io, const char *tag4)
{
    char t[4];
    memcpy(t, tag4, 4);
    state_io(io, t, 4);
    if (io->loading && io->buf && !io->error && memcmp(t, tag4, 4) != 0) io->error = 1;
}
