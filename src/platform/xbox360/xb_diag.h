// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_DIAG_H
#define XB_DIAG_H

struct DiagResult
{
    char name[48];
    int ok;
    char detail[160];
};

#define DIAG_MAX_RESULTS 12

int xb_diag_run(int level, DiagResult *out, int max);
void xb_fpu_bench(void);

#endif
