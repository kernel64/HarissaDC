// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "arm7_jit_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arm7.h"
#include "../core/log.h"
#include "../mem/mem.h"
#include "../sh4/sh4_jit.h"

#define MAX_BLOCKS (1u << 14)
#define ARAM_WORDS (ARAM_SIZE >> 2)
#define CODE_PAGES (ARAM_SIZE >> 10)
#define BLOCK_ROOM (64u * 96u + 256u)

struct ArmBlock
{
    u32 pc;
    u32 *code;
};

static u32 *s_mem;
static u32 s_memWords, s_pos, s_firstBlockWord;
static void (*s_flush)(void *addr, u32 len);
static ArmBlock *s_blocks;
static u32 s_blockCount;
static u32 *s_blockAt;
static u8 s_codePages[CODE_PAGES];
static u8 s_codeWords[ARAM_WORDS / 8];
static volatile u8 s_dirtyPages[CODE_PAGES];
static volatile int s_dirty;
static int s_enabled, s_ready;
static u32 *s_exit;
static Arm7JitStats s_stats;
u8 *g_arm7_jit_code;

typedef void (*EnterFn)(const u32 *code);
static EnterFn s_enter;
#if defined(SH4_JIT_ABI_ELFV1)
struct FnDesc { u64 entry, toc, env; };
static FnDesc s_enterDesc;
#endif

int arm7_jit_init(void *execMem, u32 size, void (*flushIcache)(void *addr, u32 len))
{
#if SH4_JIT_CAN_RUN
    PpcCode c;
    u32 *enter;
    if (!execMem || size < (256u << 10)) return -1;
    if (!s_blocks) s_blocks = (ArmBlock *)calloc(MAX_BLOCKS, sizeof(ArmBlock));
    if (!s_blockAt) s_blockAt = (u32 *)calloc(ARAM_WORDS, sizeof(u32));
    if (!s_blocks || !s_blockAt) return -1;
    s_mem = (u32 *)execMem;
    s_memWords = size / 4;
    s_flush = flushIcache;
    c.buf = s_mem;
    c.pos = 0;
    c.cap = s_memWords;
    c.overflow = 0;
    if (arm7_jit_gen_trampoline(&c, &enter, &s_exit) != 0) return -1;
    s_firstBlockWord = (c.pos + 15) & ~15u;
    if (s_flush) s_flush(s_mem, c.pos * 4);
#if defined(SH4_JIT_ABI_ELFV1)
    s_enterDesc.entry = (u64)(uintptr_t)enter;
    s_enterDesc.toc = ((const u64 *)(const void *)&arm7_jit_init)[1];
    s_enterDesc.env = 0;
    s_enter = (EnterFn)(void *)&s_enterDesc;
#else
    s_enter = (EnterFn)(void *)enter;
#endif
    s_ready = 1;
    arm7_jit_flush();
    memset(&s_stats, 0, sizeof(s_stats));
    LOG_INFO("[arm7 jit] %u KB of code memory at %p", size >> 10, execMem);
    return 0;
#else
    (void)execMem;
    (void)size;
    (void)flushIcache;
    return -1;
#endif
}

void arm7_jit_enable(int on)
{
    s_enabled = on && s_ready;
    g_arm7_jit_code = s_enabled ? s_codePages : NULL;
    arm7_jit_flush();
}

int arm7_jit_active(void)
{
    return s_enabled;
}

void arm7_jit_flush(void)
{
    if (!s_ready) return;
    memset(s_blockAt, 0, ARAM_WORDS * sizeof(u32));
    memset(s_codePages, 0, sizeof(s_codePages));
    memset(s_codeWords, 0, sizeof(s_codeWords));
    memset((void *)s_dirtyPages, 0, sizeof(s_dirtyPages));
    s_dirty = 0;
    s_blockCount = 0;
    s_pos = s_firstBlockWord;
    s_stats.flushes++;
}

static int is_code(u32 off)
{
    u32 w = (off & (ARAM_SIZE - 1)) >> 2;
    return (s_codeWords[w >> 3] >> (w & 7)) & 1;
}

static void drop_page(u32 page)
{
    u32 first = page ? page - 1 : 0, w;
    for (w = first * 256; w < (page + 1) * 256 && w < ARAM_WORDS; w++) s_blockAt[w] = 0;
    memset(&s_codeWords[first * 32], 0, (page + 1 - first) * 32);
    s_codePages[page] = 0;
    if (page) s_codePages[page - 1] = 0;
}

void arm7_jit_code_written(u32 off)
{
    if (is_code(off)) drop_page(off >> 10);
}

void arm7_jit_sh4_wrote(u32 off)
{
    if (!is_code(off)) return;
    s_dirtyPages[off >> 10] = 1;
    s_dirty = 1;
}

static void drop_dirty(void)
{
    u32 p;
    s_dirty = 0;
    for (p = 0; p < CODE_PAGES; p++)
        if (s_dirtyPages[p])
        {
            s_dirtyPages[p] = 0;
            drop_page(p);
        }
}

static const void *compile(u32 pc)
{
    PpcCode c;
    Arm7JitGenInfo info;
    u32 end, a;
    if (s_memWords - s_pos < BLOCK_ROOM || s_blockCount >= MAX_BLOCKS) arm7_jit_flush();
    c.buf = s_mem + s_pos;
    c.pos = 0;
    c.cap = s_memWords - s_pos;
    c.overflow = 0;
    info.exit = s_exit;
    info.codePages = s_codePages;
    info.fallbacks = 0;
    end = arm7_jit_gen_block(&c, pc, &info);
    if (!end || c.overflow) return NULL;
    if (s_flush) s_flush(c.buf, c.pos * 4);
    s_blocks[s_blockCount].pc = pc;
    s_blocks[s_blockCount].code = c.buf;
    s_blockCount++;
    s_blockAt[(pc & (ARAM_SIZE - 1)) >> 2] = s_blockCount;
    for (a = pc & ~1023u; a < end; a += 1024) s_codePages[(a & (ARAM_SIZE - 1)) >> 10] = 1;
    for (a = pc; a < end; a += 4)
    {
        u32 w = (a & (ARAM_SIZE - 1)) >> 2;
        s_codeWords[w >> 3] |= (u8)(1u << (w & 7));
    }
    s_pos += (c.pos + 3) & ~3u;
    s_stats.blocksCompiled++;
    s_stats.fallbacks += info.fallbacks;
    s_stats.codeBytes = s_pos * 4;
    return c.buf;
}

const void *arm7_jit_block(u32 pc)
{
    u32 idx;
    if (s_dirty) drop_dirty();
    if (pc & 3) return NULL;
    idx = s_blockAt[(pc & (ARAM_SIZE - 1)) >> 2];
    s_stats.blocksRun++;
    if (idx && s_blocks[idx - 1].pc == pc) return s_blocks[idx - 1].code;
    return compile(pc);
}

void arm7_jit_enter(const void *code)
{
    s_enter((const u32 *)code);
}

void arm7_jit_stats(Arm7JitStats *out)
{
    *out = s_stats;
}
