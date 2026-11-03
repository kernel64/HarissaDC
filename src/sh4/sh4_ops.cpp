// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "sh4.h"
#include "sh4_internal.h"

#include "../core/log.h"
#include "../mem/mem.h"
#include "sh4_mmu.h"
#include "sh4_mmr.h"

#define mem_read8(a) mem_read8(sh4_va((a), MMU_READ))
#define mem_read16(a) mem_read16(sh4_va((a), MMU_READ))
#define mem_read32(a) mem_read32(sh4_va((a), MMU_READ))
#define mem_write8(a, v) mem_write8(sh4_va((a), MMU_WRITE), (v))
#define mem_write16(a, v) mem_write16(sh4_va((a), MMU_WRITE), (v))
#define mem_write32(a, v) mem_write32(sh4_va((a), MMU_WRITE), (v))

#define C g_sh4
#define R g_sh4.r
#define N ((op >> 8) & 15)
#define M ((op >> 4) & 15)
#define IMM8 ((u32)(op & 0xFF))
#define SIMM8 ((u32)(s32)(s8)(op & 0xFF))
#define PC4 (g_sh4.pc + 2)

void sh4_exec_delay_slot(void)
{
    u16 op = mem_fetch16(g_mmu_on ? sh4_va(C.pc, MMU_FETCH) : C.pc);
    C.pc += 2;
    C.cycles--;
    g_sh4_ops[op](op);
}

SH4OP(mov) { R[N] = R[M]; }
SH4OP(mov_imm) { R[N] = SIMM8; }
SH4OP(movw_pc) { R[N] = (u32)(s32)(s16)mem_read16(PC4 + IMM8 * 2); }
SH4OP(movl_pc) { R[N] = mem_read32((PC4 & ~3u) + IMM8 * 4); }
SH4OP(mova) { R[0] = (PC4 & ~3u) + IMM8 * 4; }

SH4OP(movb_st) { mem_write8(R[N], (u8)R[M]); }
SH4OP(movw_st) { mem_write16(R[N], (u16)R[M]); }
SH4OP(movl_st) { mem_write32(R[N], R[M]); }
SH4OP(movb_ld) { R[N] = (u32)(s32)(s8)mem_read8(R[M]); }
SH4OP(movw_ld) { R[N] = (u32)(s32)(s16)mem_read16(R[M]); }
SH4OP(movl_ld) { R[N] = mem_read32(R[M]); }

SH4OP(movb_dec) { u32 n = N, v = R[M]; mem_write8(R[n] - 1, (u8)v); R[n] -= 1; }
SH4OP(movw_dec) { u32 n = N, v = R[M]; mem_write16(R[n] - 2, (u16)v); R[n] -= 2; }
SH4OP(movl_dec) { u32 n = N, v = R[M]; mem_write32(R[n] - 4, v); R[n] -= 4; }

SH4OP(movb_inc) { u32 n = N, m = M, v = (u32)(s32)(s8)mem_read8(R[m]); if (n != m) R[m] += 1; R[n] = v; }
SH4OP(movw_inc) { u32 n = N, m = M, v = (u32)(s32)(s16)mem_read16(R[m]); if (n != m) R[m] += 2; R[n] = v; }
SH4OP(movl_inc) { u32 n = N, m = M, v = mem_read32(R[m]); if (n != m) R[m] += 4; R[n] = v; }

SH4OP(movb_st_r0disp) { mem_write8(R[M] + (op & 15), (u8)R[0]); }
SH4OP(movw_st_r0disp) { mem_write16(R[M] + (op & 15) * 2, (u16)R[0]); }
SH4OP(movl_st_disp) { mem_write32(R[N] + (op & 15) * 4, R[M]); }
SH4OP(movb_ld_r0disp) { R[0] = (u32)(s32)(s8)mem_read8(R[M] + (op & 15)); }
SH4OP(movw_ld_r0disp) { R[0] = (u32)(s32)(s16)mem_read16(R[M] + (op & 15) * 2); }
SH4OP(movl_ld_disp) { R[N] = mem_read32(R[M] + (op & 15) * 4); }

SH4OP(movb_st_r0) { mem_write8(R[N] + R[0], (u8)R[M]); }
SH4OP(movw_st_r0) { mem_write16(R[N] + R[0], (u16)R[M]); }
SH4OP(movl_st_r0) { mem_write32(R[N] + R[0], R[M]); }
SH4OP(movb_ld_r0) { R[N] = (u32)(s32)(s8)mem_read8(R[M] + R[0]); }
SH4OP(movw_ld_r0) { R[N] = (u32)(s32)(s16)mem_read16(R[M] + R[0]); }
SH4OP(movl_ld_r0) { R[N] = mem_read32(R[M] + R[0]); }

