// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "sh4_jit_internal.h"
#include "ppc_call.h"

#include <stddef.h>
#include <string.h>

#include "sh4.h"
#include "sh4_internal.h"
#include "sh4_mmr.h"
#include "sh4_mmu.h"
#include "../core/log.h"
#include "../mem/mem.h"

#define FRAME 416
#define SAVE_SLOT(r) (FRAME - 16 - 8 * (s32)(31 - (r)))
#define FSAVE_SLOT(f) (112 + 8 * (s32)((f) - 14))
#define OFF_R(n) ((s32)offsetof(Sh4Ctx, r) + 4 * (s32)(n))
#define OFF_T ((s32)offsetof(Sh4Ctx, t))
#define OFF_PC ((s32)offsetof(Sh4Ctx, pc))
#define OFF_PR ((s32)offsetof(Sh4Ctx, pr))
#define OFF_GBR ((s32)offsetof(Sh4Ctx, gbr))
#define OFF_SR ((s32)offsetof(Sh4Ctx, sr))
#define OFF_MACL ((s32)offsetof(Sh4Ctx, macl))
#define OFF_MACH ((s32)offsetof(Sh4Ctx, mach))
#define OFF_CYCLES ((s32)offsetof(Sh4Ctx, cycles))
#define OFF_JRAM ((s32)offsetof(Sh4Ctx, jitRam))
#define OFF_JCODE ((s32)offsetof(Sh4Ctx, jitCode))
#define OFF_TEMP(i) ((s32)offsetof(Sh4Ctx, jitTemp) + 4 * (s32)(i))
#define OFF_TEMP64 ((s32)offsetof(Sh4Ctx, jitTemp64))
#define OFF_FMIN ((s32)offsetof(Sh4Ctx, jitFloatMin))
#define OFF_FZERO ((s32)offsetof(Sh4Ctx, jitFloatZero))
#define OFF_FONE ((s32)offsetof(Sh4Ctx, jitFloatOne))
#define OFF_FR(i) ((s32)offsetof(Sh4Ctx, fr) + 4 * (s32)(i))
#define OFF_XF(i) ((s32)offsetof(Sh4Ctx, xf) + 4 * (s32)(i))
#define OFF_FPUL ((s32)offsetof(Sh4Ctx, fpul))
#define OFF_FPSCR ((s32)offsetof(Sh4Ctx, fpscr))
#define OFF_IRQREADY ((s32)offsetof(Sh4Ctx, irqReady))
#define OFF_BREAK ((s32)offsetof(Sh4Ctx, breakSlice))
#define OFF_WROTE ((s32)offsetof(Sh4Ctx, jitWrote))
#define OFF_WHY ((s32)offsetof(Sh4Ctx, jitWhy))
#define OFF_ASIDTAG ((s32)offsetof(Sh4Ctx, jitAsidTag))
#define OFF_SQKEY ((s32)offsetof(Sh4Ctx, jitSqKey))
#define OFF_SQPTR ((s32)offsetof(Sh4Ctx, jitSq))
#define OFF_SQTAG ((s32)offsetof(Sh4Ctx, jitSqTag))
#define OFF_SQPA ((s32)offsetof(Sh4Ctx, jitSqPa))
#define OFF_WHYKEY(i) ((s32)offsetof(Sh4Ctx, jitWhyKey) + 4 * (s32)(i))
extern int g_sh4_jit_count_calls;
#define FP_UNKNOWN 0xFFFFFFFFu
enum { R_CYC = 11, R_T = 30, R_CTX = 31 };
enum { R_TC_DELTA = 9, R_TC_PAGE = 10 };

#define N ((op >> 8) & 15)
#define M ((op >> 4) & 15)

enum { RS_NONE = 0, RS_CLEAN = 1, RS_DIRTY = 2 };

enum { COLD_LOAD, COLD_STORE_OTHER, COLD_STORE_CODE, COLD_FLOAD, COLD_FSTORE_OTHER, COLD_FSTORE_CODE, COLD_BAIL, COLD_SIDE,
       COLD_MMU_LOAD, COLD_MMU_STORE, COLD_MMU_FLOAD, COLD_MMU_FSTORE, COLD_MMU_FLOAD2, COLD_MMU_FSTORE2, COLD_MMU_PREF,
       COLD_MMU_XLOAD2, COLD_MMU_FPUDIS };
#define MMU_MAX_AT 6
struct ColdStub
{
    u32 at, back;
    u32 site, fault;
    u8 kind, size, reg;
    u8 areg;
    u32 atMore[MMU_MAX_AT - 1], atCount;
    u8 sreg[16], sfr[16], st;
    u32 instPc;
};
#define MAX_COLD 160
#define MAX_SIDE 64

struct SideExit
{
    u8 reg[16], fr[16], t;
    u32 fp, target;
    s32 cycles;
};

struct Gen
{
    PpcCode *c;
    u8 reg[16];
    u8 t;
    u8 fr[16];
    u32 pc;
    int inSlot;
    const Sh4JitGenInfo *info;
    u32 start;
    u32 firstSub;
    u32 lastSub;
    u32 fp;
    int wrote;
    int marked;
    int fastChecked, fastOk;
    int fdOk;
    ColdStub cold[MAX_COLD];
    u32 coldCount;
    u32 coldPos, coldCap;
    SideExit side[MAX_SIDE];
    u32 sideCount;
    u32 prev;
    int srInR5;
    int fpulFr;
    int sqHint;
    int ocHint;
    struct { u8 base, src, used; u8 disp; } fwd[4];
    u32 fwdNext;
    u8 lastDef, lastDefPrev;
    u32 known;
    u32 knownVal[16];
    u8 litWhy[16];
    struct
    {
        int active;
        u32 at, index;
        u8 reg[16], fr[16], t;
        u32 known, knownVal[16];
        int fpulFr, marked, fdOk;
    } skip;
};

static u32 H(u32 n) { return 14 + n; }

static void call_c(PpcCode *c, const void *fn)
{
    ppc_stw(c, R_CYC, OFF_CYCLES, R_CTX);
    ppc_call(c, fn);
    ppc_lwz(c, R_CYC, OFF_CYCLES, R_CTX);
    ppc_li(c, R_TC_PAGE, -1);
#if defined(SH4_JIT_ABI_XBOX)
    ppc_ld(c, 0, FRAME - 8, 1);
    ppc_mtlr(c, 0);
#endif
}

static void probe_add(Gen *g, int k, u32 n);

static u32 get(Gen *g, u32 n)
{
    if (g->reg[n] == RS_NONE)
    {
        ppc_lwz(g->c, H(n), OFF_R(n), R_CTX);
        g->reg[n] = RS_CLEAN;
    }
    return H(n);
}

static void fwd_drop_reg(Gen *g, u32 n)
{
    u32 i;
    for (i = 0; i < 4; i++)
        if (g->fwd[i].used && (g->fwd[i].base == n || g->fwd[i].src == n)) g->fwd[i].used = 0;
}

static void fwd_clear(Gen *g)
{
    u32 i;
    for (i = 0; i < 4; i++) g->fwd[i].used = 0;
}

static u32 def(Gen *g, u32 n)
{
    g->lastDef = (u8)n;
    g->lastDefPrev = g->reg[n];
    g->reg[n] = RS_DIRTY;
    g->known &= ~(1u << n);
    g->litWhy[n] = 5;
    fwd_drop_reg(g, n);
    return H(n);
}

static u32 rw(Gen *g, u32 n)
{
    get(g, n);
    g->reg[n] = RS_DIRTY;
    g->known &= ~(1u << n);
    g->litWhy[n] = 5;
    fwd_drop_reg(g, n);
    return H(n);
}

static void def_const(Gen *g, u32 n, u32 v)
{
    ppc_li32(g->c, def(g, n), v);
    g->known |= 1u << n;
    g->knownVal[n] = v;
}

static u32 getT(Gen *g)
{
    if (g->t == RS_NONE)
    {
        ppc_lwz(g->c, R_T, OFF_T, R_CTX);
        g->t = RS_CLEAN;
    }
    return R_T;
}

static u32 defT(Gen *g)
{
    g->t = RS_DIRTY;
    return R_T;
}

static u32 FH(u32 n) { return 14 + n; }

static u32 fget(Gen *g, u32 n)
{
    if (g->fr[n] == RS_NONE)
    {
        ppc_lfs(g->c, FH(n), OFF_FR(n), R_CTX);
        g->fr[n] = RS_CLEAN;
    }
    return FH(n);
}

static u32 fdef(Gen *g, u32 n)
{
    g->fr[n] = RS_DIRTY;
    if (g->fpulFr == (int)n) g->fpulFr = -1;
    return FH(n);
}

static void fflush(Gen *g, u32 n)
{
    if (g->fr[n] == RS_DIRTY)
    {
        ppc_stfs(g->c, FH(n), OFF_FR(n), R_CTX);
        g->fr[n] = RS_CLEAN;
    }
}

static void fdrop(Gen *g, u32 n)
{
    g->fr[n] = RS_NONE;
    if (g->fpulFr == (int)n) g->fpulFr = -1;
}

static u16 code_at(Gen *g, u32 addr)
{
    if (g->info->mmu) return buf_r16(g_ram, (g->info->physStart + (addr - g->start)) & (RAM_SIZE - 1));
    return buf_r16(g_ram, addr & (RAM_SIZE - 1));
}

static void writeback(Gen *g)
{
    u32 n;
    for (n = 0; n < 16; n++)
        if (g->reg[n] == RS_DIRTY)
        {
            ppc_stw(g->c, H(n), OFF_R(n), R_CTX);
            g->reg[n] = RS_CLEAN;
        }
    for (n = 0; n < 16; n++) fflush(g, n);
    if (g->t == RS_DIRTY)
    {
        ppc_stw(g->c, R_T, OFF_T, R_CTX);
        g->t = RS_CLEAN;
    }
}

static void reload(Gen *g)
{
    u32 n, k = 0;
    if (g_sh4_jit_count_calls)
    {
        for (n = 0; n < 16; n++) k += (g->reg[n] == RS_NONE) + (g->fr[n] == RS_NONE);
        k += g->t == RS_NONE;
        probe_add(g, 9, k);
    }
    for (n = 0; n < 16; n++) get(g, n);
    getT(g);
    for (n = 0; n < 16; n++) fget(g, n);
}

static void forget(Gen *g)
{
    memset(g->reg, RS_NONE, sizeof(g->reg));
    memset(g->fr, RS_NONE, sizeof(g->fr));
    g->t = RS_NONE;
    g->known = 0;
    g->fpulFr = -1;
    fwd_clear(g);
}

static u32 *s_tcRefill[2][2];

static void gen_tc_refill_to(PpcCode *c, u32 *out[2][2])
{
    int store, md;
    for (store = 0; store < 2; store++)
        for (md = 0; md < 2; md++)
        {
            u32 miss;
            out[store][md] = c->buf + c->pos;
            ppc_rlwinm(c, 4, 3, 27, 15, 26);
            ppc_load_ptr_const(c, 5, ppc_addr_of(sh4_mmu_tc()));
            ppc_add(c, 5, 5, 4);
            ppc_lwz(c, 6, store ? MMU_TC_OFF_TAG_WRITE : MMU_TC_OFF_TAG_READ, 5);
            ppc_rlwinm(c, 12, 3, 23, 9, 30);
            ppc_addi(c, 12, 12, md + 1);
            ppc_lwz(c, 0, OFF_ASIDTAG, R_CTX);
            ppc_or(c, 12, 12, 0);
            ppc_cmplw(c, 0, 6, 12);
            miss = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            ppc_lwz(c, R_TC_DELTA, MMU_TC_OFF_RAM_DELTA, 5);
            ppc_rlwinm(c, R_TC_PAGE, 3, 0, 0, 21);
            if (store) ppc_ori(c, R_TC_PAGE, R_TC_PAGE, 1);
            ppc_patch_here(c, miss);
            ppc_blr(c);
        }
}

static void gen_tc_refill(PpcCode *c)
{
    gen_tc_refill_to(c, s_tcRefill);
}

void sh4_jit_gen_refill_copy(PpcCode *c, u32 *out[2][2])
{
    gen_tc_refill_to(c, out);
}

static u32 *s_exitLookup[3];

static void gen_exit_lookup(PpcCode *c, int mmu, int md, const u32 *exit)
{
    {
        const Sh4JitLookup *l = sh4_jit_lookup();
        u32 slow[10], i, ram = 0, hit, hitAny[2] = { 0, 0 };
        for (i = 0; i < 10; i++) slow[i] = 0;
        if (mmu)
        {
            u32 p1;
            ppc_rlwinm(c, 4, 3, 2, 30, 31);
            ppc_cmplwi(c, 0, 4, 2);
            p1 = ppc_bc_fwd(c, 12, 0, PPC_EQ);
            slow[6] = ppc_bc_fwd(c, 12, 0, PPC_GT);
            ppc_rlwinm(c, 4, 3, 27, 15, 26);
            ppc_load_ptr_const(c, 5, ppc_addr_of(sh4_mmu_tc()));
            ppc_add(c, 5, 5, 4);
            ppc_lwz(c, 6, MMU_TC_OFF_TAG_READ, 5);
            ppc_rlwinm(c, 12, 3, 23, 9, 30);
            ppc_addi(c, 12, 12, md + 1);
            ppc_lwz(c, 0, OFF_ASIDTAG, R_CTX);
            ppc_or(c, 12, 12, 0);
            ppc_cmplw(c, 0, 6, 12);
            slow[7] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            ppc_lwz(c, 6, MMU_TC_OFF_RAM_DELTA, 5);
            ppc_add(c, 4, 3, 6);
            ppc_rlwinm(c, 4, 4, 1, 7, 29);
            ram = ppc_b_fwd(c);
            ppc_patch_here(c, p1);
        }
        ppc_rlwinm(c, 4, 3, 6, 29, 31);
        ppc_cmplwi(c, 0, 4, 3);
        slow[2] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
        ppc_rlwinm(c, 4, 3, 1, 7, 29);
        if (ram) ppc_patch_here(c, ram);
        if (mmu)
        {
            ppc_rlwinm(c, 8, 7, 0, 14, 10);
            ppc_ori(c, 8, 8, SH4_JIT_FP_ANY);
        }
        {
            u32 miss[3], hitC[2] = { 0, 0 };
            ppc_mr(c, 0, 4);
            ppc_rlwinm(c, 12, 4, 3, 14, 26);
            ppc_load_ptr_const(c, 5, ppc_addr_of(l->jcache));
            ppc_add(c, 12, 12, 5);
            ppc_lwz(c, 6, 0, 12);
            ppc_cmplw(c, 0, 6, 4);
            miss[0] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            ppc_lwz(c, 6, 4, 12);
            ppc_cmplw(c, 0, 6, 3);
            miss[2] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            ppc_lwz(c, 6, 8, 12);
            ppc_cmplw(c, 0, 6, 7);
            hitC[0] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
            if (mmu)
            {
                ppc_cmplw(c, 0, 6, 8);
                hitC[1] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
            }
            miss[1] = ppc_b_fwd(c);
            ppc_patch_here(c, hitC[0]);
            if (hitC[1]) ppc_patch_here(c, hitC[1]);
            ppc_load_ptr(c, 6, 16, 12);
            ppc_mtctr(c, 6);
            ppc_bctr(c);
            ppc_patch_here(c, miss[0]);
            ppc_patch_here(c, miss[1]);
            ppc_patch_here(c, miss[2]);
        }
        ppc_load_ptr_const(c, 5, ppc_addr_of(l->ramBlock));
        ppc_lwzx(c, 4, 5, 4);
        ppc_cmpwi(c, 0, 4, 0);
        slow[3] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
        ppc_mulli(c, 4, 4, (s32)l->blockSize);
        ppc_load_ptr_const(c, 5, ppc_addr_of(l->blocks - l->blockSize));
        ppc_add(c, 5, 5, 4);
        ppc_lwz(c, 6, (s32)l->offPc, 5);
        ppc_cmplw(c, 0, 6, 3);
        slow[4] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
        ppc_lwz(c, 6, (s32)l->offFpMode, 5);
        ppc_cmplw(c, 0, 6, 7);
        hit = ppc_bc_fwd(c, 12, 0, PPC_EQ);
        if (mmu)
        {
            ppc_cmplw(c, 0, 6, 8);
            hitAny[0] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
        }
        ppc_lwz(c, 4, (s32)l->offNextVersion, 5);
        ppc_cmpwi(c, 0, 4, 0);
        slow[9] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
        ppc_mulli(c, 4, 4, (s32)l->blockSize);
        ppc_load_ptr_const(c, 5, ppc_addr_of(l->blocks - l->blockSize));
        ppc_add(c, 5, 5, 4);
        ppc_lwz(c, 6, (s32)l->offFpMode, 5);
        ppc_cmplw(c, 0, 6, 7);
        if (mmu)
        {
            hitAny[1] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
            ppc_cmplw(c, 0, 6, 8);
        }
        slow[5] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
        ppc_patch_here(c, hit);
        if (mmu)
        {
            ppc_patch_here(c, hitAny[0]);
            ppc_patch_here(c, hitAny[1]);
        }
        ppc_stw(c, 0, 0, 12);
        ppc_stw(c, 3, 4, 12);
        ppc_lwz(c, 6, (s32)l->offFpMode, 5);
        ppc_stw(c, 6, 8, 12);
        ppc_load_ptr(c, 6, (s32)l->offCode, 5);
        ppc_store_ptr(c, 6, 16, 12);
        ppc_mtctr(c, 6);
        ppc_bctr(c);
        {
            u32 join[10], nj = 0;
            for (i = 1; i < 10; i++)
                if (slow[i])
                {
                    ppc_patch_here(c, slow[i]);
                    ppc_li(c, 0, (s32)i + 1);
                    ppc_stw(c, 0, OFF_WHY, R_CTX);
                    if (i == 5 || i == 9)
                    {
                        ppc_stw(c, 7, OFF_WHYKEY(0), R_CTX);
                        ppc_stw(c, 6, OFF_WHYKEY(1), R_CTX);
                        ppc_stw(c, 3, OFF_WHYKEY(2), R_CTX);
                    }
                    join[nj++] = ppc_b_fwd(c);
                }
            for (i = 0; i < nj; i++) ppc_patch_here(c, join[i]);
        }
    }
    ppc_b_to(c, exit);
}

