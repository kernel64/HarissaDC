// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "aica.h"
#include "cdda.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arm7.h"
#include "holly.h"
#include "../core/log.h"
#include "../core/state.h"
#include "../core/prof.h"
#include "../core/sched.h"
#include "../core/thread.h"
#include "../mem/mem.h"

#define AICA_REG_BASE 0x00700000u
#define AICA_REG_SIZE 0x8000u
#define RTC_BASE      0x00710000u

static u16 s_regs[AICA_REG_SIZE / 4];
#define REG(off) s_regs[((off) & 0x7FFF) >> 2]

#define R_TIMER_A 0x2890
#define R_TIMER_B 0x2894
#define R_TIMER_C 0x2898
#define R_SCIEB   0x289C
#define R_SCIPD   0x28A0
#define R_SCIRE   0x28A4
#define R_SCILV0  0x28A8
#define R_SCILV1  0x28AC
#define R_SCILV2  0x28B0
#define R_MCIEB   0x28B4
#define R_MCIPD   0x28B8
#define R_MCIRE   0x28BC
#define R_DMA_EA_HI 0x2880
#define R_DMA_EA_LO 0x2884
#define R_DMA_RG    0x2888
#define R_DMA_CTRL  0x288C
#define R_ARMRST  0x2C00
#define R_L       0x2D00
#define R_M       0x2D04

#define INT_DMA    (1u << 4)
#define INT_SCPU   (1u << 5)
#define INT_TIMERA (1u << 6)
#define INT_SAMPLE (1u << 10)

#define SAMPLE_CYCLES (SH4_CLOCK_HZ / 44100)

static u32 s_rtcBase;
static u64 s_rtcTime;
static int s_rtcWriteEnable;
static u32 s_timerPrescale[3];
static int s_evSample;
static u32 s_armLevel;

#define EPOCH_1950_TO_1970 631152000u

u32 g_fixed_time;

u32 host_time(void)
{
    return g_fixed_time ? g_fixed_time : (u32)time(NULL);
}

void aica_rtc_set(u32 seconds)
{
    s_rtcBase = seconds;
    s_rtcTime = sched_now_exact();
}

static u32 rtc_now(void)
{
    return s_rtcBase + (u32)((sched_now_exact() - s_rtcTime) / SH4_CLOCK_HZ);
}

static void update_arm_irq(void)
{
    u32 pending = REG(R_SCIEB) & REG(R_SCIPD) & 0x7FF, i;
    s_armLevel = 0;
    for (i = 0; i < 11; i++)
        if (pending & (1u << i))
        {
            u32 bit = 1u << (i > 7 ? 7 : i);
            s_armLevel = ((REG(R_SCILV0) & bit) ? 1 : 0) | ((REG(R_SCILV1) & bit) ? 2 : 0) | ((REG(R_SCILV2) & bit) ? 4 : 0);
            break;
        }
    arm7_set_fiq(pending != 0);
}

static int s_threaded;
static volatile int s_sh4Irq;

static void update_sh4_irq(void)
{
    int level = (REG(R_MCIEB) & REG(R_MCIPD) & 0x7FF) != 0;
    if (s_threaded) s_sh4Irq = level;
    else holly_ext(HOLLY_EXT_AICA, level);
}

void aica_set_threaded(int on)
{
    s_threaded = on;
    sched_cancel(s_evSample);
    if (!on) sched_after(s_evSample, SAMPLE_CYCLES);
}

int aica_sh4_irq(void)
{
    return s_sh4Irq;
}

static void raise(u32 bits)
{
    REG(R_SCIPD) |= (u16)bits;
    REG(R_MCIPD) |= (u16)bits;
    update_arm_irq();
    update_sh4_irq();
}

static void timer_sample(void)
{
    int t;
    for (t = 0; t < 3; t++)
    {
        u16 *r = &REG(R_TIMER_A + t * 4);
        u32 pre = 1u << ((*r >> 8) & 7);
        if (++s_timerPrescale[t] < pre) continue;
        s_timerPrescale[t] = 0;
        *r = (u16)((*r & 0xFF00) | ((*r + 1) & 0xFF));
        if ((*r & 0xFF) == 0) raise(INT_TIMERA << t);
    }
}

