// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "arm7.h"

#include <string.h>

#include "aica.h"
#include "arm7_jit.h"
#include "../core/log.h"
#include "../core/state.h"
#include "../core/sched.h"
#include "../mem/mem.h"

Arm7Ctx g_arm7;

#define A g_arm7
#define PC A.r[15]

#define MODE_USR 0x10
#define MODE_FIQ 0x11
#define MODE_IRQ 0x12
#define MODE_SVC 0x13
#define MODE_ABT 0x17
#define MODE_UND 0x1B
#define MODE_SYS 0x1F

#define FLAG_N 0x80000000u
#define FLAG_Z 0x40000000u
#define FLAG_C 0x20000000u
#define FLAG_V 0x10000000u
#define FLAG_I 0x00000080u
#define FLAG_F 0x00000040u

static u32 s_logged;

#define IDLE_WINDOW 1024
#define IDLE_NAP 4096
static u32 s_idlePc = 0xFFFFFFFFu, s_idleRegs[17], s_idleBranches;
static int s_wrote, s_asleep;
static s32 s_napCycles;
static u32 s_idleSkips;
void (*g_arm7_step_hook)(u32 pc);

static __inline void note_write(u32 addr, u32 sp)
{
    if (((addr & 0x00FFFFFFu) - (sp - 512)) >= 1024u) s_wrote = 1;
}

static __inline void code_check(u32 off)
{
    if (g_arm7_jit_code && g_arm7_jit_code[off >> 10]) arm7_jit_code_written(off);
}

static void idle_branch(u32 target)
{
    if (target == s_idlePc && !s_wrote && A.cpsr == s_idleRegs[16] && memcmp(s_idleRegs, A.r, sizeof(A.r)) == 0)
    {
        A.cycles = 0;
        s_asleep = 1;
        s_napCycles = 0;
        s_idleSkips++;
        return;
    }
    if (s_idlePc != 0xFFFFFFFFu && !s_wrote && ++s_idleBranches < IDLE_WINDOW) return;
    s_idlePc = target;
    s_idleBranches = 0;
    memcpy(s_idleRegs, A.r, sizeof(A.r));
    s_idleRegs[16] = A.cpsr;
    s_wrote = 0;
}

static u32 read32(u32 addr)
{
    u32 v;
    addr &= 0x00FFFFFFu;
    if (addr < 0x800000u) v = buf_r32(g_aram, addr & (ARAM_SIZE - 1) & ~3u);
    else if (addr < 0x810000u) v = aica_read(0x00700000u + (addr & 0xFFFC), 4);
    else v = 0;
    if (addr & 3) v = (v >> ((addr & 3) * 8)) | (v << (32 - (addr & 3) * 8));
    return v;
}

static u32 read16(u32 addr)
{
    addr &= 0x00FFFFFFu;
    if (addr < 0x800000u) return buf_r16(g_aram, addr & (ARAM_SIZE - 1) & ~1u);
    if (addr < 0x810000u) return aica_read(0x00700000u + (addr & 0xFFFE), 2) & 0xFFFF;
    return 0;
}

static u32 read8(u32 addr)
{
    addr &= 0x00FFFFFFu;
    if (addr < 0x800000u) return buf_r8(g_aram, addr & (ARAM_SIZE - 1));
    if (addr < 0x810000u) return aica_read(0x00700000u + (addr & 0xFFFF), 1) & 0xFF;
    return 0;
}

static void write32_sp(u32 addr, u32 v, u32 sp)
{
    addr &= 0x00FFFFFCu;
    if (addr < 0x800000u)
    {
        if (buf_r32(g_aram, addr & (ARAM_SIZE - 1)) != v) note_write(addr, sp);
        buf_w32(g_aram, addr & (ARAM_SIZE - 1), v);
        code_check(addr & (ARAM_SIZE - 1));
    }
    else if (addr < 0x810000u)
    {
        note_write(addr, sp);
        aica_write(0x00700000u + (addr & 0xFFFF), v, 4);
    }
}

static void write32(u32 addr, u32 v)
{
    write32_sp(addr, v, A.r[13]);
}