int sh4_jit_gen_trampoline(PpcCode *c, u32 **enter, u32 **exit)
{
    u32 r;
    *enter = c->buf + c->pos;
#if defined(SH4_JIT_ABI_XBOX)
    ppc_stwu(c, 1, -FRAME, 1);
#else
    ppc_stdu(c, 1, -FRAME, 1);
#endif
    ppc_mflr(c, 0);
    ppc_std(c, 0, FRAME - 8, 1);
    for (r = 14; r < 32; r++) ppc_std(c, r, SAVE_SLOT(r), 1);
    for (r = 14; r < 32; r++) ppc_stfd(c, r, FSAVE_SLOT(r), 1);
    ppc_load_ptr_const(c, R_CTX, ppc_addr_of(&g_sh4));
    for (r = 0; r < 16; r++) ppc_lwz(c, H(r), OFF_R(r), R_CTX);
    ppc_lwz(c, R_T, OFF_T, R_CTX);
    for (r = 0; r < 16; r++) ppc_lfs(c, FH(r), OFF_FR(r), R_CTX);
    ppc_lwz(c, R_CYC, OFF_CYCLES, R_CTX);
    ppc_li(c, R_TC_PAGE, -1);
    ppc_lfs(c, 30, OFF_FMIN, R_CTX);
    ppc_lfs(c, 31, OFF_FZERO, R_CTX);
    ppc_mtctr(c, 3);
    ppc_bctr(c);
    *exit = c->buf + c->pos;
    ppc_stw(c, R_CYC, OFF_CYCLES, R_CTX);
    for (r = 14; r < 32; r++) ppc_ld(c, r, SAVE_SLOT(r), 1);
    for (r = 14; r < 32; r++) ppc_lfd(c, r, FSAVE_SLOT(r), 1);
    ppc_ld(c, 0, FRAME - 8, 1);
    ppc_mtlr(c, 0);
    ppc_addi(c, 1, 1, FRAME);
    ppc_blr(c);
    gen_tc_refill(c);
    for (r = 0; r < 3; r++)
    {
        s_exitLookup[r] = c->buf + c->pos;
        gen_exit_lookup(c, r != 0, r == 2, *exit);
    }
    return c->overflow ? -1 : 0;
}

static void mark_write(Gen *g, u16 op)
{
    if (g->info->pure || g->marked || sh4_jit_op_pure(op)) return;
    ppc_li(g->c, 0, 1);
    ppc_stw(g->c, 0, OFF_WROTE, R_CTX);
    g->marked = 1;
}

static void emit_epilogue(Gen *g)
{
    ppc_b_to(g->c, g->info->exit);
}

static void exit_to_reg(Gen *g, u32 r);

static u32 link_mode(Gen *g)
{
    return (g->info->fpAny ? SH4_JIT_FP_ANY : g->fp) | g->info->linkMmu;
}

static void probe_exit(Gen *g)
{
    u32 n, k = 0;
    if (!g_sh4_jit_count_calls) return;
    for (n = 0; n < 16; n++) k += (g->reg[n] == RS_DIRTY) + (g->fr[n] == RS_DIRTY);
    k += g->t == RS_DIRTY;
    probe_add(g, 8, k);
    probe_add(g, 10, 1);
    if (g->info->link && g->fp == FP_UNKNOWN) probe_add(g, 4, 1);
}

static void exit_to(Gen *g, u32 target)
{
    PpcCode *c = g->c;
    u8 regs[16], frs[16], t = g->t;
    memcpy(regs, g->reg, sizeof(regs));
    memcpy(frs, g->fr, sizeof(frs));
    probe_exit(g);
    writeback(g);
    u32 off = target, linkable = 0;
    if (g->info->link && g->fp != FP_UNKNOWN && !g->info->mmu)
        linkable = (target & 0x1C000000u) == 0x0C000000u;
    else if (g->info->link && g->fp != FP_UNKNOWN)
    {
        if (target >= 0x80000000u && target < 0xC0000000u && (target & 0x1C000000u) == 0x0C000000u)
            linkable = 1;
        else if (!((target ^ g->start) & ~1023u))
        {
            off = (g->info->physStart & ~1023u) | (target & 1023u);
            linkable = 1;
        }
    }
    if (!linkable && g->info->mmu && g->info->link && g->fp != FP_UNKNOWN && g_sh4_jit_direct_indirect && target < 0x80000000u)
    {
        int mapped;
        u32 phys = sh4_mmu_peek(target, &mapped);
        if (mapped && (phys & 0x1C000000u) == 0x0C000000u && !g->info->dry)
        {
            const u8 *e = (const u8 *)sh4_mmu_tc() + ((target >> 10) & 4095) * MMU_TC_ENTRY;
            u32 tag0 = (((target >> 10) << 1) | (u32)g->info->md) + 1;
            u32 delta0 = ((phys & 0x00FFFFFFu) & ~0x3FFu) - (target & ~0x3FFu), guard[2], *site;
            reload(g);
            ppc_load_ptr_const(c, 4, ppc_addr_of(e));
            ppc_lwz(c, 6, MMU_TC_OFF_TAG_READ, 4);
            ppc_lwz(c, 0, OFF_ASIDTAG, R_CTX);
            ppc_lwz(c, 5, MMU_TC_OFF_RAM_DELTA, 4);
            ppc_li32(c, 12, tag0);
            ppc_or(c, 12, 12, 0);
            ppc_cmplw(c, 0, 6, 12);
            guard[0] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            ppc_li32(c, 12, delta0);
            ppc_cmplw(c, 0, 5, 12);
            guard[1] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            site = c->buf + c->pos;
            ppc_put(c, ppc_b_word(site, site + 1));
            if (!c->overflow) sh4_jit_add_link(site, site + 1, target, phys, link_mode(g), 0);
            ppc_patch_here(c, guard[0]);
            ppc_patch_here(c, guard[1]);
        }
        ppc_li32(c, 3, target);
        exit_to_reg(g, 3);
        memcpy(g->reg, regs, sizeof(regs));
        memcpy(g->fr, frs, sizeof(frs));
        g->t = t;
        return;
    }
    if (linkable)
    {
        u32 *site;
        reload(g);
        site = c->buf + c->pos;
        ppc_put(c, ppc_b_word(site, site + 1));
        if (!c->overflow && !g->info->dry)
            sh4_jit_add_link(site, site + 1, target, off, link_mode(g),
                             (!g->info->mmu || (target >= 0x80000000u && target < 0xC0000000u)) && target <= g->start);
        if (g->info->fpAny)
        {
            ppc_li32(c, 3, target);
            exit_to_reg(g, 3);
            memcpy(g->reg, regs, sizeof(regs));
            memcpy(g->fr, frs, sizeof(frs));
            g->t = t;
            return;
        }
    }
    ppc_li32(c, 3, target);
    ppc_stw(c, 3, OFF_PC, R_CTX);
    emit_epilogue(g);
    memcpy(g->reg, regs, sizeof(regs));
    memcpy(g->fr, frs, sizeof(frs));
    g->t = t;
}

static void exit_to_reg(Gen *g, u32 r)
{
    PpcCode *c = g->c;
    if (r != 4 && r != 12) probe_exit(g);
    writeback(g);
    ppc_stw(c, r, OFF_PC, R_CTX);
    if (g_sh4_jit_direct_indirect && g->info->link && g->fp != FP_UNKNOWN)
    {
        if (r != 3) ppc_mr(c, 3, r);
        reload(g);
        if (g->info->fpAny)
        {
            ppc_lwz(c, 7, OFF_FPSCR, R_CTX);
            ppc_rlwinm(c, 7, 7, 0, 11, 13);
            ppc_oris(c, 7, 7, g->info->linkMmu >> 16);
        }
        else
            ppc_li32(c, 7, g->fp | g->info->linkMmu);
        ppc_b_to(c, s_exitLookup[g->info->mmu ? 1 + g->info->md : 0]);
        return;
    }
    emit_epilogue(g);
}

static void set_phase(Gen *g, int phase)
{
    if (!g_sh4_jit_mark_blocks) return;
    ppc_load_ptr_const(g->c, 4, ppc_addr_of((const void *)&g_sh4_jit_phase));
    ppc_li(g->c, 0, phase);
    ppc_stw(g->c, 0, 0, 4);
}

static int writes_sr(u16 op)
{
    return (op & 0xF0FF) == 0x400E || (op & 0xF0FF) == 0x4007;
}

static void call_handler(Gen *g, u16 op)
{
    writeback(g);
    set_phase(g, SH4_PHASE_HANDLER);
    ppc_li32(g->c, 3, g->pc + 2);
    ppc_stw(g->c, 3, OFF_PC, R_CTX);
    ppc_li32(g->c, 3, op);
    call_c(g->c, g_sh4_jit_fallbacks ? (const void *)sh4_jit_counted_op : (const void *)g_sh4_ops[op]);
    set_phase(g, SH4_PHASE_CODE);
    forget(g);
    if (writes_sr(op)) g->fdOk = 0;
    g->wrote = 1;
}

static void emit_ldc_sr(Gen *g, u16 op, s32 rest)
{
    PpcCode *c = g->c;
    u32 rm = get(g, N), fast, same, quiet, n;
    writeback(g);
    fwd_clear(g);
    g->known &= ~0xFFu;
    ppc_li32(c, 4, SR_MASK & ~SR_T);
    ppc_and(c, 4, rm, 4);
    ppc_lwz(c, 5, OFF_SR, R_CTX);
    ppc_xor(c, 6, 4, 5);
    ppc_rlwinm(c, 0, 6, 0, 1, 2);
    ppc_cmplwi(c, 0, 0, 0);
    fast = ppc_bc_fwd(c, 12, 0, PPC_EQ);
    ppc_li32(c, 3, g->pc + 2);
    ppc_stw(c, 3, OFF_PC, R_CTX);
    ppc_li32(c, 3, op);
    call_c(c, g_sh4_jit_fallbacks ? (const void *)sh4_jit_counted_op : (const void *)g_sh4_ops[op]);
    if (g->info->mmu)
    {
        ppc_lwz(c, 3, OFF_SR, R_CTX);
        ppc_rlwinm(c, 3, 3, 2, 31, 31);
        ppc_cmpwi(c, 0, 3, g->info->md ? 1 : 0);
        same = ppc_bc_fwd(c, 12, 0, PPC_EQ);
        if (rest) ppc_addi(c, R_CYC, R_CYC, rest);
        emit_epilogue(g);
        ppc_patch_here(c, same);
    }
    for (n = 0; n < 16; n++)
        if (g->reg[n] != RS_NONE) ppc_lwz(c, H(n), OFF_R(n), R_CTX);
    ppc_lwz(c, R_T, OFF_T, R_CTX);
    quiet = ppc_b_fwd(c);
    ppc_patch_here(c, fast);
    ppc_stw(c, 4, OFF_SR, R_CTX);
    ppc_li32(c, 7, SR_BL | SR_IMASK);
    ppc_and(c, 7, 6, 7);
    ppc_cmplwi(c, 0, 7, 0);
    ppc_rlwinm(c, R_T, rm, 0, 31, 31);
    ppc_stw(c, R_T, OFF_T, R_CTX);
    n = ppc_bc_fwd(c, 12, 0, PPC_EQ);
    call_c(c, (const void *)sh4_irq_update);
    ppc_patch_here(c, n);
    ppc_patch_here(c, quiet);
    g->t = RS_CLEAN;
    g->wrote = 1;
}

static u32 ram_check(Gen *g)
{
    PpcCode *c = g->c;
    ppc_rlwinm(c, 4, 3, 6, 29, 31);
    ppc_cmplwi(c, 0, 4, 3);
    return ppc_bc_fwd(c, 4, 0, PPC_EQ);
}

static const void *write_fn(int size)
{
    return size == 4 ? (const void *)mem_write32 : size == 2 ? (const void *)mem_write16 : (const void *)mem_write8;
}

static void exit_to(Gen *g, u32 target);
static void cycles_add(Gen *g, s32 delta);

static void mirror_path(Gen *g, const ColdStub *s)
{
    PpcCode *c = g->c;
    int size = s->kind == COLD_FLOAD || s->kind == COLD_FSTORE_OTHER ? 4 : s->size;
    int store = s->kind == COLD_STORE_OTHER || s->kind == COLD_FSTORE_OTHER;
    u32 notRam, code = 0;
    ppc_rlwinm(c, 4, 3, 6, 29, 31);
    ppc_cmplwi(c, 0, 4, 3);
    notRam = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    if (store)
    {
        if (s->kind == COLD_FSTORE_OTHER)
        {
            ppc_stfs(c, s->reg, OFF_FR(s->size), R_CTX);
            ppc_lwz(c, 0, OFF_FR(s->size), R_CTX);
        }
        else ppc_mr(c, 0, s->reg);
    }
    ppc_rlwinm(c, 4, 3, 0, 8, size == 4 ? 29 : size == 2 ? 30 : 31);
    if (store)
    {
        ppc_rlwinm(c, 6, 4, 28, 4, 31);
        ppc_load_ptr(c, 5, OFF_JCODE, R_CTX);
        ppc_lbzx(c, 6, 5, 6);
        ppc_rlwinm(c, 5, 4, 31, 29, 31);
        ppc_srw(c, 6, 6, 5);
        ppc_rlwinm(c, 6, 6, 0, size == 4 ? 30 : 31, 31);
        ppc_cmplwi(c, 0, 6, 0);
        code = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    }
    if (size == 1) ppc_xori(c, 4, 4, 3);
    else if (size == 2) ppc_xori(c, 4, 4, 2);
    ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
    if (store)
    {
        if (size == 4) ppc_stwx(c, 0, 5, 4);
        else if (size == 2) ppc_sthx(c, 0, 5, 4);
        else ppc_stbx(c, 0, 5, 4);
    }
    else if (s->kind == COLD_FLOAD)
    {
        ppc_lwzx(c, 6, 5, 4);
        ppc_stw(c, 6, OFF_FR(s->size), R_CTX);
        ppc_lfs(c, FH(s->size), OFF_FR(s->size), R_CTX);
    }
    else if (size == 4) ppc_lwzx(c, s->reg, 5, 4);
    else if (size == 2) ppc_lhax(c, s->reg, 5, 4);
    else
    {
        ppc_lbzx(c, s->reg, 5, 4);
        ppc_extsb(c, s->reg, s->reg);
    }
    ppc_b_to(c, c->buf + s->back);
    if (store)
    {
        ppc_patch_here(c, code);
        ppc_mr(c, 4, 0);
        if (s->kind == COLD_FSTORE_OTHER) ppc_stw(c, 3, OFF_TEMP(2), R_CTX);
        call_c(c, write_fn(size));
        if (s->kind == COLD_FSTORE_OTHER) ppc_lwz(c, 3, OFF_TEMP(2), R_CTX);
        ppc_b_to(c, c->buf + s->back);
    }
    ppc_patch_here(c, notRam);
}

