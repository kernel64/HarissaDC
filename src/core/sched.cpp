// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "sched.h"

#include <stdio.h>

#include "log.h"
#include "state.h"

struct SchedEvent
{
    const char *name;
    SchedCallback cb;
    void *arg;
    u64 when;
    u32 runs;
};

static SchedEvent s_events[SCHED_MAX_EVENTS];
static int s_count;
static u64 s_now;
static u64 s_next = SCHED_NEVER;
static u64 (*s_elapsed)(void);
static void (*s_stop)(void);
static u64 s_sliceEnd = SCHED_NEVER;

void sched_report(char *out, u32 size)
{
    u32 pos = 0, done = 0;
    out[0] = 0;
    while (pos + 40 < size)
    {
        int i, best = -1;
        for (i = 0; i < s_count; i++)
            if (!(done & (1u << i)) && s_events[i].runs && (best < 0 || s_events[i].runs > s_events[best].runs)) best = i;
        if (best < 0) break;
        done |= 1u << best;
        pos += (u32)sprintf(out + pos, "%s%.24s %u", pos ? ", " : "", s_events[best].name, s_events[best].runs);
    }
    for (int i = 0; i < s_count; i++) s_events[i].runs = 0;
}

void sched_set_cpu(u64 (*elapsed)(void), void (*stop)(void))
{
    s_elapsed = elapsed;
    s_stop = stop;
}

void sched_slice_end(u64 end)
{
    s_sliceEnd = end;
}

u64 sched_now_exact(void)
{
    return s_now + (s_sliceEnd != SCHED_NEVER && s_elapsed ? s_elapsed() : 0);
}

static void update_next(void)
{
    u64 next = SCHED_NEVER;
    int i;
    for (i = 0; i < s_count; i++)
        if (s_events[i].when < next) next = s_events[i].when;
    s_next = next;
}

void sched_reset(void)
{
    int i;
    s_now = 0;
    for (i = 0; i < s_count; i++) s_events[i].when = SCHED_NEVER;
    s_next = SCHED_NEVER;
}

int sched_register(const char *name, SchedCallback cb, void *arg)
{
    int i;
    for (i = 0; i < s_count; i++)
        if (s_events[i].name == name)
        {
            s_events[i].cb = cb;
            s_events[i].arg = arg;
            s_events[i].when = SCHED_NEVER;
            return i;
        }
    if (s_count >= SCHED_MAX_EVENTS)
    {
        LOG_ERROR("[sched] too many events (%s)", name);
        return -1;
    }
    s_events[s_count].name = name;
    s_events[s_count].cb = cb;
    s_events[s_count].arg = arg;
    s_events[s_count].when = SCHED_NEVER;
    return s_count++;
}

void sched_at(int id, u64 when)
{
    if (id < 0) return;
    s_events[id].when = when;
    if (when < s_next) s_next = when;
    else update_next();
    if (when < s_sliceEnd && s_stop) s_stop();
}

void sched_after(int id, u32 cycles)
{
    sched_at(id, sched_now_exact() + cycles);
}

void sched_cancel(int id)
{
    if (id < 0 || s_events[id].when == SCHED_NEVER) return;
    s_events[id].when = SCHED_NEVER;
    update_next();
}

int sched_is_scheduled(int id)
{
    return id >= 0 && s_events[id].when != SCHED_NEVER;
}

u64 sched_remaining(int id)
{
    if (id < 0 || s_events[id].when == SCHED_NEVER) return SCHED_NEVER;
    u64 now = sched_now_exact();
    return s_events[id].when > now ? s_events[id].when - now : 0;
}

u64 sched_now(void) { return s_now; }
void sched_advance(u32 cycles) { s_now += cycles; }
u64 sched_next(void) { return s_next; }

void sched_run_due(void)
{
    while (s_next <= s_now)
    {
        int i, best = -1;
        u64 when = SCHED_NEVER;
        for (i = 0; i < s_count; i++)
            if (s_events[i].when < when)
            {
                when = s_events[i].when;
                best = i;
            }
        if (best < 0 || when > s_now) break;
        s_events[best].when = SCHED_NEVER;
        s_events[best].runs++;
        update_next();
        s_events[best].cb(s_events[best].arg, (u32)(s_now - when));
    }
}

void sched_state(StateIO *io)
{
    int i, n = s_count;
    state_tag(io, "SCHD");
    STATE(io, s_now);
    STATE(io, n);
    if (io->loading && n > s_count)
    {
        io->error = 1;
        return;
    }
    for (i = 0; i < n; i++) STATE(io, s_events[i].when);
    if (io->loading)
        for (i = n; i < s_count; i++) s_events[i].when = SCHED_NEVER;
    if (io->loading) update_next();
}
