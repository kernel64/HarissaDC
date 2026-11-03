// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "dreamcast.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aica.h"
#include "sound_thread.h"
#include "arm7.h"
#include "flash.h"
#include "gdrom.h"
#include "cdda.h"
#include "holly.h"
#include "maple.h"
#include "pvr.h"
#include "../bios/hle_bios.h"
#include "../core/log.h"
#include "../core/prof.h"
#include "../core/sched.h"
#include "../core/state.h"
#include "../disc/disc.h"
#include "../mem/mem.h"
#include "../sh4/sh4.h"
#include "../sh4/sh4_jit.h"
#include "../sh4/sh4_mmr.h"
#include "../sh4/sh4_mmu.h"

#define SLICE_MAX 16384
static s32 s_sliceMax = SLICE_MAX;
static s32 s_slice;
static int s_cpuPercent = 100;

void dc_set_slice(int cycles)
{
    s_sliceMax = cycles > 0 ? cycles : SLICE_MAX;
}

void dc_set_cpu_percent(int percent)
{
    s_cpuPercent = percent >= 25 && percent <= 100 ? percent : 100;
}

int dc_cpu_percent(void)
{
    return s_cpuPercent;
}

static u64 cpu_elapsed(void)
{
    s32 left = sh4_jit_active() ? sh4_jit_cycles_left() : g_sh4.cycles;
    u64 ran = left < s_slice ? (u64)(s_slice - left) : 0;
    return s_cpuPercent == 100 ? ran : ran * 100 / (u32)s_cpuPercent;
}

static u32 area0_read(u32 addr, int size)
{
    u32 a = addr & 0x01FFFFFFu;
    if (a >= 0x00200000u && a < 0x00400000u) return flash_read(a, size);
    if (a >= 0x005F7000u && a < 0x005F7100u) return gdrom_read(a, size);
    if (a >= 0x005F6800u && a < 0x005F8000u) return holly_read(a, size);
    if (a >= 0x005F8000u && a < 0x005FA000u) return pvr_read(a, size);
    if (a >= 0x00600000u && a < 0x00700000u) return 0;
    if (a >= 0x00700000u && a < 0x00800000u) return sound_sh4_read(a, size);
    return 0;
}

static void area0_write(u32 addr, u32 value, int size)
{
    u32 a = addr & 0x01FFFFFFu;
    if (a >= 0x00200000u && a < 0x00400000u) { flash_write(a, value, size); return; }
    if (a >= 0x005F7000u && a < 0x005F7100u) { gdrom_write(a, value, size); return; }
    if (a >= 0x005F6800u && a < 0x005F8000u) { holly_write(a, value, size); return; }
    if (a >= 0x005F8000u && a < 0x005FA000u) { pvr_write(a, value, size); return; }
    if (a >= 0x00700000u && a < 0x00800000u) { sound_sh4_write(a, value, size); return; }
}

static u32 vram32_read(u32 addr, int size)
{
    u32 off = vram_offset64(addr & 0x007FFFFFu);
    if (size == 4) return buf_r32(g_vram, off & ~3u);
    if (size == 2) return buf_r16(g_vram, off & ~1u);
    return buf_r8(g_vram, off);
}

static void vram32_write(u32 addr, u32 value, int size)
{
    u32 off = vram_offset64(addr & 0x007FFFFFu);
    vram_mark(off);
    if (size == 4) buf_w32(g_vram, off & ~3u, value);
    else if (size == 2) buf_w16(g_vram, off & ~1u, (u16)value);
    else buf_w8(g_vram, off, (u8)value);
}

static u32 area7_read(u32 addr, int size)
{
    if ((addr & 0xFC000000u) == 0x7C000000u) return sh4_ocram_read(addr, size);
    return sh4_p4_read(addr | 0xE0000000u, size);
}

static void area7_write(u32 addr, u32 value, int size)
{
    if ((addr & 0xFC000000u) == 0x7C000000u) sh4_ocram_write(addr, value, size);
    else sh4_p4_write(addr | 0xE0000000u, value, size);
}

