// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include <math.h>

#include "sh4.h"
#include "sh4_internal.h"
#include "../mem/mem.h"
#include "sh4_mmu.h"

#define mem_read32(a) mem_read32(sh4_va((a), MMU_READ))
#define mem_write32(a, v) mem_write32(sh4_va((a), MMU_WRITE), (v))

#define C g_sh4
#define R g_sh4.r
#define N ((op >> 8) & 15)
#define M ((op >> 4) & 15)

union FBits { u32 u; f32 f; };
union DBits { u64 u; f64 d; };

static __inline f32 getf(u32 bits) { FBits x; x.u = bits; return x.f; }
static __inline u32 fbits(f32 f) { FBits x; x.f = f; return x.u; }

static __inline f32 FR(u32 i) { return getf(C.fr[i]); }
static __inline void setFR(u32 i, f32 v)
{
    u32 b = fbits(v);
    if ((C.fpscr & FPSCR_DN) && (b & 0x7F800000u) == 0) b &= 0x80000000u;
    C.fr[i] = b;
}
static __inline f64 DR(u32 i)
{
    DBits x;
    x.u = ((u64)C.fr[i & 14] << 32) | C.fr[(i & 14) + 1];
    return x.d;
}
static __inline void setDR(u32 i, f64 v)
{
    DBits x;
    x.d = v;
    if ((C.fpscr & FPSCR_DN) && (x.u & 0x7FF0000000000000ull) == 0) x.u &= 0x8000000000000000ull;
    C.fr[i & 14] = (u32)(x.u >> 32);
    C.fr[(i & 14) + 1] = (u32)x.u;
}

#undef SH4OP
#define SH4OP(name) \
    static void fpu_##name(u16 op); \
    void sh4op_##name(u16 op) \
    { \
        if (C.sr & SR_FD) \
        { \
            sh4_fpu_disabled(); \
            return; \
        } \
        fpu_##name(op); \
    } \
    static void fpu_##name(u16 op)

#define PR (C.fpscr & FPSCR_PR)
#define SZ (C.fpscr & FPSCR_SZ)

void sh4_fpu_bank_swap(void)
{
    int i;
    for (i = 0; i < 16; i++)
    {
        u32 t = C.fr[i];
        C.fr[i] = C.xf[i];
        C.xf[i] = t;
    }
}

static __inline u32 *pair(u32 reg) { return (reg & 1) ? &C.xf[reg & 14] : &C.fr[reg & 14]; }

SH4OP(fmov)
{
    if (!SZ) C.fr[N] = C.fr[M];
    else
    {
        u32 *d = pair(N), *s = pair(M);
        d[0] = s[0];
        d[1] = s[1];
    }
}
SH4OP(fmov_load)
{
    u32 a = R[M];
    if (!SZ) C.fr[N] = mem_read32(a);
    else { u32 *d = pair(N); d[0] = mem_read32(a); d[1] = mem_read32(a + 4); }
}
SH4OP(fmov_store)
{
    u32 a = R[N];
    if (!SZ) mem_write32(a, C.fr[M]);
    else { u32 *s = pair(M); mem_write32(a, s[0]); mem_write32(a + 4, s[1]); }
}
SH4OP(fmov_inc)
{
    u32 m = M, a = R[m];
    if (!SZ) { C.fr[N] = mem_read32(a); R[m] += 4; }
    else { u32 *d = pair(N); d[0] = mem_read32(a); d[1] = mem_read32(a + 4); R[m] += 8; }
}
SH4OP(fmov_dec)
{
    u32 n = N;
    if (!SZ) { mem_write32(R[n] - 4, C.fr[M]); R[n] -= 4; }
    else { u32 *s = pair(M); mem_write32(R[n] - 8, s[0]); mem_write32(R[n] - 4, s[1]); R[n] -= 8; }
}
SH4OP(fmov_load_r0)
{
    u32 a = R[M] + R[0];
    if (!SZ) C.fr[N] = mem_read32(a);
    else { u32 *d = pair(N); d[0] = mem_read32(a); d[1] = mem_read32(a + 4); }
}
SH4OP(fmov_store_r0)
{
    u32 a = R[N] + R[0];
    if (!SZ) mem_write32(a, C.fr[M]);
    else { u32 *s = pair(M); mem_write32(a, s[0]); mem_write32(a + 4, s[1]); }
}

SH4OP(fldi0) { C.fr[N] = 0x00000000u; }
SH4OP(fldi1) { C.fr[N] = 0x3F800000u; }
SH4OP(flds) { C.fpul = C.fr[N]; }
SH4OP(fsts) { C.fr[N] = C.fpul; }

