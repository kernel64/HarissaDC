// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_MEM_H
#define EMU_MEM_H

#include "../core/types.h"

#define RAM_SIZE   (16u << 20)
#define VRAM_SIZE  (8u << 20)
#define ARAM_SIZE  (2u << 20)
#define BIOS_SIZE  (2u << 20)
#define FLASH_SIZE (128u << 10)

#if EMU_HOST_BIG_ENDIAN
#define MEM_SWZ8 3
#define MEM_SWZ16 2
#else
#define MEM_SWZ8 0
#define MEM_SWZ16 0
#endif

extern u8 *g_ram, *g_vram, *g_aram, *g_bios, *g_flash;
#define MEM_WINDOW_SIZE 0x20000000u
#define MEM_WINDOW_RAM 0x0C000000u
extern u8 *g_mem_window;

static __inline u8 buf_r8(const u8 *b, u32 off) { return b[off ^ MEM_SWZ8]; }
static __inline u16 buf_r16(const u8 *b, u32 off) { return *(const u16 *)(b + (off ^ MEM_SWZ16)); }
static __inline u32 buf_r32(const u8 *b, u32 off) { return *(const u32 *)(b + off); }
static __inline void buf_w8(u8 *b, u32 off, u8 v) { b[off ^ MEM_SWZ8] = v; }
static __inline void buf_w16(u8 *b, u32 off, u16 v) { *(u16 *)(b + (off ^ MEM_SWZ16)) = v; }
static __inline void buf_w32(u8 *b, u32 off, u32 v) { *(u32 *)(b + off) = v; }

void mem_swap_words(u8 *buf, u32 size);
void mem_import_bytes(u8 *dst, const u8 *src, u32 size);
void mem_export_bytes(u8 *dst, const u8 *src, u32 size);

typedef u32 (*MemReadFn)(u32 addr, int size);
typedef void (*MemWriteFn)(u32 addr, u32 value, int size);

extern u8 *g_mem_code_pages;
extern void (*g_mem_code_written)(u32 ramOffset);

#define VRAM_PAGES (VRAM_SIZE >> 12)
struct EMU_CACHE_ALIGNED SoundTouched
{
    volatile int value;
    char pad[EMU_CACHE_LINE - sizeof(int)];
};
extern SoundTouched g_soundTouched;
#define g_sound_touched (g_soundTouched.value)
extern u32 g_vram_gen;
extern u32 g_vram_page_gen[VRAM_PAGES];
static __inline void vram_mark(u32 off64) { g_vram_page_gen[(off64 & (VRAM_SIZE - 1)) >> 12] = g_vram_gen; }
static __inline u32 vram_cache_stamp(void) { return g_vram_gen++; }
int vram_written_since(u32 off64, u32 bytes, u32 stamp);

#define MEM_COUNT_BUCKETS 513
extern u32 *g_mem_reads, *g_mem_writes;

extern u32 g_mem_watch, g_mem_watch_lo, g_mem_watch_hi, g_mem_watch_bad;

int mem_init(void);
void mem_shutdown(void);
void mem_reset(void);

void mem_map_direct(u32 start, u32 end, u8 *base, u32 mask, int readOnly);
void mem_map_handler(u32 start, u32 end, MemReadFn read, MemWriteFn write);
void mem_map_p4(MemReadFn read, MemWriteFn write);

extern void (*g_mem_io_hook)(u32 addr, u32 value, int size, int write);

u8 mem_read8(u32 addr);
u16 mem_read16(u32 addr);
u32 mem_read32(u32 addr);
void mem_write8(u32 addr, u8 value);
void mem_write16(u32 addr, u16 value);
void mem_write32(u32 addr, u32 value);
u16 mem_fetch16(u32 addr);

u8 *mem_word_ptr(u32 addr, u32 *avail);
int mem_write_burst(u32 addr, const u32 *words, u32 n);

u32 reg_lane_read(u32 reg, u32 addr, int size);
u32 reg_lane_write(u32 reg, u32 addr, u32 value, int size);

struct StateIO;
void mem_state(StateIO *io);

#endif
