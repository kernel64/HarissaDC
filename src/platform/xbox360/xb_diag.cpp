// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_diag.h"

#include <xtl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../core/log.h"
#include "../../core/types.h"
#include "../../hw/dreamcast.h"
#include "../../sh4/sh4.h"
#include "../../sh4/sh4_bench.h"
#include "xb_codemem.h"
#include "xb_log.h"
#include "../../sh4/ppc_emit.h"

typedef u32 (*GenFn)(u32 arg);

#define PPC_LI_R3(v)   (0x38600000u | ((u32)(v) & 0xFFFFu))
#define PPC_LWZ_R3_R3  0x80630000u
#define PPC_BLR        0x4E800020u

static DiagResult *add_result(DiagResult *out, int *n, int max, const char *name)
{
    DiagResult *r;
    if (*n >= max) return NULL;
    r = &out[(*n)++];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->ok = -1;
    return r;
}

static void finish(DiagResult *r, int ok, const char *fmt, ...)
{
    va_list ap;
    if (!r) return;
    r->ok = ok;
    va_start(ap, fmt);
    _vsnprintf(r->detail, sizeof(r->detail) - 1, fmt, ap);
    va_end(ap);
    r->detail[sizeof(r->detail) - 1] = 0;
    LOG_INFO("[diag] %s: %s - %s", r->name, ok > 0 ? "OK" : ok == 0 ? "FAILED" : "not run", r->detail);
}

static void test_codemem(DiagResult *r)
{
    u32 *code = xb_codemem_base();
    GenFn fn = (GenFn)code;
    u32 a, b;
    code[0] = PPC_LI_R3(42);
    code[1] = PPC_BLR;
    xb_codemem_flush(code, 8);
    a = fn(0);
    code[0] = PPC_LI_R3(43);
    xb_codemem_flush(code, 8);
    b = fn(0);
    finish(r, a == 42 && b == 43, "code at %p, returned %u then %u (want 42, 43)", (void *)code, a, b);
}

static u8 *s_window;
static u32 s_windowSize;

static void test_reserve(DiagResult *r)
{
    static const u32 sizes[] = { 0x20000000u, 0x18000000u, 0x10000000u };
    u32 i;
    for (i = 0; i < EMU_ARRAY_COUNT(sizes); i++)
    {
        s_window = (u8 *)VirtualAlloc(NULL, sizes[i], MEM_RESERVE, PAGE_READWRITE);
        if (s_window)
        {
            s_windowSize = sizes[i];
            break;
        }
        LOG_INFO("[diag] reserve %u MB failed (error %u)", sizes[i] >> 20, (u32)GetLastError());
    }
    if (!s_window)
    {
        finish(r, 0, "no reservation of 256 MB or more succeeded");
        return;
    }
    {
        u8 *ram = (u8 *)VirtualAlloc(s_window + 0x0C000000, 16u << 20, MEM_COMMIT, PAGE_READWRITE);
        u32 k, bad = 0;
        if (!ram)
        {
            finish(r, 0, "reserved %u MB at %p, commit of 16 MB at +0x0C000000 failed (error %u)", s_windowSize >> 20,
                   (void *)s_window, (u32)GetLastError());
            return;
        }
        for (k = 0; k < (16u << 20); k += 4096) *(u32 *)(ram + k) = k ^ 0xA5A5A5A5u;
        for (k = 0; k < (16u << 20); k += 4096) bad += *(u32 *)(ram + k) != (k ^ 0xA5A5A5A5u);
        finish(r, s_windowSize == 0x20000000u && bad == 0, "reserved %u MB at %p, committed 16 MB at %p, %u bad words",
               s_windowSize >> 20, (void *)s_window, (void *)ram, bad);
    }
}

static volatile u32 *fault_address(void)
{
    return (volatile u32 *)(s_window ? s_window : (u8 *)0x10);
}

static u32 s_lastFaultCode, s_lastFaultAddr;

