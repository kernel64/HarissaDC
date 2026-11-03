// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "sh4_jit.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh4.h"
#include "sh4_jit_internal.h"
#include "sh4_mmr.h"
#include "sh4_mmu.h"
#if defined(_XBOX)
#include <xtl.h>
#elif defined(SH4_JIT_ABI_ELFV1)
#include <signal.h>
#include <ucontext.h>
#endif
#include "../core/log.h"
#include "../core/sched.h"
#include "../mem/mem.h"

#define MAX_BLOCKS (1u << 18)
#define MAX_LINKS (1u << 18)
#define RAM_PAGES (RAM_SIZE >> 12)
#define DEAD_TARGET 0xFFFFFFFFu

struct JitBlock
{
    u32 pc;
    u32 off;
    u32 *code;
    u32 words;
    int pure;
    int headPure;
    u32 fpMode;
    u32 nextVersion;
    u32 firstLink, endLink;
    u16 idleFails;
    u16 idleHit;
    u16 writeTurns;
    u16 promoted;
    u16 samples;
    u16 tier2;
    u16 countdown;
    s8 cdReg;
    s16 cdOff;
    u16 cdTurn;
};
#define CD_NO_COPY 0x7FFF

struct LinkSite
{
    u32 *site, *exit;
    u32 target, off, fpMode;
    u32 next;
    u32 deferred;
};

#define FP_MODE_MASK (FPSCR_PR | FPSCR_SZ | FPSCR_DN)
#define MMU_MODE 0x80000000u
#define MMU_MD 0x40000000u

static u32 *s_mem;
static u32 s_memWords, s_pos, s_firstBlockWord;
#define HOT_WORDS 3840
#define COLD_WORDS 4096
static u32 s_hotEnd, s_coldPos, s_pairEnd;
int g_sh4_jit_split = 1;

static u32 *s_nearRefill[2][2];
static void (*s_flush)(void *addr, u32 len);

static void new_pair(void)
{
    PpcCode rc;
    if (s_pairEnd > s_pos) s_pos = s_pairEnd;
    s_hotEnd = s_pos + HOT_WORDS;
    s_coldPos = s_hotEnd;
    s_pairEnd = s_hotEnd + COLD_WORDS;
    rc.buf = s_mem + s_coldPos;
    rc.pos = 0;
    rc.cap = COLD_WORDS;
    rc.overflow = 0;
    sh4_jit_gen_refill_copy(&rc, s_nearRefill);
    if (s_flush) s_flush(rc.buf, rc.pos * 4);
    s_coldPos += rc.pos;
}
static JitBlock *s_blocks;
static u32 s_blockCount;
static u32 *s_ramBlock;
static u8 s_pages[RAM_PAGES];
static u8 s_spills[RAM_PAGES];
static u8 s_codeBits[RAM_SIZE / 16];
#define CODE_BIT(off) (s_codeBits[(off) >> 4] & (1u << (((off) >> 1) & 7)))
static u8 s_busyBits[RAM_SIZE / 16];
static u8 s_loopBits[RAM_SIZE / 16];
#define LOOP_BIT(off) (s_loopBits[(off) >> 4] & (1u << (((off) >> 1) & 7)))
#define BUSY_BIT(off) (s_busyBits[(off) >> 4] & (1u << (((off) >> 1) & 7)))
static u8 s_libWaitBits[RAM_SIZE / 16], s_libPollBits[RAM_SIZE / 16];
#define LIBPOLL_BIT(off) (s_libPollBits[(off) >> 4] & (1u << (((off) >> 1) & 7)))
#define LIBWAIT_BIT(off) (s_libWaitBits[(off) >> 4] & (1u << (((off) >> 1) & 7)))
static void mark_code(u32 off, u32 bytes)
{
    u32 end = off + bytes;
    for (off &= ~1u; off < end && off < RAM_SIZE; off += 2) s_codeBits[off >> 4] |= (u8)(1u << ((off >> 1) & 7));
}

static u8 s_insnBits[RAM_SIZE / 16];
static u8 s_litWritten[RAM_SIZE / 32];

static void mark_insns(u32 off, u32 bytes)
{
    u32 end = off + bytes;
    for (off &= ~1u; off < end && off < RAM_SIZE; off += 2) s_insnBits[off >> 4] |= (u8)(1u << ((off >> 1) & 7));
}

void sh4_jit_note_literal(u32 addr, u32 bytes)
{
    mark_code(addr & (RAM_SIZE - 1), bytes);
}

int sh4_jit_literal_written(u32 addr)
{
    u32 w = (addr & (RAM_SIZE - 1)) >> 2;
    return (s_litWritten[w >> 3] >> (w & 7)) & 1;
}
static LinkSite *s_links;
static u32 s_linkCount;
static u32 s_pageLinks[RAM_PAGES];
static u32 *s_exit;
static int s_enabled, s_reference;
static Sh4JitStats s_stats;
void (*g_sh4_jit_block_hook)(u32 pc);
void (*g_sh4_jit_dispatch_hook)(u32 pc);

typedef void (*EnterFn)(u32 *code);
static EnterFn s_enter;
#if defined(SH4_JIT_ABI_ELFV1)
struct FnDesc { u64 entry, toc, env; };
static FnDesc s_enterDesc;
#endif

static u32 find_block_at(u32 off, u32 pc, u32 mode)
{
    u32 *slot = &s_ramBlock[(off & (RAM_SIZE - 1)) >> 1], idx = *slot, *prev = slot, depth = 0;
    while (idx)
    {
        JitBlock *b = &s_blocks[idx - 1];
        if (b->pc != pc) return 0;
        if (sh4_jit_key_match(mode, b->fpMode))
        {
            if (depth >= 2)
            {
                *prev = b->nextVersion;
                b->nextVersion = *slot;
                *slot = idx;
            }
            return idx;
        }
        prev = &b->nextVersion;
        idx = b->nextVersion;
        depth++;
    }
    return 0;
}

static u32 find_block(u32 pc, u32 mode)
{
    return find_block_at(pc & (RAM_SIZE - 1), pc, mode);
}

u32 g_sh4_jit_lookup_why[8];
int g_sh4_jit_exit_why;
u32 g_sh4_jit_exit_fail[11];

static u32 mode_of_first(u32 pc)
{
    int ok;
    u32 off = sh4_mmu_peek(pc, &ok) & (RAM_SIZE - 1), idx = ok ? s_ramBlock[off >> 1] : 0;
    return idx ? s_blocks[idx - 1].fpMode : 0;
}

static void lookup_why(u32 pc, u32 mode)
{
    const u8 *tc = (const u8 *)sh4_mmu_tc() + ((pc >> 10) & 4095) * MMU_TC_ENTRY;
    u32 tag = ((((pc >> 10) << 1) | ((mode & MMU_MD) ? 1u : 0u)) + 1) | (g_ccn.pteh << 24), idx, off;
    if (*(const u32 *)(tc + MMU_TC_OFF_TAG_READ) != tag) { g_sh4_jit_lookup_why[0]++; return; }
    off = (pc + *(const u32 *)(tc + MMU_TC_OFF_RAM_DELTA)) & 0xFFFFFFu;
    idx = s_ramBlock[off >> 1];
    if (!idx) { g_sh4_jit_lookup_why[1]++; return; }
    if (s_blocks[idx - 1].pc != pc) { g_sh4_jit_lookup_why[2]++; return; }
    while (idx && !sh4_jit_key_match(mode, s_blocks[idx - 1].fpMode)) idx = s_blocks[idx - 1].nextVersion;
    g_sh4_jit_lookup_why[idx ? 4 : 3]++;
}

