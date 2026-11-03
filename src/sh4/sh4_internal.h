// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_SH4_INTERNAL_H
#define EMU_SH4_INTERNAL_H

#include "sh4.h"

#define SH4OP(name) void sh4op_##name(u16 op)

void sh4_exec_delay_slot(void);
void sh4_abort_exception(u32 expevt, u32 vectorOffset);
extern u32 g_sh4_inst_pc;

void sh4_sq_flush(u32 addr);

void sh4_fpu_disabled(void);

void sh4_fpu_bank_swap(void);

#endif
