// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_PPC_EMIT_H
#define EMU_PPC_EMIT_H

#include "../core/types.h"

struct PpcCode
{
    u32 *buf;
    u32 pos;
    u32 cap;
    int overflow;
};

static inline void ppc_put(PpcCode *c, u32 w)
{
    if (c->pos < c->cap) c->buf[c->pos++] = w;
    else c->overflow = 1;
}

static inline u32 ppc_d(u32 op, u32 rt, u32 ra, u32 imm) { return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF); }
static inline u32 ppc_ds(u32 op, u32 rt, u32 ra, s32 ds, u32 xo) { return (op << 26) | (rt << 21) | (ra << 16) | ((u32)ds & 0xFFFC) | xo; }
static inline u32 ppc_x(u32 rt, u32 ra, u32 rb, u32 xo, u32 rc) { return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc; }
static inline u32 ppc_md(u32 rs, u32 ra, u32 sh, u32 mb, u32 xo)
{
    return (30u << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | ((((mb & 31) << 1) | (mb >> 5)) << 5) | (xo << 2) | (((sh >> 5) & 1) << 1);
}

static inline void ppc_addi(PpcCode *c, u32 rt, u32 ra, s32 imm) { ppc_put(c, ppc_d(14, rt, ra, (u32)imm)); }
static inline void ppc_addis(PpcCode *c, u32 rt, u32 ra, s32 imm) { ppc_put(c, ppc_d(15, rt, ra, (u32)imm)); }
static inline void ppc_li(PpcCode *c, u32 rt, s32 imm) { ppc_addi(c, rt, 0, imm); }
static inline void ppc_lis(PpcCode *c, u32 rt, s32 imm) { ppc_addis(c, rt, 0, imm); }
static inline void ppc_ori(PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(24, rs, ra, imm)); }
static inline void ppc_oris(PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(25, rs, ra, imm)); }
static inline void ppc_xori(PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(26, rs, ra, imm)); }
static inline void ppc_xoris(PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(27, rs, ra, imm)); }
static inline void ppc_andi_(PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(28, rs, ra, imm)); }
static inline void ppc_andis_(PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(29, rs, ra, imm)); }
static inline void ppc_mulli(PpcCode *c, u32 rt, u32 ra, s32 imm) { ppc_put(c, ppc_d(7, rt, ra, (u32)imm)); }
static inline void ppc_subfic(PpcCode *c, u32 rt, u32 ra, s32 imm) { ppc_put(c, ppc_d(8, rt, ra, (u32)imm)); }

static inline void ppc_add(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 266, 0)); }
static inline void ppc_subf(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 40, 0)); }
static inline void ppc_neg(PpcCode *c, u32 rt, u32 ra) { ppc_put(c, ppc_x(rt, ra, 0, 104, 0)); }
static inline void ppc_mullw(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 235, 0)); }
static inline void ppc_mulhw(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 75, 0)); }
static inline void ppc_mulhwu(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 11, 0)); }
static inline void ppc_mulld(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 233, 0)); }
static inline void ppc_mulhd(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 73, 0)); }
static inline void ppc_mulhdu(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 9, 0)); }
static inline void ppc_divw(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 491, 0)); }
static inline void ppc_divwu(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 459, 0)); }
static inline void ppc_divd(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 489, 0)); }
static inline void ppc_divdu(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 457, 0)); }

static inline void ppc_and(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 28, 0)); }
static inline void ppc_and_(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 28, 1)); }
static inline void ppc_andc(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 60, 0)); }
static inline void ppc_or(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 444, 0)); }
static inline void ppc_mr(PpcCode *c, u32 ra, u32 rs) { ppc_or(c, ra, rs, rs); }
static inline void ppc_or_(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 444, 1)); }
static inline void ppc_xor(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 316, 0)); }
static inline void ppc_nor(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 124, 0)); }
static inline void ppc_extsb(PpcCode *c, u32 ra, u32 rs) { ppc_put(c, ppc_x(rs, ra, 0, 954, 0)); }
static inline void ppc_extsh(PpcCode *c, u32 ra, u32 rs) { ppc_put(c, ppc_x(rs, ra, 0, 922, 0)); }
static inline void ppc_extsw(PpcCode *c, u32 ra, u32 rs) { ppc_put(c, ppc_x(rs, ra, 0, 986, 0)); }