const Sh4JitLookup *sh4_jit_lookup(void)
{
    static Sh4JitLookup l;
    l.ramBlock = s_ramBlock;
    l.blocks = (const u8 *)s_blocks;
    l.blockSize = (u32)sizeof(JitBlock);
    l.offPc = (u32)offsetof(JitBlock, pc);
    l.offCode = (u32)offsetof(JitBlock, code);
    l.offFpMode = (u32)offsetof(JitBlock, fpMode);
    l.offNextVersion = (u32)offsetof(JitBlock, nextVersion);
    return &l;
}

static void patch_branch(u32 *site, const u32 *to)
{
    *site = ppc_b_word(site, to);
    if (s_flush) s_flush(site, 4);
}

#define MAX_FAST_SITES (1u << 19)
struct FastSite
{
    u32 fault;
    u32 site, stub;
};
static FastSite *s_fast;
static u32 s_fastCount;

int sh4_jit_fast_room(u32 n)
{
    return s_fast && s_fastCount + n <= MAX_FAST_SITES;
}

int sh4_jit_add_fast_site(const u32 *fault, const u32 *site, const u32 *stub)
{
    FastSite *f;
    if (!sh4_jit_fast_room(1)) return 0;
    f = &s_fast[s_fastCount++];
    f->fault = (u32)(fault - s_mem);
    f->site = (u32)(site - s_mem);
    f->stub = (u32)(stub - s_mem);
    return 1;
}

static uintptr_t fast_fault(uintptr_t pc)
{
    u32 lo = 0, hi = s_fastCount, off;
    if (!s_fast || pc < (uintptr_t)s_mem) return 0;
    off = (u32)((pc - (uintptr_t)s_mem) / 4);
    while (lo < hi)
    {
        u32 mid = (lo + hi) / 2;
        if (s_fast[mid].fault < off) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= s_fastCount || s_fast[lo].fault != off) return 0;
    patch_branch(s_mem + s_fast[lo].site, s_mem + s_fast[lo].stub);
    return (uintptr_t)(s_mem + s_fast[lo].site);
}

