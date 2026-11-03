// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_THREAD_H
#define EMU_THREAD_H

#include "types.h"

typedef void (*ThreadFn)(void *arg);
struct Thread;
struct Event;

Thread *thread_start(ThreadFn fn, void *arg, int hwThread);
void thread_join(Thread *t);
void thread_yield(void);

Event *event_create(void);
void event_destroy(Event *e);
void event_signal(Event *e);
int event_wait(Event *e, int ms);

struct Mutex;
Mutex *mutex_create(void);
void mutex_destroy(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);

void thread_barrier(void);

#endif