static void write16_sp(u32 addr, u32 v, u32 sp)
{
    addr &= 0x00FFFFFEu;
    if (addr < 0x800000u)
    {
        if (buf_r16(g_aram, addr & (ARAM_SIZE - 1)) != (u16)v) note_write(addr, sp);
        buf_w16(g_aram, addr & (ARAM_SIZE - 1), (u16)v);
        code_check(addr & (ARAM_SIZE - 1));
    }
    else if (addr < 0x810000u)
    {
        note_write(addr, sp);
        aica_write(0x00700000u + (addr & 0xFFFF), v & 0xFFFF, 2);
    }
}

static void write16(u32 addr, u32 v)
{
    write16_sp(addr, v, A.r[13]);
}

static void write8_sp(u32 addr, u32 v, u32 sp)
{
    addr &= 0x00FFFFFFu;
    if (addr < 0x800000u)
    {
        if (buf_r8(g_aram, addr & (ARAM_SIZE - 1)) != (u8)v) note_write(addr, sp);
        buf_w8(g_aram, addr & (ARAM_SIZE - 1), (u8)v);
        code_check(addr & (ARAM_SIZE - 1));
    }
    else if (addr < 0x810000u)
    {
        note_write(addr, sp);
        aica_write(0x00700000u + (addr & 0xFFFF), v & 0xFF, 1);
    }
}

static void write8(u32 addr, u32 v)
{
    write8_sp(addr, v, A.r[13]);
}

static u32 *bank13(u32 mode)
{
    switch (mode)
    {
    case MODE_IRQ: return A.irq;
    case MODE_SVC: return A.svc;
    case MODE_ABT: return A.abt;
    case MODE_UND: return A.und;
    default: return &A.usr[5];
    }
}

static u32 *spsr_slot(u32 mode)
{
    switch (mode)
    {
    case MODE_FIQ: return &A.spsrFiq;
    case MODE_IRQ: return &A.spsrIrq;
    case MODE_SVC: return &A.spsrSvc;
    case MODE_ABT: return &A.spsrAbt;
    case MODE_UND: return &A.spsrUnd;
    default: return NULL;
    }
}

static void switch_mode(u32 newMode)
{
    u32 old = A.cpsr & 0x1F, *b, *s;
    int i;
    if (old == newMode) return;
    if (old == MODE_FIQ)
        for (i = 0; i < 7; i++) A.fiq[i] = A.r[8 + i];
    else
    {
        for (i = 0; i < 5; i++) A.usr[i] = A.r[8 + i];
        b = bank13(old);
        b[0] = A.r[13];
        b[1] = A.r[14];
    }
    s = spsr_slot(old);
    if (s) *s = A.spsr;
    if (newMode == MODE_FIQ)
        for (i = 0; i < 7; i++) A.r[8 + i] = A.fiq[i];
    else
    {
        for (i = 0; i < 5; i++) A.r[8 + i] = A.usr[i];
        b = bank13(newMode);
        A.r[13] = b[0];
        A.r[14] = b[1];
    }
    s = spsr_slot(newMode);
    A.spsr = s ? *s : A.cpsr;
    A.cpsr = (A.cpsr & ~0x1Fu) | newMode;
}

static void set_cpsr(u32 v)
{
    switch_mode(v & 0x1F);
    A.cpsr = v;
}

static void exception(u32 mode, u32 vector, u32 returnAddr, u32 disableFiq)
{
    u32 old = A.cpsr;
    switch_mode(mode);
    A.spsr = old;
    A.r[14] = returnAddr;
    A.cpsr |= FLAG_I | (disableFiq ? FLAG_F : 0);
    PC = vector;
}

void arm7_reset(void)
{
    int fiq = A.fiqLine, enabled = A.enabled;
    arm7_jit_flush();
    memset(&A, 0, sizeof(A));
    A.cpsr = MODE_SVC | FLAG_I | FLAG_F;
    A.fiqLine = fiq;
    A.enabled = enabled;
    PC = 0;
    s_idlePc = 0xFFFFFFFFu;
    s_asleep = 0;
}

