// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "cdda.h"

#include <string.h>

#include "gdrom.h"
#include "../core/log.h"
#include "../core/sched.h"
#include "../core/state.h"
#include "../core/thread.h"
#include "../disc/disc.h"

#define SECTOR_FRAMES 588
#define RING_SECTORS 48
#define EMU_SECTORS 8
#define RING_FRAMES (SECTOR_FRAMES * RING_SECTORS)
#define SECTOR_CYCLES (SH4_CLOCK_HZ / 75)

struct CddaState
{
    u32 status;
    u32 start, end, cur;
    u32 repeat;
    u32 played;
};

static CddaState s_cd;
static s16 s_ring[RING_FRAMES * 2];
static volatile u32 s_write, s_read;
static int s_ev = -1;

int g_cdda_realtime;
static u32 s_ringFad[RING_SECTORS];
static int s_ended;
static Mutex *s_mx;

static void ring_lock(void)
{
    if (s_mx) mutex_lock(s_mx);
}

static void ring_unlock(void)
{
    if (s_mx) mutex_unlock(s_mx);
}

static void ring_empty(void)
{
    ring_lock();
    s_read = s_write;
    ring_unlock();
}

static void heard(void)
{
    u32 r, w;
    if (!g_cdda_realtime) return;
    thread_barrier();
    r = s_read;
    w = s_write;
    if (r != w) s_cd.played = s_ringFad[(r / SECTOR_FRAMES) % RING_SECTORS];
    else if (s_ended && s_cd.status == CDDA_PLAYING)
    {
        s_cd.status = CDDA_DONE;
        s_ended = 0;
    }
}

static void fill(void)
{
    u8 raw[2352];
    u32 room = (g_cdda_realtime ? RING_SECTORS : EMU_SECTORS) * SECTOR_FRAMES;
    while (s_cd.status == CDDA_PLAYING && !s_ended && s_write - s_read <= room - SECTOR_FRAMES * 2)
    {
        Disc *d = gdrom_disc();
        u32 i, at = s_write % RING_FRAMES;
        if (s_cd.cur > s_cd.end)
        {
            if (s_cd.repeat == 0)
            {
                if (g_cdda_realtime) s_ended = 1;
                else s_cd.status = CDDA_DONE;
                break;
            }
            if (s_cd.repeat != 15) s_cd.repeat--;
            s_cd.cur = s_cd.start;
        }
        if (!d || disc_read(d, s_cd.cur, 1, raw, 2352) == 0) memset(raw, 0, sizeof(raw));
        for (i = 0; i < SECTOR_FRAMES * 2; i++) s_ring[(at * 2 + i) % (RING_FRAMES * 2)] = (s16)(raw[i * 2] | (raw[i * 2 + 1] << 8));
        s_ringFad[(s_write / SECTOR_FRAMES) % RING_SECTORS] = s_cd.cur;
        thread_barrier();
        s_write += SECTOR_FRAMES;
        if (!g_cdda_realtime) s_cd.played = s_cd.cur;
        s_cd.cur++;
    }
}

static void sector_event(void *, u32)
{
    heard();
    fill();
    if (s_cd.status == CDDA_PLAYING) sched_after(s_ev, SECTOR_CYCLES);
}

void cdda_init(void)
{
    if (s_ev < 0) s_ev = sched_register("cdda", sector_event, NULL);
    if (!s_mx) s_mx = mutex_create();
}

void cdda_reset(void)
{
    memset(&s_cd, 0, sizeof(s_cd));
    s_cd.status = CDDA_NONE;
    s_cd.cur = s_cd.played = 150;
    ring_lock();
    s_write = s_read = 0;
    ring_unlock();
    s_ended = 0;
    if (s_ev >= 0) sched_cancel(s_ev);
}

void cdda_play(u32 start, u32 end, u32 repeat)
{
    if (end < start) end = start;
    LOG_INFO("[cdda] play FAD %u-%u, repeat %u", start, end, repeat);
    s_cd.start = start;
    s_cd.end = end;
    s_cd.cur = s_cd.played = start;
    s_cd.repeat = repeat > 15 ? 15 : repeat;
    s_cd.status = CDDA_PLAYING;
    s_ended = 0;
    ring_empty();
    fill();
    sched_after(s_ev, SECTOR_CYCLES);
}

void cdda_pause(void)
{
    LOG_DEBUG("[cdda] pause at FAD %u (status %02X)", s_cd.played, s_cd.status);
    heard();
    if (s_cd.status != CDDA_PLAYING) return;
    s_cd.status = CDDA_PAUSED;
    s_cd.cur = s_cd.played;
    s_ended = 0;
    ring_empty();
    sched_cancel(s_ev);
}

void cdda_resume(void)
{
    LOG_DEBUG("[cdda] resume at FAD %u (status %02X)", s_cd.played, s_cd.status);
    if (s_cd.status != CDDA_PAUSED) return;
    s_cd.status = CDDA_PLAYING;
    fill();
    sched_after(s_ev, SECTOR_CYCLES);
}

void cdda_seek(u32 fad)
{
    LOG_DEBUG("[cdda] seek to FAD %u (status %02X)", fad, s_cd.status);
    cdda_pause();
    s_cd.start = s_cd.cur = s_cd.played = fad;
}

void cdda_stop(void)
{
    LOG_DEBUG("[cdda] stop at FAD %u (status %02X)", s_cd.played, s_cd.status);
    heard();
    if (s_cd.status == CDDA_PLAYING || s_cd.status == CDDA_PAUSED) s_cd.status = CDDA_NONE;
    s_ended = 0;
    ring_empty();
    sched_cancel(s_ev);
}

int cdda_playing(void)
{
    heard();
    return s_cd.status == CDDA_PLAYING;
}

u32 cdda_status(void)
{
    heard();
    return s_cd.status;
}

u32 cdda_fad(void)
{
    heard();
    return s_cd.played;
}

u32 cdda_repeats(void)
{
    return s_cd.repeat;
}

int cdda_frame(s32 *left, s32 *right)
{
    u32 at;
    if (g_cdda_realtime || s_read == s_write) return 0;
    thread_barrier();
    at = (s_read % RING_FRAMES) * 2;
    *left = s_ring[at];
    *right = s_ring[at + 1];
    s_read++;
    return 1;
}

u32 cdda_rt_read(s16 *out, u32 frames)
{
    u32 n = 0;
    ring_lock();
    thread_barrier();
    while (n < frames && s_read != s_write)
    {
        u32 at = (s_read % RING_FRAMES) * 2;
        out[n * 2] = s_ring[at];
        out[n * 2 + 1] = s_ring[at + 1];
        s_read++;
        n++;
    }
    thread_barrier();
    ring_unlock();
    return n;
}

void cdda_state(StateIO *io)
{
    state_tag(io, "CDDA");
    if (!io->loading) heard();
    STATE(io, s_cd);
    if (io->loading)
    {
        ring_lock();
        s_write = s_read = 0;
        ring_unlock();
        s_ended = 0;
        if (s_cd.status == CDDA_PLAYING)
        {
            s_cd.cur = s_cd.played;
            fill();
            sched_after(s_ev, SECTOR_CYCLES);
        }
    }
}