static void fast_addr(Gen *g, u32 ra, u32 me)
{
    ppc_rlwinm(g->c, 4, ra, 0, 3, me);
    if (g->info->mirror) ppc_rlwinm(g->c, 4, 4, 0, 8, 5);
}

static void emit_cold_stub(Gen *g, const ColdStub *s)
{
    PpcCode *c = g->c;
    u32 notSq;
    if (s->site != ~0u)
    {
        if (!g->info->dry) sh4_jit_add_fast_site(c->buf + s->fault, c->buf + s->site, c->buf + c->pos);
    }
    else
        ppc_patch_here(c, s->at);
    if (s->kind == COLD_SIDE)
    {
        const SideExit *e = &g->side[s->reg];
        u8 regs[16], frs[16], t = g->t;
        u32 fp = g->fp;
        memcpy(regs, g->reg, sizeof(regs));
        memcpy(frs, g->fr, sizeof(frs));
        memcpy(g->reg, e->reg, sizeof(regs));
        memcpy(g->fr, e->fr, sizeof(frs));
        g->t = e->t;
        g->fp = e->fp;
        cycles_add(g, e->cycles);
        exit_to(g, e->target);
        memcpy(g->reg, regs, sizeof(regs));
        memcpy(g->fr, frs, sizeof(frs));
        g->t = t;
        g->fp = fp;
        return;
    }
    if (s->kind == COLD_BAIL)
    {
        u32 k;
        for (k = 0; k < 16; k++)
            if (g->info->initDirty & (1u << k)) ppc_stw(c, H(k), OFF_R(k), R_CTX);
        if (g->info->initDirty & (1u << 16)) ppc_stw(c, R_T, OFF_T, R_CTX);
        for (k = 0; k < 16; k++)
            if (g->info->initDirtyFr & (1u << k)) ppc_stfs(c, FH(k), OFF_FR(k), R_CTX);
        ppc_li32(c, 3, g->start);
        ppc_stw(c, 3, OFF_PC, R_CTX);
        ppc_b_to(c, g->info->exit);
        return;
    }
    if (s->kind == COLD_MMU_FPUDIS)
    {
        u32 k;
        for (k = 0; k < 16; k++)
            if (s->sreg[k] == RS_DIRTY) ppc_stw(c, H(k), OFF_R(k), R_CTX);
        for (k = 0; k < 16; k++)
            if (s->sfr[k] == RS_DIRTY) ppc_stfs(c, FH(k), OFF_FR(k), R_CTX);
        if (s->st == RS_DIRTY) ppc_stw(c, R_T, OFF_T, R_CTX);
        ppc_li32(c, 3, s->instPc);
        ppc_li32(c, 4, s->reg);
        call_c(c, (const void *)sh4_jit_fpu_disabled);
        ppc_b_to(c, g->info->exit);
        return;
    }
    if (s->kind == COLD_MMU_LOAD || s->kind == COLD_MMU_STORE || s->kind == COLD_MMU_FLOAD || s->kind == COLD_MMU_FSTORE ||
        s->kind == COLD_MMU_FLOAD2 || s->kind == COLD_MMU_FSTORE2 || s->kind == COLD_MMU_PREF || s->kind == COLD_MMU_XLOAD2)
    {
        u32 k, ok;
        for (k = 0; k + 1 < s->atCount; k++) ppc_patch_here(c, s->atMore[k]);
        for (k = 0; k < 16; k++)
            if (s->sreg[k] == RS_DIRTY) ppc_stw(c, H(k), OFF_R(k), R_CTX);
        for (k = 0; k < 16; k++)
            if (s->sfr[k] == RS_DIRTY) ppc_stfs(c, FH(k), OFF_FR(k), R_CTX);
        if (s->st == RS_DIRTY) ppc_stw(c, R_T, OFF_T, R_CTX);
        if (s->kind == COLD_MMU_PREF)
        {
            ppc_li32(c, 4, s->instPc);
            call_c(c, (const void *)sh4_jit_mmu_pref);
        }
        else if (s->kind == COLD_MMU_FSTORE2)
        {
            ppc_stfs(c, s->reg, OFF_TEMP64, R_CTX);
            ppc_stfs(c, s->reg + 1, OFF_TEMP64 + 4, R_CTX);
            ppc_li32(c, 4, s->instPc);
            call_c(c, (const void *)sh4_jit_mmu_write_pair);
        }
        else if (s->kind == COLD_MMU_FLOAD2 || s->kind == COLD_MMU_XLOAD2)
        {
            ppc_li32(c, 4, s->instPc);
            call_c(c, (const void *)sh4_jit_mmu_read_pair);
        }
        else if (s->kind == COLD_MMU_FSTORE)
        {
            ppc_stfs(c, s->reg, OFF_TEMP64, R_CTX);
            ppc_lwz(c, 4, OFF_TEMP64, R_CTX);
            ppc_li32(c, 5, s->instPc);
            call_c(c, (const void *)sh4_jit_mmu_write32);
        }
        else if (s->kind == COLD_MMU_STORE)
        {
            ppc_mr(c, 4, s->reg);
            ppc_li32(c, 5, s->instPc);
            call_c(c, s->size == 4 ? (const void *)sh4_jit_mmu_write32 : s->size == 2 ? (const void *)sh4_jit_mmu_write16
                                                                                       : (const void *)sh4_jit_mmu_write8);
        }
        else
        {
            ppc_li32(c, 4, s->instPc);
            call_c(c, s->size == 4 ? (const void *)sh4_jit_mmu_read32 : s->size == 2 ? (const void *)sh4_jit_mmu_read16
                                                                                      : (const void *)sh4_jit_mmu_read8);
        }
        ppc_load_ptr_const(c, 6, ppc_addr_of(&g_sh4_jit_mmu_abort));
        ppc_lwz(c, 6, 0, 6);
        ppc_cmpwi(c, 0, 6, 0);
        ok = ppc_bc_fwd(c, 12, 0, PPC_EQ);
        ppc_b_to(c, g->info->exit);
        ppc_patch_here(c, ok);
        if (s->kind == COLD_MMU_FLOAD2)
        {
            ppc_lfs(c, FH(s->reg), OFF_TEMP64, R_CTX);
            ppc_lfs(c, FH(s->reg + 1), OFF_TEMP64 + 4, R_CTX);
        }
        else if (s->kind == COLD_MMU_XLOAD2)
        {
            ppc_lwz(c, 7, OFF_TEMP64, R_CTX);
            ppc_lwz(c, 8, OFF_TEMP64 + 4, R_CTX);
        }
        else if (s->kind == COLD_MMU_FLOAD)
        {
            ppc_stw(c, 3, OFF_TEMP64, R_CTX);
            ppc_lfs(c, FH(s->reg), OFF_TEMP64, R_CTX);
        }
        else if (s->kind == COLD_MMU_LOAD)
        {
            if (s->size == 4) ppc_mr(c, s->reg, 3);
            else if (s->size == 2) ppc_extsh(c, s->reg, 3);
            else ppc_extsb(c, s->reg, 3);
        }
        ppc_b_to(c, c->buf + s->back);
        return;
    }
    if (s->areg) ppc_mr(c, 3, s->areg);
    if (s->site != ~0u && (s->kind == COLD_LOAD || s->kind == COLD_STORE_OTHER || s->kind == COLD_FLOAD || s->kind == COLD_FSTORE_OTHER))
        mirror_path(g, s);
    switch (s->kind)
    {
    case COLD_LOAD:
        call_c(c, s->size == 4 ? (const void *)mem_read32 : s->size == 2 ? (const void *)mem_read16 : (const void *)mem_read8);
        if (s->size == 4) ppc_mr(c, s->reg, 3);
        else if (s->size == 2) ppc_extsh(c, s->reg, 3);
        else ppc_extsb(c, s->reg, 3);
        break;
    case COLD_STORE_OTHER:
        if (s->size == 4)
        {
            ppc_rlwinm(c, 6, 3, 6, 26, 31);
            ppc_cmplwi(c, 0, 6, 0x38);
            notSq = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            ppc_rlwinm(c, 6, 3, 0, 26, 29);
            ppc_load_ptr_const(c, 5, ppc_addr_of(g_sq));
            ppc_stwx(c, s->reg, 5, 6);
            ppc_b_to(c, c->buf + s->back);
            ppc_patch_here(c, notSq);
        }
    case COLD_STORE_CODE:
        ppc_mr(c, 4, s->reg);
        call_c(c, write_fn(s->size));
        break;
    case COLD_FLOAD:
        ppc_stw(c, 3, OFF_TEMP(2), R_CTX);
        call_c(c, (const void *)mem_read32);
        ppc_stw(c, 3, OFF_FR(s->size), R_CTX);
        ppc_lfs(c, FH(s->size), OFF_FR(s->size), R_CTX);
        ppc_lwz(c, 3, OFF_TEMP(2), R_CTX);
        break;
    case COLD_FSTORE_OTHER:
        ppc_rlwinm(c, 6, 3, 6, 26, 31);
        ppc_cmplwi(c, 0, 6, 0x38);
        notSq = ppc_bc_fwd(c, 4, 0, PPC_EQ);
        ppc_rlwinm(c, 6, 3, 0, 26, 29);
        ppc_load_ptr_const(c, 5, ppc_addr_of(g_sq));
        ppc_stfsx(c, s->reg, 5, 6);
        ppc_b_to(c, c->buf + s->back);
        ppc_patch_here(c, notSq);
    case COLD_FSTORE_CODE:
        ppc_stfs(c, s->reg, OFF_FR(s->size), R_CTX);
        ppc_lwz(c, 4, OFF_FR(s->size), R_CTX);
        ppc_stw(c, 3, OFF_TEMP(2), R_CTX);
        call_c(c, (const void *)mem_write32);
        ppc_lwz(c, 3, OFF_TEMP(2), R_CTX);
        break;
    }
    ppc_b_to(c, c->buf + s->back);
}

static void add_cold(Gen *g, u32 at, int kind, int size, u32 reg)
{
    ColdStub st;
    st.at = at;
    st.site = ~0u;
    st.fault = 0;
    st.back = g->c->pos;
    st.kind = (u8)kind;
    st.size = (u8)size;
    st.reg = (u8)reg;
    st.areg = 0;
    if (g->coldCount < MAX_COLD)
        g->cold[g->coldCount++] = st;
    else
    {
        u32 over = ppc_b_fwd(g->c);
        emit_cold_stub(g, &st);
        ppc_patch_here(g->c, over);
    }
}

static void add_cold_mmu(Gen *g, const u32 *br, u32 nb, int kind, int size, u32 reg)
{
    ColdStub st;
    u32 k;
    memset(&st, 0, sizeof(st));
    st.at = br[0];
    for (k = 1; k < nb; k++) st.atMore[k - 1] = br[k];
    st.atCount = nb;
    st.site = ~0u;
    st.back = g->c->pos;
    st.kind = (u8)kind;
    st.size = (u8)size;
    st.reg = (u8)reg;
    memcpy(st.sreg, g->reg, sizeof(st.sreg));
    memcpy(st.sfr, g->fr, sizeof(st.sfr));
    if (kind == COLD_MMU_LOAD && reg >= 14 && reg < 30 && g->lastDef == reg - 14) st.sreg[reg - 14] = g->lastDefPrev;
    st.st = g->t;
    st.instPc = g->inSlot ? g->pc - 2 : g->pc;
    if (g->coldCount < MAX_COLD)
        g->cold[g->coldCount++] = st;
    else
    {
        u32 over = ppc_b_fwd(g->c);
        emit_cold_stub(g, &st);
        ppc_patch_here(g->c, over);
    }
}

static u32 code_test(Gen *g, int size);