void arm7_set_enabled(int on)
{
    if (on && !A.enabled)
    {
        arm7_reset();
        LOG_INFO("[arm7] released from reset (field %llu)", (unsigned long long)(sched_now() / 3333333));
    }
    A.enabled = on;
}

void arm7_set_fiq(int asserted)
{
    A.fiqLine = asserted;
}

static __inline u32 reg(u32 n)
{
    return n == 15 ? PC + 4 : A.r[n];
}

static int condition(u32 op)
{
    u32 f = A.cpsr;
    int n = (f & FLAG_N) != 0, z = (f & FLAG_Z) != 0, c = (f & FLAG_C) != 0, v = (f & FLAG_V) != 0;
    switch (op >> 28)
    {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    case 0xE: return 1;
    default: return 0;
    }
}

static __inline void set_nz(u32 v)
{
    A.cpsr = (A.cpsr & ~(FLAG_N | FLAG_Z)) | (v & FLAG_N) | (v ? 0 : FLAG_Z);
}

static __inline void set_c(u32 c) { A.cpsr = c ? A.cpsr | FLAG_C : A.cpsr & ~FLAG_C; }
static __inline void set_v(u32 v) { A.cpsr = v ? A.cpsr | FLAG_V : A.cpsr & ~FLAG_V; }

static u32 shift(u32 v, u32 type, u32 amount, int byReg, u32 *carry)
{
    u32 c = (A.cpsr & FLAG_C) ? 1 : 0;
    if (byReg)
    {
        amount &= 0xFF;
        if (amount == 0) { *carry = c; return v; }
    }
    switch (type)
    {
    case 0:
        if (amount == 0) { *carry = c; return v; }
        if (amount < 32) { *carry = (v >> (32 - amount)) & 1; return v << amount; }
        *carry = amount == 32 ? v & 1 : 0;
        return 0;
    case 1:
        if (amount == 0 && !byReg) amount = 32;
        if (amount < 32) { *carry = (v >> (amount - 1)) & 1; return v >> amount; }
        *carry = amount == 32 ? v >> 31 : 0;
        return 0;
    case 2:
        if (amount == 0 && !byReg) amount = 32;
        if (amount < 32) { *carry = (v >> (amount - 1)) & 1; return (u32)((s32)v >> amount); }
        *carry = v >> 31;
        return (s32)v < 0 ? 0xFFFFFFFFu : 0;
    default:
        if (amount == 0 && !byReg) { *carry = v & 1; return (c << 31) | (v >> 1); }
        amount &= 31;
        if (amount == 0) { *carry = v >> 31; return v; }
        *carry = (v >> (amount - 1)) & 1;
        return (v >> amount) | (v << (32 - amount));
    }
}

static u32 operand2(u32 op, u32 *carry)
{
    if (op & (1u << 25))
    {
        u32 imm = op & 0xFF, rot = ((op >> 8) & 15) * 2;
        if (rot == 0) { *carry = (A.cpsr & FLAG_C) ? 1 : 0; return imm; }
        imm = (imm >> rot) | (imm << (32 - rot));
        *carry = imm >> 31;
        return imm;
    }
    if (op & 0x10)
    {
        u32 rm = (op & 15) == 15 ? PC + 8 : A.r[op & 15];
        return shift(rm, (op >> 5) & 3, A.r[(op >> 8) & 15], 1, carry);
    }
    return shift(reg(op & 15), (op >> 5) & 3, (op >> 7) & 31, 0, carry);
}