#define CH_REG(ch, r) REG((u32)(ch) * 0x80 + (r))
#define R_MVOL    0x2800
#define R_MSLC    0x280C
#define R_STATUS  0x2810
#define R_CA      0x2814

enum { EG_ATTACK, EG_DECAY1, EG_DECAY2, EG_RELEASE, EG_OFF };

struct AicaChan
{
    int on;
    int eg;
    u32 level;
    u32 pos, phase;
    u32 sa, lsa, lea, fmt;
    s32 cur;
    s32 adpcmPrev, adpcmStep, loopPrev, loopStep;
    int loopSaved, loopEnd;
};

static AicaChan s_ch[64];
static s16 *s_out;
#define OUT_FRAMES 16384u
static volatile u32 s_outWrite, s_outRead;
static s32 s_gain[1024];

static void build_gain_table(void)
{
    int i;
    for (i = 0; i < 1024; i++)
    {
        double db = i * 0.09375, g = 1.0;
        int k;
        while (db >= 6.0206) { g *= 0.5; db -= 6.0206; }
        for (k = 0; k < 8; k++) g *= 1.0 - (db / 8.0) * 0.1151292546497;
        s_gain[i] = i >= 1023 ? 0 : (s32)(g * 65536.0);
    }
}

static u32 eg_step(u32 rate, int attack)
{
    double ms, samples;
    if (rate == 0) return 0;
    if (attack && rate >= 62) return 0x3FF << 16;
    ms = (attack ? 8100.0 : 118200.0);
    while (rate > 4) { ms *= 0.5; rate -= 4; }
    ms *= 1.0 - (rate - 1) * 0.1591;
    samples = ms * 44.1;
    if (samples < 1.0) samples = 1.0;
    return (u32)((double)(0x3FF << 16) / samples);
}

static u32 s_egStep[64][2];

static u32 effective_rate(int ch, u32 rate)
{
    u32 krs = (CH_REG(ch, 0x14) >> 10) & 15, r = rate * 2;
    if (rate == 0) return 0;
    if (krs != 15)
    {
        s32 oct = (s32)((CH_REG(ch, 0x18) >> 11) & 15);
        if (oct & 8) oct -= 16;
        r += (u32)(s32)((s32)krs + oct) * 2 + ((CH_REG(ch, 0x18) >> 9) & 1);
        if ((s32)r < 0) r = 0;
    }
    return r > 63 ? 63 : r;
}

int g_aica_trace;
static void update_addresses(int ch);
static u32 s_sampleNo;

static void key_on(int ch)
{
    AicaChan *c = &s_ch[ch];
    u32 r0 = CH_REG(ch, 0x00);
    if (g_aica_trace)
        LOG_INFO("[aica] t %u key on ch %d SA %06X fmt %u loop %u LSA %X LEA %X%s", s_sampleNo, ch, ((r0 & 0x7F) << 16) | CH_REG(ch, 0x04),
                 (r0 >> 7) & 3, (r0 >> 9) & 1, CH_REG(ch, 0x08), CH_REG(ch, 0x0C), c->on ? " (restart)" : "");
    c->on = 1;
    c->eg = EG_ATTACK;
    c->level = 0x3FF << 16;
    c->pos = 0;
    c->phase = 0;
    update_addresses(ch);
    c->adpcmPrev = 0;
    c->adpcmStep = 0x7F;
    c->loopSaved = 0;
    c->loopEnd = 0;
    c->cur = 0;
}

