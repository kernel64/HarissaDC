// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "arm7_jit_internal.h"

#include <stddef.h>
#include <string.h>

#include "arm7.h"
#include "../mem/mem.h"
#include "../sh4/ppc_call.h"

#define FRAME 288
#define SAVE_SLOT(r) (FRAME - 16 - 8 * (s32)(31 - (r)))
#define OFF_R(n) ((s32)offsetof(Arm7Ctx, r) + 4 * (s32)(n))
#define OFF_CPSR ((s32)offsetof(Arm7Ctx, cpsr))
#define OFF_CYCLES ((s32)offsetof(Arm7Ctx, cycles))
#define OFF_WROTE ((s32)offsetof(Arm7Ctx, jitWrote))
#define OFF_BACK ((s32)offsetof(Arm7Ctx, jitBack))
#define OFF_TEMP(i) ((s32)offsetof(Arm7Ctx, jitTemp) + 4 * (s32)(i))
#define MAX_INSNS 64
enum { R_PSR = 29, R_ADDR = 30, R_CTX = 31 };
enum { RS_NONE = 0, RS_CLEAN = 1, RS_DIRTY = 2 };

#define BIT_N 31
#define BIT_Z 30
#define BIT_C 29
#define BIT_V 28

struct Gen
{
    PpcCode *c;
    u8 reg[15];
    u8 psr;
    u32 pc;
    u32 cycles;
    Arm7JitGenInfo *info;
};

static u32 H(u32 n) { return 14 + n; }

static u32 get(Gen *g, u32 n)
{
    if (g->reg[n] == RS_NONE)
    {
        ppc_lwz(g->c, H(n), OFF_R(n), R_CTX);
        g->reg[n] = RS_CLEAN;
    }
    return H(n);
}

static u32 def(Gen *g, u32 n)
{
    g->reg[n] = RS_DIRTY;
    return H(n);
}

static u32 getP(Gen *g)
{
    if (g->psr == RS_NONE)
    {
        ppc_lwz(g->c, R_PSR, OFF_CPSR, R_CTX);
        g->psr = RS_CLEAN;
    }
    return R_PSR;
}

static u32 rwP(Gen *g)
{
    getP(g);
    g->psr = RS_DIRTY;
    return R_PSR;
}

static u32 src(Gen *g, u32 n, u32 tmp)
{
    if (n == 15)
    {
        ppc_li32(g->c, tmp, g->pc + 8);
        return tmp;
    }
    return get(g, n);
}

static void writeback(Gen *g)
{
    u32 n;
    for (n = 0; n < 15; n++)
        if (g->reg[n] == RS_DIRTY)
        {
            ppc_stw(g->c, H(n), OFF_R(n), R_CTX);
            g->reg[n] = RS_CLEAN;
        }
    if (g->psr == RS_DIRTY)
    {
        ppc_stw(g->c, R_PSR, OFF_CPSR, R_CTX);
        g->psr = RS_CLEAN;
    }
}

static void forget(Gen *g)
{
    memset(g->reg, RS_NONE, sizeof(g->reg));
    g->psr = RS_NONE;
}

int arm7_jit_gen_trampoline(PpcCode *c, u32 **enter, u32 **exit)
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
    ppc_load_ptr_const(c, R_CTX, ppc_addr_of(&g_arm7));
    ppc_mtctr(c, 3);
    ppc_bctr(c);
    *exit = c->buf + c->pos;
    for (r = 14; r < 32; r++) ppc_ld(c, r, SAVE_SLOT(r), 1);
    ppc_ld(c, 0, FRAME - 8, 1);
    ppc_mtlr(c, 0);
    ppc_addi(c, 1, 1, FRAME);
    ppc_blr(c);
    return c->overflow ? -1 : 0;
}

static void pay(Gen *g)
{
    PpcCode *c = g->c;
    if (!g->cycles) return;
    ppc_lwz(c, 4, OFF_CYCLES, R_CTX);
    ppc_addi(c, 4, 4, -(s32)g->cycles);
    ppc_stw(c, 4, OFF_CYCLES, R_CTX);
}

static void exit_const(Gen *g, u32 target, int back)
{
    PpcCode *c = g->c;
    u8 regs[15], psr = g->psr;
    memcpy(regs, g->reg, sizeof(regs));
    writeback(g);
    pay(g);
    ppc_li32(c, 3, target);
    ppc_stw(c, 3, OFF_R(15), R_CTX);
    if (back)
    {
        ppc_li(c, 3, 1);
        ppc_stw(c, 3, OFF_BACK, R_CTX);
    }
    ppc_b_to(c, g->info->exit);
    memcpy(g->reg, regs, sizeof(regs));
    g->psr = psr;
}