static void data_processing(u32 op)
{
    u32 opcode = (op >> 21) & 15, s = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    u32 carry, b = operand2(op, &carry), a = reg(rn), r = 0, c = (A.cpsr & FLAG_C) ? 1 : 0;
    int write = 1, logical = 0;
    u64 wide;
    switch (opcode)
    {
    case 0x0: r = a & b; logical = 1; break;
    case 0x1: r = a ^ b; logical = 1; break;
    case 0x2: r = a - b; if (s) { set_c(a >= b); set_v(((a ^ b) & (a ^ r)) >> 31); } break;
    case 0x3: r = b - a; if (s) { set_c(b >= a); set_v(((b ^ a) & (b ^ r)) >> 31); } break;
    case 0x4: wide = (u64)a + b; r = (u32)wide; if (s) { set_c((u32)(wide >> 32)); set_v((~(a ^ b) & (a ^ r)) >> 31); } break;
    case 0x5: wide = (u64)a + b + c; r = (u32)wide; if (s) { set_c((u32)(wide >> 32)); set_v((~(a ^ b) & (a ^ r)) >> 31); } break;
    case 0x6: wide = (u64)a - b - (1 - c); r = (u32)wide; if (s) { set_c((u64)a >= (u64)b + (1 - c)); set_v(((a ^ b) & (a ^ r)) >> 31); } break;
    case 0x7: wide = (u64)b - a - (1 - c); r = (u32)wide; if (s) { set_c((u64)b >= (u64)a + (1 - c)); set_v(((b ^ a) & (b ^ r)) >> 31); } break;
    case 0x8: r = a & b; logical = 1; write = 0; break;
    case 0x9: r = a ^ b; logical = 1; write = 0; break;
    case 0xA: r = a - b; write = 0; set_c(a >= b); set_v(((a ^ b) & (a ^ r)) >> 31); break;
    case 0xB: wide = (u64)a + b; r = (u32)wide; write = 0; set_c((u32)(wide >> 32)); set_v((~(a ^ b) & (a ^ r)) >> 31); break;
    case 0xC: r = a | b; logical = 1; break;
    case 0xD: r = b; logical = 1; break;
    case 0xE: r = a & ~b; logical = 1; break;
    default: r = ~b; logical = 1; break;
    }
    if (s || !write)
    {
        set_nz(r);
        if (logical) set_c(carry);
    }
    if (write)
    {
        A.r[rd] = r;
        if (rd == 15)
        {
            PC &= ~3u;
            if (s && (A.cpsr & 0x1F) != MODE_USR && (A.cpsr & 0x1F) != MODE_SYS) set_cpsr(A.spsr);
        }
    }
}

static void psr_transfer(u32 op)
{
    int useSpsr = (op >> 22) & 1;
    if ((op & 0x0FBF0FFF) == 0x010F0000)
    {
        A.r[(op >> 12) & 15] = useSpsr ? A.spsr : A.cpsr;
        return;
    }
    {
        u32 value, mask = 0, carry;
        if (op & (1u << 25)) value = operand2(op, &carry);
        else value = A.r[op & 15];
        if (op & (1u << 16)) mask |= 0x000000FFu;
        if (op & (1u << 17)) mask |= 0x0000FF00u;
        if (op & (1u << 18)) mask |= 0x00FF0000u;
        if (op & (1u << 19)) mask |= 0xFF000000u;
        if (useSpsr) A.spsr = (A.spsr & ~mask) | (value & mask);
        else
        {
            if ((A.cpsr & 0x1F) == MODE_USR) mask &= 0xFF000000u;
            set_cpsr((A.cpsr & ~mask) | (value & mask));
        }
    }
}

static void multiply(u32 op)
{
    u32 rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
    if ((op & 0x0F8000F0) == 0x00800090)
    {
        u64 r;
        if (op & (1u << 22)) r = (u64)((s64)(s32)A.r[rm] * (s64)(s32)A.r[rs]);
        else r = (u64)A.r[rm] * A.r[rs];
        if (op & (1u << 21)) r += ((u64)A.r[rd] << 32) | A.r[rn];
        A.r[rn] = (u32)r;
        A.r[rd] = (u32)(r >> 32);
        if (op & (1u << 20)) A.cpsr = (A.cpsr & ~(FLAG_N | FLAG_Z)) | ((u32)(r >> 32) & FLAG_N) | (r ? 0 : FLAG_Z);
        return;
    }
    {
        u32 r = A.r[rm] * A.r[rs];
        if (op & (1u << 21)) r += A.r[rn];
        A.r[rd] = r;
        if (op & (1u << 20)) set_nz(r);
    }
}

static void swap(u32 op)
{
    u32 addr = A.r[(op >> 16) & 15], rd = (op >> 12) & 15, rm = op & 15, v;
    if (op & (1u << 22))
    {
        v = read8(addr);
        write8(addr, A.r[rm]);
    }
    else
    {
        v = read32(addr);
        write32(addr, A.r[rm]);
    }
    A.r[rd] = v;
}

