// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "sound_thread.h"

#include <string.h>

#include "aica.h"
#include "arm7.h"
#include "holly.h"
#include "../core/log.h"
#include "../core/prof.h"
#include "../core/sched.h"
#include "../core/thread.h"
#include "../mem/mem.h"

#define SAMPLE_RATE 44100u
#define SH4_PER_SAMPLE (SH4_CLOCK_HZ / SAMPLE_RATE)
#define SH4_PER_SAMPLE_REM (SH4_CLOCK_HZ % SAMPLE_RATE)
#define ARM_PER_SAMPLE ((s32)(ARM7_CLOCK_HZ / SAMPLE_RATE))
#define SOUND_LAG_MAX g_sound_lag_max
u64 g_sound_lag_max = SH4_CLOCK_HZ / 60u;
#define QUEUE_SIZE 4096u
#define PROF_BATCH 256
#define PUBLISH_STEP (SH4_PER_SAMPLE * 16)
#define KICK_CYCLES ((u64)SH4_PER_SAMPLE * 32)

struct QueuedWrite
{
    u64 when;
    u32 addr, value;
    int size;
};

static Thread *s_thread;
static Event *s_wake, *s_parkedEvent;
static int s_on, s_irq;
static EMU_CACHE_ALIGNED volatile int s_quit;
static EMU_CACHE_ALIGNED volatile int s_parked;
static EMU_CACHE_ALIGNED volatile int s_kicked;
static EMU_CACHE_ALIGNED volatile int s_syncing;
static u64 s_published;
static EMU_CACHE_ALIGNED volatile u64 s_target;
static EMU_CACHE_ALIGNED volatile u64 s_now;
static u32 s_frac;
static QueuedWrite s_queue[QUEUE_SIZE];
static EMU_CACHE_ALIGNED volatile u32 s_head;
static EMU_CACHE_ALIGNED volatile u32 s_tail;

static void apply_writes(u64 upTo)
{
    u32 tail = s_tail, head = s_head;
    if (tail == head) return;
    thread_barrier();
    while (tail != head)
    {
        const QueuedWrite *w = &s_queue[tail % QUEUE_SIZE];
        if (w->when > upTo) break;
        aica_write(w->addr, w->value, w->size);
        tail++;
    }
    thread_barrier();
    s_tail = tail;
}

static SoundWaitStats s_waits;

void sound_thread_wait_stats(SoundWaitStats *out)
{
    *out = s_waits;
    memset(&s_waits, 0, sizeof(s_waits));
}

static int can_step(void)
{
    return s_now + SH4_PER_SAMPLE + 1 <= s_target;
}

static int spin_until(int (*cond)(void))
{
    u64 rate = prof_tick_rate(), start = prof_ticks(), limit;
    if (!start || rate < 1000) return cond();
    limit = rate / 500;
    while (!cond())
    {
        if (prof_ticks() - start > limit) return 0;
        thread_barrier();
    }
    return 1;
}

static int work_or_quit(void)
{
    return can_step() || s_quit;
}

static int parked_and_done(void)
{
    thread_barrier();
    return s_parked && !can_step();
}

static void sound_main(void *)
{
    u64 busyStart = 0, aicaTicks = 0;
    int batch = 0;
    while (!s_quit)
    {
        if (!can_step())
        {
            if (batch)
            {
                prof_add(PROF_ARM7, prof_ticks() - busyStart - aicaTicks);
                prof_add(PROF_AICA, aicaTicks);
                aicaTicks = 0;
                batch = 0;
            }
            thread_barrier();
            s_parked = 1;
            thread_barrier();
            if (s_syncing) event_signal(s_parkedEvent);
            if (!spin_until(work_or_quit)) event_wait(s_wake, 4);
            s_kicked = 0;
            continue;
        }
        s_parked = 0;
        if (!batch) busyStart = prof_ticks();
        apply_writes(s_now);
        arm7_run(ARM_PER_SAMPLE);
        {
            u64 t = prof_ticks();
            aica_sample();
            aicaTicks += prof_ticks() - t;
        }
        {
            u64 now = s_now + SH4_PER_SAMPLE;
            s_frac += SH4_PER_SAMPLE_REM;
            if (s_frac >= SAMPLE_RATE)
            {
                s_frac -= SAMPLE_RATE;
                now++;
            }
            s_now = now;
        }
        if (++batch >= PROF_BATCH)
        {
            u64 t = prof_ticks();
            prof_add(PROF_ARM7, t - busyStart - aicaTicks);
            prof_add(PROF_AICA, aicaTicks);
            aicaTicks = 0;
            busyStart = t;
            batch = 1;
        }
    }
}