SH4OP(movb_st_gbr) { mem_write8(C.gbr + IMM8, (u8)R[0]); }
SH4OP(movw_st_gbr) { mem_write16(C.gbr + IMM8 * 2, (u16)R[0]); }
SH4OP(movl_st_gbr) { mem_write32(C.gbr + IMM8 * 4, R[0]); }
SH4OP(movb_ld_gbr) { R[0] = (u32)(s32)(s8)mem_read8(C.gbr + IMM8); }
SH4OP(movw_ld_gbr) { R[0] = (u32)(s32)(s16)mem_read16(C.gbr + IMM8 * 2); }
SH4OP(movl_ld_gbr) { R[0] = mem_read32(C.gbr + IMM8 * 4); }

SH4OP(movt) { R[N] = C.t; }
SH4OP(swapb) { u32 v = R[M]; R[N] = (v & 0xFFFF0000u) | ((v & 0xFF) << 8) | ((v >> 8) & 0xFF); }
SH4OP(swapw) { u32 v = R[M]; R[N] = (v << 16) | (v >> 16); }
SH4OP(xtrct) { R[N] = (R[N] >> 16) | (R[M] << 16); }

SH4OP(add) { R[N] += R[M]; }
SH4OP(add_imm) { R[N] += SIMM8; }
SH4OP(addc)
{
    u32 n = N, a = R[n], b = R[M], tmp = a + b, res = tmp + C.t;
    C.t = (tmp < a) | (res < tmp);
    R[n] = res;
}
SH4OP(addv)
{
    u32 n = N, a = R[n], b = R[M], res = a + b;
    C.t = ((~(a ^ b) & (a ^ res)) >> 31) & 1;
    R[n] = res;
}
SH4OP(cmpeq_imm) { C.t = R[0] == SIMM8; }
SH4OP(cmpeq) { C.t = R[N] == R[M]; }
SH4OP(cmphs) { C.t = R[N] >= R[M]; }
SH4OP(cmpge) { C.t = (s32)R[N] >= (s32)R[M]; }
SH4OP(cmphi) { C.t = R[N] > R[M]; }
SH4OP(cmpgt) { C.t = (s32)R[N] > (s32)R[M]; }
SH4OP(cmppl) { C.t = (s32)R[N] > 0; }
SH4OP(cmppz) { C.t = (s32)R[N] >= 0; }
SH4OP(cmpstr)
{
    u32 v = R[N] ^ R[M];
    C.t = (v & 0xFF000000u) == 0 || (v & 0x00FF0000u) == 0 || (v & 0x0000FF00u) == 0 || (v & 0xFFu) == 0;
}

static __inline u32 get_q(void) { return (C.sr >> 8) & 1; }
static __inline u32 get_m(void) { return (C.sr >> 9) & 1; }
static __inline void set_q(u32 q) { C.sr = (C.sr & ~SR_Q) | (q << 8); }
static __inline void set_m(u32 m) { C.sr = (C.sr & ~SR_M) | (m << 9); }

SH4OP(div0s)
{
    u32 q = R[N] >> 31, m = R[M] >> 31;
    set_q(q);
    set_m(m);
    C.t = q ^ m;
}
SH4OP(div0u) { set_q(0); set_m(0); C.t = 0; }

SH4OP(div1)
{
    u32 n = N, divisor = R[M], old_q = get_q(), q, m = get_m(), tmp0, tmp1;
    q = R[n] >> 31;
    R[n] = (R[n] << 1) | C.t;
    tmp0 = R[n];
    if (old_q == m)
    {
        R[n] -= divisor;
        tmp1 = R[n] > tmp0;
    }
    else
    {
        R[n] += divisor;
        tmp1 = R[n] < tmp0;
    }
    q = q ? (tmp1 == 0) : tmp1;
    if (m) q ^= 1;
    set_q(q);
    C.t = q == m;
}