static void halfword_transfer(u32 op)
{
    u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, base = reg(rn), offset, addr;
    int pre = (op >> 24) & 1, up = (op >> 23) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
    u32 sh = (op >> 5) & 3;
    offset = (op & (1u << 22)) ? (((op >> 4) & 0xF0) | (op & 15)) : A.r[op & 15];
    addr = pre ? (up ? base + offset : base - offset) : base;
    if (load)
    {
        u32 v = sh == 1 ? read16(addr) : sh == 2 ? (u32)(s32)(s8)read8(addr) : (u32)(s32)(s16)read16(addr);
        if (!pre || wb) A.r[rn] = pre ? addr : (up ? base + offset : base - offset);
        A.r[rd] = v;
    }
    else
    {
        write16(addr, reg(rd));
        if (!pre || wb) A.r[rn] = pre ? addr : (up ? base + offset : base - offset);
    }
}

static void single_transfer(u32 op)
{
    u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, base = reg(rn), offset, addr, carry;
    int pre = (op >> 24) & 1, up = (op >> 23) & 1, byte = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
    if (op & (1u << 25)) offset = shift(A.r[op & 15], (op >> 5) & 3, (op >> 7) & 31, 0, &carry);
    else offset = op & 0xFFF;
    addr = pre ? (up ? base + offset : base - offset) : base;
    if (load)
    {
        u32 v = byte ? read8(addr) : read32(addr);
        if (!pre || wb) A.r[rn] = pre ? addr : (up ? base + offset : base - offset);
        A.r[rd] = v;
        if (rd == 15) PC &= ~3u;
    }
    else
    {
        u32 v = rd == 15 ? PC + 8 : A.r[rd];
        if (byte) write8(addr, v);
        else write32(addr, v);
        if (!pre || wb) A.r[rn] = pre ? addr : (up ? base + offset : base - offset);
    }
}

static void block_transfer(u32 op)
{
    u32 rn = (op >> 16) & 15, list = op & 0xFFFF, count = 0, base = A.r[rn], addr, i;
    int pre = (op >> 24) & 1, up = (op >> 23) & 1, userBank = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
    u32 oldMode = A.cpsr & 0x1F;
    int restoreMode = 0;
    for (i = 0; i < 16; i++) count += (list >> i) & 1;
    addr = up ? base : base - count * 4;
    if (pre == up) addr += 4;
    if (userBank && !(load && (list & 0x8000)) && oldMode != MODE_USR && oldMode != MODE_SYS)
    {
        switch_mode(MODE_USR);
        restoreMode = 1;
    }
    for (i = 0; i < 16; i++)
    {
        if (!(list & (1u << i))) continue;
        if (load)
        {
            A.r[i] = read32(addr);
            if (i == 15) PC &= ~3u;
        }
        else
            write32(addr, i == 15 ? PC + 8 : A.r[i]);
        addr += 4;
    }
    if (restoreMode) switch_mode(oldMode);
    if (wb && !(load && (list & (1u << rn)))) A.r[rn] = up ? base + count * 4 : base - count * 4;
    if (load && userBank && (list & 0x8000)) set_cpsr(A.spsr);
}

static s32 s_extra;

