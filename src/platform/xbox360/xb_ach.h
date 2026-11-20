// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_ACH_H
#define XB_ACH_H

#include "xb_ach_ids.h"

void XbAchInit(int enabled);
void XbAchUnlock(int id);
void XbAchCheckRa(void);

#endif