int sound_thread_start(int hwThread)
{
    if (s_on) return 0;
    s_wake = event_create();
    s_parkedEvent = event_create();
    if (!s_wake || !s_parkedEvent) goto fail;
    s_quit = 0;
    s_parked = s_kicked = s_syncing = 0;
    s_published = 0;
    s_target = s_now = 0;
    s_frac = 0;
    s_head = s_tail = 0;
    s_irq = 0;
    aica_set_threaded(1);
    s_thread = thread_start(sound_main, NULL, hwThread);
    if (!s_thread)
    {
        aica_set_threaded(0);
        goto fail;
    }
    s_on = 1;
    LOG_INFO("[sound] ARM7 and AICA on their own thread (hardware thread %d)", hwThread);
    return 0;
fail:
    event_destroy(s_wake);
    event_destroy(s_parkedEvent);
    s_wake = s_parkedEvent = NULL;
    LOG_ERROR("[sound] cannot start the sound thread");
    return -1;
}

void sound_thread_stop(void)
{
    if (!s_on) return;
    s_quit = 1;
    thread_barrier();
    event_signal(s_wake);
    thread_join(s_thread);
    s_thread = NULL;
    event_destroy(s_wake);
    event_destroy(s_parkedEvent);
    s_wake = s_parkedEvent = NULL;
    s_on = 0;
    aica_set_threaded(0);
}

int sound_thread_on(void)
{
    return s_on;
}

static void pick_up_irq(void)
{
    int irq = aica_sh4_irq();
    if (irq != s_irq)
    {
        s_irq = irq;
        holly_ext(HOLLY_EXT_AICA, irq);
    }
}

static void publish(u64 sh4Now, int force)
{
    s_target = sh4Now;
    s_published = sh4Now;
    thread_barrier();
    if (s_parked && !s_kicked && (force || sh4Now >= s_now + KICK_CYCLES))
    {
        s_kicked = 1;
        event_signal(s_wake);
    }
}

void sound_thread_advance(u64 sh4Now)
{
    if (!s_on) return;
    pick_up_irq();
    if (sh4Now - s_published < PUBLISH_STEP && sh4Now <= s_now + SOUND_LAG_MAX) return;
    publish(sh4Now, 0);
    if (sh4Now > s_now + SOUND_LAG_MAX) prof_begin(PROF_SOUND_WAIT);
    else return;
    s_waits.ahead++;
    while (!s_quit && sh4Now > s_now + SOUND_LAG_MAX)
    {
        thread_barrier();
        if (s_parked && can_step() && !s_kicked)
        {
            s_kicked = 1;
            event_signal(s_wake);
        }
        thread_yield();
    }
    prof_end(PROF_SOUND_WAIT);
}

void sound_thread_sync(void)
{
    if (!s_on) return;
    prof_begin(PROF_SOUND_WAIT);
    s_syncing = 1;
    publish(sched_now_exact(), 1);
    while (!spin_until(parked_and_done)) event_wait(s_parkedEvent, 1);
    s_syncing = 0;
    apply_writes(~(u64)0);
    prof_end(PROF_SOUND_WAIT);
}

void sound_thread_hold(void)
{
    sound_thread_sync();
}

void sound_thread_restart(void)
{
    if (!s_on) return;
    s_target = 0;
    s_published = 0;
    thread_barrier();
    s_head = s_tail = 0;
    s_frac = 0;
    s_irq = 0;
    s_now = 0;
    thread_barrier();
}

static int immediate(u32 addr)
{
    u32 off = addr & 0x1FFFFu;
    return off >= 0x10000u || (off >= 0x28B4u && off < 0x28C0u) || (off & ~3u) == 0x2C00u;
}

static int write_queued(u32 addr)
{
    u32 tail, head = s_head;
    thread_barrier();
    for (tail = s_tail; tail != head; tail++)
        if (((s_queue[tail % QUEUE_SIZE].addr ^ addr) & ~3u) == 0) return 1;
    return 0;
}

u32 sound_sh4_read(u32 addr, int size)
{
    u32 v;
    if (!s_on) return aica_read(addr, size);
    if ((addr & 0xFFFFu) < 0x2000u && !write_queued(addr))
    {
        s_waits.fastReads++;
        thread_barrier();
        return aica_read(addr, size);
    }
    s_waits.reads++;
    sound_thread_sync();
    v = aica_read(addr, size);
    pick_up_irq();
    return v;
}

void sound_sh4_write(u32 addr, u32 value, int size)
{
    if (!g_sound_touched) g_sound_touched = 1;
    if (!s_on)
    {
        aica_write(addr, value, size);
        return;
    }
    if (immediate(addr))
    {
        s_waits.writes++;
        sound_thread_sync();
        aica_write(addr, value, size);
        pick_up_irq();
        return;
    }
    {
        u32 head = s_head;
        QueuedWrite *w;
        if (head - s_tail >= QUEUE_SIZE)
        {
            s_waits.queueFull++;
            sound_thread_sync();
            head = s_head;
        }
        w = &s_queue[head % QUEUE_SIZE];
        w->when = sched_now_exact();
        w->addr = addr;
        w->value = value;
        w->size = size;
        thread_barrier();
        s_head = head + 1;
    }
}

void sound_thread_set_time(u64 sh4Now)
{
    if (!s_on) return;
    s_target = sh4Now;
    s_published = sh4Now;
    thread_barrier();
    s_head = s_tail = 0;
    s_frac = 0;
    s_now = sh4Now;
    thread_barrier();
}