static void execute(u32 op)
{
    u32 next = PC;
    if (!condition(op)) return;
    switch ((op >> 25) & 7)
    {
    case 0:
        if ((op & 0x0FC000F0) == 0x00000090 || (op & 0x0F8000F0) == 0x00800090) { multiply(op); s_extra += 2; return; }
        if ((op & 0x0FB00FF0) == 0x01000090) { swap(op); s_extra += 3; return; }
        if ((op & 0x0E000090) == 0x00000090)
        {
            halfword_transfer(op);
            s_extra += (op >> 20) & 1 ? 2 : 1;
            return;
        }
        if ((op & 0x0F900000) == 0x01000000) { psr_transfer(op); return; }
        data_processing(op);
        if (PC != next) s_extra += 2;
        return;
    case 1:
        if ((op & 0x0FB00000) == 0x03200000) { psr_transfer(op); return; }
        data_processing(op);
        if (PC != next) s_extra += 2;
        return;
    case 2:
    case 3:
        single_transfer(op);
        s_extra += (op >> 20) & 1 ? (PC != next ? 4 : 2) : 1;
        return;
    case 4:
    {
        u32 list = op & 0xFFFF, n = 0, i;
        for (i = 0; i < 16; i++) n += (list >> i) & 1;
        block_transfer(op);
        s_extra += (s32)n + ((op >> 20) & 1 ? (PC != next ? 3 : 1) : 0) - 1;
        return;
    }
    case 5:
    {
        s32 offset = (s32)(op << 8) >> 6;
        s_extra += 2;
        if (op & (1u << 24)) A.r[14] = PC;
        PC = PC + 4 + (u32)offset;
        if (offset < 0 && !(op & (1u << 24))) idle_branch(PC);
        return;
    }
    case 7:
        if ((op >> 24) & 1)
        {
            exception(MODE_SVC, 0x08, PC, 0);
            s_extra += 2;
            return;
        }
        break;
    default:
        break;
    }
    if (s_logged < 16)
    {
        s_logged++;
        LOG_WARN("[arm7] undefined instruction %08X at %08X", op, PC - 4);
    }
    exception(MODE_UND, 0x04, PC, 0);
}

u32 arm7_jit_read32(u32 addr) { return read32(addr); }
u32 arm7_jit_read16(u32 addr) { return read16(addr); }
u32 arm7_jit_read8(u32 addr) { return read8(addr); }
void arm7_jit_write32(u32 addr, u32 v, u32 sp) { write32_sp(addr, v, sp); }
void arm7_jit_write16(u32 addr, u32 v, u32 sp) { write16_sp(addr, v, sp); }
void arm7_jit_write8(u32 addr, u32 v, u32 sp) { write8_sp(addr, v, sp); }

void arm7_interp_op(u32 op)
{
    s_extra = 0;
    execute(op);
    A.cycles -= 1 + s_extra;
}

static void take_fiq(void)
{
    if (A.fiqLine && !(A.cpsr & FLAG_F))
        exception(MODE_FIQ, 0x1C, PC + 4, 1);
}

void arm7_step(void)
{
    u32 op;
    take_fiq();
    if (g_arm7_step_hook) g_arm7_step_hook(PC);
    op = read32(PC);
    PC += 4;
    execute(op);
}

void arm7_run(s32 cycles)
{
    if (!A.enabled) return;
    if (g_sound_touched)
    {
        g_sound_touched = 0;
        s_idlePc = 0xFFFFFFFFu;
        s_asleep = 0;
    }
    if (s_asleep)
    {
        s_napCycles += cycles;
        if (!(A.fiqLine && !(A.cpsr & FLAG_F)) && s_napCycles < IDLE_NAP) return;
        s_asleep = 0;
        cycles = 0;
    }
    A.cycles += cycles;
    while (A.cycles > 0)
    {
        const void *code;
        if (arm7_jit_active() && !g_arm7_step_hook) take_fiq();
        if (arm7_jit_active() && !g_arm7_step_hook && (PC & 0x00FFFFFFu) < 0x800000u && (code = arm7_jit_block(PC)) != NULL)
        {
            A.jitWrote = 0;
            A.jitBack = 0;
            arm7_jit_enter(code);
            if (A.jitWrote) s_wrote = 1;
            if (A.jitBack) idle_branch(PC);
            if (s_asleep)
            {
                A.cycles = 0;
                break;
            }
            continue;
        }
        s_extra = 0;
        arm7_step();
        A.cycles -= 1 + s_extra;
        if (s_asleep)
        {
            A.cycles = 0;
            break;
        }
    }
}

u32 arm7_idle_skips(void)
{
    return s_idleSkips;
}

void arm7_state(StateIO *io)
{
    state_tag(io, "ARM7");
    STATE(io, g_arm7);
    if (io->loading)
    {
        s_idlePc = 0xFFFFFFFFu;
        s_asleep = 0;
        s_wrote = 0;
        arm7_jit_flush();
    }
}