SH4OP(fabs_) { C.fr[PR ? (N & 14) : N] &= 0x7FFFFFFFu; }
SH4OP(fneg) { C.fr[PR ? (N & 14) : N] ^= 0x80000000u; }
SH4OP(fadd) { if (PR) setDR(N, DR(N) + DR(M)); else setFR(N, FR(N) + FR(M)); }
SH4OP(fsub) { if (PR) setDR(N, DR(N) - DR(M)); else setFR(N, FR(N) - FR(M)); }
SH4OP(fmul) { if (PR) setDR(N, DR(N) * DR(M)); else setFR(N, FR(N) * FR(M)); }
SH4OP(fdiv) { if (PR) setDR(N, DR(N) / DR(M)); else setFR(N, FR(N) / FR(M)); }
SH4OP(fsqrt) { if (PR) setDR(N, sqrt(DR(N))); else setFR(N, sqrtf(FR(N))); }
SH4OP(fmac) { u32 n = N; setFR(n, FR(0) * FR(M) + FR(n)); }
SH4OP(fcmpeq) { C.t = PR ? DR(N) == DR(M) : FR(N) == FR(M); }
SH4OP(fcmpgt) { C.t = PR ? DR(N) > DR(M) : FR(N) > FR(M); }

SH4OP(float_)
{
    if (PR) setDR(N, (f64)(s32)C.fpul);
    else setFR(N, (f32)(s32)C.fpul);
}

static u32 to_int_sat(f64 v)
{
    if (v != v) return 0x80000000u;
    if (v >= 2147483648.0) return 0x7FFFFFFFu;
    if (v <= -2147483648.0) return 0x80000000u;
    return (u32)(s32)v;
}
SH4OP(ftrc) { C.fpul = to_int_sat(PR ? DR(N) : (f64)FR(N)); }
SH4OP(fcnvsd) { if (PR) setDR(N, (f64)getf(C.fpul)); }
SH4OP(fcnvds) { if (PR) C.fpul = fbits((f32)DR(N)); }

SH4OP(fsrra) { if (!PR) { u32 n = N; setFR(n, 1.0f / sqrtf(FR(n))); } }
SH4OP(fsca)
{
    u32 n = N & 14;
    f64 a = (f64)(C.fpul & 0xFFFF) * (6.283185307179586 / 65536.0);
    if (PR) return;
    setFR(n, (f32)sin(a));
    setFR(n + 1, (f32)cos(a));
}

SH4OP(fipr)
{
    u32 n = (op >> 8) & 12, m = (op >> 6) & 12;
    f64 s;
    if (PR) return;
    s = (f64)FR(n) * FR(m) + (f64)FR(n + 1) * FR(m + 1) + (f64)FR(n + 2) * FR(m + 2) + (f64)FR(n + 3) * FR(m + 3);
    setFR(n + 3, (f32)s);
}

SH4OP(ftrv)
{
    u32 n = (op >> 8) & 12, i;
    f64 v[4], r[4];
    if (PR) return;
    for (i = 0; i < 4; i++) v[i] = FR(n + i);
    for (i = 0; i < 4; i++)
        r[i] = (f64)getf(C.xf[i]) * v[0] + (f64)getf(C.xf[4 + i]) * v[1] + (f64)getf(C.xf[8 + i]) * v[2] +
               (f64)getf(C.xf[12 + i]) * v[3];
    for (i = 0; i < 4; i++) setFR(n + i, (f32)r[i]);
}

SH4OP(frchg) { (void)op; if (!PR) sh4_set_fpscr(C.fpscr ^ FPSCR_FR); }
SH4OP(fschg) { (void)op; if (!PR) sh4_set_fpscr(C.fpscr ^ FPSCR_SZ); }

SH4OP(lds_fpscr) { sh4_set_fpscr(R[N]); }
SH4OP(lds_fpul) { C.fpul = R[N]; }
SH4OP(ldsl_fpscr) { u32 n = N; sh4_set_fpscr(mem_read32(R[n])); R[n] += 4; }
SH4OP(ldsl_fpul) { u32 n = N; C.fpul = mem_read32(R[n]); R[n] += 4; }
SH4OP(sts_fpscr) { R[N] = C.fpscr; }
SH4OP(sts_fpul) { R[N] = C.fpul; }
SH4OP(stsl_fpscr) { u32 n = N; mem_write32(R[n] - 4, C.fpscr); R[n] -= 4; }
SH4OP(stsl_fpul) { u32 n = N; mem_write32(R[n] - 4, C.fpul); R[n] -= 4; }