static int note_fault(EXCEPTION_POINTERS *ep)
{
    s_lastFaultCode = ep->ExceptionRecord->ExceptionCode;
    s_lastFaultAddr = ep->ExceptionRecord->NumberParameters >= 2 ? (u32)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

__declspec(noinline) static int c_fault_caught(volatile u32 *p)
{
    __try
    {
        u32 v = *p;
        (void)v;
    }
    __except (note_fault(GetExceptionInformation()))
    {
        return 1;
    }
    return 0;
}

static int resume_filter(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Iar += 4;
    return EXCEPTION_CONTINUE_EXECUTION;
}

__declspec(noinline) static int c_fault_resumed(volatile u32 *p)
{
    volatile int reached = 0;
    __try
    {
        u32 v = *p;
        (void)v;
        reached = 1;
    }
    __except (resume_filter(GetExceptionInformation()))
    {
        return -1;
    }
    return reached;
}

#define GEN_MARKER 0x600DF00Du
static u32 s_stubStart, s_stubEnd;
static volatile LONG s_genResumed;

static int gen_filter(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (c->Iar < s_stubStart || c->Iar >= s_stubEnd) return EXCEPTION_EXECUTE_HANDLER;
    c->Gpr3 = GEN_MARKER;
    c->Iar += 4;
    s_genResumed++;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static GenFn make_load_stub(void)
{
    u32 *code = xb_codemem_base() + 16;
    code[0] = PPC_LWZ_R3_R3;
    code[1] = PPC_BLR;
    xb_codemem_flush(code, 8);
    s_stubStart = (u32)code;
    s_stubEnd = (u32)(code + 2);
    return (GenFn)code;
}

__declspec(noinline) static u32 gen_fault_try(GenFn fn, u32 addr)
{
    __try
    {
        return fn(addr);
    }
    __except (gen_filter(GetExceptionInformation()))
    {
        return 0xFFFFFFFFu;
    }
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep)
{
    return gen_filter(ep) == EXCEPTION_CONTINUE_EXECUTION ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

static double qpc_seconds(LARGE_INTEGER a, LARGE_INTEGER b)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart;
}

static void test_gen_fault(DiagResult *r, DiagResult *cost, int viaUnhandled)
{
    GenFn fn = make_load_stub();
    u32 addr = (u32)fault_address(), v, ok = 0;
    LPTOP_LEVEL_EXCEPTION_FILTER prev = NULL;
    LOG_INFO("[diag] running a fault inside generated code (%s); if the title ends here, it was not catchable",
             viaUnhandled ? "SetUnhandledExceptionFilter" : "__try/__except");
    xb_log_flush();
    s_genResumed = 0;
    if (viaUnhandled)
    {
        prev = SetUnhandledExceptionFilter(unhandled_filter);
        v = fn(addr);
    }
    else
        v = gen_fault_try(fn, addr);
    ok = v == GEN_MARKER && s_genResumed == 1;
    finish(r, ok, "returned %08X (want %08X), resumed %d time(s)", v, GEN_MARKER, (int)s_genResumed);
    if (ok && cost)
    {
        LARGE_INTEGER t0, t1;
        int i, n = 1000;
        QueryPerformanceCounter(&t0);
        for (i = 0; i < n; i++) v = viaUnhandled ? fn(addr) : gen_fault_try(fn, addr);
        QueryPerformanceCounter(&t1);
        finish(cost, v == GEN_MARKER, "%.1f us per caught and resumed fault (%d faults)", qpc_seconds(t0, t1) * 1e6 / n, n);
    }
    if (viaUnhandled) SetUnhandledExceptionFilter(prev);
}

static void test_interp_speed(DiagResult *r)
{
    LARGE_INTEGER t0, t1;
    u32 cycles;
    double secs;
    if (dc_init() != 0)
    {
        finish(r, 0, "dc_init failed");
        return;
    }
    dc_set_bios(NULL, 0, NULL, 0);
    dc_reset();
    sh4_set_sr(0x40000000u);
    QueryPerformanceCounter(&t0);
    cycles = sh4_bench_run(2000000);
    QueryPerformanceCounter(&t1);
    secs = qpc_seconds(t0, t1);
    finish(r, cycles != 0, "%.1f MIPS (%.1f%% of the SH-4's 200 MHz), %u cycles in %.3f s", cycles / secs / 1e6,
           cycles / secs / 2e6, cycles, secs);
}

typedef u32 (*BenchFn)(const float *vals, u32 turns);

static double bench_loop(int variant, u32 turns)
{
    static const float vals[2] = { 1.0f, 2.0f };
    PpcCode c;
    u32 loop, skip;
    BenchFn fn;
    LARGE_INTEGER f, t0, t1;
    c.buf = xb_codemem_base() + 64;
    c.pos = 0;
    c.cap = 900;
    c.overflow = 0;
    ppc_lfs(&c, 1, 0, 3);
    ppc_lfs(&c, 2, 4, 3);
    ppc_li(&c, 8, 1);
    ppc_li(&c, 9, 2);
    ppc_li(&c, 7, 0);
    ppc_mtctr(&c, 4);
    loop = c.pos;
    switch (variant)
    {
    case 0:
        ppc_fcmpu(&c, 0, 2, 1);
        ppc_mfocrf(&c, 5, 0x80);
        ppc_rlwinm(&c, 6, 5, 2, 31, 31);
        ppc_cmpwi(&c, 0, 6, 0);
        skip = ppc_bc_fwd(&c, 12, 0, PPC_EQ);
        ppc_addi(&c, 7, 7, 1);
        ppc_patch_here(&c, skip);
        break;
    case 1:
        ppc_fcmpu(&c, 1, 2, 1);
        skip = ppc_bc_fwd(&c, 4, 1, PPC_GT);
        ppc_addi(&c, 7, 7, 1);
        ppc_patch_here(&c, skip);
        break;
    case 2:
        ppc_cmpw(&c, 0, 9, 8);
        skip = ppc_bc_fwd(&c, 4, 0, PPC_GT);
        ppc_addi(&c, 7, 7, 1);
        ppc_patch_here(&c, skip);
        break;
    default:
        ppc_fcmpu(&c, 0, 2, 1);
        ppc_mfocrf(&c, 5, 0x80);
        ppc_rlwinm(&c, 6, 5, 2, 31, 31);
        ppc_add(&c, 7, 7, 6);
        break;
    }
    ppc_put(&c, (16u << 26) | (16u << 21) | ((u32)((s32)(loop - c.pos) * 4) & 0xFFFCu));
    ppc_mr(&c, 3, 7);
    ppc_blr(&c);
    xb_codemem_flush(c.buf, c.pos * 4);
    fn = (BenchFn)(void *)c.buf;
    fn(vals, 1000);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    fn(vals, turns);
    QueryPerformanceCounter(&t1);
    return (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart * 3.2e9 / (double)turns;
}

void xb_fpu_bench(void)
{
    static const char *const names[4] = { "fcmpu+mfocrf+cmpwi+branch", "fcmpu+branch", "cmpw+branch", "fcmpu+mfocrf (no branch)" };
    int v, k;
    for (k = 0; k < 2; k++)
        for (v = 0; v < 4; v++) LOG_INFO("[bench] %-28s %.1f cycles a turn", names[v], bench_loop(v, 20000000));
}

int xb_diag_run(int level, DiagResult *out, int max)
{
    int n = 0, c;
    LOG_INFO("[diag] level %d", level);
    test_codemem(add_result(out, &n, max, "Code in .jitc runs"));
    test_reserve(add_result(out, &n, max, "Reserve 512 MB, commit RAM"));

    c = c_fault_caught(fault_address());
    finish(add_result(out, &n, max, "Fault caught in C"), c == 1, "caught=%d code %08X address %08X", c,
           s_lastFaultCode, s_lastFaultAddr);
    c = c_fault_resumed(fault_address());
    finish(add_result(out, &n, max, "Fault resumed in C"), c == 1, "result %d (1 resumed, -1 handler, 0 no fault)", c);

    test_interp_speed(add_result(out, &n, max, "SH-4 interpreter speed"));

    if (level >= 2)
    {
        DiagResult *r = add_result(out, &n, max, level == 3 ? "JIT fault, unhandled filter" : "JIT fault, __try in caller");
        DiagResult *cost = add_result(out, &n, max, "Cost of one fault");
        test_gen_fault(r, cost, level == 3);
        if (cost && cost->ok < 0) finish(cost, -1, "skipped: the fault was not resumed");
    }

    if (s_window)
    {
        VirtualFree(s_window, 0, MEM_RELEASE);
        s_window = NULL;
    }
    xb_log_flush();
    return n;
}