static void exit_r3(Gen *g)
{
    PpcCode *c = g->c;
    ppc_rlwinm(c, 3, 3, 0, 0, 29);
    ppc_stw(c, 3, OFF_R(15), R_CTX);
    writeback(g);
    pay(g);
    ppc_b_to(c, g->info->exit);
}

static void exit_ctx(Gen *g)
{
    writeback(g);
    pay(g);
    ppc_b_to(g->c, g->info->exit);
}

static void call_interp(Gen *g, u32 op)
{
    PpcCode *c = g->c;
    writeback(g);
    ppc_li32(c, 3, g->pc + 4);
    ppc_stw(c, 3, OFF_R(15), R_CTX);
    ppc_li32(c, 3, op);
    ppc_call(c, (const void *)arm7_interp_op);
    forget(g);
    g->info->fallbacks++;
}

static void flag(Gen *g, u32 rt, u32 k)
{
    ppc_rlwinm(g->c, rt, getP(g), (32 - k) & 31, 31, 31);
}

static void set_flag(Gen *g, u32 r, u32 k)
{
    u32 b = 31 - k;
    ppc_rlwimi(g->c, rwP(g), r, k, b, b);
}

static void set_flag_const(Gen *g, u32 k, u32 v)
{
    PpcCode *c = g->c;
    u32 p = rwP(g), b = 31 - k;
    if (v) ppc_oris(c, p, p, (1u << k) >> 16);
    else ppc_rlwinm(c, p, p, 0, (b + 1) & 31, (b + 31) & 31);
}

static void set_nz(Gen *g, u32 r)
{
    PpcCode *c = g->c;
    u32 p = rwP(g);
    ppc_rlwimi(c, p, r, 0, 0, 0);
    ppc_put(c, ppc_x(r, 3, 0, 26, 0));
    ppc_rlwinm(c, 3, 3, 27, 31, 31);
    set_flag(g, 3, BIT_Z);
}

static int cond_skip(Gen *g, u32 cond)
{
    PpcCode *c = g->c;
    if (cond == 14) return -1;
    if (cond == 15) return (int)ppc_b_fwd(c);
    switch (cond >> 1)
    {
    case 0: flag(g, 3, BIT_Z); break;
    case 1: flag(g, 3, BIT_C); break;
    case 2: flag(g, 3, BIT_N); break;
    case 3: flag(g, 3, BIT_V); break;
    case 4:
        flag(g, 3, BIT_C);
        flag(g, 4, BIT_Z);
        ppc_andc(c, 3, 3, 4);
        break;
    case 5:
        flag(g, 3, BIT_N);
        flag(g, 4, BIT_V);
        ppc_xor(c, 3, 3, 4);
        ppc_xori(c, 3, 3, 1);
        break;
    default:
        flag(g, 3, BIT_N);
        flag(g, 4, BIT_V);
        ppc_xor(c, 3, 3, 4);
        ppc_xori(c, 3, 3, 1);
        flag(g, 4, BIT_Z);
        ppc_andc(c, 3, 3, 4);
        break;
    }
    ppc_cmpwi(c, 0, 3, 0);
    return (int)ppc_bc_fwd(c, (cond & 1) ? 4 : 12, 0, PPC_EQ);
}

static void emit_load(Gen *g, int size)
{
    PpcCode *c = g->c;
    u32 slow, slow2 = 0, done;
    ppc_rlwinm(c, 4, 3, 0, 8, 8);
    ppc_cmpwi(c, 0, 4, 0);
    slow = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    if (size == 4)
    {
        ppc_andi_(c, 4, 3, 3);
        slow2 = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    }
    ppc_rlwinm(c, 4, 3, 0, 11, size == 4 ? 29 : size == 2 ? 30 : 31);
    if (size == 2) ppc_xori(c, 4, 4, 2);
    else if (size == 1) ppc_xori(c, 4, 4, 3);
    ppc_load_ptr_const(c, 5, ppc_addr_of(g_aram));
    if (size == 4) ppc_lwzx(c, 3, 5, 4);
    else if (size == 2) ppc_lhzx(c, 3, 5, 4);
    else ppc_lbzx(c, 3, 5, 4);
    done = ppc_b_fwd(c);
    ppc_patch_here(c, slow);
    if (size == 4) ppc_patch_here(c, slow2);
    ppc_call(c, size == 4 ? (const void *)arm7_jit_read32 : size == 2 ? (const void *)arm7_jit_read16 : (const void *)arm7_jit_read8);
    ppc_patch_here(c, done);
}