static void build_map(void)
{
    u32 mirror;
    for (mirror = 0; mirror < 0x04000000u; mirror += 0x02000000u)
    {
        mem_map_direct(mirror, mirror + 0x00200000u, g_bios, BIOS_SIZE - 1, 1);
        mem_map_handler(mirror + 0x00200000u, mirror + 0x00800000u, area0_read, area0_write);
        mem_map_direct(mirror + 0x00800000u, mirror + 0x01000000u, g_aram, ARAM_SIZE - 1, 0);
    }
    mem_map_direct(0x04000000u, 0x05000000u, g_vram, VRAM_SIZE - 1, 0);
    mem_map_handler(0x05000000u, 0x06000000u, vram32_read, vram32_write);
    mem_map_direct(0x06000000u, 0x07000000u, g_vram, VRAM_SIZE - 1, 0);
    mem_map_handler(0x07000000u, 0x08000000u, vram32_read, vram32_write);
    mem_map_direct(0x0C000000u, 0x10000000u, g_ram, RAM_SIZE - 1, 0);
    mem_map_handler(0x10000000u, 0x14000000u, pvr_area4_read, pvr_area4_write);
    mem_map_handler(0x1C000000u, 0x20000000u, area7_read, area7_write);
    mem_map_p4(sh4_p4_read, sh4_p4_write);
}

int dc_init(void)
{
    if (mem_init() != 0) return -1;
    build_map();
    sched_set_cpu(cpu_elapsed, sh4_break_slice);
    sh4_init();
    sh4_mmr_init();
    holly_init();
    pvr_init();
    maple_init();
    gdrom_init();
    aica_init();
    return 0;
}

void dc_shutdown(void)
{
    gdrom_set_disc(NULL);
    mem_shutdown();
}

static int load_file(const char *path, u8 *dst, u32 size, char *error, int errorSize)
{
    FILE *f = fopen(path, "rb");
    u8 *tmp;
    size_t got;
    if (!f)
    {
        _snprintf(error, errorSize, "cannot open %s", path);
        error[errorSize - 1] = 0;
        return -1;
    }
    tmp = (u8 *)malloc(size + 1);
    got = tmp ? fread(tmp, 1, size + 1, f) : 0;
    fclose(f);
    if (got != size)
    {
        _snprintf(error, errorSize, "%s: %u bytes, expected %u", path, (u32)got, size);
        error[errorSize - 1] = 0;
        free(tmp);
        return -1;
    }
    mem_import_bytes(dst, tmp, size);
    free(tmp);
    return 0;
}

int dc_load_bios(const char *bootPath, const char *flashPath, char *error, int errorSize)
{
    g_sh4_hle_trap = NULL;
    if (load_file(bootPath, g_bios, BIOS_SIZE, error, errorSize)) return -1;
    if (load_file(flashPath, g_flash, FLASH_SIZE, error, errorSize)) return -1;
    LOG_INFO("[dc] BIOS %s and flash %s loaded", bootPath, flashPath);
    return 0;
}

void dc_set_bios(const u8 *boot, u32 bootSize, const u8 *flash, u32 flashSize)
{
    memset(g_bios, 0, BIOS_SIZE);
    memset(g_flash, 0xFF, FLASH_SIZE);
    if (boot) mem_import_bytes(g_bios, boot, bootSize < BIOS_SIZE ? bootSize & ~3u : BIOS_SIZE);
    if (flash) mem_import_bytes(g_flash, flash, flashSize < FLASH_SIZE ? flashSize & ~3u : FLASH_SIZE);
}

void dc_use_hle_bios(int region, int language)
{
    static const char *const names[3] = { "Japan", "USA", "Europe" };
    hle_bios_install(region, language);
    LOG_INFO("[dc] HLE BIOS (no Sega BIOS files), region %s, language %d", names[region >= 0 && region <= 2 ? region : 1], language);
}

int dc_insert_disc(const char *path)
{
    Disc *d = NULL;
    if (path && path[0])
    {
        d = disc_open(path);
        if (!d) return -1;
    }
    gdrom_set_disc(d);
    return 0;
}

void dc_reset(void)
{
    sound_thread_hold();
    sched_reset();
    mem_reset();
    sh4_reset();
    sh4_mmr_reset();
    holly_reset();
    pvr_reset();
    maple_reset();
    gdrom_reset();
    cdda_init();
    holly_late_init();
    cdda_reset();
    aica_reset();
    flash_reset();
    hle_bios_reset();
    sound_thread_restart();
}

static u64 s_armFraction;

static void run_arm(s32 sh4Cycles)
{
    u64 scaled = (u64)sh4Cycles * ARM7_CLOCK_HZ + s_armFraction;
    s_armFraction = scaled % SH4_CLOCK_HZ;
    prof_begin(PROF_ARM7);
    arm7_run((s32)(scaled / SH4_CLOCK_HZ));
    prof_end(PROF_ARM7);
}

