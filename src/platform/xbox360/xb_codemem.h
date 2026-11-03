// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_CODEMEM_H
#define XB_CODEMEM_H

#include "../../core/types.h"

#define XB_CODEMEM_BYTES (28u * 1024u * 1024u)

u32 *xb_codemem_base(void);
void xb_codemem_flush(void *p, u32 size);

#endif