static void emit_store(Gen *g, u32 rv, int size)
{
    PpcCode *c = g->c;
    u32 sp = get(g, 13), slow, slow2, same, near, done;
    ppc_rlwinm(c, 4, 3, 0, 8, 8);
    ppc_cmpwi(c, 0, 4, 0);
    slow = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    ppc_rlwinm(c, 4, 3, 0, 11, size == 4 ? 29 : size == 2 ? 30 : 31);
    ppc_rlwinm(c, 6, 4, 22, 21, 31);
    ppc_load_ptr_const(c, 5, ppc_addr_of(g->info->codePages));
    ppc_lbzx(c, 6, 5, 6);
    ppc_cmpwi(c, 0, 6, 0);
    slow2 = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    if (size == 2) ppc_xori(c, 4, 4, 2);
    else if (size == 1) ppc_xori(c, 4, 4, 3);
    ppc_load_ptr_const(c, 5, ppc_addr_of(g_aram));
    if (size == 4)
    {
        ppc_lwzx(c, 6, 5, 4);
        ppc_cmpw(c, 0, 6, rv);
    }
    else
    {
        if (size == 2) ppc_lhzx(c, 6, 5, 4);
        else ppc_lbzx(c, 6, 5, 4);
        ppc_rlwinm(c, 7, rv, 0, size == 2 ? 16 : 24, 31);
        ppc_cmpw(c, 0, 6, 7);
    }
    same = ppc_bc_fwd(c, 12, 0, PPC_EQ);
    ppc_rlwinm(c, 7, 3, 0, 8, 31);
    ppc_subf(c, 7, sp, 7);
    ppc_addi(c, 7, 7, 512);
    ppc_rlwinm(c, 7, 7, 0, 0, 31);
    ppc_cmplwi(c, 0, 7, 1024);
    near = ppc_bc_fwd(c, 12, 0, PPC_LT);
    ppc_li(c, 7, 1);
    ppc_stw(c, 7, OFF_WROTE, R_CTX);
    ppc_patch_here(c, same);
    ppc_patch_here(c, near);
    if (size == 4) ppc_stwx(c, rv, 5, 4);
    else if (size == 2) ppc_sthx(c, rv, 5, 4);
    else ppc_stbx(c, rv, 5, 4);
    done = ppc_b_fwd(c);
    ppc_patch_here(c, slow);
    ppc_patch_here(c, slow2);
    ppc_mr(c, 4, rv);
    ppc_mr(c, 5, sp);
    ppc_call(c, size == 4 ? (const void *)arm7_jit_write32 : size == 2 ? (const void *)arm7_jit_write16 : (const void *)arm7_jit_write8);
    ppc_patch_here(c, done);
}

static u32 shifted(Gen *g, u32 op, int wantCarry, int *carryKind)
{
    PpcCode *c = g->c;
    u32 rm = src(g, op & 15, 5), type = (op >> 5) & 3, sh = (op >> 7) & 31;
    *carryKind = 0;
    switch (type)
    {
    case 0:
        if (sh == 0) return rm;
        if (wantCarry) { ppc_rlwinm(c, 4, rm, sh, 31, 31); *carryKind = 1; }
        ppc_rlwinm(c, 7, rm, sh, 0, 31 - sh);
        return 7;
    case 1:
        if (sh == 0)
        {
            if (wantCarry) { ppc_rlwinm(c, 4, rm, 1, 31, 31); *carryKind = 1; }
            ppc_li(c, 7, 0);
            return 7;
        }
        if (wantCarry) { ppc_rlwinm(c, 4, rm, (33 - sh) & 31, 31, 31); *carryKind = 1; }
        ppc_rlwinm(c, 7, rm, 32 - sh, sh, 31);
        return 7;
    case 2:
        if (wantCarry)
        {
            ppc_rlwinm(c, 4, rm, sh ? (33 - sh) & 31 : 1, 31, 31);
            *carryKind = 1;
        }
        ppc_srawi(c, 7, rm, sh ? sh : 31);
        return 7;
    default:
        if (sh == 0)
        {
            flag(g, 6, BIT_C);
            if (wantCarry) { ppc_rlwinm(c, 4, rm, 0, 31, 31); *carryKind = 1; }
            ppc_rlwinm(c, 7, rm, 31, 1, 31);
            ppc_rlwimi(c, 7, 6, 31, 0, 0);
            return 7;
        }
        if (wantCarry) { ppc_rlwinm(c, 4, rm, (33 - sh) & 31, 31, 31); *carryKind = 1; }
        ppc_rlwinm(c, 7, rm, 32 - sh, 0, 31);
        return 7;
    }
}