#if defined(_XBOX)
static int fast_filter(EXCEPTION_POINTERS *ep)
{
    uintptr_t to;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    to = fast_fault((uintptr_t)ep->ContextRecord->Iar);
    if (!to) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Iar = (DWORD)to;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#elif defined(SH4_JIT_ABI_ELFV1)
static struct sigaction s_oldSegv;
static void fast_sigsegv(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    uintptr_t to = fast_fault((uintptr_t)uc->uc_mcontext.gp_regs[32]);
    if (to)
    {
        uc->uc_mcontext.gp_regs[32] = to;
        return;
    }
    sigaction(SIGSEGV, &s_oldSegv, NULL);
    if ((s_oldSegv.sa_flags & SA_SIGINFO) && s_oldSegv.sa_sigaction) s_oldSegv.sa_sigaction(sig, si, ctx);
    else raise(sig);
}
#endif
void sh4_jit_add_link(u32 *site, u32 *exit, u32 target, u32 off, u32 fpMode, int deferred)
{
    u32 page, idx;
    LinkSite *l;
    if (s_linkCount >= MAX_LINKS) return;
    off &= RAM_SIZE - 1;
    page = off >> 12;
    l = &s_links[s_linkCount];
    l->site = site;
    l->exit = exit;
    l->target = target;
    l->off = off;
    l->fpMode = fpMode;
    l->deferred = deferred && !BUSY_BIT(off);
    if (l->deferred) s_loopBits[off >> 4] |= (u8)(1u << ((off >> 1) & 7));
    l->next = s_pageLinks[page];
    s_pageLinks[page] = ++s_linkCount;
    idx = l->deferred ? 0 : find_block_at(off, target, fpMode);
    if (idx)
    {
        *site = ppc_b_word(site, s_blocks[idx - 1].code);
        s_stats.links++;
    }
}

static void link_resolve(const JitBlock *b)
{
    u32 *prev = &s_pageLinks[b->off >> 12], i;
    while ((i = *prev) != 0)
    {
        LinkSite *l = &s_links[i - 1];
        if (l->target == DEAD_TARGET)
        {
            *prev = l->next;
            continue;
        }
        if (l->target == b->pc && l->off == b->off && sh4_jit_key_match(l->fpMode, b->fpMode) && !l->deferred)
        {
            patch_branch(l->site, b->code);
            s_stats.links++;
        }
        prev = &l->next;
    }
}

#define BUSY_FAILS 16
static void promote_head(u32 pc)
{
    u32 off = pc & (RAM_SIZE - 1), i;
    if (LIBWAIT_BIT(off)) return;
    s_busyBits[off >> 4] |= (u8)(1u << ((off >> 1) & 7));
    for (i = s_pageLinks[off >> 12]; i; i = s_links[i - 1].next)
    {
        LinkSite *l = &s_links[i - 1];
        u32 idx;
        if (!l->deferred || l->target != pc) continue;
        l->deferred = 0;
        idx = find_block(pc, l->fpMode);
        if (idx)
        {
            patch_branch(l->site, s_blocks[idx - 1].code);
            s_stats.links++;
        }
    }
    s_stats.busyHeads++;
}

static void unlink_page(u32 page)
{
    u32 *prev = &s_pageLinks[page], i;
    while ((i = *prev) != 0)
    {
        LinkSite *l = &s_links[i - 1];
        if (l->target == DEAD_TARGET)
        {
            *prev = l->next;
            continue;
        }
        if (*l->site != ppc_b_word(l->site, l->exit)) patch_branch(l->site, l->exit);
        prev = &l->next;
    }
}

static void kill_block(u32 idx)
{
    while (idx)
    {
        JitBlock *b = &s_blocks[idx - 1];
        u32 k;
        for (k = b->firstLink; k < b->endLink; k++) s_links[k].target = DEAD_TARGET;
        b->endLink = b->firstLink;
        idx = b->nextVersion;
    }
}

static void clear_page(u32 page)
{
    u32 *slot = &s_ramBlock[page * 2048], i;
    for (i = 0; i < 2048; i++)
        if (slot[i]) kill_block(slot[i]);
    memset(slot, 0, 2048 * sizeof(u32));
    memset(&s_codeBits[page * 256], 0, 256);
    memset(&s_insnBits[page * 256], 0, 256);
    memset(&s_busyBits[page * 256], 0, 256);
    memset(&s_loopBits[page * 256], 0, 256);
    memset(&s_libWaitBits[page * 256], 0, 256);
    memset(&s_libPollBits[page * 256], 0, 256);
    s_pages[page] = 0;
    s_spills[page] = 0;
    unlink_page(page);
}

int sh4_jit_ram_has_code(u32 off)
{
    off &= (RAM_SIZE - 1) & ~1023u;
    return (s_pages[off >> 12] >> ((off >> 9) & 7)) & 3;
}

void sh4_jit_ram_written(u32 off)
{
    u32 page = off >> 12;
    if (!(s_pages[page] & (1u << ((off >> 9) & 7)))) return;
    s_stats.codePartWrites++;
    if (!CODE_BIT(off) && !CODE_BIT((off & ~3u) + 2) && !CODE_BIT(off & ~3u)) return;
    {
        u32 a = off & ~3u;
        if (!((s_insnBits[a >> 4] >> ((a >> 1) & 7)) & 3)) s_litWritten[a >> 5] |= (u8)(1u << ((a >> 2) & 7));
    }
    clear_page(page);
    sh4_mmu_code_changed(page << 12);
    sh4_jit_stop_chain();
    if (page > 0 && s_spills[page - 1])
    {
        clear_page(page - 1);
        sh4_mmu_code_changed((page - 1) << 12);
    }
    s_stats.invalidations++;
}

void sh4_jit_flush(void)
{
    s_fastCount = 0;
    if (!s_ramBlock) return;
    memset(s_ramBlock, 0, (RAM_SIZE / 2) * sizeof(u32));
    memset(s_pages, 0, sizeof(s_pages));
    sh4_mmu_code_changed(~0u);
    memset(s_spills, 0, sizeof(s_spills));
    memset(s_codeBits, 0, sizeof(s_codeBits));
    memset(s_insnBits, 0, sizeof(s_insnBits));
    memset(s_busyBits, 0, sizeof(s_busyBits));
    memset(s_loopBits, 0, sizeof(s_loopBits));
    memset(s_libWaitBits, 0, sizeof(s_libWaitBits));
    memset(s_libPollBits, 0, sizeof(s_libPollBits));
    memset(s_litWritten, 0, sizeof(s_litWritten));
    memset(s_pageLinks, 0, sizeof(s_pageLinks));
    s_blockCount = 0;
    s_linkCount = 0;
    s_pos = s_firstBlockWord;
    s_hotEnd = s_coldPos = s_pairEnd = s_pos;
    s_stats.flushes++;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

static int op_is_pure(u16 op)
{
    const char *name = sh4_op_name(op);
    if (strncmp(name, "bsr", 3) == 0 || strncmp(name, "jsr", 3) == 0) return 1;
    if (!sh4_op_writes_memory(op)) return 1;
    return ((op >> 8) & 15) == 15 && strncmp(name, "mov.b R0", 8) != 0 && strncmp(name, "mov.w R0", 8) != 0 &&
           (ends_with(name, ",@-Rn") || ends_with(name, ",@(disp,Rn)") || ends_with(name, ",@Rn"));
}

int sh4_jit_op_pure(u16 op)
{
    return op_is_pure(op);
}

u32 sh4_jit_skip_target(const u32 *ops, u32 i, u32 n)
{
    u16 op = (u16)ops[i];
    u32 side = (u32)sh4_jit_side_exit(op), t, j;
    if (!side || !g_sh4_jit_skips) return 0;
    t = i + 2 + (op & 0xFF);
    if ((op & 0xFF) > 16 || t >= n) return 0;
    for (j = i + side; j < t; j++)
    {
        u16 o = (u16)ops[j];
        if (sh4_jit_side_exit(o) || sh4_op_ends_block(o) || sh4_jit_has_delay_slot(o) || o == 0xF3FD || o == 0xFBFD ||
            (o & 0xF0FF) == 0x406A || (o & 0xF0FF) == 0x4066 || (o & 0xF0FF) == 0x400E)
            return 0;
    }
    return t;
}

int sh4_jit_side_exit(u16 op)
{
    u32 top = op >> 8;
    if (!g_sh4_jit_side_exits || (s8)(op & 0xFF) < 0) return 0;
    if (top == 0x89 || top == 0x8B) return 1;
    if (top == 0x8D || top == 0x8F) return 2;
    return 0;
}

static int is_head_pure(const u32 *ops, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
    {
        int side = sh4_jit_side_exit((u16)ops[i]);
        if (!op_is_pure((u16)ops[i])) return 0;
        if (side) return side == 1 || i + 1 >= n || op_is_pure((u16)ops[i + 1]);
    }
    return 1;
}

static int is_pure(u32 pc, const u32 *ops, u32 n, u32 endsWithBranch)
{
    u32 i;
    for (i = 0; i < n; i++)
        if (!op_is_pure((u16)ops[i])) return 0;
    if (endsWithBranch && sh4_jit_has_delay_slot((u16)ops[n - 1]) &&
        !op_is_pure(buf_r16(g_ram, (pc + n * 2) & (RAM_SIZE - 1))))
        return 0;
    return 1;
}

#define IDLE_STATE_SIZE offsetof(Sh4Ctx, sleeping)
#define IDLE_MAX_BLOCKS 32
#define IDLE_RETRY 16
static u8 s_idleState[IDLE_STATE_SIZE];
static int idle_detect(u32 pc, u32 mode)
{
    return !(mode & MMU_MODE) || (pc >= 0x80000000u && pc < 0xC0000000u);
}
static u32 s_idlePc;
static u32 s_idleBlocks;
static u32 s_idleWait;
static u32 s_idleWrote;

static int countdown_reg(const u8 *before)
{
    const Sh4Ctx *old = (const Sh4Ctx *)before;
    int i, k = -1;
    if (memcmp(before + sizeof(g_sh4.r), (const u8 *)&g_sh4 + sizeof(g_sh4.r), IDLE_STATE_SIZE - sizeof(g_sh4.r)) != 0)
        return -1;
    for (i = 0; i < 16; i++)
    {
        if (g_sh4.r[i] == old->r[i]) continue;
        if (k >= 0 || g_sh4.r[i] != old->r[i] - 1 || g_sh4.r[i] == 0) return -1;
        k = i;
    }
    return k;
}

#define CD_WINDOW 1024
static int countdown_verify(u32 pc, int k, s16 *cdOff, u16 *cdTurn)
{
    u8 before[IDLE_STATE_SIZE];
    u32 sp = g_sh4.r[15] & 0x1FFFFFFFu, steps, i, lo;
    static u32 s_stack[CD_WINDOW];
    int ok;
    memcpy(before, &g_sh4, IDLE_STATE_SIZE);
    lo = (sp & ~3u) > CD_WINDOW * 2 ? (sp & ~3u) - CD_WINDOW * 2 : 0;
    for (i = 0; i < CD_WINDOW; i++) s_stack[i] = mem_read32(lo + i * 4);
    g_mem_watch_lo = sp > 4096 ? sp - 4096 : 0;
    g_mem_watch_hi = sp + 4096;
    g_mem_watch_bad = 0;
    g_mem_watch = 1;
    for (steps = 0; steps < 4096; steps++)
    {
        if (g_sh4_irq_ready) break;
        sh4_step();
        s_stats.interpSteps++;
        if (g_sh4.pc == pc || (g_sh4.pc & 0x1C000000u) != 0x0C000000u) break;
    }
    g_mem_watch = 0;
    if (g_sh4.pc != pc) return g_sh4_irq_ready ? -1 : 0;
    ok = !g_mem_watch_bad && countdown_reg(before) == k;
    if (ok)
    {
        *cdOff = CD_NO_COPY;
        *cdTurn = (u16)(steps + 1);
        for (i = 0; i < CD_WINDOW; i++)
        {
            u32 v = mem_read32(lo + i * 4);
            if (v == g_sh4.r[k] && s_stack[i] == v + 1)
            {
                *cdOff = (s16)(s32)(lo + i * 4 - sp);
                break;
            }
        }
    }
    return ok;
}

static int untranslated(u32 a)
{
    return a >= 0x80000000u && a < 0xC0000000u;
}

static void countdown_skip(const JitBlock *b)
{
    u64 now = sched_now_exact(), next = sched_next(), skip = g_sh4.cycles > 0 ? (u64)g_sh4.cycles : 0;
    u32 turns, count = g_sh4.r[(int)b->cdReg], n;
    if (next != SCHED_NEVER && next > now && next - now > skip) skip = next - now;
    if (skip > 0x10000000u) skip = 0x10000000u;
    turns = b->cdTurn ? (u32)(skip / b->cdTurn) : 0;
    if (turns == 0 || count <= 1) return;
    n = turns < count - 1 ? turns : count - 1;
    if (b->cdOff != CD_NO_COPY)
    {
        u32 a = (g_sh4.r[15] + (u32)(s32)b->cdOff) & 0x1FFFFFFFu;
        if (mem_read32(a) != count) return;
        mem_write32(a, count - n);
    }
    g_sh4.r[(int)b->cdReg] = count - n;
}

static u32 scan_block(u32 off, u32 *ops, u32 *endsWithBranch, u32 max)
{
    u32 n;
    *endsWithBranch = 0;
    for (n = 0; n < max; n++)
    {
        u16 op = buf_r16(g_ram, (off + n * 2) & (RAM_SIZE - 1));
        int side = sh4_jit_side_exit(op);
        ops[n] = op;
        if (side && n + side < max - 1)
        {
            if (side == 2)
            {
                u16 slot = buf_r16(g_ram, (off + n * 2 + 2) & (RAM_SIZE - 1));
                if (sh4_op_ends_block(slot))
                {
                    *endsWithBranch = 1;
                    return n + 1;
                }
                ops[++n] = slot;
            }
            continue;
        }
        if (sh4_op_ends_block(op))
        {
            *endsWithBranch = 1;
            return n + 1;
        }
    }
    return n;
}

static u32 scan_block_mmu(u32 off, u32 pc, u32 *ops, u32 *endsWithBranch, u32 max)
{
    u32 room = (1024 - (pc & 1023)) / 2, n;
    if (max > room) max = room;
    n = scan_block(off, ops, endsWithBranch, max);
    if (n == room && *endsWithBranch && sh4_jit_has_delay_slot((u16)ops[n - 1]))
    {
        n--;
        *endsWithBranch = 0;
    }
    return n;
}

#define LIB_TURN_CYCLES 16384

struct LibLoop
{
    const char *name;
    u32 head;
    u32 count;
    const u16 *code;
    int poll;
};

static const u16 s_sofdecServerLoop[42] = {
    0x2FE6, 0x2FD6, 0x2FC6, 0xD416, 0x4F22, 0x9C23, 0x6E43, 0xA008, 0x3C4C, 0x6DE3,
    0x62D2, 0x2228, 0x8902, 0x62D2, 0x420B, 0x54D1, 0x7E08, 0x3EC2, 0x8BF5, 0x4F26,
    0x6CF6, 0x6DF6, 0x000B, 0x6EF6, 0xDC0E, 0xEE00, 0xDB0C, 0xED01, 0xD208, 0x6022,
    0x8801, 0x8B03, 0x4C0B, 0x64B2, 0xAFF8, 0x0009, 0xD303, 0xBFD9, 0x23D2, 0xD202,
    0xAFF2, 0x22E2,
};

static const u16 s_wceDdrawFlip[32] = {
    0x2FE6, 0x2F86, 0x2F96, 0x2FA6, 0x2FB6, 0x2FC6, 0x2FD6, 0x4F22, 0x1F6A, 0x1F48,
    0x6C53, 0x7FEC, 0x6EF3, 0x7FB0, 0xD045, 0xD844, 0x400B, 0x6482, 0x51ED, 0x2CC8,
    0x1F14, 0xE3FF, 0x5911, 0x633A, 0x5A91, 0x2338, 0x6DC3, 0x8D04, 0x1F3C, 0x52D1,
    0x1F28, 0xA002,
};

static const u16 s_wceFrameCounter[32] = {
    0x2F86, 0x4F22, 0x6853, 0x7FF0, 0x2888, 0x8927, 0xD435, 0x6442, 0x2448, 0x8923,
    0xD334, 0x6442, 0x6332, 0x3438, 0xD133, 0xE203, 0x6512, 0x75FF, 0x3526, 0x8915,
    0xE103, 0x3150, 0x8910, 0x2558, 0x8904, 0xE101, 0x3150, 0x8903, 0xA005, 0x0009,
    0xA00A, 0x2842,
};

static const LibLoop s_libLoops[] = {
    { "Sofdec server loop", 28, 42, s_sofdecServerLoop, 0 },
    { "Windows CE DirectDraw Flip", 0, 32, s_wceDdrawFlip, 1 },
    { "Windows CE frame counter", 0, 32, s_wceFrameCounter, 1 },
};

#define LIB_POLL_CYCLES 4096
#define LIB_POLL_CALLS 3
static u64 s_pollTurn;
static u32 s_pollPc, s_pollStreak;
static int s_pollWaited;

u32 g_sh4_jit_poll_stats[4];

int g_sh4_jit_countdown_skip = 1;

void sh4_jit_lib_poll(u32 pc)
{
    u64 now = sched_now_exact();
    int soon = pc == s_pollPc && now - s_pollTurn < LIB_POLL_CYCLES;
    g_sh4_jit_poll_stats[0]++;
    if (soon) g_sh4_jit_poll_stats[1]++;
    if (soon && g_sh4_irq_ready) g_sh4_jit_poll_stats[3]++;
    s_pollPc = pc;
    s_pollTurn = now;
    if (s_pollWaited)
    {
        s_pollWaited = 0;
        s_pollStreak = LIB_POLL_CALLS - 1;
        return;
    }
    s_pollStreak = soon ? s_pollStreak + 1 : 0;
    if (s_pollStreak >= LIB_POLL_CALLS && g_sh4_jit_idle_skip && !g_sh4_irq_ready)
    {
        s_pollWaited = 1;
        g_sh4_jit_poll_stats[2]++;
        s_stats.idleSkips++;
        g_sh4_idle = 1;
        g_sh4.cycles = 0;
    }
}

static u64 s_libTurn;
static u32 s_libPc;

static void lib_wait_check(u32 off)
{
    u32 k, i;
    for (k = 0; k < sizeof(s_libLoops) / sizeof(s_libLoops[0]); k++)
    {
        const LibLoop *l = &s_libLoops[k];
        u32 start = off - l->head * 2;
        if (off < l->head * 2 || start + l->count * 2 > RAM_SIZE || buf_r16(g_ram, off) != l->code[l->head]) continue;
        for (i = 0; i < l->count && buf_r16(g_ram, start + i * 2) == l->code[i]; i++) {}
        if (i < l->count) continue;
        if (l->poll)
        {
            if (!LIBPOLL_BIT(off)) LOG_INFO("[jit] %s at %08X", l->name, 0x8C000000u | off);
            s_libPollBits[off >> 4] |= (u8)(1u << ((off >> 1) & 7));
            continue;
        }
        if (!LIBWAIT_BIT(off)) LOG_INFO("[jit] %s at %08X", l->name, 0x8C000000u | off);
        s_libWaitBits[off >> 4] |= (u8)(1u << ((off >> 1) & 7));
    }
}

static u32 compile_at(u32 off, u32 pc, u32 fpMode, int tier2)
{
    PpcCode c;
    if (g_sh4_jit_mark_blocks) g_sh4_jit_phase = SH4_PHASE_COMPILE;
    Sh4JitGenInfo info;
    Sh4JitGenOut out;
    u32 n, ops[SH4_JIT_MAX_INSNS2], endsWithBranch, words;
    u32 startPos, startLink, a, end, max = tier2 ? SH4_JIT_MAX_INSNS2 : SH4_JIT_MAX_INSNS, sites, attempt, k;
    JitBlock *b;
    off &= RAM_SIZE - 1;
    if (s_memWords - (s_pairEnd > s_pos ? s_pairEnd : s_pos) < max * 96 + 256 + HOT_WORDS + COLD_WORDS ||
        s_blockCount >= MAX_BLOCKS)
        sh4_jit_flush();
    n = (fpMode & MMU_MODE) ? scan_block_mmu(off, pc, ops, &endsWithBranch, max) : scan_block(off, ops, &endsWithBranch, max);
    if (!n) return 0;
    memset(&info, 0, sizeof(info));
    info.fpMode = fpMode & ~(MMU_MODE | MMU_MD);
    info.pure = is_pure(pc, ops, n, endsWithBranch);
    info.link = g_sh4_jit_block_hook == NULL;
    if (fpMode & MMU_MODE)
    {
        info.mmu = 1;
        info.md = (fpMode & MMU_MD) != 0;
        info.physStart = off;
        info.linkMmu = fpMode & (MMU_MODE | MMU_MD);
        info.fpAny = 1;
        for (k = 0; k < n && info.fpAny; k++)
            if (sh4_jit_is_fpu_op((u16)ops[k])) info.fpAny = 0;
        if (info.fpAny && endsWithBranch && sh4_jit_has_delay_slot((u16)ops[n - 1]) &&
            sh4_jit_is_fpu_op(buf_r16(g_ram, (off + n * 2) & (RAM_SIZE - 1))))
            info.fpAny = 0;
        if (info.fpAny) fpMode = sh4_jit_any_key(fpMode);
    }
    if (!tier2) lib_wait_check(off);
    info.libPoll = LIBPOLL_BIT(off) != 0;
    info.exit = s_exit;
    info.tier2 = tier2;
    info.busy = BUSY_BIT(off) != 0;
    for (a = 0; a < 16; a++)
    {
        if ((g_sh4.r[a] >> 26) == 0x38) info.sqRegs |= 1u << a;
        if ((g_sh4.r[a] >> 26) == 0x1F && sh4_ocram_simple()) info.ocRegs |= 1u << a;
        if (((g_sh4.r[a] >> 26) & 7) == 3 && ((g_sh4.r[a] >> 24) & 3)) info.mirror = 1;
    }
    startLink = s_linkCount;
    sites = s_fastCount;
    for (attempt = g_sh4_jit_split ? 0 : 2;; attempt++)
    {
        if (attempt == 0 && s_hotEnd <= s_pos) continue;
        if (attempt == 1)
            new_pair();
        else if (attempt == 2 && g_sh4_jit_split)
            s_pos = s_pairEnd > s_pos ? s_pairEnd : s_pos;
        startPos = s_pos;
        startLink = s_linkCount;
        c.buf = s_mem + s_pos;
        c.pos = 0;
        c.overflow = 0;
        if (attempt < 2)
        {
            c.cap = s_hotEnd > s_pos ? s_hotEnd - s_pos : 0;
            info.coldPos = s_coldPos - s_pos;
            info.coldCap = s_pairEnd - s_pos;
            memcpy(info.nearRefill, s_nearRefill, sizeof(info.nearRefill));
        }
        else
        {
            c.cap = s_memWords - s_pos;
            info.coldPos = info.coldCap = 0;
            memset(info.nearRefill, 0, sizeof(info.nearRefill));
        }
        memset(&out, 0, sizeof(out));
        info.out = &out;
        info.dry = 0;
        info.initDirty = info.initDirtyFr = 0;
        if (tier2)
        {
            info.dry = 1;
            sh4_jit_gen_block(&c, pc, ops, n, endsWithBranch, &info);
            s_fastCount = sites;
            if (!c.overflow && !out.loops) return 0;
            if (!c.overflow)
            {
                info.dry = 0;
                info.initDirty = out.loopDirty;
                info.initDirtyFr = out.loopDirtyFr;
                memset(&out, 0, sizeof(out));
                c.pos = 0;
                sh4_jit_gen_block(&c, pc, ops, n, endsWithBranch, &info);
            }
        }
        else
            sh4_jit_gen_block(&c, pc, ops, n, endsWithBranch, &info);
        if (!c.overflow) break;
        s_fastCount = sites;
        for (k = startLink; k < s_linkCount; k++) s_links[k].target = DEAD_TARGET;
        if (attempt >= 2) return 0;
    }
    if (tier2)
    {
        s_stats.tier2Blocks++;
        if (out.loops) s_stats.tier2Loops++;
        if (g_sh4_jit_tier2_log)
            LOG_INFO("[jit] second tier %08X: %u instructions, %u words%s", pc, n, c.pos, out.loops ? ", loop" : "");
    }
    words = c.pos;
    s_pos += words;
    if (s_flush) s_flush(s_mem + startPos, words * 4);
    if (attempt < 2)
    {
        if (out.coldEnd > info.coldPos)
        {
            if (s_flush) s_flush(s_mem + s_coldPos, (startPos + out.coldEnd - s_coldPos) * 4);
            s_coldPos = startPos + out.coldEnd;
        }
    }
    else if (g_sh4_jit_split)
        s_hotEnd = s_coldPos = s_pairEnd = s_pos;

    b = &s_blocks[s_blockCount];
    b->pc = pc;
    b->off = off;
    b->code = s_mem + startPos;
    b->words = words;
    b->pure = info.pure;
    b->headPure = info.pure || is_head_pure(ops, n);
    b->fpMode = fpMode;
    b->nextVersion = s_ramBlock[off >> 1] && s_blocks[s_ramBlock[off >> 1] - 1].pc == pc ? s_ramBlock[off >> 1] : 0;
    b->firstLink = startLink;
    b->endLink = s_linkCount;
    b->idleFails = 0;
    b->idleHit = 0;
    b->countdown = 0;
    b->cdReg = -1;
    b->cdOff = CD_NO_COPY;
    b->cdTurn = 0;
    b->writeTurns = 0;
    b->promoted = 0;
    b->samples = 0;
    b->tier2 = (u16)tier2;
    s_blockCount++;
    s_ramBlock[off >> 1] = s_blockCount;
    end = off + n * 2 + 2;
    for (a = off & ~511u; a < end && a < RAM_SIZE; a += 512)
    {
        u8 bit = (u8)(1u << ((a >> 9) & 7));
        if (!(s_pages[a >> 12] & bit))
        {
            s_pages[a >> 12] |= bit;
            sh4_mmu_code_changed(a & ~4095u);
        }
    }
    mark_code(off, n * 2 + 2);
    mark_insns(off, n * 2 + 2);
    if (((end - 1) >> 12) != (off >> 12)) s_spills[off >> 12] = 1;
    link_resolve(b);
    s_stats.blocksCompiled++;
    s_stats.codeBytes = (s_pairEnd > s_pos ? s_pairEnd : s_pos) * 4;
    return s_blockCount;
}

static u32 compile_ex(u32 pc, u32 fpMode, int tier2)
{
    return compile_at(pc & (RAM_SIZE - 1), pc, fpMode, tier2);
}

static u32 compile(u32 pc)
{
    return compile_ex(pc, g_sh4.fpscr & FP_MODE_MASK, 0);
}

#define TIER2_SAMPLES 16

static u32 branch_target(u16 op, u32 pc)
{
    u32 top = op >> 8;
    if (top == 0x89 || top == 0x8B || top == 0x8D || top == 0x8F) return pc + 4 + (u32)((s32)(s8)(op & 0xFF) * 2);
    if ((op >> 12) == 0xA)
    {
        s32 d = op & 0xFFF;
        if (d & 0x800) d -= 0x1000;
        return pc + 4 + (u32)(d * 2);
    }
    return 0;
}

static u32 loop_head(u32 pc)
{
    u32 ops[SH4_JIT_MAX_INSNS2], n, ends, head, last;
    n = scan_block(pc & (RAM_SIZE - 1), ops, &ends, SH4_JIT_MAX_INSNS2);
    if (!ends) return pc;
    head = branch_target((u16)ops[n - 1], pc + (n - 1) * 2);
    if (!head || head > pc || pc - head > 256) return pc;
    if (head == pc) return pc;
    n = scan_block(head & (RAM_SIZE - 1), ops, &ends, SH4_JIT_MAX_INSNS2);
    last = head + (n - 1) * 2;
    return ends && branch_target((u16)ops[n - 1], last) == head && last >= pc ? head : pc;
}

static void replace_block(u32 oldIdx, u32 newIdx)
{
    JitBlock *o = &s_blocks[oldIdx - 1], *nb = &s_blocks[newIdx - 1];
    u32 *prev = &nb->nextVersion, k;
    while (*prev && *prev != oldIdx) prev = &s_blocks[*prev - 1].nextVersion;
    if (*prev == oldIdx) *prev = o->nextVersion;
    for (k = o->firstLink; k < o->endLink; k++) s_links[k].target = DEAD_TARGET;
    o->endLink = o->firstLink;
    o->tier2 = 1;
    patch_branch(o->code, nb->code);
}

static u32 tier2_compile(u32 idx, u32 pc)
{
    u32 mode = s_blocks[idx - 1].fpMode, head, oldIdx, newIdx, flushes = (u32)s_stats.flushes;
    s_blocks[idx - 1].tier2 = 1;
    head = loop_head(pc);
    if (LIBWAIT_BIT(head & (RAM_SIZE - 1)) || LIBWAIT_BIT(pc & (RAM_SIZE - 1))) return idx;
    oldIdx = find_block(head, mode);
    if (oldIdx && s_blocks[oldIdx - 1].tier2 && oldIdx != idx) return idx;
    newIdx = compile_ex(head, mode, 1);
    if ((u32)s_stats.flushes != flushes) return find_block(pc, mode);
    if (!newIdx) return idx;
    if (oldIdx) replace_block(oldIdx, newIdx);
    return head == pc ? newIdx : idx;
}

static void run_reference_block(const u32 *ops, u32 n, u32 endsWithBranch, int mmu)
{
    u32 i, last = ops[n - 1];
    int slot = endsWithBranch && sh4_jit_has_delay_slot((u16)last) && last != 0x002B;
    g_sh4.cycles -= (s32)(n + slot);
    for (i = 0; i < n; i++)
    {
        u16 op = (u16)ops[i];
        u32 next, oldSr = g_sh4.sr;
        int side = i + 1 < n || !endsWithBranch ? sh4_jit_side_exit(op) : 0;
        g_sh4.pc += 2;
        next = g_sh4.pc + (side == 2 ? 2 : 0);
        if (i == n - 1 && slot) g_sh4.cycles++;
        if (mmu)
        {
            if (sh4_jit_mmu_op(op, g_sh4.pc - 2))
            {
                if (side == 2) g_sh4.cycles++;
                return;
            }
        }
        else
            g_sh4_ops[op](op);
        if (mmu && (op & 0xF0FF) == 0x400E && ((g_sh4.sr ^ oldSr) & SR_MD))
        {
            g_sh4.cycles += (s32)(n - 1 - i + slot);
            return;
        }
        if (side)
        {
            if (side == 2)
            {
                g_sh4.cycles++;
                i++;
            }
            if (g_sh4.pc != next)
            {
                u32 t = sh4_jit_skip_target(ops, i + 1 - side, n);
                if (t)
                {
                    g_sh4.cycles += (s32)(t - i - 1);
                    i = t - 1;
                    continue;
                }
                g_sh4.cycles += (s32)(n - 1 - i + slot);
                return;
            }
        }
    }
}

static int s_running;
#define TAKEN_CYCLES jitTemp[3]

s32 sh4_jit_cycles_left(void)
{
    return g_sh4.cycles + (s32)g_sh4.TAKEN_CYCLES;
}

void sh4_jit_stop_chain(void)
{
    if (s_running && g_sh4.cycles > 0)
    {
        g_sh4.TAKEN_CYCLES += (u32)g_sh4.cycles;
        g_sh4.cycles = 0;
    }
}

void sh4_jit_run(s32 cycles)
{
    g_sh4.jitRam = g_ram;
    g_sh4.jitFloatMin = 1.17549435e-38f;
    g_sh4.jitFloatZero = 0.0f;
    g_sh4.jitFloatOne = 1.0f;
    g_sh4.jitCode = s_codeBits;
    g_sh4.cycles = cycles;
    g_sh4.TAKEN_CYCLES = 0;
    g_sh4.jitAsidTag = g_ccn.pteh << 24;
    g_sh4.jitSqKey = (g_ccn.mmucr & 0x200u) ? ~0u : 0x38u;
    g_sh4.jitSq = &g_sq[0][0];
    s_stats.slices++;
    g_sh4_break = 0;
    s_running = 1;
    if (g_sh4_jit_mark_blocks) g_sh4_jit_phase = SH4_PHASE_DISPATCH;
    s_idlePc = 0;
    if (g_sh4_irq_ready) sh4_irq_check();
    for (;;)
    {
        u32 pc, off, mode, idx = 0, ops[SH4_JIT_MAX_INSNS], n = 0, ends = 0;
        if (g_sh4.jitWhy)
        {
            if (g_sh4.jitWhy < 11) g_sh4_jit_exit_fail[g_sh4.jitWhy]++;
            if (g_sh4.jitWhy == 6 && !(g_sh4_jit_exit_fail[6] & 1023))
                LOG_INFO("[jit] lookup to %08X wanted mode %08X, found %08X (block's first version %08X)", g_sh4.jitWhyKey[2],
                         g_sh4.jitWhyKey[0], g_sh4.jitWhyKey[1], mode_of_first(g_sh4.jitWhyKey[2]));
            g_sh4.jitWhy = 0;
        }
        if (g_sh4.TAKEN_CYCLES)
        {
            g_sh4_jit_lookup_why[5]++;
            g_sh4.cycles += (s32)g_sh4.TAKEN_CYCLES;
            g_sh4.TAKEN_CYCLES = 0;
        }
        if (g_sh4.cycles <= 0 || g_sh4_break) break;
        pc = g_sh4.pc;
        int pure;
        if (g_sh4.sleeping)
        {
            g_sh4.cycles = 0;
            g_sh4_idle = 1;
            break;
        }
        off = pc;
        mode = g_sh4.fpscr & FP_MODE_MASK;
        if (g_mmu_on)
        {
            int mapped;
            if (g_sh4_jit_dispatch_hook && g_sh4_irq_ready) g_sh4_jit_lookup_why[6]++;
            if (g_sh4_jit_dispatch_hook && pc < 0x80000000u)
                lookup_why(pc, mode | MMU_MODE | (g_sh4.sr & SR_MD ? MMU_MD : 0));
            mapped = 0;
            if (!(pc & 1) && !(pc >= 0x80000000u && !(g_sh4.sr & SR_MD)))
            {
                off = sh4_mmu_peek(pc, &mapped);
                if (!mapped && sh4_mmu_fetch_refill(pc))
                {
                    off = sh4_mmu_peek(pc, &mapped);
                    if (g_sh4_jit_dispatch_hook) g_sh4_jit_lookup_why[7]++;
                }
            }
            if (!mapped)
            {
                sh4_step();
                s_stats.interpSteps++;
                continue;
            }
            mode |= MMU_MODE | (g_sh4.sr & SR_MD ? MMU_MD : 0);
        }
        if ((off & 0x1C000000u) != 0x0C000000u)
        {
            sh4_step();
            s_stats.interpSteps++;
            s_idlePc = 0;
            continue;
        }
        if (g_sh4_jit_block_hook) g_sh4_jit_block_hook(pc);
        if (g_sh4_jit_dispatch_hook) g_sh4_jit_dispatch_hook(pc);
        if (s_reference)
        {
            n = (mode & MMU_MODE) ? scan_block_mmu(off & (RAM_SIZE - 1), pc, ops, &ends, SH4_JIT_MAX_INSNS)
                                  : scan_block(off & (RAM_SIZE - 1), ops, &ends, SH4_JIT_MAX_INSNS);
            if (!n)
            {
                sh4_step();
                s_stats.interpSteps++;
                s_idlePc = 0;
                continue;
            }
            pure = is_pure(pc, ops, n, ends);
        }
        else
        {
            idx = find_block_at(off, pc, mode);
            if (!idx) idx = compile_at(off, pc, mode, 0);
            if (!idx)
            {
                sh4_step();
                s_stats.interpSteps++;
                s_idlePc = 0;
                continue;
            }
            if (g_sh4_jit_tier2 && !(mode & MMU_MODE) && !s_blocks[idx - 1].tier2 && ++s_blocks[idx - 1].samples >= TIER2_SAMPLES)
            {
                idx = tier2_compile(idx, pc);
                if (!idx) idx = compile(pc);
                if (!idx)
                {
                    sh4_step();
                    s_stats.interpSteps++;
                    s_idlePc = 0;
                    continue;
                }
            }
            pure = s_blocks[idx - 1].headPure;
        }
        if (LIBWAIT_BIT(off & (RAM_SIZE - 1)) && !(mode & MMU_MODE) && g_sh4_jit_idle_skip)
        {
            u64 now = sched_now_exact();
            int idle = pc == s_libPc && now - s_libTurn < LIB_TURN_CYCLES && !g_sh4_irq_ready;
            s_libPc = pc;
            s_libTurn = now;
            if (idle)
            {
                s_stats.idleSkips++;
                g_sh4_idle = 1;
                g_sh4.cycles = 0;
                break;
            }
        }
        if (g_sh4.jitWrote)
        {
            g_sh4.jitWrote = 0;
            s_idleWrote = 1;
            if (idx)
            {
                JitBlock *b = &s_blocks[idx - 1];
                if (idle_detect(pc, mode) && ++b->writeTurns >= BUSY_FAILS && !b->idleHit && !b->promoted && b->countdown != 1 &&
                    (b->countdown == 2 || b->writeTurns >= 4 * BUSY_FAILS))
                {
                    b->promoted = 1;
                    promote_head(pc);
                }
            }
        }
        else if (idx)
            s_blocks[idx - 1].writeTurns = 0;
        if (!idle_detect(pc, mode))
            s_idlePc = 0;
        else if (pc == s_idlePc && s_idleWrote && (mode & MMU_MODE) && !(untranslated(pc) && untranslated(g_sh4.r[15])))
            s_idlePc = 0;
        else if (pc == s_idlePc && g_sh4_jit_idle_skip && s_idleWrote)
        {
            int k = g_sh4_irq_ready ? -1 : countdown_reg(s_idleState), v = -1;
            JitBlock *b = idx ? &s_blocks[idx - 1] : NULL;
            if (k >= 0 && b && b->countdown != 2 && g_sh4_jit_countdown_skip)
            {
                s16 cdOff = CD_NO_COPY;
                u16 cdTurn = 0;
                if (b->countdown == 0 && (v = countdown_verify(pc, k, &cdOff, &cdTurn)) == 1)
                {
                    b->cdReg = (s8)k;
                    b->cdOff = cdOff;
                    b->cdTurn = cdTurn;
                }
                if (b->countdown == 1 || v == 1)
                {
                    if (b->countdown == 1 && b->cdReg != k) b->cdTurn = 0;
                    b->countdown = 1;
                    b->idleHit = 1;
                    countdown_skip(b);
                    s_stats.idleSkips++;
                    g_sh4_idle = 1;
                    g_sh4.cycles = 0;
                    break;
                }
                if (v == 0) b->countdown = 2;
                s_idlePc = 0;
                continue;
            }
            s_idlePc = 0;
        }
        else if (pc == s_idlePc && g_sh4_jit_idle_skip)
        {
            if (!g_sh4_irq_ready && memcmp(s_idleState, &g_sh4, sizeof(g_sh4.r)) == 0 &&
                memcmp(s_idleState, &g_sh4, IDLE_STATE_SIZE) == 0)
            {
                if (idx) s_blocks[idx - 1].idleHit = 1;
                s_stats.idleSkips++;
                g_sh4_idle = 1;
                g_sh4.cycles = 0;
                break;
            }
            if (idx && !g_sh4_irq_ready)
            {
                JitBlock *b = &s_blocks[idx - 1];
                if (!b->idleHit && ++b->idleFails >= BUSY_FAILS && !b->promoted)
                {
                    b->promoted = 1;
                    promote_head(pc);
                }
            }
            s_idlePc = 0;
            s_idleWait = IDLE_RETRY;
        }
        if (!idle_detect(pc, mode))
            ;
        else if (s_idleWait) s_idleWait--;
        else if (pure && (!s_idlePc || s_idleBlocks > IDLE_MAX_BLOCKS ||
                          (pc != s_idlePc && LOOP_BIT(pc & (RAM_SIZE - 1)) && !LOOP_BIT(s_idlePc & (RAM_SIZE - 1)))))
        {
            s_idlePc = pc;
            s_idleBlocks = 0;
            s_idleWrote = 0;
            memcpy(s_idleState, &g_sh4, IDLE_STATE_SIZE);
        }
        if (s_reference)
        {
            run_reference_block(ops, n, ends, (mode & MMU_MODE) != 0);
            if (!pure) g_sh4.jitWrote = 1;
        }
        else
        {
#if defined(_XBOX)
            __try
            {
                if (g_sh4_jit_mark_blocks) g_sh4_jit_phase = SH4_PHASE_CODE;
                s_enter(s_blocks[idx - 1].code);
            }
            __except (fast_filter(GetExceptionInformation()))
            {
            }
            if (g_sh4_jit_mark_blocks) g_sh4_jit_phase = SH4_PHASE_DISPATCH;
#else
            s_enter(s_blocks[idx - 1].code);
#endif
        }
        s_stats.blocksRun++;
        s_idleBlocks++;
        if (g_sh4_irq_ready && sh4_irq_check()) s_idlePc = 0;
    }
    s_running = 0;
}

int sh4_jit_init(void *execMem, u32 size, void (*flushIcache)(void *addr, u32 len))
{
#if SH4_JIT_CAN_RUN
    PpcCode c;
    u32 *enter;
    if (!execMem || size < (1u << 20)) return -1;
    s_mem = (u32 *)execMem;
    s_memWords = size / 4;
    s_flush = flushIcache;
    if (!s_ramBlock) s_ramBlock = (u32 *)calloc(RAM_SIZE / 2, sizeof(u32));
    if (!s_blocks) s_blocks = (JitBlock *)calloc(MAX_BLOCKS, sizeof(JitBlock));
    if (!s_links) s_links = (LinkSite *)calloc(MAX_LINKS, sizeof(LinkSite));
    if (!s_fast) s_fast = (FastSite *)malloc(MAX_FAST_SITES * sizeof(FastSite));
#if defined(SH4_JIT_ABI_ELFV1)
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = fast_sigsegv;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, &s_oldSegv);
    }
#endif
    if (!s_ramBlock || !s_blocks || !s_links) return -1;
    c.buf = s_mem;
    c.pos = 0;
    c.cap = s_memWords;
    c.overflow = 0;
    if (sh4_jit_gen_trampoline(&c, &enter, &s_exit) != 0) return -1;
    s_firstBlockWord = (c.pos + 15) & ~15u;
    if (s_flush) s_flush(s_mem, c.pos * 4);
#if defined(SH4_JIT_ABI_ELFV1)
    s_enterDesc.entry = (u64)(uintptr_t)enter;
    s_enterDesc.toc = ((const u64 *)(const void *)&sh4_jit_run)[1];
    s_enterDesc.env = 0;
    s_enter = (EnterFn)(void *)&s_enterDesc;
#else
    s_enter = (EnterFn)(void *)enter;
#endif
    sh4_jit_flush();
    memset(&s_stats, 0, sizeof(s_stats));
    LOG_INFO("[jit] %u KB of code memory at %p", size >> 10, execMem);
    return 0;
#else
    (void)execMem;
    (void)size;
    (void)flushIcache;
    return -1;
#endif
}