static void update_addresses(int ch)
{
    AicaChan *c = &s_ch[ch];
    u32 r0 = CH_REG(ch, 0x00), sa = ((r0 & 0x7F) << 16) | CH_REG(ch, 0x04);
    if (((r0 >> 7) & 3) == 0) sa &= ~1u;
    if (g_aica_trace && c->on && (sa != c->sa || CH_REG(ch, 0x08) != c->lsa || CH_REG(ch, 0x0C) != c->lea))
        LOG_INFO("[aica] t %u ch %d while playing (pos %X): SA %06X>%06X LSA %X>%X LEA %X>%X", s_sampleNo, ch, c->pos, c->sa, sa,
                 c->lsa, CH_REG(ch, 0x08), c->lea, CH_REG(ch, 0x0C));
    c->fmt = (r0 >> 7) & 3;
    c->sa = sa;
    c->lsa = CH_REG(ch, 0x08);
    c->lea = CH_REG(ch, 0x0C);
}

static void key_off(int ch)
{
    if (s_ch[ch].on && s_ch[ch].eg != EG_OFF) s_ch[ch].eg = EG_RELEASE;
}

static void key_execute(void)
{
    int ch;
    for (ch = 0; ch < 64; ch++)
    {
        int want = (CH_REG(ch, 0x00) >> 14) & 1;
        if (want && (!s_ch[ch].on || s_ch[ch].eg == EG_RELEASE)) key_on(ch);
        else if (!want) key_off(ch);
    }
}

