// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_sampler.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/log.h"
#include "../../core/prof.h"
#include "../../sh4/sh4.h"
#include "../../sh4/sh4_jit.h"
#include "../../sh4/sh4_mmu.h"
#include "../../sh4/sh4_mmr.h"
#include "../../sh4/sh4_internal.h"
#include "../../mem/mem.h"

#define SLOTS 4096

static u32 s_key[SLOTS], s_count[SLOTS];
static u32 s_section[PROF_COUNT + 1];
static u32 s_phase[SH4_PHASE_COUNT];
static volatile LONG s_on;

static DWORD WINAPI sampler_main(LPVOID)
{
    while (s_on)
    {
        int section = g_prof_section;
        Sleep(1);
        if (section < 0 || section >= PROF_COUNT)
        {
            s_section[PROF_COUNT]++;
            continue;
        }
        s_section[section]++;
        if (section == PROF_SH4)
        {
            int phase = g_sh4_jit_phase;
            u32 pc = g_sh4.pc, h = (pc >> 1) & (SLOTS - 1), n;
            for (n = 0; n < 16; n++, h = (h + 1) & (SLOTS - 1))
                if (s_count[h] == 0 || s_key[h] == pc)
                {
                    s_key[h] = pc;
                    s_count[h]++;
                    break;
                }
            if (phase >= 0 && phase < SH4_PHASE_COUNT) s_phase[phase]++;
        }
    }
    return 0;
}

static void tea_loop_hook(u32 expevt)
{
    static u32 lastTea, count, logged;
    if ((expevt == 0x040 || expevt == 0x060) && g_ccn.tea == lastTea)
    {
        if (++count == 200 && logged < 4)
        {
            char line[400];
            int ok = 1, k, n;
            u32 pc = g_sh4_inst_pc, phys = sh4_mmu_peek(pc & ~1u, &ok);
            logged++;
            n = _snprintf(line, sizeof(line), "[sampler] TLB miss loop: expevt %03X tea %08X pc %08X (phys %08X) sr %08X r15 %08X code:", expevt,
                          g_ccn.tea, pc, phys, g_sh4.sr, g_sh4.r[15]);
            for (k = -8; k < 8 && ok && n > 0 && n < (int)sizeof(line); k++) n += _snprintf(line + n, sizeof(line) - n, " %04X", mem_read16(phys + k * 2));
            line[sizeof(line) - 1] = 0;
            LOG_INFO("%s", line);
        }
    }
    else
        count = 0;
    lastTea = g_ccn.tea;
}

void xb_sampler_start(void)
{
    HANDLE h;
    if (s_on) return;
    g_sh4_jit_mark_blocks = 1;
    if (!g_sh4_jit_mmu_op_counts) g_sh4_jit_mmu_op_counts = (u32 *)calloc(65536, sizeof(u32));
    if (!g_sh4_exception_hook) g_sh4_exception_hook = tea_loop_hook;
    s_on = 1;
    h = CreateThread(NULL, 16 * 1024, sampler_main, NULL, CREATE_SUSPENDED, NULL);
    if (!h)
    {
        s_on = 0;
        return;
    }
    XSetThreadProcessor(h, 5);
    SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
    ResumeThread(h);
    CloseHandle(h);
    LOG_INFO("[sampler] on: hot SH-4 blocks every 5 s");
}

#define TOP 40