SH4OP(dmuls) { s64 r = (s64)(s32)R[N] * (s64)(s32)R[M]; C.mach = (u32)((u64)r >> 32); C.macl = (u32)r; }
SH4OP(dmulu) { u64 r = (u64)R[N] * (u64)R[M]; C.mach = (u32)(r >> 32); C.macl = (u32)r; }
SH4OP(dt) { R[N]--; C.t = R[N] == 0; }
SH4OP(extsb) { R[N] = (u32)(s32)(s8)R[M]; }
SH4OP(extsw) { R[N] = (u32)(s32)(s16)R[M]; }
SH4OP(extub) { R[N] = R[M] & 0xFF; }
SH4OP(extuw) { R[N] = R[M] & 0xFFFF; }

SH4OP(macl)
{
    u32 n = N, m = M;
    s64 a, b, mac;
    a = (s32)mem_read32(R[n]);
    b = (s32)mem_read32(n == m ? R[n] + 4 : R[m]);
    R[n] += 4;
    R[m] += 4;
    mac = (s64)(((u64)C.mach << 32) | C.macl) + a * b;
    if (C.sr & SR_S)
    {
        const s64 hi = 0x00007FFFFFFFFFFFll, lo = -0x0000800000000000ll;
        if (mac > hi) mac = hi;
        if (mac < lo) mac = lo;
    }
    C.mach = (u32)((u64)mac >> 32);
    C.macl = (u32)mac;
}

SH4OP(macw)
{
    u32 n = N, m = M;
    s32 a, b;
    a = (s16)mem_read16(R[n]);
    b = (s16)mem_read16(n == m ? R[n] + 2 : R[m]);
    R[n] += 2;
    R[m] += 2;
    if (C.sr & SR_S)
    {
        s64 r = (s64)(s32)C.macl + (s64)a * b;
        if (r > 0x7FFFFFFFll) { r = 0x7FFFFFFFll; C.mach |= 1; }
        else if (r < -0x80000000ll) { r = -0x80000000ll; C.mach |= 1; }
        C.macl = (u32)r;
    }
    else
    {
        s64 mac = (s64)(((u64)C.mach << 32) | C.macl) + (s64)a * b;
        C.mach = (u32)((u64)mac >> 32);
        C.macl = (u32)mac;
    }
}

SH4OP(mull) { C.macl = R[N] * R[M]; }
SH4OP(mulsw) { C.macl = (u32)((s32)(s16)R[N] * (s32)(s16)R[M]); }
SH4OP(muluw) { C.macl = (u32)(u16)R[N] * (u32)(u16)R[M]; }
SH4OP(neg) { R[N] = 0 - R[M]; }
SH4OP(negc)
{
    u32 n = N, m = R[M], tmp = 0 - m, res = tmp - C.t;
    C.t = (0 < tmp) | (tmp < res);
    R[n] = res;
}
SH4OP(sub) { R[N] -= R[M]; }
SH4OP(subc)
{
    u32 n = N, a = R[n], b = R[M], tmp = a - b, res = tmp - C.t;
    C.t = (a < tmp) | (tmp < res);
    R[n] = res;
}
SH4OP(subv)
{
    u32 n = N, a = R[n], b = R[M], res = a - b;
    C.t = (((a ^ b) & (a ^ res)) >> 31) & 1;
    R[n] = res;
}

SH4OP(and_) { R[N] &= R[M]; }
SH4OP(and_imm) { R[0] &= IMM8; }
SH4OP(andb) { u32 a = C.gbr + R[0]; mem_write8(a, (u8)(mem_read8(a) & IMM8)); }
SH4OP(not_) { R[N] = ~R[M]; }
SH4OP(or_) { R[N] |= R[M]; }
SH4OP(or_imm) { R[0] |= IMM8; }
SH4OP(orb) { u32 a = C.gbr + R[0]; mem_write8(a, (u8)(mem_read8(a) | IMM8)); }
SH4OP(tasb) { u32 a = R[N]; u8 v = mem_read8(a); C.t = v == 0; mem_write8(a, (u8)(v | 0x80)); }
SH4OP(tst) { C.t = (R[N] & R[M]) == 0; }
SH4OP(tst_imm) { C.t = (R[0] & IMM8) == 0; }
SH4OP(tstb) { C.t = (mem_read8(C.gbr + R[0]) & IMM8) == 0; }
SH4OP(xor_) { R[N] ^= R[M]; }
SH4OP(xor_imm) { R[0] ^= IMM8; }
SH4OP(xorb) { u32 a = C.gbr + R[0]; mem_write8(a, (u8)(mem_read8(a) ^ IMM8)); }