static int emit_dp(Gen *g, u32 op)
{
    PpcCode *c = g->c;
    u32 opc = (op >> 21) & 15, s = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    int logical = opc <= 1 || (opc >= 8 && opc <= 9) || opc >= 12, writes = opc < 8 || opc >= 12;
    int carryKind = 0, constCarry = -1;
    u32 b, a = 0;
    if (!(op & (1u << 25)) && (op & 0x10)) return 0;
    if (rd == 15 && (s || !writes)) return 0;
    if (opc >= 8 && opc <= 11 && !s) return 0;
    if (op & (1u << 25))
    {
        u32 imm = op & 0xFF, rot = ((op >> 8) & 15) * 2;
        if (rot) imm = (imm >> rot) | (imm << (32 - rot));
        if (rot && s && logical) constCarry = (int)(imm >> 31);
        ppc_li32(c, 7, imm);
        b = 7;
    }
    else
        b = shifted(g, op, s && logical, &carryKind);
    if (opc != 13 && opc != 15) a = src(g, rn, 6);
    switch (opc)
    {
    case 0x0: case 0x8: ppc_and(c, 8, a, b); break;
    case 0x1: case 0x9: ppc_xor(c, 8, a, b); break;
    case 0xC: ppc_or(c, 8, a, b); break;
    case 0xD: ppc_mr(c, 8, b); break;
    case 0xE: ppc_andc(c, 8, a, b); break;
    case 0xF: ppc_nor(c, 8, b, b); break;
    case 0x2: case 0xA:
    case 0x3:
    case 0x6: case 0x7:
    {
        u32 x = opc == 3 || opc == 7 ? b : a, y = opc == 3 || opc == 7 ? a : b;
        int carryIn = opc == 6 || opc == 7;
        if (!s && !carryIn)
        {
            ppc_subf(c, 8, y, x);
            break;
        }
        ppc_clrldi(c, 9, x, 32);
        ppc_clrldi(c, 10, y, 32);
        ppc_subf(c, 8, 10, 9);
        if (carryIn)
        {
            flag(g, 11, BIT_C);
            ppc_addi(c, 8, 8, -1);
            ppc_add(c, 8, 8, 11);
        }
        if (s)
        {
            ppc_srdi(c, 4, 8, 63);
            ppc_xori(c, 4, 4, 1);
            ppc_xor(c, 11, x, y);
            ppc_xor(c, 12, x, 8);
            ppc_and(c, 12, 12, 11);
            ppc_rlwinm(c, 5, 12, 1, 31, 31);
        }
        break;
    }
    default:
    {
        int carryIn = opc == 5;
        if (!s)
        {
            ppc_add(c, 8, a, b);
            if (carryIn)
            {
                flag(g, 11, BIT_C);
                ppc_add(c, 8, 8, 11);
            }
            break;
        }
        ppc_clrldi(c, 9, a, 32);
        ppc_clrldi(c, 10, b, 32);
        ppc_add(c, 8, 9, 10);
        if (carryIn)
        {
            flag(g, 11, BIT_C);
            ppc_add(c, 8, 8, 11);
        }
        ppc_srdi(c, 4, 8, 32);
        ppc_xor(c, 11, a, b);
        ppc_xor(c, 12, a, 8);
        ppc_andc(c, 12, 12, 11);
        ppc_rlwinm(c, 5, 12, 1, 31, 31);
        break;
    }
    }
    if (s)
    {
        set_nz(g, 8);
        if (!logical)
        {
            set_flag(g, 4, BIT_C);
            set_flag(g, 5, BIT_V);
        }
        else if (carryKind) set_flag(g, 4, BIT_C);
        else if (constCarry >= 0) set_flag_const(g, BIT_C, (u32)constCarry);
    }
    g->cycles += 1;
    if (!writes) return 1;
    if (rd == 15)
    {
        g->cycles += 2;
        ppc_mr(c, 3, 8);
        exit_r3(g);
        return 2;
    }
    ppc_mr(c, def(g, rd), 8);
    return 1;
}