static inline void ppc_slw(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 24, 0)); }
static inline void ppc_srw(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 536, 0)); }
static inline void ppc_sraw(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 792, 0)); }
static inline void ppc_srawi(PpcCode *c, u32 ra, u32 rs, u32 sh) { ppc_put(c, ppc_x(rs, ra, sh & 31, 824, 0)); }
static inline void ppc_sld(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 27, 0)); }
static inline void ppc_srd(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 539, 0)); }
static inline void ppc_srad(PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 794, 0)); }
static inline void ppc_sradi(PpcCode *c, u32 ra, u32 rs, u32 sh)
{
    ppc_put(c, (31u << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | (413u << 2) | (((sh >> 5) & 1) << 1));
}
static inline void ppc_rlwinm(PpcCode *c, u32 ra, u32 rs, u32 sh, u32 mb, u32 me)
{
    ppc_put(c, (21u << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1));
}
static inline void ppc_rlwinm_(PpcCode *c, u32 ra, u32 rs, u32 sh, u32 mb, u32 me)
{
    ppc_put(c, (21u << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | 1u);
}
static inline void ppc_rlwimi(PpcCode *c, u32 ra, u32 rs, u32 sh, u32 mb, u32 me)
{
    ppc_put(c, (20u << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1));
}
static inline void ppc_rldicl(PpcCode *c, u32 ra, u32 rs, u32 sh, u32 mb) { ppc_put(c, ppc_md(rs, ra, sh, mb, 0)); }
static inline void ppc_rldicr(PpcCode *c, u32 ra, u32 rs, u32 sh, u32 me) { ppc_put(c, ppc_md(rs, ra, sh, me, 1)); }
static inline void ppc_rldimi(PpcCode *c, u32 ra, u32 rs, u32 sh, u32 mb) { ppc_put(c, ppc_md(rs, ra, sh, mb, 3)); }
static inline void ppc_sldi(PpcCode *c, u32 ra, u32 rs, u32 n) { ppc_rldicr(c, ra, rs, n, 63 - n); }
static inline void ppc_srdi(PpcCode *c, u32 ra, u32 rs, u32 n) { ppc_rldicl(c, ra, rs, 64 - n, n); }
static inline void ppc_clrldi(PpcCode *c, u32 ra, u32 rs, u32 n) { ppc_rldicl(c, ra, rs, 0, n); }

static inline void ppc_cmpw(PpcCode *c, u32 cr, u32 ra, u32 rb) { ppc_put(c, ppc_x(cr << 2, ra, rb, 0, 0)); }
static inline void ppc_cmpd(PpcCode *c, u32 cr, u32 ra, u32 rb) { ppc_put(c, ppc_x((cr << 2) | 1, ra, rb, 0, 0)); }
static inline void ppc_cmplw(PpcCode *c, u32 cr, u32 ra, u32 rb) { ppc_put(c, ppc_x(cr << 2, ra, rb, 32, 0)); }
static inline void ppc_cmpld(PpcCode *c, u32 cr, u32 ra, u32 rb) { ppc_put(c, ppc_x((cr << 2) | 1, ra, rb, 32, 0)); }
static inline void ppc_cmpwi(PpcCode *c, u32 cr, u32 ra, s32 imm) { ppc_put(c, ppc_d(11, cr << 2, ra, (u32)imm)); }
static inline void ppc_cmpdi(PpcCode *c, u32 cr, u32 ra, s32 imm) { ppc_put(c, ppc_d(11, (cr << 2) | 1, ra, (u32)imm)); }
static inline void ppc_cmplwi(PpcCode *c, u32 cr, u32 ra, u32 imm) { ppc_put(c, ppc_d(10, cr << 2, ra, imm)); }
static inline void ppc_cmpldi(PpcCode *c, u32 cr, u32 ra, u32 imm) { ppc_put(c, ppc_d(10, (cr << 2) | 1, ra, imm)); }

static inline void ppc_lbz(PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_d(34, rt, ra, (u32)d)); }
static inline void ppc_lhz(PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_d(40, rt, ra, (u32)d)); }
static inline void ppc_lha(PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_d(42, rt, ra, (u32)d)); }
static inline void ppc_lwz(PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_d(32, rt, ra, (u32)d)); }
static inline void ppc_lwa(PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_ds(58, rt, ra, d, 2)); }
static inline void ppc_ld(PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_ds(58, rt, ra, d, 0)); }
static inline void ppc_stb(PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_d(38, rs, ra, (u32)d)); }
static inline void ppc_sth(PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_d(44, rs, ra, (u32)d)); }
static inline void ppc_stw(PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_d(36, rs, ra, (u32)d)); }
static inline void ppc_stwu(PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_d(37, rs, ra, (u32)d)); }
static inline void ppc_std(PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_ds(62, rs, ra, d, 0)); }
static inline void ppc_stdu(PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_ds(62, rs, ra, d, 1)); }
static inline void ppc_lbzx(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 87, 0)); }
static inline void ppc_lhzx(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 279, 0)); }
static inline void ppc_lhax(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 343, 0)); }
static inline void ppc_lwzx(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 23, 0)); }
static inline void ppc_dcbt(PpcCode *c, u32 ra, u32 rb) { ppc_put(c, ppc_x(0, ra, rb, 278, 0)); }
static inline void ppc_lwax(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 341, 0)); }
static inline void ppc_ldx(PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 21, 0)); }
static inline void ppc_stbx(PpcCode *c, u32 rs, u32 ra, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 215, 0)); }
static inline void ppc_sthx(PpcCode *c, u32 rs, u32 ra, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 407, 0)); }
static inline void ppc_stwx(PpcCode *c, u32 rs, u32 ra, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 151, 0)); }
static inline void ppc_stdx(PpcCode *c, u32 rs, u32 ra, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 149, 0)); }

