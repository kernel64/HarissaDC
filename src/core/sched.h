// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_SCHED_H
#define EMU_SCHED_H

#include "types.h"

#define SH4_CLOCK_HZ 200000000u
#define SCHED_MAX_EVENTS 32
#define SCHED_NEVER 0xFFFFFFFFFFFFFFFFull

typedef void (*SchedCallback)(void *arg, u32 late);

void sched_reset(void);
int sched_register(const char *name, SchedCallback cb, void *arg);
void sched_after(int id, u32 cycles);
void sched_at(int id, u64 when);
void sched_cancel(int id);
int sched_is_scheduled(int id);
u64 sched_remaining(int id);

u64 sched_now(void);
u64 sched_now_exact(void);
void sched_set_cpu(u64 (*elapsed)(void), void (*stop)(void));
void sched_slice_end(u64 end);
void sched_advance(u32 cycles);
u64 sched_next(void);
void sched_run_due(void);
void sched_report(char *out, u32 size);

struct StateIO;
void sched_state(StateIO *io);

#endif