void sh4_jit_enable(int on)
{
    s_enabled = on && s_mem != NULL;
    g_mem_code_pages = s_enabled ? s_pages : NULL;
    g_mem_code_written = s_enabled ? sh4_jit_ram_written : NULL;
    if (s_enabled) sh4_jit_flush();
}

void sh4_jit_set_reference(int on)
{
    s_reference = on;
    s_enabled = on || s_mem != NULL;
}

int sh4_jit_active(void)
{
    return s_enabled;
}

void sh4_jit_block_map(void (*fn)(void *ctx, const u32 *code, u32 words, u32 pc, u32 off, u32 mode), void *ctx)
{
    u32 i;
    for (i = 0; i < s_blockCount; i++)
        if (s_blocks[i].code) fn(ctx, s_blocks[i].code, s_blocks[i].words, s_blocks[i].pc, s_blocks[i].off, s_blocks[i].fpMode);
}

u32 sh4_jit_block_code(u32 pc, const u32 **code)
{
    u32 idx = 0, off = pc;
    const u32 *end;
    if (g_mmu_on)
    {
        int mapped;
        off = sh4_mmu_peek(pc, &mapped);
        if (!mapped) return 0;
    }
    off &= RAM_SIZE - 1;
    if (s_ramBlock && s_ramBlock[off >> 1] && s_blocks[s_ramBlock[off >> 1] - 1].pc == pc) idx = s_ramBlock[off >> 1];
    if (!idx) return 0;
    *code = s_blocks[idx - 1].code;
    end = *code + s_blocks[idx - 1].words;
    return (u32)((end - *code) * 4);
}

void sh4_jit_stats(Sh4JitStats *out)
{
    *out = s_stats;
}

u32 *g_sh4_jit_fallbacks;
int g_sh4_jit_mark_blocks;
volatile int g_sh4_jit_phase;
u32 *g_sh4_jit_mmu_op_counts;
int g_sh4_jit_direct_indirect = 1;
int g_sh4_jit_side_exits = 1;
int g_sh4_jit_skips = 1;
int g_sh4_jit_tier2 = 1;
int g_sh4_jit_tier2_log;
int g_sh4_jit_fast_fpu;
int g_sh4_jit_fastmem = 1;
int g_sh4_jit_idle_skip = 1;

void sh4_jit_counted_op(u32 op)
{
    g_sh4_jit_fallbacks[op & 0xFFFF]++;
    g_sh4_ops[op & 0xFFFF](op);
}