static inline u32 ppc_a(u32 op, u32 frt, u32 fra, u32 frb, u32 frc, u32 xo) { return (op << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1); }
static inline void ppc_lfs(PpcCode *c, u32 frt, s32 d, u32 ra) { ppc_put(c, ppc_d(48, frt, ra, (u32)d)); }
static inline void ppc_lfd(PpcCode *c, u32 frt, s32 d, u32 ra) { ppc_put(c, ppc_d(50, frt, ra, (u32)d)); }
static inline void ppc_stfs(PpcCode *c, u32 frs, s32 d, u32 ra) { ppc_put(c, ppc_d(52, frs, ra, (u32)d)); }
static inline void ppc_stfd(PpcCode *c, u32 frs, s32 d, u32 ra) { ppc_put(c, ppc_d(54, frs, ra, (u32)d)); }
static inline void ppc_stfiwx(PpcCode *c, u32 frs, u32 ra, u32 rb) { ppc_put(c, ppc_x(frs, ra, rb, 983, 0)); }
static inline void ppc_lfsx(PpcCode *c, u32 frt, u32 ra, u32 rb) { ppc_put(c, ppc_x(frt, ra, rb, 535, 0)); }
static inline void ppc_lfdx(PpcCode *c, u32 frt, u32 ra, u32 rb) { ppc_put(c, ppc_x(frt, ra, rb, 599, 0)); }
static inline void ppc_stfsx(PpcCode *c, u32 frs, u32 ra, u32 rb) { ppc_put(c, ppc_x(frs, ra, rb, 663, 0)); }
static inline void ppc_stfdx(PpcCode *c, u32 frs, u32 ra, u32 rb) { ppc_put(c, ppc_x(frs, ra, rb, 727, 0)); }
static inline void ppc_fadds(PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(59, t, a, b, 0, 21)); }
static inline void ppc_fsubs(PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(59, t, a, b, 0, 20)); }
static inline void ppc_fmuls(PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(59, t, a, 0, b, 25)); }
static inline void ppc_fdivs(PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(59, t, a, b, 0, 18)); }
static inline void ppc_fadd(PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(63, t, a, b, 0, 21)); }
static inline void ppc_fsub(PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(63, t, a, b, 0, 20)); }
static inline void ppc_fmul(PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(63, t, a, 0, b, 25)); }
static inline void ppc_fsel(PpcCode *c, u32 t, u32 a, u32 cc, u32 b) { ppc_put(c, ppc_a(63, t, a, b, cc, 23)); }
static inline void ppc_fdiv(PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(63, t, a, b, 0, 18)); }
static inline void ppc_fmadd(PpcCode *c, u32 t, u32 a, u32 cc, u32 b) { ppc_put(c, ppc_a(63, t, a, b, cc, 29)); }
static inline void ppc_fsqrt(PpcCode *c, u32 t, u32 b) { ppc_put(c, ppc_a(63, t, 0, b, 0, 22)); }
static inline void ppc_frsp(PpcCode *c, u32 t, u32 b) { ppc_put(c, ppc_a(63, t, 0, b, 0, 12)); }
static inline void ppc_fctiw(PpcCode *c, u32 t, u32 b) { ppc_put(c, ppc_a(63, t, 0, b, 0, 14)); }
static inline void ppc_fctiwz(PpcCode *c, u32 t, u32 b) { ppc_put(c, ppc_a(63, t, 0, b, 0, 15)); }
static inline void ppc_fcfid(PpcCode *c, u32 t, u32 b) { ppc_put(c, (63u << 26) | (t << 21) | (b << 11) | (846u << 1)); }
static inline void ppc_fneg(PpcCode *c, u32 t, u32 b) { ppc_put(c, (63u << 26) | (t << 21) | (b << 11) | (40u << 1)); }
static inline void ppc_fabs(PpcCode *c, u32 t, u32 b) { ppc_put(c, (63u << 26) | (t << 21) | (b << 11) | (264u << 1)); }
static inline void ppc_fmr(PpcCode *c, u32 t, u32 b) { ppc_put(c, (63u << 26) | (t << 21) | (b << 11) | (72u << 1)); }
static inline void ppc_fcmpu(PpcCode *c, u32 cr, u32 a, u32 b) { ppc_put(c, (63u << 26) | (cr << 23) | (a << 16) | (b << 11)); }
static inline void ppc_mffs(PpcCode *c, u32 t) { ppc_put(c, (63u << 26) | (t << 21) | (583u << 1)); }