int dc_run_frame(void)
{
    u32 frame = pvr_frame_count();
    u64 start = sched_now();
    int result = 0;
    prof_begin(PROF_SH4);
    while (pvr_frame_count() == frame)
    {
        u64 now = sched_now(), next = sched_next();
        s32 slice = s_sliceMax, executed, budget;
        if (next != SCHED_NEVER && next > now && next - now < (u64)slice) slice = (s32)(next - now);
        if (next != SCHED_NEVER && next <= now) slice = 1;
        g_sh4_idle = 0;
        budget = s_cpuPercent == 100 ? slice : (s32)((s64)slice * s_cpuPercent / 100);
        if (budget < 1) budget = 1;
        s_slice = budget;
        sched_slice_end(now + (u64)slice);
        if (sh4_jit_active()) sh4_jit_run(budget);
        else sh4_run(budget);
        sched_slice_end(SCHED_NEVER);
        executed = budget - g_sh4.cycles;
        if (executed < 1) executed = 1;
        if (s_cpuPercent != 100) executed = executed >= budget ? slice : (s32)((s64)executed * 100 / s_cpuPercent);
        if (executed < 1) executed = 1;
        next = sched_next();
        if (g_sh4_idle && next != SCHED_NEVER && next > now + (u64)executed)
            executed = (s32)(next - now > 0x10000000u ? 0x10000000u : next - now);
        if (!sound_thread_on()) run_arm(executed);
        sched_advance((u32)executed);
        sched_run_due();
        sound_thread_advance(sched_now());
        if (sched_now() - start > 20u * SH4_CLOCK_HZ / 10u)
        {
            LOG_ERROR("[dc] no vblank in 2 s of emulated time (pc %08X)", g_sh4.pc);
            result = -1;
            break;
        }
    }
    prof_end(PROF_SH4);
    return result;
}

#define STATE_MAGIC 0x54534344u
#define STATE_VERSION 1u

struct StateHeader
{
    u32 magic, version, layout, total;
};

static u32 state_layout(void)
{
    u32 sh4Size = (u32)((offsetof(Sh4Ctx, jitWhy) + 7) & ~(size_t)7);
    return (u32)(sh4Size * 7 + sizeof(Arm7Ctx) * 5 + RAM_SIZE / 4096 + VRAM_SIZE / 4096);
}

static void machine_state(StateIO *io)
{
    StateHeader h;
    h.magic = STATE_MAGIC;
    h.version = STATE_VERSION;
    h.layout = state_layout();
    h.total = 0;
    STATE(io, h);
    sched_state(io);
    mem_state(io);
    sh4_state(io);
    sh4_mmr_state(io);
    holly_state(io);
    pvr_state(io);
    gdrom_state(io);
    aica_state(io);
    arm7_state(io);
    flash_state(io);
    hle_bios_state(io);
    STATE(io, s_armFraction);
    state_tag(io, "END ");
    if (!io->loading || io->pos < h.total) sh4_mmu_state(io);
    else sh4_mmu_reset();
    if (!io->loading || io->pos < h.total) cdda_state(io);
    else cdda_reset();
}

u32 dc_state_size(void)
{
    StateIO io;
    memset(&io, 0, sizeof(io));
    machine_state(&io);
    return io.pos;
}

int dc_state_save(u8 *buf, u32 size)
{
    StateIO io;
    sound_thread_hold();
    memset(&io, 0, sizeof(io));
    io.buf = buf;
    io.size = size;
    machine_state(&io);
    if (io.error) return -1;
    ((StateHeader *)buf)->total = io.pos;
    return 0;
}

u32 dc_state_bytes(const u8 *buf, u32 size)
{
    StateHeader h;
    if (size < sizeof(h)) return 0;
    memcpy(&h, buf, sizeof(h));
    if (h.magic != STATE_MAGIC || h.version != STATE_VERSION || h.layout != state_layout()) return 0;
    return h.total;
}

int dc_state_load(const u8 *buf, u32 size)
{
    StateIO io;
    if (dc_state_bytes(buf, size) != size || size < sizeof(StateHeader)) return -1;
    sound_thread_hold();
    memset(&io, 0, sizeof(io));
    io.buf = (u8 *)buf;
    io.size = size;
    io.loading = 1;
    machine_state(&io);
    if (io.error || io.pos != size)
    {
        LOG_ERROR("[dc] damaged save state (at byte %u of %u): reset", io.pos, size);
        dc_reset();
        return -1;
    }
    sh4_jit_flush();
    sound_thread_set_time(sched_now());
    sh4_irq_update();
    return 0;
}