SH4OP(rotl) { u32 n = N; C.t = R[n] >> 31; R[n] = (R[n] << 1) | C.t; }
SH4OP(rotr) { u32 n = N; C.t = R[n] & 1; R[n] = (R[n] >> 1) | (C.t << 31); }
SH4OP(rotcl) { u32 n = N, t = R[n] >> 31; R[n] = (R[n] << 1) | C.t; C.t = t; }
SH4OP(rotcr) { u32 n = N, t = R[n] & 1; R[n] = (R[n] >> 1) | (C.t << 31); C.t = t; }
SH4OP(shad)
{
    u32 n = N;
    s32 s = (s32)R[M];
    if (s >= 0) R[n] <<= (s & 31);
    else if ((s & 31) == 0) R[n] = (s32)R[n] < 0 ? 0xFFFFFFFFu : 0;
    else R[n] = (u32)((s32)R[n] >> ((~s & 31) + 1));
}
SH4OP(shld)
{
    u32 n = N;
    s32 s = (s32)R[M];
    if (s >= 0) R[n] <<= (s & 31);
    else if ((s & 31) == 0) R[n] = 0;
    else R[n] >>= ((~s & 31) + 1);
}
SH4OP(shal) { u32 n = N; C.t = R[n] >> 31; R[n] <<= 1; }
SH4OP(shar) { u32 n = N; C.t = R[n] & 1; R[n] = (u32)((s32)R[n] >> 1); }
SH4OP(shll) { u32 n = N; C.t = R[n] >> 31; R[n] <<= 1; }
SH4OP(shlr) { u32 n = N; C.t = R[n] & 1; R[n] >>= 1; }
SH4OP(shll2) { R[N] <<= 2; }
SH4OP(shlr2) { R[N] >>= 2; }
SH4OP(shll8) { R[N] <<= 8; }
SH4OP(shlr8) { R[N] >>= 8; }
SH4OP(shll16) { R[N] <<= 16; }
SH4OP(shlr16) { R[N] >>= 16; }

static __inline u32 disp8_target(u16 op) { return PC4 + ((u32)(s32)(s8)(op & 0xFF) << 1); }
static __inline u32 disp12_target(u16 op)
{
    s32 d = op & 0xFFF;
    if (d & 0x800) d -= 0x1000;
    return PC4 + ((u32)d << 1);
}

SH4OP(bf) { if (!C.t) { C.pc = disp8_target(op); C.cycles--; } }
SH4OP(bt) { if (C.t) { C.pc = disp8_target(op); C.cycles--; } }
SH4OP(bfs)
{
    u32 target = disp8_target(op), taken = !C.t;
    sh4_exec_delay_slot();
    if (taken) C.pc = target;
}
SH4OP(bts)
{
    u32 target = disp8_target(op), taken = C.t;
    sh4_exec_delay_slot();
    if (taken) C.pc = target;
}
SH4OP(bra) { u32 target = disp12_target(op); sh4_exec_delay_slot(); C.pc = target; }
SH4OP(braf) { u32 target = PC4 + R[N]; sh4_exec_delay_slot(); C.pc = target; }
SH4OP(bsr) { u32 target = disp12_target(op); C.pr = PC4; sh4_exec_delay_slot(); C.pc = target; }
SH4OP(bsrf) { u32 target = PC4 + R[N]; C.pr = PC4; sh4_exec_delay_slot(); C.pc = target; }
SH4OP(jmp) { u32 target = R[N]; sh4_exec_delay_slot(); C.pc = target; }
SH4OP(jsr) { u32 target = R[N]; C.pr = PC4; sh4_exec_delay_slot(); C.pc = target; }
SH4OP(rts) { u32 target = C.pr; sh4_exec_delay_slot(); C.pc = target; }
SH4OP(rte)
{
    u32 target = C.spc;
    u16 slot = mem_fetch16(g_mmu_on ? sh4_va(C.pc, MMU_FETCH) : C.pc);
    sh4_set_sr(C.ssr);
    C.pc += 2;
    C.cycles--;
    g_sh4_ops[slot](slot);
    C.pc = target;
    sh4_irq_update();
}

SH4OP(clrmac) { C.mach = C.macl = 0; }
SH4OP(clrs) { C.sr &= ~SR_S; }
SH4OP(clrt) { C.t = 0; }
SH4OP(sets) { C.sr |= SR_S; }
SH4OP(sett) { C.t = 1; }
SH4OP(nop) { (void)op; }

