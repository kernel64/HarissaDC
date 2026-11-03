// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "mem.h"

#include <stdlib.h>
#include <string.h>
#if defined(_XBOX)
#include <xtl.h>
#elif defined(__linux__)
#include <sys/mman.h>
#endif

#include "../core/log.h"
#include "../core/state.h"
#include "../hw/arm7_jit.h"

u8 *g_ram, *g_vram, *g_aram, *g_bios, *g_flash;
u8 *g_mem_window;
u8 *g_mem_code_pages;
void (*g_mem_code_written)(u32 ramOffset);

SoundTouched g_soundTouched;
u32 g_vram_gen = 1;
u32 g_vram_page_gen[VRAM_PAGES];

u32 *g_mem_reads, *g_mem_writes;
u32 g_mem_watch, g_mem_watch_lo, g_mem_watch_hi, g_mem_watch_bad;
#define WATCH(addr) \
    if (g_mem_watch) \
    { \
        u32 a_ = (addr) & 0x1FFFFFFFu; \
        if (a_ < g_mem_watch_lo || a_ >= g_mem_watch_hi || (addr) >= 0xE0000000u) g_mem_watch_bad++; \
    }
#ifdef _XBOX
#define COUNT(table, addr)
#else
#define COUNT(table, addr) \
    if (table) table[(addr) >= 0xE0000000u ? 512 : ((addr) & 0x1FFFFFFFu) >> 20]++
#endif

static __inline void aram_written(u32 off)
{
    if (!g_sound_touched) g_sound_touched = 1;
    if (g_arm7_jit_code && g_arm7_jit_code[off >> 10]) arm7_jit_sh4_wrote(off);
}

#define CODE_CHECK(s, addr) \
    if (g_mem_code_pages && (s)->base == g_ram &&         (g_mem_code_pages[((addr) & (RAM_SIZE - 1)) >> 12] & (1u << (((addr) >> 9) & 7)))) \
        g_mem_code_written((addr) & (RAM_SIZE - 1)); \
    else if ((s)->base == g_vram) vram_mark((addr) & (s)->mask); \
    else if ((s)->base == g_aram) aram_written((addr) & (ARAM_SIZE - 1))

int vram_written_since(u32 off64, u32 bytes, u32 stamp)
{
    u32 first = (off64 & (VRAM_SIZE - 1)) >> 12, last, i;
    if (!bytes) return 0;
    last = ((off64 & (VRAM_SIZE - 1)) + bytes - 1) >> 12;
    if (last >= VRAM_PAGES) last = VRAM_PAGES - 1;
    for (i = first; i <= last; i++)
        if (g_vram_page_gen[i] > stamp) return 1;
    return 0;
}

struct MemSlot
{
    u8 *base;
    u32 mask;
    int readOnly;
    MemReadFn read;
    MemWriteFn write;
};

#define SLOT_COUNT 512
static MemSlot s_slots[SLOT_COUNT];
static MemReadFn s_p4Read;
static MemWriteFn s_p4Write;
static u32 s_unmappedLogs;

void mem_swap_words(u8 *buf, u32 size)
{
#if EMU_HOST_BIG_ENDIAN
    u32 i;
    for (i = 0; i + 4 <= size; i += 4)
    {
        u8 t0 = buf[i], t1 = buf[i + 1];
        buf[i] = buf[i + 3];
        buf[i + 1] = buf[i + 2];
        buf[i + 2] = t1;
        buf[i + 3] = t0;
    }
#else
    (void)buf;
    (void)size;
#endif
}

void mem_import_bytes(u8 *dst, const u8 *src, u32 size)
{
    memcpy(dst, src, size);
    mem_swap_words(dst, size);
}

void mem_export_bytes(u8 *dst, const u8 *src, u32 size)
{
    memcpy(dst, src, size);
    mem_swap_words(dst, size);
}

static u32 unmapped_read(u32 addr, int size)
{
    if (s_unmappedLogs < 32)
    {
        s_unmappedLogs++;
        LOG_WARN("[mem] read%d from unmapped %08X", size * 8, addr);
    }
    return 0;
}

static void unmapped_write(u32 addr, u32 value, int size)
{
    if (s_unmappedLogs < 32)
    {
        s_unmappedLogs++;
        LOG_WARN("[mem] write%d %08X to unmapped %08X", size * 8, value, addr);
    }
}