static inline u32 ppc_spr(u32 spr) { return ((spr & 31) << 16) | ((spr >> 5) << 11); }
static inline void ppc_mflr(PpcCode *c, u32 rt) { ppc_put(c, (31u << 26) | (rt << 21) | ppc_spr(8) | (339u << 1)); }
static inline void ppc_mtlr(PpcCode *c, u32 rs) { ppc_put(c, (31u << 26) | (rs << 21) | ppc_spr(8) | (467u << 1)); }
static inline void ppc_mtctr(PpcCode *c, u32 rs) { ppc_put(c, (31u << 26) | (rs << 21) | ppc_spr(9) | (467u << 1)); }
static inline void ppc_blr(PpcCode *c) { ppc_put(c, 0x4E800020u); }
static inline void ppc_bctr(PpcCode *c) { ppc_put(c, 0x4E800420u); }
static inline void ppc_mfcr(PpcCode *c, u32 rt) { ppc_put(c, 0x7C000026u | (rt << 21)); }
static inline void ppc_mfocrf(PpcCode *c, u32 rt, u32 fxm) { ppc_put(c, 0x7C100026u | (rt << 21) | (fxm << 12)); }
static inline void ppc_mtcrf(PpcCode *c, u32 fxm, u32 rs) { ppc_put(c, 0x7C000120u | (rs << 21) | ((fxm & 0xFF) << 12)); }
static inline void ppc_bctrl(PpcCode *c) { ppc_put(c, 0x4E800421u); }
static inline void ppc_nop(PpcCode *c) { ppc_put(c, 0x60000000u); }

enum { PPC_LT = 0, PPC_GT = 1, PPC_EQ = 2, PPC_UN = 3 };
static inline u32 ppc_bc_fwd(PpcCode *c, u32 bo, u32 cr, u32 bit)
{
    u32 at = c->pos;
    ppc_put(c, (16u << 26) | (bo << 21) | ((cr * 4 + bit) << 16));
    return at;
}
static inline u32 ppc_b_fwd(PpcCode *c)
{
    u32 at = c->pos;
    ppc_put(c, 18u << 26);
    return at;
}
static inline void ppc_patch_here(PpcCode *c, u32 at)
{
    s32 off = (s32)(c->pos - at) * 4;
    if (at >= c->cap || c->overflow) return;
    if ((c->buf[at] >> 26) == 16)
    {
        if (off < -32768 || off > 32764) c->overflow = 1;
        c->buf[at] = (c->buf[at] & ~0xFFFCu) | ((u32)off & 0xFFFCu);
    }
    else c->buf[at] = (c->buf[at] & ~0x03FFFFFCu) | ((u32)off & 0x03FFFFFCu);
}

static inline u32 ppc_b_word(const u32 *from, const u32 *to)
{
    return (18u << 26) | ((u32)((const u8 *)to - (const u8 *)from) & 0x03FFFFFCu);
}
static inline void ppc_b_to(PpcCode *c, const u32 *to)
{
    ppc_put(c, ppc_b_word(c->buf + c->pos, to));
}

void ppc_li64(PpcCode *c, u32 rt, u64 v);
void ppc_li32u(PpcCode *c, u32 rt, u32 v);

#endif