SH4OP(ldc_sr) { sh4_set_sr(R[N]); sh4_irq_update(); }
SH4OP(ldc_gbr) { C.gbr = R[N]; }
SH4OP(ldc_vbr) { C.vbr = R[N]; }
SH4OP(ldc_ssr) { C.ssr = R[N]; }
SH4OP(ldc_spc) { C.spc = R[N]; }
SH4OP(ldc_dbr) { C.dbr = R[N]; }
SH4OP(ldc_bank) { C.rbank[(op >> 4) & 7] = R[N]; }

static __inline u32 pop32(u32 n) { u32 v = mem_read32(R[n]); R[n] += 4; return v; }
static __inline void push32(u32 n, u32 v) { mem_write32(R[n] - 4, v); R[n] -= 4; }

SH4OP(ldcl_sr) { sh4_set_sr(pop32(N)); sh4_irq_update(); }
SH4OP(ldcl_gbr) { C.gbr = pop32(N); }
SH4OP(ldcl_vbr) { C.vbr = pop32(N); }
SH4OP(ldcl_ssr) { C.ssr = pop32(N); }
SH4OP(ldcl_spc) { C.spc = pop32(N); }
SH4OP(ldcl_dbr) { C.dbr = pop32(N); }
SH4OP(ldcl_bank) { C.rbank[(op >> 4) & 7] = pop32(N); }

SH4OP(lds_mach) { C.mach = R[N]; }
SH4OP(lds_macl) { C.macl = R[N]; }
SH4OP(lds_pr) { C.pr = R[N]; }
SH4OP(ldsl_mach) { C.mach = pop32(N); }
SH4OP(ldsl_macl) { C.macl = pop32(N); }
SH4OP(ldsl_pr) { C.pr = pop32(N); }

SH4OP(stc_sr) { R[N] = sh4_get_sr(); }
SH4OP(stc_gbr) { R[N] = C.gbr; }
SH4OP(stc_vbr) { R[N] = C.vbr; }
SH4OP(stc_ssr) { R[N] = C.ssr; }
SH4OP(stc_spc) { R[N] = C.spc; }
SH4OP(stc_sgr) { R[N] = C.sgr; }
SH4OP(stc_dbr) { R[N] = C.dbr; }
SH4OP(stc_bank) { R[N] = C.rbank[(op >> 4) & 7]; }

SH4OP(stcl_sr) { push32(N, sh4_get_sr()); }
SH4OP(stcl_gbr) { push32(N, C.gbr); }
SH4OP(stcl_vbr) { push32(N, C.vbr); }
SH4OP(stcl_ssr) { push32(N, C.ssr); }
SH4OP(stcl_spc) { push32(N, C.spc); }
SH4OP(stcl_sgr) { push32(N, C.sgr); }
SH4OP(stcl_dbr) { push32(N, C.dbr); }
SH4OP(stcl_bank) { push32(N, C.rbank[(op >> 4) & 7]); }

SH4OP(sts_mach) { R[N] = C.mach; }
SH4OP(sts_macl) { R[N] = C.macl; }
SH4OP(sts_pr) { R[N] = C.pr; }
SH4OP(stsl_mach) { push32(N, C.mach); }
SH4OP(stsl_macl) { push32(N, C.macl); }
SH4OP(stsl_pr) { push32(N, C.pr); }

SH4OP(illegal);

SH4OP(trapa) { sh4_trapa(IMM8); }
SH4OP(sleep) { (void)op; C.sleeping = 1; C.cycles = 0; }
SH4OP(ldtlb) { (void)op; sh4_mmu_ldtlb(); }

SH4OP(pref)
{
    u32 a = R[N];
    if ((a & 0xFC000000u) == 0xE0000000u)
    {
        if (g_mmu_on) sh4_sq_flush_to(sh4_mmu_translate_sq(a), a);
        else sh4_sq_flush(a);
    }
}
SH4OP(ocbi) { (void)op; }
SH4OP(ocbp) { (void)op; }
SH4OP(ocbwb) { (void)op; }
SH4OP(movcal) { mem_write32(R[N], R[0]); }

Sh4HleTrap g_sh4_hle_trap;

SH4OP(hle_trap)
{
    if (g_sh4_hle_trap) g_sh4_hle_trap(C.pc - 2);
    else sh4op_illegal(op);
}

void (*g_sh4_illegal_hook)(u32 pc);

SH4OP(illegal)
{
    LOG_ERROR("[sh4] illegal instruction %04X at %08X", op, C.pc - 2);
    if (g_sh4_illegal_hook) g_sh4_illegal_hook(C.pc - 2);
    sh4_exception(0x180, C.pc - 2);
}