static s32 adpcm_next(AicaChan *c, u32 nibble)
{
    static const s32 scale[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
    s32 diff = ((s32)(nibble & 7) * 2 + 1) * c->adpcmStep / 8;
    s32 v = (nibble & 8) ? c->adpcmPrev - diff : c->adpcmPrev + diff;
    c->adpcmPrev = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
    c->adpcmStep = c->adpcmStep * scale[nibble & 7] >> 8;
    if (c->adpcmStep < 0x7F) c->adpcmStep = 0x7F;
    if (c->adpcmStep > 0x6000) c->adpcmStep = 0x6000;
    return c->adpcmPrev;
}

static s32 fetch(AicaChan *c)
{
    u32 a;
    switch (c->fmt)
    {
    case 0:
        a = (c->sa + c->pos * 2) & (ARAM_SIZE - 2);
        return (s16)buf_r16(g_aram, a);
    case 1:
        a = (c->sa + c->pos) & (ARAM_SIZE - 1);
        return (s32)(s8)buf_r8(g_aram, a) << 8;
    default:
        a = (c->sa + (c->pos >> 1)) & (ARAM_SIZE - 1);
        return adpcm_next(c, (buf_r8(g_aram, a) >> ((c->pos & 1) * 4)) & 15);
    }
}

static void advance(int ch, AicaChan *c, u32 steps)
{
    while (steps--)
    {
        c->pos++;
        if (c->fmt >= 2 && c->pos == c->lsa && !c->loopSaved)
        {
            c->loopPrev = c->adpcmPrev;
            c->loopStep = c->adpcmStep;
            c->loopSaved = 1;
        }
        if (c->pos >= c->lea)
        {
            int stop = !((CH_REG(ch, 0x00) >> 9) & 1);
            if (c->lsa > c->lea)
            {
                if (c->pos < c->lsa) goto next;
                stop = 1;
            }
            c->loopEnd = 1;
            if (!stop)
            {
                c->pos = c->lsa;
                if (c->fmt >= 2 && c->loopSaved)
                {
                    c->adpcmPrev = c->loopPrev;
                    c->adpcmStep = c->loopStep;
                }
            }
            else
            {
                c->pos = 0;
                c->on = 0;
                c->eg = EG_OFF;
                return;
            }
        }
    next:
        c->cur = fetch(c);
    }
}

static void envelope(int ch, AicaChan *c)
{
    u32 r4 = CH_REG(ch, 0x10), r5 = CH_REG(ch, 0x14), dl = ((r5 >> 5) & 31) << (5 + 16);
    switch (c->eg)
    {
    case EG_ATTACK:
    {
        u32 s = s_egStep[effective_rate(ch, r4 & 31)][1];
        c->level = c->level > s ? c->level - s : 0;
        if (!c->level) c->eg = EG_DECAY1;
        break;
    }
    case EG_DECAY1:
        c->level += s_egStep[effective_rate(ch, (r4 >> 6) & 31)][0];
        if (c->level >= dl) c->eg = EG_DECAY2;
        break;
    case EG_DECAY2:
        c->level += s_egStep[effective_rate(ch, (r4 >> 11) & 31)][0];
        break;
    case EG_RELEASE:
        c->level += s_egStep[effective_rate(ch, r5 & 31)][0];
        break;
    }
    if (c->level >= (0x3FFu << 16))
    {
        c->level = 0x3FF << 16;
        if (c->eg == EG_RELEASE || c->eg == EG_DECAY2) { c->on = 0; c->eg = EG_OFF; }
    }
}

static u32 level_att(u32 v15) { return v15 ? (15 - v15) * 32 : 1023; }

static void mix_sample(void)
{
    s32 left = 0, right = 0;
    u32 mvol = REG(R_MVOL) & 15;
    int ch;
    for (ch = 0; ch < 64; ch++)
    {
        AicaChan *c = &s_ch[ch];
        u32 r6, step, att, pan, sendl, sendr, disdl;
        s32 oct, v;
        if (!c->on) continue;
        r6 = CH_REG(ch, 0x18);
        oct = (s32)((r6 >> 11) & 15);
        if (oct & 8) oct -= 16;
        step = oct >= 0 ? (1024 + (r6 & 0x7FF)) << oct : (1024 + (r6 & 0x7FF)) >> -oct;
        if (c->pos == 0 && c->phase == 0) c->cur = fetch(c);
        envelope(ch, c);
        if (!c->on) continue;
        disdl = (CH_REG(ch, 0x24) >> 8) & 15;
        if (!disdl) disdl = (CH_REG(ch, 0x20) >> 4) & 15;
        att = (CH_REG(ch, 0x28) >> 8) * 4 + (c->level >> 16) + level_att(disdl);
        if (att < 1023)
        {
            v = (s32)(((s64)c->cur * s_gain[att]) >> 16);
            pan = CH_REG(ch, 0x24) & 31;
            sendl = sendr = 0;
            if (pan & 16) sendr = (pan & 15) * 32;
            else sendl = (pan & 15) * 32;
            if ((pan & 15) == 15) { if (pan & 16) sendr = 1023; else sendl = 1023; }
            left += sendl >= 1023 ? 0 : (s32)(((s64)v * s_gain[sendl]) >> 16);
            right += sendr >= 1023 ? 0 : (s32)(((s64)v * s_gain[sendr]) >> 16);
        }
        c->phase += step;
        if (c->phase >= 1024)
        {
            advance(ch, c, c->phase >> 10);
            c->phase &= 1023;
        }
    }
    {
        s32 in[2];
        int k;
        if (cdda_frame(&in[0], &in[1]))
            for (k = 0; k < 2; k++)
            {
                u32 r = REG(0x2040 + k * 4), sdl = (r >> 8) & 15, pan = r & 31, att, sendl = 0, sendr = 0;
                s32 v;
                if (!sdl) continue;
                att = level_att(sdl);
                v = (s32)(((s64)in[k] * s_gain[att]) >> 16);
                if (pan & 16) sendr = (pan & 15) * 32;
                else sendl = (pan & 15) * 32;
                if ((pan & 15) == 15) { if (pan & 16) sendr = 1023; else sendl = 1023; }
                left += sendl >= 1023 ? 0 : (s32)(((s64)v * s_gain[sendl]) >> 16);
                right += sendr >= 1023 ? 0 : (s32)(((s64)v * s_gain[sendr]) >> 16);
            }
    }
    if (mvol != 15)
    {
        s32 g = mvol ? s_gain[(15 - mvol) * 32] : 0;
        left = (s32)(((s64)left * g) >> 16);
        right = (s32)(((s64)right * g) >> 16);
    }
    if (left > 32767) left = 32767;
    if (left < -32768) left = -32768;
    if (right > 32767) right = 32767;
    if (right < -32768) right = -32768;
    if (s_out && s_outWrite - s_outRead < OUT_FRAMES)
    {
        u32 i = (s_outWrite % OUT_FRAMES) * 2;
        s_out[i] = (s16)left;
        s_out[i + 1] = (s16)right;
        if (s_threaded) thread_barrier();
        s_outWrite++;
    }
}

u32 aica_audio_read(s16 *out, u32 frames)
{
    u32 n = 0;
    thread_barrier();
    while (n < frames && s_outRead != s_outWrite)
    {
        u32 i = (s_outRead % OUT_FRAMES) * 2;
        out[n * 2] = s_out[i];
        out[n * 2 + 1] = s_out[i + 1];
        s_outRead++;
        n++;
    }
    thread_barrier();
    return n;
}

u32 aica_audio_available(void)
{
    return s_outWrite - s_outRead;
}

void aica_sample(void)
{
    s_sampleNo++;
    timer_sample();
    mix_sample();
    raise(INT_SAMPLE);
}

static void sample_event(void *, u32 late)
{
    prof_begin(PROF_AICA);
    aica_sample();
    prof_end(PROF_AICA);
    sched_after(s_evSample, SAMPLE_CYCLES > late ? SAMPLE_CYCLES - late : 1);
}

static void internal_dma(void)
{
    u32 wave = ((((u32)REG(R_DMA_EA_HI) >> 9) & 0x7F) << 16 | (REG(R_DMA_EA_LO) & 0xFFFC)) & (ARAM_SIZE - 4);
    u32 regOff = REG(R_DMA_RG) & 0x7FFC, gate = REG(R_DMA_RG) >> 15;
    u32 len = (REG(R_DMA_CTRL) >> 2) & 0x1FFF, toRam = REG(R_DMA_CTRL) >> 15, i;
    for (i = 0; i < len; i++)
    {
        u32 w = (wave + i * 4) & (ARAM_SIZE - 4), r = (regOff + i * 4) & 0x7FFC;
        if (gate)
        {
            if (toRam) buf_w32(g_aram, w, 0);
            else aica_write(AICA_REG_BASE + r, 0, 4);
        }
        else if (toRam)
            buf_w32(g_aram, w, aica_read(AICA_REG_BASE + r, 4));
        else
            aica_write(AICA_REG_BASE + r, buf_r32(g_aram, w), 4);
    }
    REG(R_DMA_CTRL) &= (u16)~1u;
    raise(INT_DMA);
}

void aica_init(void)
{
    u32 r;
    s_evSample = sched_register("aica_sample", sample_event, NULL);
    if (!s_out) s_out = (s16 *)calloc(OUT_FRAMES * 2, sizeof(s16));
    build_gain_table();
    for (r = 0; r < 64; r++)
    {
        s_egStep[r][0] = eg_step(r, 0);
        s_egStep[r][1] = eg_step(r, 1);
    }
}

void aica_reset(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    memset(s_timerPrescale, 0, sizeof(s_timerPrescale));
    memset(s_ch, 0, sizeof(s_ch));
    {
        int ch;
        for (ch = 0; ch < 64; ch++)
        {
            s_ch[ch].eg = EG_OFF;
            s_ch[ch].level = 0x3FFu << 16;
        }
    }
    s_outWrite = s_outRead = 0;
    REG(R_ARMRST) = 1;
    arm7_set_enabled(0);
    arm7_set_fiq(0);
    s_armLevel = 0;
    aica_rtc_set(host_time() + EPOCH_1950_TO_1970);
    s_rtcWriteEnable = 0;
    s_sh4Irq = 0;
    if (!s_threaded) sched_after(s_evSample, SAMPLE_CYCLES);
}

u32 aica_read(u32 addr, int size)
{
    u32 off, v;
    if (addr >= RTC_BASE)
    {
        switch (addr & 0xC)
        {
        case 0x0: return rtc_now() >> 16;
        case 0x4: return rtc_now() & 0xFFFF;
        default: return 0;
        }
    }
    off = addr - AICA_REG_BASE;
    if (off >= AICA_REG_SIZE) return 0;
    if ((off & ~3u) == R_L) v = s_armLevel;
    else if ((off & ~3u) == R_STATUS || (off & ~3u) == R_CA)
    {
        AicaChan *c = &s_ch[(REG(R_MSLC) >> 8) & 63];
        if ((off & ~3u) == R_CA)
        {
            u32 ca = c->pos;
            if (c->lea > c->lsa && c->lea > 3 && ca > c->lea - 3) ca = c->lea - 3;
            v = ca & 0xFFFF;
        }
        else
        {
            u32 eg = c->on ? (c->eg > 3 ? 3 : (u32)c->eg) : 3, level = c->on ? (c->level >> 16) << 3 : 0x1FFF;
            v = ((u32)c->loopEnd << 15) | (eg << 13) | (level & 0x1FFF);
            c->loopEnd = 0;
        }
    }
    else v = REG(off);
    if (size == 1) return (off & 1) ? (v >> 8) & 0xFF : v & 0xFF;
    return v;
}

void aica_write(u32 addr, u32 value, int size)
{
    u32 off, v;
    if (addr >= RTC_BASE)
    {
        u32 now = rtc_now();
        switch (addr & 0xC)
        {
        case 0x0:
            if (s_rtcWriteEnable) aica_rtc_set((now & 0xFFFF) | ((value & 0xFFFF) << 16));
            return;
        case 0x4:
            if (s_rtcWriteEnable) aica_rtc_set((now & 0xFFFF0000u) | (value & 0xFFFF));
            s_rtcWriteEnable = 0;
            return;
        case 0x8:
            s_rtcWriteEnable = value & 1;
            return;
        }
        return;
    }
    off = addr - AICA_REG_BASE;
    if (off >= AICA_REG_SIZE) return;
    if (size == 1) v = (off & 1) ? (u32)((REG(off) & 0x00FF) | ((value & 0xFF) << 8)) : (u32)((REG(off) & 0xFF00) | (value & 0xFF));
    else v = value & 0xFFFF;
    if (off < 0x2000 && (off & 0x7C) == 0 && !(off & 2))
    {
        REG(off) = (u16)(v & 0x7FFF);
        if (s_ch[off >> 7].on) update_addresses((int)(off >> 7));
        if (v & 0x8000) key_execute();
        return;
    }
    switch (off & ~3u)
    {
    case R_SCIEB:
        REG(off) = (u16)(v & 0x7FF);
        update_arm_irq();
        return;
    case R_SCIPD:
        if (v & INT_SCPU) { REG(R_SCIPD) |= INT_SCPU; update_arm_irq(); }
        return;
    case R_SCIRE:
        REG(R_SCIPD) &= (u16)~v;
        update_arm_irq();
        return;
    case R_MCIEB:
        REG(off) = (u16)(v & 0x7FF);
        update_sh4_irq();
        return;
    case R_MCIPD:
        if (v & INT_SCPU) { REG(R_MCIPD) |= INT_SCPU; update_sh4_irq(); }
        return;
    case R_MCIRE:
        REG(R_MCIPD) &= (u16)~v;
        update_sh4_irq();
        return;
    case R_SCILV0:
    case R_SCILV1:
    case R_SCILV2:
        REG(off) = (u16)v;
        update_arm_irq();
        return;
    case R_M:
        update_arm_irq();
        return;
    case R_ARMRST:
        REG(off) = (u16)v;
        arm7_set_enabled(!(v & 1));
        return;
    case R_DMA_CTRL:
        REG(off) = (u16)v;
        if (v & 1) internal_dma();
        return;
    }
    REG(off) = (u16)v;
    if (off < 0x2000 && ((off & 0x7C) == 0x04 || (off & 0x7C) == 0x08 || (off & 0x7C) == 0x0C) && s_ch[off >> 7].on)
        update_addresses((int)(off >> 7));
}

void aica_state(StateIO *io)
{
    state_tag(io, "AICA");
    STATE(io, s_regs);
    STATE(io, s_rtcBase); STATE(io, s_rtcTime); STATE(io, s_rtcWriteEnable);
    STATE(io, s_timerPrescale); STATE(io, s_armLevel);
    STATE(io, s_ch);
    if (io->loading)
    {
        s_outRead = s_outWrite;
        update_arm_irq();
        update_sh4_irq();
    }
}