static u32 transfer_offset(Gen *g, u32 op, int halfword)
{
    int carryKind;
    if (halfword)
    {
        if (op & (1u << 22))
        {
            ppc_li(g->c, 7, (s32)(((op >> 4) & 0xF0) | (op & 15)));
            return 7;
        }
        return src(g, op & 15, 7);
    }
    if (!(op & (1u << 25)))
    {
        ppc_li(g->c, 7, (s32)(op & 0xFFF));
        return 7;
    }
    return shifted(g, op, 0, &carryKind);
}

static int emit_transfer(Gen *g, u32 op, int halfword)
{
    PpcCode *c = g->c;
    u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, base, off;
    int pre = (op >> 24) & 1, up = (op >> 23) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
    int size, sign = 0, writesBase = !pre || wb;
    if (halfword)
    {
        u32 sh = (op >> 5) & 3;
        if (!load && sh != 1) return 0;
        size = sh == 2 ? 1 : 2;
        sign = sh != 1;
    }
    else
        size = (op & (1u << 22)) ? 1 : 4;
    if (writesBase && rn == 15) return 0;
    base = src(g, rn, 6);
    off = transfer_offset(g, op, halfword);
    if (up) ppc_add(c, R_ADDR, base, off);
    else ppc_subf(c, R_ADDR, off, base);
    ppc_mr(c, 3, pre ? R_ADDR : base);
    if (load)
    {
        emit_load(g, size);
        if (sign) size == 1 ? ppc_extsb(c, 3, 3) : ppc_extsh(c, 3, 3);
        if (writesBase) ppc_mr(c, def(g, rn), R_ADDR);
        g->cycles += 3;
        if (rd == 15)
        {
            g->cycles += 2;
            exit_r3(g);
            return 2;
        }
        ppc_mr(c, def(g, rd), 3);
        return 1;
    }
    {
        u32 v = rd == 15 ? 8 : get(g, rd);
        if (rd == 15) ppc_li32(c, 8, g->pc + 12);
        emit_store(g, v, size);
        if (writesBase) ppc_mr(c, def(g, rn), R_ADDR);
        g->cycles += 2;
    }
    return 1;
}

static int emit_block_transfer(Gen *g, u32 op)
{
    PpcCode *c = g->c;
    u32 rn = (op >> 16) & 15, list = op & 0xFFFF, count = 0, i, base;
    int pre = (op >> 24) & 1, up = (op >> 23) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
    s32 start;
    if ((op & (1u << 22)) || rn == 15 || !list) return 0;
    for (i = 0; i < 16; i++) count += (list >> i) & 1;
    start = up ? 0 : -(s32)count * 4;
    if (pre == up) start += 4;
    base = get(g, rn);
    ppc_addi(c, 3, base, up ? (s32)count * 4 : -(s32)count * 4);
    ppc_stw(c, 3, OFF_TEMP(0), R_CTX);
    ppc_addi(c, R_ADDR, base, start);
    for (i = 0; i < 16; i++)
    {
        if (!(list & (1u << i))) continue;
        ppc_mr(c, 3, R_ADDR);
        if (load)
        {
            emit_load(g, 4);
            if (i == 15) ppc_stw(c, 3, OFF_TEMP(1), R_CTX);
            else ppc_mr(c, def(g, i), 3);
        }
        else
        {
            u32 v = i == 15 ? 8 : get(g, i);
            if (i == 15) ppc_li32(c, 8, g->pc + 12);
            emit_store(g, v, 4);
        }
        ppc_addi(c, R_ADDR, R_ADDR, 4);
    }
    if (wb && !(load && (list & (1u << rn))))
        ppc_lwz(c, def(g, rn), OFF_TEMP(0), R_CTX);
    g->cycles += count + (load ? 1 : 0);
    if (load && (list & 0x8000))
    {
        g->cycles += 2;
        ppc_lwz(c, 3, OFF_TEMP(1), R_CTX);
        exit_r3(g);
        return 2;
    }
    return 1;
}