static u32 mmu_translate_inline(Gen *g, int size, int store, u32 *br)
{
    PpcCode *c = g->c;
    u32 nb = 0, tlb, done = 0, hit, alignEnd = size == 4 ? 29 : size == 2 ? 30 : 31;
    int md = g->info->md;
    if (md || !g_mmu_lut[0] || !g_mmu_lut[1]) ppc_cmpwi(c, 0, 3, 0);
    if (md)
    {
        tlb = ppc_bc_fwd(c, 4, 0, PPC_LT);
        ppc_rlwinm(c, 6, 3, 3, 29, 31);
        ppc_cmplwi(c, 0, 6, 5);
        br[nb++] = ppc_bc_fwd(c, 12, 0, PPC_GT);
        ppc_rlwinm(c, 4, 3, 6, 29, 31);
        ppc_cmplwi(c, 0, 4, 3);
        br[nb++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
        ppc_rlwinm(c, 4, 3, 0, 8, alignEnd);
        if (store) br[nb++] = code_test(g, size);
        done = ppc_b_fwd(c);
        ppc_patch_here(c, tlb);
    }
    else if (!g_mmu_lut[0] || !g_mmu_lut[1])
        br[nb++] = ppc_bc_fwd(c, 12, 0, PPC_LT);
    if (g_mmu_lut[0] && g_mmu_lut[1])
    {
        ppc_rlwinm(c, 6, 3, 22, 10, 29);
        ppc_load_ptr_const(c, 5, ppc_addr_of(g_mmu_lut[store]));
        ppc_lwzx(c, 6, 5, 6);
        ppc_cmpwi(c, 0, 6, -1);
        br[nb++] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
        ppc_add(c, 4, 3, 6);
        ppc_rlwinm(c, 4, 4, 0, 8, alignEnd);
        if (done) ppc_patch_here(c, done);
        return nb;
    }
    if (store)
    {
        ppc_ori(c, 6, 3, 1);
        ppc_xor(c, 6, 6, R_TC_PAGE);
        ppc_rlwinm(c, 6, 6, 0, 31, 21);
    }
    else
    {
        ppc_xor(c, 6, 3, R_TC_PAGE);
        ppc_rlwinm(c, 6, 6, 0, 0, 21);
    }
    ppc_cmplwi(c, 0, 6, 0);
    {
        u32 *at = c->buf + c->pos, *near = g->info->nearRefill[store][md];
        s32 d = near ? (s32)((near - at) * 4) : 0x8000;
        if (d > -0x8000 && d < 0x8000)
        {
            ppc_put(c, (16u << 26) | (4u << 21) | (PPC_EQ << 16) | ((u32)d & 0xFFFCu) | 1u);
            br[nb++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
        }
        else
        {
            hit = ppc_bc_fwd(c, 12, 0, PPC_EQ);
            at = c->buf + c->pos;
            ppc_put(c, ppc_b_word(at, s_tcRefill[store][md]) | 1u);
            br[nb++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            ppc_patch_here(c, hit);
        }
    }
    ppc_add(c, 4, 3, R_TC_DELTA);
    ppc_rlwinm(c, 4, 4, 0, 8, alignEnd);
    if (done) ppc_patch_here(c, done);
    return nb;
}

static void emit_load_mmu(Gen *g, u32 rd, int size)
{
    PpcCode *c = g->c;
    u32 br[MMU_MAX_AT], nb = mmu_translate_inline(g, size, 0, br);
    if (size == 1) ppc_xori(c, 4, 4, 3);
    else if (size == 2) ppc_xori(c, 4, 4, 2);
    ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
    if (size == 4) ppc_lwzx(c, rd, 5, 4);
    else if (size == 2) ppc_lhax(c, rd, 5, 4);
    else
    {
        ppc_lbzx(c, rd, 5, 4);
        ppc_extsb(c, rd, rd);
    }
    add_cold_mmu(g, br, nb, COLD_MMU_LOAD, size, rd);
}

static u32 code_test(Gen *g, int size);

static void emit_store_mmu(Gen *g, u32 rs, int size)
{
    PpcCode *c = g->c;
    u32 br[MMU_MAX_AT], nb = mmu_translate_inline(g, size, 1, br);
    if (size == 1) ppc_xori(c, 4, 4, 3);
    else if (size == 2) ppc_xori(c, 4, 4, 2);
    ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
    if (size == 4) ppc_stwx(c, rs, 5, 4);
    else if (size == 2) ppc_sthx(c, rs, 5, 4);
    else ppc_stbx(c, rs, 5, 4);
    add_cold_mmu(g, br, nb, COLD_MMU_STORE, size, rs);
}

static void emit_cold(Gen *g)
{
    PpcCode *c = g->c;
    u32 i, hotPos = c->pos, hotCap = c->cap;
    if (c->overflow)
    {
        g->coldCount = 0;
        g->sideCount = 0;
        return;
    }
    if (g->coldCap)
    {
        c->pos = g->coldPos;
        c->cap = g->coldCap;
    }
    for (i = 0; i < g->coldCount; i++) emit_cold_stub(g, &g->cold[i]);
    g->coldCount = 0;
    g->sideCount = 0;
    if (g->coldCap)
    {
        g->coldPos = c->pos;
        c->pos = hotPos;
        c->cap = hotCap;
        if (g->info->out) g->info->out->coldEnd = g->coldPos;
    }
}

static int add_side_exit(Gen *g, u32 target, s32 cycles)
{
    SideExit *e;
    if (g->sideCount >= MAX_SIDE || g->coldCount >= MAX_COLD) return 0;
    e = &g->side[g->sideCount];
    memcpy(e->reg, g->reg, sizeof(e->reg));
    memcpy(e->fr, g->fr, sizeof(e->fr));
    e->t = g->t;
    e->fp = g->fp;
    e->target = target;
    e->cycles = cycles;
    return 1;
}

static void add_side_cold(Gen *g, u32 at)
{
    ColdStub st;
    st.at = at;
    st.site = ~0u;
    st.fault = 0;
    st.back = g->c->pos;
    st.kind = COLD_SIDE;
    st.size = 0;
    st.areg = 0;
    st.reg = (u8)g->sideCount++;
    g->cold[g->coldCount++] = st;
}

static int fastmem(Gen *g)
{
    if (!g_sh4_jit_fastmem || !g_mem_window) return 0;
    if (!g->fastChecked)
    {
        g->fastOk = sh4_jit_fast_room(MAX_COLD * 2);
        g->fastChecked = 1;
    }
    return g->fastOk;
}

static void add_cold_fast_a(Gen *g, int kind, int size, u32 reg, u32 site, u32 fault, u32 areg)
{
    PpcCode *c = g->c;
    ColdStub st;
    st.at = 0;
    st.back = c->pos;
    st.site = site;
    st.fault = fault;
    st.kind = (u8)kind;
    st.size = (u8)size;
    st.reg = (u8)reg;
    st.areg = (u8)areg;
    if (g->coldCount < MAX_COLD)
        g->cold[g->coldCount++] = st;
    else
    {
        u32 over = ppc_b_fwd(c);
        emit_cold_stub(g, &st);
        ppc_patch_here(c, over);
    }
}

static void add_cold_fast(Gen *g, int kind, int size, u32 reg, u32 site, u32 fault)
{
    add_cold_fast_a(g, kind, size, reg, site, fault, 0);
}

static void emit_load(Gen *g, u32 rd, int size);

static void oc_hint(Gen *g, u32 n)
{
    g->ocHint = (int)((g->info->ocRegs >> n) & 1);
}

static u32 oc_inline(Gen *g, u32 reg, int size, int store)
{
    PpcCode *c = g->c;
    u32 notOc, done;
    if (!g->ocHint) return 0;
    g->ocHint = 0;
    ppc_rlwinm(c, 6, 3, 6, 26, 31);
    ppc_cmplwi(c, 0, 6, 0x1F);
    notOc = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    ppc_rlwinm(c, 4, 3, 0, 20, size == 4 ? 29 : size == 2 ? 30 : 31);
    ppc_rlwinm(c, 6, 3, 31, 19, 19);
    ppc_or(c, 4, 4, 6);
    if (size == 1) ppc_xori(c, 4, 4, 3);
    else if (size == 2) ppc_xori(c, 4, 4, 2);
    ppc_load_ptr_const(c, 5, ppc_addr_of(sh4_ocram_bytes()));
    if (store)
    {
        if (size == 4) ppc_stwx(c, reg, 5, 4);
        else if (size == 2) ppc_sthx(c, reg, 5, 4);
        else ppc_stbx(c, reg, 5, 4);
    }
    else if (size == 4) ppc_lwzx(c, reg, 5, 4);
    else if (size == 2) ppc_lhax(c, reg, 5, 4);
    else
    {
        ppc_lbzx(c, reg, 5, 4);
        ppc_extsb(c, reg, reg);
    }
    done = ppc_b_fwd(c);
    ppc_patch_here(c, notOc);
    return done;
}

static void emit_load_from(Gen *g, u32 rd, int size, u32 ra)
{
    PpcCode *c = g->c;
    u32 site, fault;
    if (g->info->mmu || !fastmem(g) || g->ocHint || rd == 4 || rd == 5 || ra == 4 || ra == 5)
    {
        ppc_mr(c, 3, ra);
        emit_load(g, rd, size);
        return;
    }
    site = c->pos;
    fast_addr(g, ra, size == 4 ? 29 : size == 2 ? 30 : 31);
    if (size == 1) ppc_xori(c, 4, 4, 3);
    else if (size == 2) ppc_xori(c, 4, 4, 2);
    ppc_load_ptr_const(c, 5, ppc_addr_of(g_mem_window));
    fault = c->pos;
    if (size == 4) ppc_lwzx(c, rd, 5, 4);
    else if (size == 2) ppc_lhax(c, rd, 5, 4);
    else
    {
        ppc_lbzx(c, rd, 5, 4);
        ppc_extsb(c, rd, rd);
    }
    add_cold_fast_a(g, COLD_LOAD, size, rd, site, fault, ra);
}

static void emit_load(Gen *g, u32 rd, int size)
{
    PpcCode *c = g->c;
    u32 slow, oc = oc_inline(g, rd, size, 0);
    if (oc)
    {
        emit_load(g, rd, size);
        ppc_patch_here(c, oc);
        return;
    }
    if (g->info->mmu)
    {
        emit_load_mmu(g, rd, size);
        return;
    }
    if (fastmem(g))
    {
        u32 site = c->pos, fault;
        fast_addr(g, 3, size == 4 ? 29 : size == 2 ? 30 : 31);
        if (size == 1) ppc_xori(c, 4, 4, 3);
        else if (size == 2) ppc_xori(c, 4, 4, 2);
        ppc_load_ptr_const(c, 5, ppc_addr_of(g_mem_window));
        fault = c->pos;
        if (size == 4) ppc_lwzx(c, rd, 5, 4);
        else if (size == 2) ppc_lhax(c, rd, 5, 4);
        else
        {
            ppc_lbzx(c, rd, 5, 4);
            ppc_extsb(c, rd, rd);
        }
        add_cold_fast(g, COLD_LOAD, size, rd, site, fault);
        return;
    }
    slow = ram_check(g);
    ppc_rlwinm(c, 4, 3, 0, 8, size == 4 ? 29 : size == 2 ? 30 : 31);
    if (size == 1) ppc_xori(c, 4, 4, 3);
    else if (size == 2) ppc_xori(c, 4, 4, 2);
    ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
    if (size == 4) ppc_lwzx(c, rd, 5, 4);
    else if (size == 2) ppc_lhax(c, rd, 5, 4);
    else
    {
        ppc_lbzx(c, rd, 5, 4);
        ppc_extsb(c, rd, rd);
    }
    add_cold(g, slow, COLD_LOAD, size, rd);
}

static void sq_hint(Gen *g, u32 n)
{
    g->sqHint = (int)((g->info->sqRegs >> n) & 1);
}

static u32 sq_check_mmu(Gen *g)
{
    PpcCode *c = g->c;
    u32 notSq;
    g->sqHint = 0;
    ppc_rlwinm(c, 6, 3, 6, 26, 31);
    if (g->info->md)
        ppc_cmplwi(c, 0, 6, 0x38);
    else
    {
        ppc_lwz(c, 5, OFF_SQKEY, R_CTX);
        ppc_cmplw(c, 0, 6, 5);
    }
    notSq = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    ppc_rlwinm(c, 6, 3, 0, 26, 29);
    ppc_load_ptr(c, 5, OFF_SQPTR, R_CTX);
    return notSq;
}

static u32 sq_inline(Gen *g, u32 rs, int fpr)
{
    PpcCode *c = g->c;
    u32 notSq, done;
    if (!g->sqHint) return 0;
    g->sqHint = 0;
    ppc_rlwinm(c, 6, 3, 6, 26, 31);
    ppc_cmplwi(c, 0, 6, 0x38);
    notSq = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    ppc_rlwinm(c, 6, 3, 0, 26, 29);
    ppc_load_ptr_const(c, 5, ppc_addr_of(g_sq));
    if (fpr) ppc_stfsx(c, rs, 5, 6);
    else ppc_stwx(c, rs, 5, 6);
    done = ppc_b_fwd(c);
    ppc_patch_here(c, notSq);
    return done;
}

static void fwd_add(Gen *g, u32 base, u32 disp, u32 src)
{
    u32 i = g->fwdNext++ & 3;
    g->fwd[i].base = (u8)base;
    g->fwd[i].disp = (u8)disp;
    g->fwd[i].src = (u8)src;
    g->fwd[i].used = 1;
}

static int fwd_load(Gen *g, u32 base, u32 disp, u32 n)
{
    PpcCode *c = g->c;
    u32 i, slow, src;
    if (g->info->mmu) return 0;
    for (i = 0; i < 4; i++)
        if (g->fwd[i].used && g->fwd[i].base == base && g->fwd[i].disp == disp) break;
    if (i == 4) return 0;
    src = g->fwd[i].src;
    if (g->ocHint)
    {
        g->ocHint = 0;
        ppc_rlwinm(c, 6, 3, 6, 26, 31);
        ppc_cmplwi(c, 0, 6, 0x1F);
        slow = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    }
    else
        slow = ram_check(g);
    if (src != n) ppc_mr(c, def(g, n), H(src));
    else def(g, n);
    add_cold(g, slow, COLD_LOAD, 4, H(n));
    return 1;
}

static u32 code_test(Gen *g, int size)
{
    PpcCode *c = g->c;
    ppc_rlwinm(c, 6, 4, 28, 12, 31);
    ppc_load_ptr(c, 5, OFF_JCODE, R_CTX);
    ppc_lbzx(c, 6, 5, 6);
    ppc_rlwinm(c, 0, 4, 31, 29, 31);
    ppc_srw(c, 6, 6, 0);
    ppc_rlwinm(c, 6, 6, 0, size == 4 ? 30 : 31, 31);
    ppc_cmplwi(c, 0, 6, 0);
    return ppc_bc_fwd(c, 4, 0, PPC_EQ);
}

static void emit_store(Gen *g, u32 rs, int size)
{
    PpcCode *c = g->c;
    u32 sq = size == 4 && !g->info->mmu ? sq_inline(g, rs, 0) : 0, oc = oc_inline(g, rs, size, 1), slow, slow2;
    g->wrote = 1;
    fwd_clear(g);
    if (g->info->mmu)
    {
        if (size == 4 && g->sqHint)
        {
            u32 notSq = sq_check_mmu(g);
            ppc_stwx(c, rs, 5, 6);
            add_cold_mmu(g, &notSq, 1, COLD_MMU_STORE, 4, rs);
        }
        else
        {
            g->sqHint = 0;
            emit_store_mmu(g, rs, size);
        }
        if (oc) ppc_patch_here(c, oc);
        return;
    }
    g->sqHint = 0;
    if (fastmem(g))
    {
        u32 site = c->pos, fault;
        fast_addr(g, 3, size == 4 ? 29 : size == 2 ? 30 : 31);
        slow2 = code_test(g, size);
        if (size == 1) ppc_xori(c, 4, 4, 3);
        else if (size == 2) ppc_xori(c, 4, 4, 2);
        ppc_load_ptr_const(c, 5, ppc_addr_of(g_mem_window));
        fault = c->pos;
        if (size == 4) ppc_stwx(c, rs, 5, 4);
        else if (size == 2) ppc_sthx(c, rs, 5, 4);
        else ppc_stbx(c, rs, 5, 4);
        add_cold_fast(g, COLD_STORE_OTHER, size, rs, site, fault);
        add_cold(g, slow2, COLD_STORE_CODE, size, rs);
        if (sq) ppc_patch_here(c, sq);
        if (oc) ppc_patch_here(c, oc);
        return;
    }
    slow = ram_check(g);
    ppc_rlwinm(c, 4, 3, 0, 8, size == 4 ? 29 : size == 2 ? 30 : 31);
    ppc_rlwinm(c, 6, 4, 28, 4, 31);
    ppc_load_ptr(c, 5, OFF_JCODE, R_CTX);
    ppc_lbzx(c, 6, 5, 6);
    ppc_rlwinm(c, 0, 4, 31, 29, 31);
    ppc_srw(c, 6, 6, 0);
    ppc_rlwinm(c, 6, 6, 0, size == 4 ? 30 : 31, 31);
    ppc_cmplwi(c, 0, 6, 0);
    slow2 = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    if (size == 1) ppc_xori(c, 4, 4, 3);
    else if (size == 2) ppc_xori(c, 4, 4, 2);
    ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
    if (size == 4) ppc_stwx(c, rs, 5, 4);
    else if (size == 2) ppc_sthx(c, rs, 5, 4);
    else ppc_stbx(c, rs, 5, 4);
    add_cold(g, slow, COLD_STORE_OTHER, size, rs);
    add_cold(g, slow2, COLD_STORE_CODE, size, rs);
    if (sq) ppc_patch_here(c, sq);
    if (oc) ppc_patch_here(c, oc);
}

static void t_is_zero(Gen *g, u32 r)
{
    PpcCode *c = g->c;
    ppc_put(c, ppc_x(r, 3, 0, 26, 0));
    ppc_rlwinm(c, defT(g), 3, 27, 31, 31);
}

static void t_less(Gen *g, u32 a, u32 b, int sign)
{
    PpcCode *c = g->c;
    if (sign)
    {
        ppc_extsw(c, 3, a);
        ppc_extsw(c, 4, b);
    }
    else
    {
        ppc_clrldi(c, 3, a, 32);
        ppc_clrldi(c, 4, b, 32);
    }
    ppc_subf(c, 3, 4, 3);
    ppc_srdi(c, defT(g), 3, 63);
}

static int is_op(u16 op, u16 mask, u16 key) { return (op & mask) == key; }

static void emit_addc_subc(Gen *g, u32 n, u32 m, int sub)
{
    PpcCode *c = g->c;
    u32 t = getT(g);
    ppc_clrldi(c, 3, get(g, n), 32);
    ppc_clrldi(c, 4, get(g, m), 32);
    if (sub)
    {
        ppc_subf(c, 3, 4, 3);
        ppc_subf(c, 3, t, 3);
        ppc_srdi(c, defT(g), 3, 63);
    }
    else
    {
        ppc_add(c, 3, 3, 4);
        ppc_add(c, 3, 3, t);
        ppc_srdi(c, defT(g), 3, 32);
    }
    ppc_mr(c, def(g, n), 3);
}

static void emit_div1(Gen *g, u32 n, u32 m)
{
    PpcCode *c = g->c;
    u32 t = getT(g), rn;
    if (!g->srInR5) ppc_lwz(c, 5, OFF_SR, R_CTX);
    ppc_rlwinm(c, 6, 5, 24, 31, 31);
    ppc_rlwinm(c, 7, 5, 23, 31, 31);
    ppc_xor(c, 6, 6, 7);
    ppc_xori(c, 6, 6, 1);
    ppc_neg(c, 6, 6);
    ppc_clrldi(c, 8, get(g, m), 32);
    ppc_xor(c, 8, 8, 6);
    ppc_subf(c, 8, 6, 8);
    rn = rw(g, n);
    ppc_rlwinm(c, 9, rn, 1, 31, 31);
    ppc_rlwinm(c, 10, rn, 1, 0, 30);
    ppc_or(c, 10, 10, t);
    ppc_clrldi(c, 10, 10, 32);
    ppc_add(c, 10, 10, 8);
    ppc_rldicl(c, 12, 10, 32, 63);
    ppc_mr(c, rn, 10);
    ppc_xor(c, 9, 9, 12);
    ppc_xori(c, defT(g), 9, 1);
    ppc_xor(c, 9, 9, 7);
    ppc_rlwimi(c, 5, 9, 8, 23, 23);
    ppc_stw(c, 5, OFF_SR, R_CTX);
    g->srInR5 = 1;
    if (g->info->mmu) ppc_li(c, R_TC_PAGE, -1);
}

static void emit_shift(Gen *g, u32 n, u32 m, int arith)
{
    PpcCode *c = g->c;
    u32 rn, right, done;
    if (g->known & (1u << m))
    {
        u32 v = g->knownVal[m], s;
        rn = rw(g, n);
        if (!(v >> 31))
        {
            s = v & 31;
            if (s) ppc_rlwinm(c, rn, rn, s, 0, 31 - s);
            return;
        }
        s = 32 - (v & 31);
        if (arith)
            ppc_srawi(c, rn, rn, s == 32 ? 31 : s);
        else if (s == 32)
            ppc_li(c, rn, 0);
        else
            ppc_rlwinm(c, rn, rn, 32 - s, s, 31);
        return;
    }
    ppc_mr(c, 5, get(g, m));
    rn = rw(g, n);
    ppc_rlwinm(c, 6, 5, 0, 27, 31);
    ppc_cmpwi(c, 0, 5, 0);
    right = ppc_bc_fwd(c, 12, 0, PPC_LT);
    ppc_slw(c, rn, rn, 6);
    done = ppc_b_fwd(c);
    ppc_patch_here(c, right);
    ppc_subfic(c, 6, 6, 32);
    if (arith)
    {
        ppc_extsw(c, 7, rn);
        ppc_srad(c, rn, 7, 6);
    }
    else
    {
        ppc_clrldi(c, 7, rn, 32);
        ppc_srd(c, rn, 7, 6);
    }
    ppc_patch_here(c, done);
}

static void emit_push_sys(Gen *g, u32 n, s32 off)
{
    PpcCode *c = g->c;
    ppc_lwz(c, 7, off, R_CTX);
    if (g->info->mmu)
    {
        ppc_addi(c, 3, get(g, n), -4);
        emit_store(g, 7, 4);
        ppc_addi(c, rw(g, n), H(n), -4);
        return;
    }
    ppc_addi(c, rw(g, n), H(n), -4);
    ppc_mr(c, 3, H(n));
    emit_store(g, 7, 4);
}

static void emit_pop_sys(Gen *g, u32 m, s32 off)
{
    PpcCode *c = g->c;
    ppc_mr(c, 3, get(g, m));
    emit_load(g, 7, 4);
    ppc_addi(c, rw(g, m), H(m), 4);
    ppc_stw(c, 7, off, R_CTX);
}

static void emit_literal(Gen *g, u32 n, u32 addr, int size)
{
    int mmu = g->info->mmu;
    u32 off = (mmu ? g->info->physStart + (addr - g->start) : addr) & (RAM_SIZE - 1), sub = off >> 9;
    u8 why = 0;
    if (g_sh4_jit_count_calls)
        why = !(mmu ? (addr ^ g->start) < 1024u : (addr & 0x1C000000u) == 0x0C000000u) ? 1
                     : !(sub >= g->firstSub && sub <= g->lastSub)                              ? 2
                     : g->wrote                                                                ? 3
                     : sh4_jit_literal_written(off)                                            ? 4 : 0;
    if ((mmu ? (addr ^ g->start) < 1024u : (addr & 0x1C000000u) == 0x0C000000u) && sub >= g->firstSub && sub <= g->lastSub &&
        !g->wrote && !sh4_jit_literal_written(off))
    {
        def_const(g, n, size == 4 ? buf_r32(g_ram, off) : (u32)(s32)(s16)buf_r16(g_ram, off));
        sh4_jit_note_literal(off, (u32)size);
    }
    else if (mmu && (addr ^ g->start) < 1024u)
    {
        ppc_load_ptr(g->c, 5, OFF_JRAM, R_CTX);
        ppc_li32(g->c, 4, size == 4 ? off : (off ^ 2));
        if (size == 4)
            ppc_lwzx(g->c, def(g, n), 5, 4);
        else
            ppc_lhax(g->c, def(g, n), 5, 4);
    }
    else
    {
        ppc_li32(g->c, 3, addr);
        emit_load(g, def(g, n), size);
    }
    g->litWhy[n] = why;
}

static s32 pair_off(u32 reg)
{
    return (reg & 1) ? OFF_XF(reg & 14) : OFF_FR(reg & 14);
}

static void dn_flush(Gen *g, u32 f)
{
    PpcCode *c = g->c;
    if (!(g->fp & FPSCR_DN) || g_sh4_jit_fast_fpu) return;
    ppc_fabs(c, 2, f);
    ppc_fsubs(c, 2, 2, 30);
    ppc_fmuls(c, 3, f, 31);
    ppc_fsel(c, f, 2, f, 3);
}

static void pair_flush(Gen *g, u32 reg)
{
    if (!(reg & 1))
    {
        fflush(g, reg & 14);
        fflush(g, (reg & 14) + 1);
    }
}

static void pair_drop(Gen *g, u32 reg)
{
    if (!(reg & 1))
    {
        fdrop(g, reg & 14);
        fdrop(g, (reg & 14) + 1);
    }
}

static void emit_fload(Gen *g, u32 n)
{
    PpcCode *c = g->c;
    u32 slow;
    if (g->info->mmu)
    {
        u32 br[MMU_MAX_AT], nb = mmu_translate_inline(g, 4, 0, br);
        ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
        ppc_lfsx(c, FH(n), 5, 4);
        add_cold_mmu(g, br, nb, COLD_MMU_FLOAD, 4, n);
        fdef(g, n);
        return;
    }
    if (fastmem(g))
    {
        u32 site = c->pos, fault;
        fast_addr(g, 3, 29);
        ppc_load_ptr_const(c, 5, ppc_addr_of(g_mem_window));
        fault = c->pos;
        ppc_lfsx(c, FH(n), 5, 4);
        add_cold_fast(g, COLD_FLOAD, (int)n, 0, site, fault);
        fdef(g, n);
        return;
    }
    slow = ram_check(g);
    ppc_rlwinm(c, 4, 3, 0, 8, 29);
    ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
    ppc_lfsx(c, FH(n), 5, 4);
    add_cold(g, slow, COLD_FLOAD, (int)n, 0);
    fdef(g, n);
}

static void emit_fstore(Gen *g, u32 m)
{
    PpcCode *c = g->c;
    u32 f, sq, slow, slow2;
    if (g->info->mmu)
    {
        u32 br[MMU_MAX_AT], nb;
        f = fget(g, m);
        g->wrote = 1;
        fwd_clear(g);
        if (g->sqHint)
        {
            br[0] = sq_check_mmu(g);
            ppc_stfsx(c, f, 5, 6);
            add_cold_mmu(g, br, 1, COLD_MMU_FSTORE, 4, f);
            return;
        }
        sq = 0;
        nb = mmu_translate_inline(g, 4, 1, br);
        ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
        ppc_stfsx(c, f, 5, 4);
        add_cold_mmu(g, br, nb, COLD_MMU_FSTORE, 4, f);
        if (sq) ppc_patch_here(c, sq);
        return;
    }
    f = fget(g, m);
    sq = sq_inline(g, f, 1);
    g->wrote = 1;
    fwd_clear(g);
    if (fastmem(g))
    {
        u32 site = c->pos, fault;
        fast_addr(g, 3, 29);
        slow2 = code_test(g, 4);
        ppc_load_ptr_const(c, 5, ppc_addr_of(g_mem_window));
        fault = c->pos;
        ppc_stfsx(c, f, 5, 4);
        add_cold_fast(g, COLD_FSTORE_OTHER, (int)m, f, site, fault);
        add_cold(g, slow2, COLD_FSTORE_CODE, (int)m, f);
        if (sq) ppc_patch_here(c, sq);
        return;
    }
    slow = ram_check(g);
    ppc_rlwinm(c, 4, 3, 0, 8, 29);
    ppc_rlwinm(c, 6, 4, 28, 4, 31);
    ppc_load_ptr(c, 5, OFF_JCODE, R_CTX);
    ppc_lbzx(c, 6, 5, 6);
    ppc_rlwinm(c, 0, 4, 31, 29, 31);
    ppc_srw(c, 6, 6, 0);
    ppc_rlwinm(c, 6, 6, 0, 30, 31);
    ppc_cmplwi(c, 0, 6, 0);
    slow2 = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
    ppc_stfsx(c, f, 5, 4);
    add_cold(g, slow, COLD_FSTORE_OTHER, (int)m, f);
    add_cold(g, slow2, COLD_FSTORE_CODE, (int)m, f);
    if (sq) ppc_patch_here(c, sq);
}

static void fmov_load(Gen *g, u32 n)
{
    PpcCode *c = g->c;
    if (!(g->fp & FPSCR_SZ))
    {
        emit_fload(g, n);
        return;
    }
    if (g->info->mmu && !(n & 1))
    {
        u32 br[MMU_MAX_AT], nb = mmu_translate_inline(g, 4, 0, br);
        ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
        ppc_lfsx(c, FH(n), 5, 4);
        ppc_addi(c, 4, 4, 4);
        ppc_lfsx(c, FH(n + 1), 5, 4);
        add_cold_mmu(g, br, nb, COLD_MMU_FLOAD2, 4, n);
        fdef(g, n);
        fdef(g, n + 1);
        return;
    }
    if (g->info->mmu)
    {
        u32 br[MMU_MAX_AT], nb = mmu_translate_inline(g, 4, 0, br);
        ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
        ppc_lwzx(c, 7, 5, 4);
        ppc_addi(c, 4, 4, 4);
        ppc_lwzx(c, 8, 5, 4);
        add_cold_mmu(g, br, nb, COLD_MMU_XLOAD2, 4, n);
        ppc_stw(c, 7, pair_off(n), R_CTX);
        ppc_stw(c, 8, pair_off(n) + 4, R_CTX);
        pair_drop(g, n);
        return;
    }
    if (!(n & 1))
    {
        emit_fload(g, n & 14);
        ppc_addi(c, 3, 3, 4);
        emit_fload(g, (n & 14) + 1);
        return;
    }
    ppc_stw(c, 3, OFF_TEMP(2), R_CTX);
    emit_load(g, 7, 4);
    ppc_stw(c, 7, pair_off(n), R_CTX);
    ppc_lwz(c, 3, OFF_TEMP(2), R_CTX);
    ppc_addi(c, 3, 3, 4);
    emit_load(g, 7, 4);
    ppc_stw(c, 7, pair_off(n) + 4, R_CTX);
    pair_drop(g, n);
}

static void fmov_store(Gen *g, u32 m)
{
    PpcCode *c = g->c;
    if (!(g->fp & FPSCR_SZ))
    {
        emit_fstore(g, m);
        return;
    }
    if (g->info->mmu)
    {
        u32 br[MMU_MAX_AT], nb, f0, f1, sq;
        if (m & 1)
        {
            ppc_lfs(c, 2, pair_off(m), R_CTX);
            ppc_lfs(c, 3, pair_off(m) + 4, R_CTX);
            f0 = 2;
            f1 = 3;
        }
        else
        {
            f0 = fget(g, m);
            f1 = fget(g, m + 1);
        }
        if (f1 != f0 + 1)
        {
            ppc_fmr(c, 2, f0);
            ppc_fmr(c, 3, f1);
            f0 = 2;
            f1 = 3;
        }
        g->wrote = 1;
        fwd_clear(g);
        if (g->sqHint)
        {
            br[0] = sq_check_mmu(g);
            ppc_stfsx(c, f0, 5, 6);
            ppc_addi(c, 6, 6, 4);
            ppc_stfsx(c, f1, 5, 6);
            add_cold_mmu(g, br, 1, COLD_MMU_FSTORE2, 4, f0);
            return;
        }
        sq = 0;
        nb = mmu_translate_inline(g, 4, 1, br);
        ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
        ppc_stfsx(c, f0, 5, 4);
        ppc_addi(c, 4, 4, 4);
        ppc_stfsx(c, f1, 5, 4);
        add_cold_mmu(g, br, nb, COLD_MMU_FSTORE2, 4, f0);
        if (sq) ppc_patch_here(c, sq);
        return;
    }
    if (!(m & 1))
    {
        int hint = g->sqHint;
        emit_fstore(g, m & 14);
        ppc_addi(c, 3, 3, 4);
        g->sqHint = hint;
        emit_fstore(g, (m & 14) + 1);
        return;
    }
    pair_flush(g, m);
    ppc_stw(c, 3, OFF_TEMP(2), R_CTX);
    ppc_lwz(c, 7, pair_off(m), R_CTX);
    emit_store(g, 7, 4);
    ppc_lwz(c, 3, OFF_TEMP(2), R_CTX);
    ppc_addi(c, 3, 3, 4);
    ppc_lwz(c, 7, pair_off(m) + 4, R_CTX);
    emit_store(g, 7, 4);
}

static void emit_fipr(Gen *g, u16 op)
{
    PpcCode *c = g->c;
    u32 n = (op >> 8) & 12, m = (op >> 6) & 12, k;
    for (k = 0; k < 4; k++) ppc_fmul(c, 4 + k, fget(g, n + k), fget(g, m + k));
    ppc_fadd(c, 0, 4, 5);
    ppc_fadd(c, 0, 0, 6);
    ppc_fadd(c, 0, 0, 7);
    k = fdef(g, n + 3);
    ppc_frsp(c, k, 0);
    dn_flush(g, k);
}

static void emit_ftrv(Gen *g, u16 op)
{
    PpcCode *c = g->c;
    static const u32 ld[4] = { 0, 1, 12, 13 };
    u32 n = (op >> 8) & 12, v[4], i, k;
    for (k = 0; k < 4; k++) v[k] = fget(g, n + k);
    for (k = 0; k < 4; k++)
    {
        for (i = 0; i < 4; i++) ppc_lfs(c, ld[i], OFF_XF(4 * k + i), R_CTX);
        for (i = 0; i < 4; i++) ppc_fmul(c, k ? 8 + i : 4 + i, ld[i], v[k]);
        if (k)
            for (i = 0; i < 4; i++) ppc_fadd(c, 4 + i, 4 + i, 8 + i);
    }
    for (i = 0; i < 4; i++) v[i] = fdef(g, n + i);
    for (i = 0; i < 4; i++) ppc_frsp(c, v[i], 4 + i);
    for (i = 0; i < 4; i++) dn_flush(g, v[i]);
}

static int emit_fpu(Gen *g, u16 op)
{
    PpcCode *c = g->c;
    u32 n = N, m = M, size;
    if ((op & 0xF0FF) == 0x405A)
    {
        ppc_stw(c, get(g, n), OFF_FPUL, R_CTX);
        return 1;
    }
    if ((op & 0xF0FF) == 0x005A)
    {
        ppc_lwz(c, def(g, n), OFF_FPUL, R_CTX);
        return 1;
    }
    if ((op >> 12) != 0xF || g->fp == FP_UNKNOWN || (g->fp & FPSCR_PR)) return 0;
    size = (g->fp & FPSCR_SZ) ? 8 : 4;
    switch (op & 15)
    {
    case 0xC:
        if (size == 4)
        {
            u32 src = fget(g, m);
            ppc_fmr(c, fdef(g, n), src);
        }
        else if (!(m & 1) && !(n & 1))
        {
            u32 a = fget(g, m), b = fget(g, m + 1);
            ppc_fmr(c, fdef(g, n), a);
            ppc_fmr(c, fdef(g, n + 1), b);
        }
        else if (!(m & 1))
        {
            ppc_stfs(c, fget(g, m), pair_off(n), R_CTX);
            ppc_stfs(c, fget(g, m + 1), pair_off(n) + 4, R_CTX);
        }
        else if (!(n & 1))
        {
            ppc_lfs(c, fdef(g, n), pair_off(m), R_CTX);
            ppc_lfs(c, fdef(g, n + 1), pair_off(m) + 4, R_CTX);
        }
        else
        {
            ppc_lwz(c, 3, pair_off(m), R_CTX);
            ppc_lwz(c, 4, pair_off(m) + 4, R_CTX);
            ppc_stw(c, 3, pair_off(n), R_CTX);
            ppc_stw(c, 4, pair_off(n) + 4, R_CTX);
        }
        return 1;
    case 0x8: ppc_mr(c, 3, get(g, m)); fmov_load(g, n); return 1;
    case 0x6: ppc_add(c, 3, get(g, m), get(g, 0)); fmov_load(g, n); return 1;
    case 0x9:
        if (g->info->mmu)
        {
            ppc_mr(c, 3, get(g, m));
            fmov_load(g, n);
            ppc_addi(c, rw(g, m), H(m), (s32)size);
            return 1;
        }
        ppc_mr(c, 3, get(g, m));
        ppc_addi(c, rw(g, m), H(m), (s32)size);
        fmov_load(g, n);
        return 1;
    case 0xA: ppc_mr(c, 3, get(g, n)); sq_hint(g, n); fmov_store(g, m); return 1;
    case 0x7: ppc_add(c, 3, get(g, n), get(g, 0)); sq_hint(g, n); fmov_store(g, m); return 1;
    case 0xB:
        if (g->info->mmu)
        {
            ppc_addi(c, 3, get(g, n), -(s32)size);
            sq_hint(g, n);
            fmov_store(g, m);
            ppc_addi(c, rw(g, n), H(n), -(s32)size);
            return 1;
        }
        ppc_addi(c, rw(g, n), H(n), -(s32)size);
        ppc_mr(c, 3, H(n));
        sq_hint(g, n);
        fmov_store(g, m);
        return 1;
    case 0x0: case 0x1: case 0x2: case 0x3:
    {
        u32 a = fget(g, n), b = fget(g, m), d = fdef(g, n);
        if ((op & 15) == 0) ppc_fadds(c, d, a, b);
        else if ((op & 15) == 1) ppc_fsubs(c, d, a, b);
        else if ((op & 15) == 2) ppc_fmuls(c, d, a, b);
        else ppc_fdivs(c, d, a, b);
        dn_flush(g, d);
        return 1;
    }
    case 0xE:
    {
        u32 r0 = fget(g, 0), b = fget(g, m), a = fget(g, n);
        ppc_fmuls(c, 0, r0, b);
        ppc_fadds(c, fdef(g, n), 0, a);
        dn_flush(g, FH(n));
        return 1;
    }
    case 0x4: case 0x5:
        ppc_fcmpu(c, 0, fget(g, n), fget(g, m));
        ppc_mfocrf(c, 3, 0x80);
        ppc_rlwinm(c, defT(g), 3, (op & 15) == 4 ? 3 : 2, 31, 31);
        return 1;
    case 0xD:
        switch ((op >> 4) & 15)
        {
        case 0x8: ppc_fmr(c, fdef(g, n), 31); return 1;
        case 0x9: ppc_lfs(c, fdef(g, n), OFF_FONE, R_CTX); return 1;
        case 0x0:
            if (g->fpulFr >= 0)
            {
                u32 k = (u32)g->fpulFr;
                if (k != n) ppc_fmr(c, fdef(g, n), fget(g, k));
            }
            else
                ppc_lfs(c, fdef(g, n), OFF_FPUL, R_CTX);
            return 1;
        case 0x1:
            ppc_stfs(c, fget(g, n), OFF_FPUL, R_CTX);
            g->fpulFr = (int)n;
            return 1;
        case 0x6:
        {
            u32 a = fget(g, n), d = fdef(g, n);
            ppc_fsqrt(c, d, a);
            ppc_frsp(c, d, d);
            return 1;
        }
        case 0x7:
        {
            u32 a = fget(g, n), d = fdef(g, n);
            ppc_fsqrt(c, 0, a);
            ppc_frsp(c, 0, 0);
            ppc_lfs(c, 1, OFF_FONE, R_CTX);
            ppc_fdivs(c, d, 1, 0);
            return 1;
        }
        case 0xE: emit_fipr(g, op); return 1;
        case 0x5:
        case 0x4:
        {
            u32 a = fget(g, n), d = fdef(g, n);
            if (((op >> 4) & 15) == 0x5) ppc_fabs(c, d, a);
            else ppc_fneg(c, d, a);
            return 1;
        }
        case 0x2:
        {
            u32 d = fdef(g, n);
            if ((g->prev & 0xF0FF) == 0x405A) ppc_extsw(c, 3, get(g, (g->prev >> 8) & 15));
            else
            {
                ppc_lwz(c, 3, OFF_FPUL, R_CTX);
                ppc_extsw(c, 3, 3);
            }
            ppc_std(c, 3, OFF_TEMP64, R_CTX);
            ppc_lfd(c, d, OFF_TEMP64, R_CTX);
            ppc_fcfid(c, d, d);
            ppc_frsp(c, d, d);
            return 1;
        }
        case 0x3:
            ppc_fctiwz(c, 0, fget(g, n));
            ppc_addi(c, 3, R_CTX, OFF_FPUL);
            ppc_stfiwx(c, 0, 0, 3);
            return 1;
        case 0xF:
            if ((op & 0x3FF) == 0x1FD)
            {
                emit_ftrv(g, op);
                return 1;
            }
            if (op == 0xF3FD)
            {
                ppc_lwz(c, 3, OFF_FPSCR, R_CTX);
                ppc_xoris(c, 3, 3, FPSCR_SZ >> 16);
                ppc_stw(c, 3, OFF_FPSCR, R_CTX);
                g->fp ^= FPSCR_SZ;
                return 1;
            }
            return 0;
        }
        return 0;
    }
    return 0;
}

static void track_fpscr(Gen *g, u16 op)
{
    if ((op & 0xF0FF) == 0x406A || (op & 0xF0FF) == 0x4066) g->fp = FP_UNKNOWN;
}

static u8 s_memOp[65536 / 8];
static int s_memOpReady;

static int mem_op(u16 op)
{
    if (!s_memOpReady)
    {
        u32 k;
        for (k = 0; k < 65536; k++)
        {
            const char *name = sh4_op_name((u16)k);
            if ((strchr(name, '@') && strncmp(name, "jmp", 3) && strncmp(name, "jsr", 3) && strncmp(name, "mova", 4) &&
                 strncmp(name, "ocb", 3)) ||
                k == SH4_HLE_TRAP_OPCODE)
                s_memOp[k >> 3] |= (u8)(1u << (k & 7));
        }
        s_memOpReady = 1;
    }
    return (s_memOp[op >> 3] >> (op & 7)) & 1;
}

static int mmu_native(u16 op)
{
    u32 hi = op >> 12, lo = op & 15, mid = (op >> 8) & 15;
    if ((op & 0xF0FF) == 0x0083) return 1;
    if ((hi == 0x2 || hi == 0x6) && (lo <= 2 || (lo >= 4 && lo <= 6))) return 1;
    if (hi == 0x0 && ((lo >= 4 && lo <= 6) || (lo >= 0xC && lo <= 0xE))) return 1;
    if (hi == 0x1 || hi == 0x5 || hi == 0x9 || hi == 0xD) return 1;
    if (hi == 0x8 && (mid == 0 || mid == 1 || mid == 4 || mid == 5)) return 1;
    if (hi == 0xC && (mid <= 2 || (mid >= 4 && mid <= 6))) return 1;
    if (hi == 0x4 && ((op & 0xFF) == 0x02 || (op & 0xFF) == 0x12 || (op & 0xFF) == 0x22 || (op & 0xFF) == 0x06 ||
                      (op & 0xFF) == 0x16 || (op & 0xFF) == 0x26))
        return 1;
    if (hi == 0x4 && ((op & 0xFF) == 0x13 || (op & 0xFF) == 0x17)) return 1;
    return 0;
}

static void emit_mmu_op(Gen *g, u16 op)
{
    PpcCode *c = g->c;
    u32 ok;
    writeback(g);
    ppc_li32(c, 3, g->pc + 2);
    ppc_stw(c, 3, OFF_PC, R_CTX);
    ppc_li32(c, 3, op);
    ppc_li32(c, 4, g->inSlot ? g->pc - 2 : g->pc);
    call_c(c, (const void *)sh4_jit_mmu_op);
    forget(g);
    if (writes_sr(op)) g->fdOk = 0;
    g->wrote = 1;
    ppc_cmpwi(c, 0, 3, 0);
    ok = ppc_bc_fwd(c, 12, 0, PPC_EQ);
    emit_epilogue(g);
    ppc_patch_here(c, ok);
    track_fpscr(g, op);
}

static void emit_andi(PpcCode *c, u32 ra, u32 rs, u32 imm)
{
    u32 lo, hi;
    if (!imm)
    {
        ppc_li(c, ra, 0);
        return;
    }
    for (lo = 0; !(imm >> lo & 1); lo++) {}
    for (hi = lo; hi < 31 && (imm >> (hi + 1) & 1); hi++) {}
    if ((imm >> lo) == (2u << (hi - lo)) - 1) ppc_rlwinm(c, ra, rs, 0, 31 - hi, 31 - lo);
    else ppc_andi_(c, ra, rs, imm);
}

static int emit_native(Gen *g, u16 op)
{
    PpcCode *c = g->c;
    u32 n, m, rn, rm;
    s32 imm8 = (s32)(s8)(op & 0xFF);
    if ((op & 0xF0FF) == 0x00C3) op = (u16)(0x2002 | (op & 0x0F00));
    n = N;
    m = M;

    if (g->info->mmu && mem_op(op) && !mmu_native(op) &&
        !((op >> 12) == 0xF && (op & 15) >= 6 && (op & 15) <= 0xB && g->fp != FP_UNKNOWN && !(g->fp & FPSCR_PR)))
    {
        emit_mmu_op(g, op);
        return 1;
    }

    if (op == 0xFBFD && g->fp != FP_UNKNOWN && !(g->fp & FPSCR_PR))
    {
        u32 k;
        for (k = 0; k < 16; k++) fflush(g, k);
        ppc_li32(c, 3, op);
        call_c(c, (const void *)g_sh4_ops[op]);
        for (k = 0; k < 16; k++) fdrop(g, k);
        return 1;
    }
    if ((op & 0xF1FF) == 0xF0FD && g->fp != FP_UNKNOWN && !(g->fp & FPSCR_PR))
    {
        u32 fn = (op >> 8) & 14;
        ppc_li32(c, 3, op);
        call_c(c, (const void *)g_sh4_ops[op]);
        fdrop(g, fn);
        fdrop(g, fn + 1);
        return 1;
    }
    if ((op >> 12) == 0xF || (op & 0xF0FF) == 0x405A || (op & 0xF0FF) == 0x005A) return emit_fpu(g, op);
    switch (op >> 12)
    {
    case 0x0:
        if (op == 0x0009) return 1;
        if (op == 0x0008) { ppc_li(c, defT(g), 0); return 1; }
        if (op == 0x0018) { ppc_li(c, defT(g), 1); return 1; }
        if (op == 0x0019)
        {
            ppc_lwz(c, 5, OFF_SR, R_CTX);
            ppc_rlwinm(c, 5, 5, 0, 24, 21);
            ppc_stw(c, 5, OFF_SR, R_CTX);
            ppc_li(c, defT(g), 0);
            return 1;
        }
        if (is_op(op, 0xF0FF, 0x0083) && g->info->mmu)
        {
            u32 br[1], notSq;
            fwd_clear(g);
            ppc_mr(c, 3, get(g, n));
            ppc_rlwinm(c, 4, 3, 6, 26, 31);
            ppc_cmplwi(c, 0, 4, 0x38);
            notSq = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            ppc_rlwinm(c, 6, 3, 0, 0, 21);
            if (g->info->md) ppc_ori(c, 6, 6, 1);
            ppc_lwz(c, 5, OFF_SQTAG, R_CTX);
            ppc_cmplw(c, 0, 5, 6);
            br[0] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            ppc_lwz(c, 5, OFF_SQPA, R_CTX);
            ppc_mr(c, 4, 3);
            ppc_rlwinm(c, 3, 3, 0, 22, 26);
            ppc_or(c, 3, 3, 5);
            call_c(c, (const void *)sh4_sq_flush_to);
            ppc_patch_here(c, notSq);
            add_cold_mmu(g, br, 1, COLD_MMU_PREF, 4, 0);
            g->wrote = 1;
            return 1;
        }
        if (is_op(op, 0xF0FF, 0x0083))
        {
            u32 notSq, notRam, done;
            fwd_clear(g);
            ppc_mr(c, 3, get(g, n));
            ppc_rlwinm(c, 4, 3, 6, 26, 31);
            ppc_cmplwi(c, 0, 4, 0x38);
            notSq = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            call_c(c, (const void *)sh4_sq_flush);
            done = ppc_b_fwd(c);
            ppc_patch_here(c, notSq);
            notRam = ram_check(g);
            ppc_rlwinm(c, 4, 3, 0, 8, 26);
            ppc_load_ptr(c, 5, OFF_JRAM, R_CTX);
            ppc_dcbt(c, 5, 4);
            ppc_patch_here(c, notRam);
            ppc_patch_here(c, done);
            g->wrote = 1;
            return 1;
        }
        if (is_op(op, 0xF0FF, 0x0093) || is_op(op, 0xF0FF, 0x00A3) || is_op(op, 0xF0FF, 0x00B3)) return 1;
        if (is_op(op, 0xF0FF, 0x00C3))
        {
            ppc_mr(c, 3, get(g, n));
            emit_store(g, get(g, 0), 4);
            return 1;
        }
        if (is_op(op, 0xF0FF, 0x0029)) { ppc_mr(c, def(g, n), getT(g)); return 1; }
        if (is_op(op, 0xF0FF, 0x001A))
        {
            if (is_op((u16)g->prev, 0xF00F, 0x0007) || is_op((u16)g->prev, 0xF00E, 0x200E)) ppc_mr(c, def(g, n), 3);
            else ppc_lwz(c, def(g, n), OFF_MACL, R_CTX);
            return 1;
        }
        if (is_op(op, 0xF0FF, 0x000A)) { ppc_lwz(c, def(g, n), OFF_MACH, R_CTX); return 1; }
        if (is_op(op, 0xF0FF, 0x002A)) { ppc_lwz(c, def(g, n), OFF_PR, R_CTX); return 1; }
        if (is_op(op, 0xF0FF, 0x0012)) { ppc_lwz(c, def(g, n), OFF_GBR, R_CTX); return 1; }
        if (is_op(op, 0xF0FF, 0x0002))
        {
            u32 t = getT(g);
            ppc_lwz(c, 3, OFF_SR, R_CTX);
            ppc_or(c, def(g, n), 3, t);
            return 1;
        }
        if (is_op(op, 0xF00F, 0x0007))
        {
            ppc_mullw(c, 3, get(g, n), get(g, m));
            ppc_stw(c, 3, OFF_MACL, R_CTX);
            return 1;
        }
        if (is_op(op, 0xF00C, 0x0004))
        {
            int size = 1 << (op & 3);
            ppc_add(c, 3, get(g, n), get(g, 0));
            sq_hint(g, n);
            emit_store(g, get(g, m), size);
            return 1;
        }
        if (is_op(op, 0xF00C, 0x000C))
        {
            int size = 1 << (op & 3);
            ppc_add(c, 3, get(g, m), get(g, 0));
            emit_load(g, def(g, n), size);
            return 1;
        }
        return 0;
    case 0x1:
        ppc_addi(c, 3, get(g, n), (op & 15) * 4);
        sq_hint(g, n);
        oc_hint(g, n);
        emit_store(g, get(g, m), 4);
        fwd_add(g, n, (op & 15) * 4, m);
        return 1;
    case 0x2:
        switch (op & 15)
        {
        case 0x0: case 0x1: case 0x2:
            ppc_mr(c, 3, get(g, n));
            sq_hint(g, n);
            oc_hint(g, n);
            emit_store(g, get(g, m), 1 << (op & 3));
            if ((op & 15) == 2) fwd_add(g, n, 0, m);
            return 1;
        case 0x4: case 0x5: case 0x6:
        {
            int size = (op & 15) == 4 ? 1 : (op & 15) == 5 ? 2 : 4;
            ppc_mr(c, 7, get(g, m));
            if (g->info->mmu)
            {
                ppc_addi(c, 3, get(g, n), -size);
                oc_hint(g, n);
                emit_store(g, 7, size);
                ppc_addi(c, rw(g, n), H(n), -size);
                return 1;
            }
            ppc_addi(c, rw(g, n), H(n), -size);
            ppc_mr(c, 3, H(n));
            oc_hint(g, n);
            emit_store(g, 7, size);
            return 1;
        }
        case 0x7:
            ppc_lwz(c, 5, OFF_SR, R_CTX);
            ppc_rlwinm(c, 3, get(g, n), 1, 31, 31);
            ppc_rlwinm(c, 4, get(g, m), 1, 31, 31);
            ppc_rlwimi(c, 5, 3, 8, 23, 23);
            ppc_rlwimi(c, 5, 4, 9, 22, 22);
            ppc_stw(c, 5, OFF_SR, R_CTX);
            ppc_xor(c, defT(g), 3, 4);
            return 1;
        case 0x8: ppc_and(c, 5, get(g, n), get(g, m)); t_is_zero(g, 5); return 1;
        case 0x9: rn = rw(g, n); ppc_and(c, rn, rn, get(g, m)); return 1;
        case 0xA: rn = rw(g, n); ppc_xor(c, rn, rn, get(g, m)); return 1;
        case 0xB: rn = rw(g, n); ppc_or(c, rn, rn, get(g, m)); return 1;
        case 0xD:
            ppc_rlwinm(c, 3, get(g, n), 16, 16, 31);
            ppc_rlwimi(c, 3, get(g, m), 16, 0, 15);
            ppc_mr(c, def(g, n), 3);
            return 1;
        case 0xE:
            ppc_rlwinm(c, 3, get(g, n), 0, 16, 31);
            ppc_rlwinm(c, 4, get(g, m), 0, 16, 31);
            ppc_mullw(c, 3, 3, 4);
            ppc_stw(c, 3, OFF_MACL, R_CTX);
            return 1;
        case 0xF:
            ppc_extsh(c, 3, get(g, n));
            ppc_extsh(c, 4, get(g, m));
            ppc_mullw(c, 3, 3, 4);
            ppc_stw(c, 3, OFF_MACL, R_CTX);
            return 1;
        }
        return 0;
    case 0x3:
        switch (op & 15)
        {
        case 0x0: ppc_xor(c, 5, get(g, n), get(g, m)); t_is_zero(g, 5); return 1;
        case 0x2: t_less(g, get(g, n), get(g, m), 0); ppc_xori(c, R_T, R_T, 1); return 1;
        case 0x3: t_less(g, get(g, n), get(g, m), 1); ppc_xori(c, R_T, R_T, 1); return 1;
        case 0x6: t_less(g, get(g, m), get(g, n), 0); return 1;
        case 0x7: t_less(g, get(g, m), get(g, n), 1); return 1;
        case 0x4: emit_div1(g, n, m); return 1;
        case 0x8: rn = rw(g, n); ppc_subf(c, rn, get(g, m), rn); return 1;
        case 0xA: emit_addc_subc(g, n, m, 1); return 1;
        case 0xC: rn = rw(g, n); ppc_add(c, rn, rn, get(g, m)); return 1;
        case 0xE: emit_addc_subc(g, n, m, 0); return 1;
        }
        return 0;
    case 0x4:
        if ((op & 15) == 0xC || (op & 15) == 0xD)
        {
            emit_shift(g, n, m, (op & 15) == 0xC);
            return 1;
        }
        switch (op & 0xFF)
        {
        case 0x24:
        {
            u32 t = getT(g);
            rn = rw(g, n);
            ppc_rlwinm(c, 3, rn, 1, 31, 31);
            ppc_rlwinm(c, rn, rn, 1, 0, 30);
            ppc_or(c, rn, rn, t);
            ppc_mr(c, defT(g), 3);
            return 1;
        }
        case 0x25:
        {
            u32 t = getT(g);
            rn = rw(g, n);
            ppc_rlwinm(c, 3, rn, 0, 31, 31);
            ppc_rlwinm(c, 4, t, 31, 0, 0);
            ppc_rlwinm(c, rn, rn, 31, 1, 31);
            ppc_or(c, rn, rn, 4);
            ppc_mr(c, defT(g), 3);
            return 1;
        }
        case 0x02: emit_push_sys(g, n, OFF_MACH); return 1;
        case 0x12: emit_push_sys(g, n, OFF_MACL); return 1;
        case 0x22: emit_push_sys(g, n, OFF_PR); return 1;
        case 0x06: emit_pop_sys(g, n, OFF_MACH); return 1;
        case 0x16: emit_pop_sys(g, n, OFF_MACL); return 1;
        case 0x26: emit_pop_sys(g, n, OFF_PR); return 1;
        case 0x13: emit_push_sys(g, n, OFF_GBR); return 1;
        case 0x17: emit_pop_sys(g, n, OFF_GBR); return 1;
        case 0x00: case 0x20:
            rn = rw(g, n);
            ppc_rlwinm(c, defT(g), rn, 1, 31, 31);
            ppc_rlwinm(c, rn, rn, 1, 0, 30);
            return 1;
        case 0x01:
            rn = rw(g, n);
            ppc_rlwinm(c, defT(g), rn, 0, 31, 31);
            ppc_rlwinm(c, rn, rn, 31, 1, 31);
            return 1;
        case 0x21:
            rn = rw(g, n);
            ppc_rlwinm(c, defT(g), rn, 0, 31, 31);
            ppc_extsw(c, rn, rn);
            ppc_srawi(c, rn, rn, 1);
            return 1;
        case 0x04:
            rn = rw(g, n);
            ppc_rlwinm(c, rn, rn, 1, 0, 31);
            ppc_rlwinm(c, defT(g), rn, 0, 31, 31);
            return 1;
        case 0x05:
            rn = rw(g, n);
            ppc_rlwinm(c, defT(g), rn, 0, 31, 31);
            ppc_rlwinm(c, rn, rn, 31, 0, 31);
            return 1;
        case 0x08: rn = rw(g, n); ppc_rlwinm(c, rn, rn, 2, 0, 29); return 1;
        case 0x09: rn = rw(g, n); ppc_rlwinm(c, rn, rn, 30, 2, 31); return 1;
        case 0x18: rn = rw(g, n); ppc_rlwinm(c, rn, rn, 8, 0, 23); return 1;
        case 0x19: rn = rw(g, n); ppc_rlwinm(c, rn, rn, 24, 8, 31); return 1;
        case 0x28: rn = rw(g, n); ppc_rlwinm(c, rn, rn, 16, 0, 15); return 1;
        case 0x29: rn = rw(g, n); ppc_rlwinm(c, rn, rn, 16, 16, 31); return 1;
        case 0x10:
            rn = rw(g, n);
            ppc_addi(c, rn, rn, -1);
            t_is_zero(g, rn);
            return 1;
        case 0x11:
            ppc_rlwinm(c, 3, get(g, n), 1, 31, 31);
            ppc_xori(c, defT(g), 3, 1);
            return 1;
        case 0x15:
            ppc_li(c, 6, 0);
            t_less(g, 6, get(g, n), 1);
            return 1;
        case 0x1A: ppc_stw(c, get(g, n), OFF_MACL, R_CTX); return 1;
        case 0x0A: ppc_stw(c, get(g, n), OFF_MACH, R_CTX); return 1;
        case 0x2A: ppc_stw(c, get(g, n), OFF_PR, R_CTX); return 1;
        case 0x1E: ppc_stw(c, get(g, n), OFF_GBR, R_CTX); return 1;
        }
        return 0;
    case 0x5:
        ppc_addi(c, 3, get(g, m), (op & 15) * 4);
        oc_hint(g, m);
        if (!fwd_load(g, m, (op & 15) * 4, n)) emit_load(g, def(g, n), 4);
        return 1;
    case 0x6:
        switch (op & 15)
        {
        case 0x0: case 0x1: case 0x2:
            ppc_mr(c, 3, get(g, m));
            oc_hint(g, m);
            if ((op & 15) != 2 || !fwd_load(g, m, 0, n)) emit_load(g, def(g, n), 1 << (op & 3));
            return 1;
        case 0x3: rm = get(g, m); ppc_mr(c, def(g, n), rm); return 1;
        case 0x4: case 0x5: case 0x6:
        {
            int size = (op & 15) == 4 ? 1 : (op & 15) == 5 ? 2 : 4;
            if (n != m)
            {
                u32 ra = get(g, m);
                oc_hint(g, m);
                emit_load_from(g, def(g, n), size, ra);
                ppc_addi(c, rw(g, m), H(m), size);
                return 1;
            }
            ppc_mr(c, 3, get(g, m));
            oc_hint(g, m);
            emit_load(g, 7, size);
            ppc_mr(c, def(g, n), 7);
            return 1;
        }
        case 0x7: rm = get(g, m); ppc_nor(c, def(g, n), rm, rm); return 1;
        case 0x8:
            rm = get(g, m);
            ppc_rlwinm(c, 3, rm, 8, 16, 23);
            ppc_rlwimi(c, 3, rm, 24, 24, 31);
            ppc_rlwimi(c, 3, rm, 0, 0, 15);
            ppc_mr(c, def(g, n), 3);
            return 1;
        case 0x9: rm = get(g, m); ppc_rlwinm(c, def(g, n), rm, 16, 0, 31); return 1;
        case 0xA:
        {
            u32 t = getT(g);
            ppc_clrldi(c, 3, get(g, m), 32);
            ppc_neg(c, 3, 3);
            ppc_subf(c, 3, t, 3);
            ppc_srdi(c, defT(g), 3, 63);
            ppc_mr(c, def(g, n), 3);
            return 1;
        }
        case 0xB: rm = get(g, m); ppc_neg(c, def(g, n), rm); return 1;
        case 0xC: rm = get(g, m); ppc_rlwinm(c, def(g, n), rm, 0, 24, 31); return 1;
        case 0xD: rm = get(g, m); ppc_rlwinm(c, def(g, n), rm, 0, 16, 31); return 1;
        case 0xE: rm = get(g, m); ppc_extsb(c, def(g, n), rm); return 1;
        case 0xF: rm = get(g, m); ppc_extsh(c, def(g, n), rm); return 1;
        }
        return 0;
    case 0x7: rn = rw(g, n); ppc_addi(c, rn, rn, imm8); return 1;
    case 0x8:
        switch ((op >> 8) & 15)
        {
        case 0x0: ppc_addi(c, 3, get(g, m), op & 15); emit_store(g, get(g, 0), 1); return 1;
        case 0x1: ppc_addi(c, 3, get(g, m), (op & 15) * 2); emit_store(g, get(g, 0), 2); return 1;
        case 0x4: ppc_addi(c, 3, get(g, m), op & 15); emit_load(g, def(g, 0), 1); return 1;
        case 0x5: ppc_addi(c, 3, get(g, m), (op & 15) * 2); emit_load(g, def(g, 0), 2); return 1;
        case 0x8:
            ppc_li32(c, 5, (u32)imm8);
            ppc_xor(c, 5, get(g, 0), 5);
            t_is_zero(g, 5);
            return 1;
        }
        return 0;
    case 0x9: emit_literal(g, n, g->pc + 4 + (op & 0xFF) * 2, 2); return 1;
    case 0xC:
        switch ((op >> 8) & 15)
        {
        case 0x0: case 0x1: case 0x2:
        {
            int size = 1 << ((op >> 8) & 3);
            ppc_lwz(c, 3, OFF_GBR, R_CTX);
            ppc_addi(c, 3, 3, (op & 0xFF) * size);
            emit_store(g, get(g, 0), size);
            return 1;
        }
        case 0x4: case 0x5: case 0x6:
        {
            int size = 1 << ((op >> 8) & 3);
            ppc_lwz(c, 3, OFF_GBR, R_CTX);
            ppc_addi(c, 3, 3, (op & 0xFF) * size);
            emit_load(g, def(g, 0), size);
            return 1;
        }
        case 0x7: def_const(g, 0, ((g->pc + 4) & ~3u) + (op & 0xFF) * 4); return 1;
        case 0x8: emit_andi(c, 5, get(g, 0), op & 0xFF); t_is_zero(g, 5); return 1;
        case 0x9: rn = rw(g, 0); emit_andi(c, rn, rn, op & 0xFF); return 1;
        case 0xA: rn = rw(g, 0); ppc_xori(c, rn, rn, op & 0xFF); return 1;
        case 0xB: rn = rw(g, 0); ppc_ori(c, rn, rn, op & 0xFF); return 1;
        }
        return 0;
    case 0xD: emit_literal(g, n, ((g->pc + 4) & ~3u) + (op & 0xFF) * 4, 4); return 1;
    case 0xE: def_const(g, n, (u32)imm8); return 1;
    }
    return 0;
}

static void cycles_add(Gen *g, s32 delta)
{
    ppc_addi(g->c, R_CYC, R_CYC, delta);
}

static void next_op(Gen *g, u16 op)
{
    if (!is_op(op, 0xF00F, 0x3004) && !is_op(op, 0xF0FF, 0x4024)) g->srInR5 = 0;
    if (is_op(op, 0xF0FF, 0x405A) || is_op(op, 0xF0FF, 0x4056) || is_op(op, 0xF0FF, 0xF03D) || is_op(op, 0xF0FF, 0xF0BD))
        g->fpulFr = -1;
}

int sh4_jit_is_fpu_op(u16 op)
{
    u32 lo = op & 0xF0FFu;
    if ((op >> 12) == 0xF) return op != 0xFFFD;
    return lo == 0x405A || lo == 0x406A || lo == 0x4056 || lo == 0x4066 || lo == 0x005A || lo == 0x006A || lo == 0x4052 ||
           lo == 0x4062;
}

static void fd_check(Gen *g, u16 op)
{
    PpcCode *c = g->c;
    u32 br;
    if (!g->info->mmu || g->fdOk || !sh4_jit_is_fpu_op(op)) return;
    ppc_lwz(c, 3, OFF_SR, R_CTX);
    ppc_rlwinm(c, 3, 3, 0, 16, 16);
    ppc_cmpwi(c, 0, 3, 0);
    br = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    add_cold_mmu(g, &br, 1, COLD_MMU_FPUDIS, 4, g->inSlot ? 1 : 0);
    g->fdOk = 1;
}

static void op_done(Gen *g, u16 op)
{
    g->prev = op;
}

static void emit_slot(Gen *g, u32 addr)
{
    u16 op = code_at(g, addr);
    u32 savedPc = g->pc;
    g->pc = addr;
    g->inSlot = 1;
    g->prev = 0;
    next_op(g, op);
    fd_check(g, op);
    mark_write(g, op);
    if (!emit_native(g, op))
    {
        call_handler(g, op);
        track_fpscr(g, op);
    }
    op_done(g, op);
    g->inSlot = 0;
    g->pc = savedPc;
}

static int slot_may_write_reg(u16 op, u32 x)
{
    if (is_op(op, 0xF0FF, 0x400E) || is_op(op, 0xF0FF, 0x4007)) return 1;
    return x == 0 || x == ((op >> 8) & 15u) || x == ((op >> 4) & 15u);
}

static int slot_keeps_t(u16 op)
{
    u32 lo = op & 15, mid = (op >> 8) & 15;
    switch (op >> 12)
    {
    case 0x1: case 0x5: case 0x7: case 0x9: case 0xD: case 0xE: return 1;
    case 0x0: return op == 0x0009 || (lo != 0x8 && lo != 0x9 && lo != 0xB);
    case 0x2: return lo != 0x7 && lo != 0x8 && lo != 0xC;
    case 0x3: return lo == 0x5 || lo == 0x8 || lo == 0xC || lo == 0xD;
    case 0x4:
        switch (op & 0xFF)
        {
        case 0x08: case 0x09: case 0x18: case 0x19: case 0x28: case 0x29:
        case 0x0A: case 0x1A: case 0x2A: case 0x5A: case 0x6A:
        case 0x06: case 0x16: case 0x26: case 0x56: case 0x66:
        case 0x02: case 0x12: case 0x22: case 0x52: case 0x62:
            return 1;
        }
        return lo == 0xC || lo == 0xD;
    case 0x6: return lo != 0xA;
    case 0x8: return mid != 0x8;
    case 0xC: return mid != 0x8 && mid != 0xC;
    case 0xF: return lo != 0x4 && lo != 0x5;
    }
    return 0;
}

static s32 disp8(u16 op) { return (s32)(s8)(op & 0xFF) * 2; }
static s32 disp12(u16 op)
{
    s32 d = op & 0xFFF;
    if (d & 0x800) d -= 0x1000;
    return d * 2;
}

static void exit_or_loop(Gen *g, u32 target)
{
    PpcCode *c = g->c;
    const Sh4JitGenInfo *info = g->info;
    u32 n, dirty = 0, dirtyFr = 0;
    if (!info->tier2 || (info->pure && !info->busy) || !info->link || target != g->start || g->fp != info->fpMode)
    {
        exit_to(g, target);
        return;
    }
    reload(g);
    for (n = 0; n < 16; n++)
    {
        if (g->reg[n] == RS_DIRTY) dirty |= 1u << n;
        if (g->fr[n] == RS_DIRTY) dirtyFr |= 1u << n;
    }
    if (g->t == RS_DIRTY) dirty |= 1u << 16;
    if (info->out)
    {
        info->out->loops++;
        info->out->loopDirty |= dirty;
        info->out->loopDirtyFr |= dirtyFr;
    }
    ppc_b_to(c, c->buf);
}

static void emit_skip(Gen *g, u16 op, u32 index)
{
    PpcCode *c = g->c;
    u32 top = op >> 8, slot = g->pc + 2, n;
    s32 back = (s32)(op & 0xFF);
    if (top == 0x89 || top == 0x8B)
        ppc_cmpwi(c, 0, getT(g), 0);
    else if (slot_keeps_t(code_at(g, slot)))
    {
        emit_slot(g, slot);
        ppc_cmpwi(c, 0, getT(g), 0);
    }
    else
    {
        ppc_stw(c, getT(g), OFF_TEMP(0), R_CTX);
        emit_slot(g, slot);
        ppc_lwz(c, 3, OFF_TEMP(0), R_CTX);
        ppc_cmpwi(c, 0, 3, 0);
    }
    if (back) cycles_add(g, back);
    memcpy(g->skip.reg, g->reg, sizeof(g->reg));
    memcpy(g->skip.fr, g->fr, sizeof(g->fr));
    g->skip.t = g->t;
    g->skip.known = g->known;
    for (n = 0; n < 16; n++) g->skip.knownVal[n] = g->knownVal[n];
    g->skip.fpulFr = g->fpulFr;
    g->skip.marked = g->marked;
    g->skip.fdOk = g->fdOk;
    g->skip.index = index;
    g->skip.at = ppc_bc_fwd(c, (top == 0x89 || top == 0x8D) ? 4 : 12, 0, PPC_EQ);
    g->skip.active = 1;
    if (back) cycles_add(g, -back);
}

static void skip_join(Gen *g)
{
    PpcCode *c = g->c;
    u32 n;
    for (n = 0; n < 16; n++)
    {
        u8 s0 = g->skip.reg[n], s1 = g->reg[n];
        if (s0 == RS_NONE)
        {
            if (s1 == RS_DIRTY) ppc_stw(c, H(n), OFF_R(n), R_CTX);
            g->reg[n] = RS_NONE;
        }
        else
        {
            if (s1 == RS_NONE) ppc_lwz(c, H(n), OFF_R(n), R_CTX);
            g->reg[n] = s0 == RS_DIRTY || s1 == RS_DIRTY ? RS_DIRTY : RS_CLEAN;
        }
        if ((g->known & (1u << n)) && (!(g->skip.known & (1u << n)) || g->knownVal[n] != g->skip.knownVal[n]))
            g->known &= ~(1u << n);
    }
    for (n = 0; n < 16; n++)
    {
        u8 s0 = g->skip.fr[n], s1 = g->fr[n];
        if (s0 == RS_NONE)
        {
            if (s1 == RS_DIRTY) ppc_stfs(c, FH(n), OFF_FR(n), R_CTX);
            g->fr[n] = RS_NONE;
        }
        else
        {
            if (s1 == RS_NONE) ppc_lfs(c, FH(n), OFF_FR(n), R_CTX);
            g->fr[n] = s0 == RS_DIRTY || s1 == RS_DIRTY ? RS_DIRTY : RS_CLEAN;
        }
    }
    if (g->skip.t == RS_NONE)
    {
        if (g->t == RS_DIRTY) ppc_stw(c, R_T, OFF_T, R_CTX);
        g->t = RS_NONE;
    }
    else
    {
        if (g->t == RS_NONE) ppc_lwz(c, R_T, OFF_T, R_CTX);
        g->t = g->skip.t == RS_DIRTY || g->t == RS_DIRTY ? RS_DIRTY : RS_CLEAN;
    }
    ppc_patch_here(c, g->skip.at);
    if (g->fpulFr != g->skip.fpulFr) g->fpulFr = -1;
    g->marked = g->marked && g->skip.marked;
    g->fdOk = g->fdOk && g->skip.fdOk;
    g->prev = 0;
    g->srInR5 = 0;
    g->sqHint = 0;
    g->ocHint = 0;
    fwd_clear(g);
    g->skip.active = 0;
}

static void emit_side_exit(Gen *g, u16 op, s32 rest)
{
    PpcCode *c = g->c;
    u32 top = op >> 8, target = g->pc + 4 + disp8(op), slot = g->pc + 2, notTaken;
    if (top == 0x89 || top == 0x8B)
    {
        ppc_cmpwi(c, 0, getT(g), 0);
        if (add_side_exit(g, target, rest - 1))
        {
            add_side_cold(g, ppc_bc_fwd(c, top == 0x89 ? 4 : 12, 0, PPC_EQ));
            return;
        }
        notTaken = ppc_bc_fwd(c, top == 0x89 ? 12 : 4, 0, PPC_EQ);
        cycles_add(g, rest - 1);
        exit_to(g, target);
        ppc_patch_here(c, notTaken);
        return;
    }
    if (slot_keeps_t(code_at(g, slot)))
    {
        emit_slot(g, slot);
        ppc_cmpwi(c, 0, getT(g), 0);
    }
    else
    {
        ppc_stw(c, getT(g), OFF_TEMP(0), R_CTX);
        emit_slot(g, slot);
        ppc_lwz(c, 3, OFF_TEMP(0), R_CTX);
        ppc_cmpwi(c, 0, 3, 0);
    }
    if (add_side_exit(g, target, rest))
    {
        add_side_cold(g, ppc_bc_fwd(c, top == 0x8D ? 4 : 12, 0, PPC_EQ));
        return;
    }
    notTaken = ppc_bc_fwd(c, top == 0x8D ? 12 : 4, 0, PPC_EQ);
    cycles_add(g, rest);
    exit_to(g, target);
    ppc_patch_here(c, notTaken);
}

int g_sh4_jit_count_calls;
u32 g_sh4_jit_call_stats[16];

static void count_bump(Gen *g, int k)
{
    PpcCode *c = g->c;
    if (!g_sh4_jit_count_calls) return;
    ppc_li32(c, 12, (u32)(size_t)&g_sh4_jit_call_stats[k]);
    ppc_lwz(c, 4, 0, 12);
    ppc_addi(c, 4, 4, 1);
    ppc_stw(c, 4, 0, 12);
}

u32 g_sh4_jit_probe[16];

static void probe_add(Gen *g, int k, u32 n)
{
    PpcCode *c = g->c;
    if (!g_sh4_jit_count_calls || !n) return;
    ppc_li32(c, 12, (u32)(size_t)&g_sh4_jit_probe[k]);
    ppc_lwz(c, 4, 0, 12);
    ppc_addi(c, 4, 4, (s32)n);
    ppc_stw(c, 4, 0, 12);
}

static void probe_op(Gen *g, u16 op)
{
    if (!g_sh4_jit_count_calls) return;
    if (g->fp == FP_UNKNOWN) probe_add(g, 0, 1);
    if ((op >> 12) == 0xF)
    {
        if (g->fp == FP_UNKNOWN) probe_add(g, 1, 1);
        else if (g->fp & FPSCR_PR) probe_add(g, 2, 1);
        if ((op & 0xF3FF) == 0xF1FD) probe_add(g, 6, 1);
        if ((op & 0xF0FF) == 0xF0ED) probe_add(g, 7, 1);
    }
    if ((op & 0xF0FF) == 0x406A || (op & 0xF0FF) == 0x4066) probe_add(g, 3, 1);
    if ((op & 0xF00F) == 0x3004) probe_add(g, 5, 1);
}

static u32 leaf_size(Gen *g, u32 target)
{
    u32 i;
    if ((target & 1) || (g->info->mmu && ((target ^ g->start) & ~0xFFFu))) return 0;
    for (i = 0; i < 64; i++)
    {
        u16 op = code_at(g, target + i * 2), top = op >> 8;
        if (op == 0x000B) return i + 2;
        if (top == 0x89 || top == 0x8B || top == 0x8D || top == 0x8F)
        {
            if ((s8)(op & 0xFF) < 0) return 0;
            continue;
        }
        if ((op >> 12) == 0xA)
        {
            if (op & 0x800) return 0;
            continue;
        }
        if ((op >> 12) == 0xB || (op & 0xF0FF) == 0x0003 || (op & 0xF0FF) == 0x0023 || (op & 0xF0FF) == 0x400B ||
            (op & 0xF0FF) == 0x402B || op == 0x002B || op == 0x001B || (op & 0xFF00) == 0xC300 ||
            (op & 0xF0FF) == 0x4022 || (op & 0xF0FF) == 0x402A || (op & 0xF0FF) == 0x4026)
            return 0;
    }
    return 0;
}

static void count_call(Gen *g, u32 target, int known, int reg)
{
    u32 n;
    if (!g_sh4_jit_count_calls) return;
    count_bump(g, 1);
    if (!known) { count_bump(g, 5); count_bump(g, 8 + (reg >= 0 ? g->litWhy[reg] : 0)); return; }
    n = leaf_size(g, target);
    if (n && n <= 16) count_bump(g, 2);
    else if (n && n <= 32) count_bump(g, 3);
}

static int emit_branch(Gen *g, u16 op)
{
    PpcCode *c = g->c;
    u32 pc = g->pc, next = pc + 2, slot = pc + 2, after = pc + 4, top = op >> 8, at;
    if (top == 0x89 || top == 0x8B)
    {
        u32 target = pc + 4 + disp8(op);
        ppc_cmpwi(c, 0, getT(g), 0);
        at = ppc_bc_fwd(c, top == 0x89 ? 4 : 12, 0, PPC_EQ);
        exit_to(g, next);
        ppc_patch_here(c, at);
        cycles_add(g, -1);
        exit_or_loop(g, target);
        return 1;
    }
    if (top == 0x8D || top == 0x8F)
    {
        u32 target = pc + 4 + disp8(op);
        if (slot_keeps_t(code_at(g, slot)))
        {
            emit_slot(g, slot);
            ppc_cmpwi(c, 0, getT(g), 0);
        }
        else
        {
            ppc_stw(c, getT(g), OFF_TEMP(0), R_CTX);
            emit_slot(g, slot);
            ppc_lwz(c, 3, OFF_TEMP(0), R_CTX);
            ppc_cmpwi(c, 0, 3, 0);
        }
        at = ppc_bc_fwd(c, top == 0x8D ? 4 : 12, 0, PPC_EQ);
        exit_to(g, after);
        ppc_patch_here(c, at);
        exit_or_loop(g, target);
        return 1;
    }
    if ((op >> 12) == 0xA || (op >> 12) == 0xB)
    {
        if ((op >> 12) == 0xB)
        {
            count_call(g, pc + 4 + disp12(op), 1, -1);
            ppc_li32(c, 3, after);
            ppc_stw(c, 3, OFF_PR, R_CTX);
        }
        emit_slot(g, slot);
        exit_or_loop(g, pc + 4 + disp12(op));
        return 1;
    }
    if ((op & 0xF0FF) == 0x0023 || (op & 0xF0FF) == 0x0003 ||
        (op & 0xF0FF) == 0x402B || (op & 0xF0FF) == 0x400B || op == 0x000B)
    {
        u16 slotOp = code_at(g, slot);
        if (op == 0x000B) count_bump(g, 4);
        else if ((op & 0xF0FF) == 0x0003 || (op & 0xF0FF) == 0x400B)
            count_call(g, (g->known >> N & 1) ? g->knownVal[N] + ((op & 0xF000) == 0x4000 ? 0 : after) : 0, g->known >> N & 1, (int)N);
        if (op != 0x000B && (g->known >> N & 1))
        {
            u32 target = g->knownVal[N] + ((op & 0xF000) == 0x4000 ? 0 : after);
            if ((op & 0xF0FF) == 0x0003 || (op & 0xF0FF) == 0x400B)
            {
                ppc_li32(c, 3, after);
                ppc_stw(c, 3, OFF_PR, R_CTX);
            }
            emit_slot(g, slot);
            exit_to(g, target);
            return 1;
        }
        int late = op == 0x000B ? !is_op(slotOp, 0xF0FF, 0x402A) && !is_op(slotOp, 0xF0FF, 0x4026)
                                : !slot_may_write_reg(slotOp, N);
        if (!late)
        {
            if (op == 0x000B) ppc_lwz(c, 3, OFF_PR, R_CTX);
            else if ((op & 0xF000) == 0x4000) ppc_mr(c, 3, get(g, N));
            else
            {
                ppc_li32(c, 3, after);
                ppc_add(c, 3, 3, get(g, N));
            }
            ppc_stw(c, 3, OFF_TEMP(1), R_CTX);
        }
        if ((op & 0xF0FF) == 0x0003 || (op & 0xF0FF) == 0x400B)
        {
            ppc_li32(c, 3, after);
            ppc_stw(c, 3, OFF_PR, R_CTX);
        }
        emit_slot(g, slot);
        if (!late) ppc_lwz(c, 3, OFF_TEMP(1), R_CTX);
        else if (op == 0x000B) ppc_lwz(c, 3, OFF_PR, R_CTX);
        else if ((op & 0xF000) == 0x4000)
        {
            exit_to_reg(g, get(g, N));
            return 1;
        }
        else
        {
            ppc_li32(c, 3, after);
            ppc_add(c, 3, 3, get(g, N));
        }
        exit_to_reg(g, 3);
        return 1;
    }
    return 0;
}

int sh4_jit_has_delay_slot(u16 op)
{
    u32 top = op >> 8;
    return top == 0x8D || top == 0x8F || (op >> 12) == 0xA || (op >> 12) == 0xB || (op & 0xF0FF) == 0x0023 ||
           (op & 0xF0FF) == 0x0003 || (op & 0xF0FF) == 0x402B || (op & 0xF0FF) == 0x400B || op == 0x000B ||
           op == 0x002B;
}

u32 sh4_jit_gen_block(PpcCode *c, u32 pc, const u32 *ops, u32 n, int endsWithBranch, const Sh4JitGenInfo *info)
{
    Gen g;
    u32 i;
    s32 cycles = (s32)n;
    memset(&g, 0, sizeof(g));
    memset(g.reg, RS_CLEAN, sizeof(g.reg));
    memset(g.fr, RS_CLEAN, sizeof(g.fr));
    g.t = RS_CLEAN;
    g.fpulFr = -1;
    for (i = 0; i < 16; i++)
    {
        if (info->initDirty & (1u << i)) g.reg[i] = RS_DIRTY;
        if (info->initDirtyFr & (1u << i)) g.fr[i] = RS_DIRTY;
    }
    if (info->initDirty & (1u << 16)) g.t = RS_DIRTY;
    g.c = c;
    g.info = info;
    g.coldPos = info->coldPos;
    g.coldCap = info->coldCap;
    g.fp = info->fpMode;
    g.start = pc;
    g.firstSub = (info->mmu ? info->physStart : pc & (RAM_SIZE - 1)) >> 9;
    g.lastSub = ((info->mmu ? info->physStart : pc & (RAM_SIZE - 1)) + n * 2 + 1) >> 9;
    if (endsWithBranch && sh4_jit_has_delay_slot((u16)ops[n - 1]) && ops[n - 1] != 0x002B) cycles++;
    if (info->libPoll)
    {
        ppc_li32(c, 3, pc);
        call_c(c, (const void *)sh4_jit_lib_poll);
    }
    {
        u32 bail;
        ppc_cmpwi(c, 0, R_CYC, 0);
        bail = ppc_bc_fwd(c, 4, 0, PPC_GT);
        ppc_addi(c, R_CYC, R_CYC, -cycles);
        add_cold(&g, bail, COLD_BAIL, 0, 0);
    }
    count_bump(&g, 0);
    if (g_sh4_jit_mark_blocks)
    {
        ppc_li32(c, 3, pc);
        ppc_stw(c, 3, OFF_PC, R_CTX);
    }
    for (i = 0; i < n; i++)
    {
        u16 op = (u16)ops[i];
        int side = i + 1 < n || !endsWithBranch ? sh4_jit_side_exit(op) : 0;
        u32 skipTo;
        if (g.skip.active && i == g.skip.index) skip_join(&g);
        g.pc = pc + i * 2;
        probe_op(&g, op);
        next_op(&g, op);
        mark_write(&g, op);
        if (side && !g.skip.active && (skipTo = sh4_jit_skip_target(ops, i, n)) != 0)
        {
            emit_skip(&g, op, skipTo);
            op_done(&g, op);
            i += side - 1;
            continue;
        }
        if (side)
        {
            emit_side_exit(&g, op, cycles - (s32)(i + side));
            op_done(&g, op);
            i += side - 1;
            continue;
        }
        if (i == n - 1 && endsWithBranch)
        {
            if (!emit_branch(&g, op))
            {
                if (info->mmu && mem_op(op)) emit_mmu_op(&g, op);
                else call_handler(&g, op);
                emit_epilogue(&g);
            }
            emit_cold(&g);
            return 1;
        }
        fd_check(&g, op);
        if (!emit_native(&g, op))
        {
            if ((op & 0xF0FF) == 0x400E)
            {
                emit_ldc_sr(&g, op, cycles - (s32)(i + 1));
                g.fdOk = 0;
            }
            else if (info->mmu && mem_op(op))
                emit_mmu_op(&g, op);
            else
            {
                call_handler(&g, op);
                track_fpscr(&g, op);
            }
        }
        op_done(&g, op);
    }
    exit_to(&g, pc + n * 2);
    emit_cold(&g);
    return 1;
}
