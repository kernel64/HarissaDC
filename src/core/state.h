// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_STATE_H
#define EMU_STATE_H

#include "types.h"

struct StateIO
{
    u8 *buf;
    u32 size;
    u32 pos;
    int loading;
    int error;
};

void state_io(StateIO *io, void *p, u32 bytes);
void state_tag(StateIO *io, const char *tag4);
#define STATE(io, v) state_io((io), &(v), (u32)sizeof(v))

#endif