static u8 *window_ram(void)
{
    u8 *w;
#if defined(_XBOX)
    w = (u8 *)VirtualAlloc(NULL, MEM_WINDOW_SIZE, MEM_RESERVE, PAGE_READWRITE);
    if (!w) return NULL;
    if (!VirtualAlloc(w + MEM_WINDOW_RAM, RAM_SIZE, MEM_COMMIT, PAGE_READWRITE))
    {
        VirtualFree(w, 0, MEM_RELEASE);
        return NULL;
    }
#elif defined(__linux__)
    w = (u8 *)mmap(NULL, MEM_WINDOW_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (w == (u8 *)MAP_FAILED) return NULL;
    if (mprotect(w + MEM_WINDOW_RAM, RAM_SIZE, PROT_READ | PROT_WRITE) != 0)
    {
        munmap(w, MEM_WINDOW_SIZE);
        return NULL;
    }
#else
    w = NULL;
    return NULL;
#endif
    g_mem_window = w;
    memset(w + MEM_WINDOW_RAM, 0, RAM_SIZE);
    return w + MEM_WINDOW_RAM;
}

static void window_release(void)
{
#if defined(_XBOX)
    VirtualFree(g_mem_window, 0, MEM_RELEASE);
#elif defined(__linux__)
    munmap(g_mem_window, MEM_WINDOW_SIZE);
#endif
    g_mem_window = NULL;
}

static void *alloc_block(u32 size)
{
    void *p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

int mem_init(void)
{
    int i;
    if (!g_ram)
    {
        g_ram = window_ram();
        if (!g_ram) g_ram = (u8 *)alloc_block(RAM_SIZE);
        LOG_INFO("[mem] RAM at %p%s", (void *)g_ram, g_mem_window ? " (fastmem window)" : "");
        g_vram = (u8 *)alloc_block(VRAM_SIZE);
        g_aram = (u8 *)alloc_block(ARAM_SIZE);
        g_bios = (u8 *)alloc_block(BIOS_SIZE);
        g_flash = (u8 *)alloc_block(FLASH_SIZE);
    }
    if (!g_ram || !g_vram || !g_aram || !g_bios || !g_flash)
    {
        LOG_ERROR("[mem] out of memory");
        return -1;
    }
    for (i = 0; i < SLOT_COUNT; i++)
    {
        s_slots[i].base = NULL;
        s_slots[i].mask = 0;
        s_slots[i].readOnly = 0;
        s_slots[i].read = unmapped_read;
        s_slots[i].write = unmapped_write;
    }
    s_p4Read = unmapped_read;
    s_p4Write = unmapped_write;
    s_unmappedLogs = 0;
    return 0;
}

void mem_shutdown(void)
{
    if (g_mem_window) window_release();
    else free(g_ram);
    free(g_vram);
    free(g_aram);
    free(g_bios);
    free(g_flash);
    g_ram = g_vram = g_aram = g_bios = g_flash = NULL;
}

void mem_reset(void)
{
    memset(g_ram, 0, RAM_SIZE);
    memset(g_vram, 0, VRAM_SIZE);
    memset(g_aram, 0, ARAM_SIZE);
}

void mem_map_direct(u32 start, u32 end, u8 *base, u32 mask, int readOnly)
{
    u32 s;
    for (s = start >> 20; s < (end >> 20) && s < SLOT_COUNT; s++)
    {
        s_slots[s].base = base;
        s_slots[s].mask = mask;
        s_slots[s].readOnly = readOnly;
        s_slots[s].read = unmapped_read;
        s_slots[s].write = unmapped_write;
    }
}

void mem_map_handler(u32 start, u32 end, MemReadFn read, MemWriteFn write)
{
    u32 s;
    for (s = start >> 20; s < (end >> 20) && s < SLOT_COUNT; s++)
    {
        s_slots[s].base = NULL;
        s_slots[s].read = read ? read : unmapped_read;
        s_slots[s].write = write ? write : unmapped_write;
    }
}

void mem_map_p4(MemReadFn read, MemWriteFn write)
{
    s_p4Read = read;
    s_p4Write = write;
}

static __inline MemSlot *slot_of(u32 addr)
{
    return &s_slots[(addr >> 20) & (SLOT_COUNT - 1)];
}

void (*g_mem_io_hook)(u32 addr, u32 value, int size, int write);
static __inline u32 io_read_seen(u32 addr, u32 value, int size)
{
    if (g_mem_io_hook) g_mem_io_hook(addr, value, size, 0);
    return value;
}
#define IO_R(expr, size) io_read_seen(addr, (expr), (size))
#define IO_W(size) do { if (g_mem_io_hook) g_mem_io_hook(addr, value, (size), 1); } while (0)

u8 mem_read8(u32 addr)
{
    MemSlot *s;
    COUNT(g_mem_reads, addr);
    if (addr >= 0xE0000000u) return (u8)IO_R(s_p4Read(addr, 1), 1);
    s = slot_of(addr);
    if (s->base) return buf_r8(s->base, addr & s->mask);
    return (u8)IO_R(s->read(addr, 1), 1);
}

u16 mem_read16(u32 addr)
{
    MemSlot *s;
    addr &= ~1u;
    COUNT(g_mem_reads, addr);
    if (addr >= 0xE0000000u) return (u16)IO_R(s_p4Read(addr, 2), 2);
    s = slot_of(addr);
    if (s->base) return buf_r16(s->base, addr & s->mask);
    return (u16)IO_R(s->read(addr, 2), 2);
}

u32 mem_read32(u32 addr)
{
    MemSlot *s;
    addr &= ~3u;
    COUNT(g_mem_reads, addr);
    if (addr >= 0xE0000000u) return IO_R(s_p4Read(addr, 4), 4);
    s = slot_of(addr);
    if (s->base) return buf_r32(s->base, addr & s->mask);
    return IO_R(s->read(addr, 4), 4);
}

void mem_write8(u32 addr, u8 value)
{
    MemSlot *s;
    COUNT(g_mem_writes, addr);
    WATCH(addr);
    if (addr >= 0xE0000000u) { IO_W(1); s_p4Write(addr, value, 1); return; }
    s = slot_of(addr);
    if (s->base) { if (!s->readOnly) buf_w8(s->base, addr & s->mask, value); CODE_CHECK(s, addr); return; }
    IO_W(1);
    s->write(addr, value, 1);
}

void mem_write16(u32 addr, u16 value)
{
    MemSlot *s;
    addr &= ~1u;
    COUNT(g_mem_writes, addr);
    WATCH(addr);
    if (addr >= 0xE0000000u) { IO_W(2); s_p4Write(addr, value, 2); return; }
    s = slot_of(addr);
    if (s->base) { if (!s->readOnly) buf_w16(s->base, addr & s->mask, value); CODE_CHECK(s, addr); return; }
    IO_W(2);
    s->write(addr, value, 2);
}

void mem_write32(u32 addr, u32 value)
{
    MemSlot *s;
    addr &= ~3u;
    COUNT(g_mem_writes, addr);
    WATCH(addr);
    if (addr >= 0xE0000000u) { IO_W(4); s_p4Write(addr, value, 4); return; }
    s = slot_of(addr);
    if (s->base) { if (!s->readOnly) buf_w32(s->base, addr & s->mask, value); CODE_CHECK(s, addr); return; }
    IO_W(4);
    s->write(addr, value, 4);
}

int mem_write_burst(u32 addr, const u32 *words, u32 n)
{
    MemSlot *s;
    u32 off, i;
    if (addr >= 0xE0000000u || (addr & 31) || n > 8) return 0;
    s = slot_of(addr);
    if (!s->base || s->readOnly) return 0;
    if (g_mem_code_pages && s->base == g_ram &&
        (g_mem_code_pages[(addr & (RAM_SIZE - 1)) >> 12] & (1u << ((addr >> 9) & 7))))
        return 0;
    off = addr & s->mask;
    for (i = 0; i < n; i++) buf_w32(s->base, off + i * 4, words[i]);
    if (s->base == g_vram) vram_mark(off);
    else if (s->base == g_aram) aram_written(off & (ARAM_SIZE - 1));
    return 1;
}

u16 mem_fetch16(u32 addr)
{
    return mem_read16(addr);
}

u8 *mem_word_ptr(u32 addr, u32 *avail)
{
    MemSlot *s;
    u32 off;
    if (addr >= 0xE0000000u) return NULL;
    s = slot_of(addr);
    if (!s->base) return NULL;
    off = addr & s->mask & ~3u;
    if (avail) *avail = s->mask + 1 - off;
    return s->base + off;
}

u32 reg_lane_read(u32 reg, u32 addr, int size)
{
    if (size == 4) return reg;
    if (size == 2) return (reg >> ((addr & 2) * 8)) & 0xFFFF;
    return (reg >> ((addr & 3) * 8)) & 0xFF;
}

u32 reg_lane_write(u32 reg, u32 addr, u32 value, int size)
{
    u32 shift, mask;
    if (size == 4) return value;
    shift = (size == 2 ? (addr & 2) : (addr & 3)) * 8;
    mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
    return (reg & ~mask) | ((value << shift) & mask);
}

void mem_state(StateIO *io)
{
    u32 i;
    state_tag(io, "MEM ");
    state_io(io, g_ram, RAM_SIZE);
    state_io(io, g_vram, VRAM_SIZE);
    state_io(io, g_aram, ARAM_SIZE);
    state_io(io, g_flash, FLASH_SIZE);
    if (io->loading)
    {
        g_vram_gen++;
        for (i = 0; i < VRAM_PAGES; i++) g_vram_page_gen[i] = g_vram_gen;
        g_vram_gen++;
    }
}