void xb_sampler_report(void)
{
    u32 total = 0, sh4 = 0, i, k;
    u32 best[TOP], bestPc[TOP];
    char line[512];
    int n;
    for (i = 0; i <= PROF_COUNT; i++) total += s_section[i];
    if (!total) return;
    sh4 = s_section[PROF_SH4];
    n = _snprintf(line, sizeof(line), "[sampler] %u samples:", total);
    for (i = 0; i <= PROF_COUNT && n > 0 && n < (int)sizeof(line); i++)
        n += _snprintf(line + n, sizeof(line) - n, " %s %u%%", i < PROF_COUNT ? prof_name((int)i) : "other", s_section[i] * 100 / total);
    line[sizeof(line) - 1] = 0;
    LOG_INFO("%s", line);
    memset(best, 0, sizeof(best));
    memset(bestPc, 0, sizeof(bestPc));
    for (i = 0; i < SLOTS; i++)
        for (k = 0; k < TOP; k++)
            if (s_count[i] > best[k])
            {
                u32 m;
                for (m = TOP - 1; m > k; m--)
                {
                    best[m] = best[m - 1];
                    bestPc[m] = bestPc[m - 1];
                }
                best[k] = s_count[i];
                bestPc[k] = s_key[i];
                break;
            }
    {
        static const char *const names[SH4_PHASE_COUNT] = { "code", "dispatcher", "compiler", "mmu-slow", "mmu-op", "handler" };
        n = _snprintf(line, sizeof(line), "[sampler] SH-4 thread (%% of SH-4 samples):");
        for (k = 0; k < SH4_PHASE_COUNT && n > 0 && n < (int)sizeof(line); k++)
            n += _snprintf(line + n, sizeof(line) - n, " %s %u", names[k], s_phase[k] * 100 / (sh4 ? sh4 : 1));
        line[sizeof(line) - 1] = 0;
        LOG_INFO("%s", line);
        memset(s_phase, 0, sizeof(s_phase));
    }
    if (g_sh4_jit_mmu_op_counts)
    {
        u32 top[8], topOp[8], o, j;
        memset(top, 0, sizeof(top));
        memset(topOp, 0, sizeof(topOp));
        for (o = 0; o < 65536; o++)
        {
            u32 v = g_sh4_jit_mmu_op_counts[o];
            for (k = 0; k < 8; k++)
                if (v > top[k])
                {
                    for (j = 7; j > k; j--)
                    {
                        top[j] = top[j - 1];
                        topOp[j] = topOp[j - 1];
                    }
                    top[k] = v;
                    topOp[k] = o;
                    break;
                }
        }
        n = _snprintf(line, sizeof(line), "[sampler] MMU ops interpreted (5 s):");
        for (k = 0; k < 8 && top[k] && n > 0 && n < (int)sizeof(line); k++)
            n += _snprintf(line + n, sizeof(line) - n, " %04X %s %u,", topOp[k], sh4_op_name((u16)topOp[k]), top[k]);
        line[sizeof(line) - 1] = 0;
        LOG_INFO("%s", line);
        memset(g_sh4_jit_mmu_op_counts, 0, 65536 * sizeof(u32));
    }
    for (i = 0; i < TOP && best[i]; i += 10)
    {
        n = _snprintf(line, sizeof(line), "[sampler] SH-4 blocks (%% of SH-4 samples)%s", i ? " ..." : ":");
        for (k = i; k < i + 10 && k < TOP && best[k] && n > 0 && n < (int)sizeof(line); k++)
            n += _snprintf(line + n, sizeof(line) - n, " %08X %u.%u", bestPc[k], best[k] * 100 / (sh4 ? sh4 : 1),
                           best[k] * 1000 / (sh4 ? sh4 : 1) % 10);
        line[sizeof(line) - 1] = 0;
        LOG_INFO("%s", line);
    }
    for (k = 0; k < 2 && best[k]; k++)
    {
        int ok = 1, j;
        u32 phys = sh4_mmu_peek(bestPc[k], &ok);
        if (!ok) continue;
        n = _snprintf(line, sizeof(line), "[sampler] code %08X (%08X):", bestPc[k], phys);
        for (j = 0; j < 40 && n > 0 && n < (int)sizeof(line); j++)
            n += _snprintf(line + n, sizeof(line) - n, " %04X", mem_read16(phys + j * 2));
        line[sizeof(line) - 1] = 0;
        LOG_INFO("%s", line);
    }
    memset(s_count, 0, sizeof(s_count));
    memset(s_key, 0, sizeof(s_key));
    memset(s_section, 0, sizeof(s_section));
}