static int emit_insn(Gen *g, u32 op)
{
    PpcCode *c = g->c;
    u32 cond = op >> 28, group = (op >> 25) & 7;
    int skip, r = -1, conditional = cond != 14;
    u8 before[15], beforePsr;
    if (group == 5)
    {
        s32 offset = (s32)(op << 8) >> 6;
        u32 target = g->pc + 8 + (u32)offset;
        int link = (op >> 24) & 1;
        u8 lr = g->reg[14];
        if (conditional) writeback(g);
        lr = g->reg[14];
        skip = cond_skip(g, cond);
        g->cycles += 3;
        if (link) ppc_li32(c, def(g, 14), g->pc + 4);
        exit_const(g, target, !link && offset < 0);
        if (skip < 0) return 0;
        ppc_patch_here(c, (u32)skip);
        g->reg[14] = lr;
        g->cycles -= 2;
        return 1;
    }
    if (conditional)
    {
        writeback(g);
        memcpy(before, g->reg, sizeof(before));
        beforePsr = g->psr;
    }
    skip = cond_skip(g, cond);
    if (group == 0 || group == 1)
    {
        if (group == 0 && ((op & 0x0FC000F0) == 0x00000090))
        {
            u32 rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
            if (rd != 15 && rm != 15 && rs != 15 && rn != 15)
            {
                ppc_mullw(c, 8, get(g, rm), get(g, rs));
                if (op & (1u << 21)) ppc_add(c, 8, 8, get(g, rn));
                if (op & (1u << 20)) set_nz(g, 8);
                ppc_mr(c, def(g, rd), 8);
                g->cycles += 3;
                r = 1;
            }
        }
        else if (group == 0 && (op & 0x0E000090) == 0x00000090 && (op & 0x0F8000F0) != 0x00800090 &&
                 (op & 0x0FB00FF0) != 0x01000090)
            r = emit_transfer(g, op, 1);
        else if (group == 0 && ((op & 0x0F8000F0) == 0x00800090 || (op & 0x0FB00FF0) == 0x01000090))
            r = 0;
        else if ((op & 0x0F900000) == 0x01000000 || (op & 0x0FB00000) == 0x03200000)
            r = 0;
        else
            r = emit_dp(g, op);
    }
    else if (group == 2 || group == 3)
    {
        if (group == 3 && (op & 0x10)) r = 0;
        else r = emit_transfer(g, op, 0);
    }
    else if (group == 4)
        r = emit_block_transfer(g, op);
    else
        r = 0;
    if (r == 0)
    {
        u32 rd = (op >> 12) & 15;
        int ends;
        if (skip >= 0)
        {
            ppc_patch_here(c, (u32)skip);
            skip = -1;
        }
        call_interp(g, op);
        ends = group >= 5 || ((group == 0 || group == 1) && (rd == 15 || (op & 0x0F900000) == 0x01000000 ||
                                                             (op & 0x0FB00000) == 0x03200000)) ||
               (group == 4 && (op & 0x8000)) || (group <= 3 && rd == 15) || (group == 3 && (op & 0x10));
        if (ends)
        {
            exit_ctx(g);
            return 0;
        }
        return 1;
    }
    if (r == 2)
    {
        if (skip < 0) return 0;
        ppc_patch_here(c, (u32)skip);
        memcpy(g->reg, before, sizeof(before));
        g->psr = beforePsr;
        return 1;
    }
    if (skip >= 0)
    {
        u32 n;
        writeback(g);
        ppc_patch_here(c, (u32)skip);
        for (n = 0; n < 15; n++)
            if (!(before[n] == RS_CLEAN && g->reg[n] == RS_CLEAN)) g->reg[n] = RS_NONE;
        if (!(beforePsr == RS_CLEAN && g->psr == RS_CLEAN)) g->psr = RS_NONE;
    }
    return 1;
}

u32 arm7_jit_gen_block(PpcCode *c, u32 pc, Arm7JitGenInfo *info)
{
    Gen g;
    u32 n;
    memset(&g, 0, sizeof(g));
    g.c = c;
    g.info = info;
    for (n = 0; n < MAX_INSNS; n++)
    {
        u32 op = buf_r32(g_aram, (pc + n * 4) & (ARAM_SIZE - 1) & ~3u);
        g.pc = pc + n * 4;
        if (!emit_insn(&g, op)) return pc + (n + 1) * 4;
        if (c->overflow) return 0;
    }
    g.pc = pc + n * 4;
    exit_const(&g, g.pc, 0);
    return c->overflow ? 0 : g.pc;
}
