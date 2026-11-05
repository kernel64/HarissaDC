// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "sh4_mmu.h"
#include "sh4_internal.h"
#include "sh4_mmr.h"
#include "sh4_jit.h"

#include <stddef.h>
#if defined(_XBOX)
#include <ppcintrinsics.h>
#elif defined(_MSC_VER)
#include <intrin.h>
#endif
#include <string.h>

#include "../core/log.h"
#include "../core/state.h"
#include "../core/sched.h"
#include "../mem/mem.h"

int g_mmu_on;

#define PTEL_V 0x100u
#define PTEL_D 0x004u
#define PTEL_SH 0x002u
#define MMUCR_AT 0x001u
#define MMUCR_TI 0x004u
#define MMUCR_SV 0x100u
#define MMUCR_SQMD 0x200u

struct TlbEntry
{
    u32 pteh, ptel, ptea;
};

static TlbEntry s_utlb[64], s_itlb[4];

#define EXC_TLB_MISS_READ 0x040u
#define EXC_TLB_MISS_WRITE 0x060u
#define EXC_INITIAL_WRITE 0x080u
#define EXC_PROT_READ 0x0A0u
#define EXC_PROT_WRITE 0x0C0u
#define EXC_ADDR_READ 0x0E0u
#define EXC_ADDR_WRITE 0x100u

#define TC_SIZE 4096
struct TcEntry
{
    u32 tag;
    u32 phys;
    u32 ptel;
    u32 flags;
    u32 tagRead, tagWrite;
    u32 ramDelta, pad;
};
static TcEntry s_tc[TC_SIZE];
typedef char TcEntrySize[sizeof(TcEntry) == MMU_TC_ENTRY && offsetof(TcEntry, tagRead) == MMU_TC_OFF_TAG_READ &&
                         offsetof(TcEntry, tagWrite) == MMU_TC_OFF_TAG_WRITE && offsetof(TcEntry, ramDelta) == MMU_TC_OFF_RAM_DELTA ? 1 : -1];

static u32 tc_tag(u32 va, int md)
{
    return ((((va >> 10) << 1) | (u32)md) + 1) | (g_ccn.pteh << 24);
}

#define TC_TRACK 16
static u16 s_tcSlots[64][TC_TRACK];
static u8 s_tcCount[64];

static void tc_flush(void)
{
    memset(s_tc, 0, sizeof(s_tc));
    memset(s_tcCount, 0, sizeof(s_tcCount));
}

const void *sh4_mmu_tc(void)
{
    return s_tc;
}

static u32 page_mask(u32 ptel)
{
    switch (((ptel >> 6) & 2) | ((ptel >> 4) & 1))
    {
    case 0: return 0x3FFu;
    case 1: return 0xFFFu;
    case 2: return 0xFFFFu;
    default: return 0xFFFFFu;
    }
}

static int entry_matches(const TlbEntry *e, u32 va, int md)
{
    u32 m;
    if (!(e->ptel & PTEL_V)) return 0;
    m = page_mask(e->ptel);
    if ((e->pteh ^ va) & ~m & 0xFFFFFC00u) return 0;
    if (e->ptel & PTEL_SH) return 1;
    if (md && (g_ccn.mmucr & MMUCR_SV)) return 1;
    return ((e->pteh ^ g_ccn.pteh) & 0xFF) == 0;
}

static u32 s_uMask[64], s_uVpn[64], s_uAsid[64];

#define UTLB_BUCKETS 64
static u64 s_uBuckets[UTLB_BUCKETS], s_uLarge;
static int s_uBucket[64];

static u32 utlb_bucket_of(u32 va)
{
    return (va >> 12) & (UTLB_BUCKETS - 1);
}

static u32 utlb_first(u64 m)
{
#if defined(_XBOX)
    return (u32)_CountLeadingZeros64(m);
#elif defined(_MSC_VER)
    unsigned long b;
    _BitScanReverse64(&b, m);
    return 63u - (u32)b;
#else
    return (u32)__builtin_clzll(m);
#endif
}

static void utlb_index(u32 i)
{
    const TlbEntry *e = &s_utlb[i];
    u64 bit = 1ull << (63 - i);
    int b = -1;
    if (s_uBucket[i] == UTLB_BUCKETS) s_uLarge &= ~bit;
    else if (s_uBucket[i] >= 0) s_uBuckets[s_uBucket[i]] &= ~bit;
    if (!(e->ptel & PTEL_V))
    {
        s_uMask[i] = 0;
        s_uVpn[i] = 1;
        s_uAsid[i] = 0x100;
    }
    else
    {
        u32 m = page_mask(e->ptel);
        s_uMask[i] = ~m & 0xFFFFFC00u;
        s_uVpn[i] = e->pteh & s_uMask[i];
        s_uAsid[i] = (e->ptel & PTEL_SH) ? 0x100 : (e->pteh & 0xFF);
        b = m > 0xFFFu ? UTLB_BUCKETS : (int)utlb_bucket_of(s_uVpn[i]);
    }
    s_uBucket[i] = b;
    if (b == UTLB_BUCKETS) s_uLarge |= bit;
    else if (b >= 0) s_uBuckets[b] |= bit;
}

static void utlb_index_all(void)
{
    u32 i;
    memset(s_uBuckets, 0, sizeof(s_uBuckets));
    s_uLarge = 0;
    for (i = 0; i < 64; i++)
    {
        s_uBucket[i] = -1;
        utlb_index(i);
    }
}

static int utlb_find(u32 va, int md)
{
    u32 i, asid = (md && (g_ccn.mmucr & MMUCR_SV)) ? 0x200 : (g_ccn.pteh & 0xFF);
    u64 m = s_uBuckets[utlb_bucket_of(va)] | s_uLarge;
    for (; m; m &= ~(1ull << (63 - i)))
    {
        i = utlb_first(m);
        if ((va & s_uMask[i]) == s_uVpn[i] && (asid == 0x200 || s_uAsid[i] == 0x100 || s_uAsid[i] == asid))
            return (int)i;
    }
    return -1;
}

static const TlbEntry *utlb_search(u32 va, int md)
{
    int i = utlb_find(va, md);
    return i >= 0 ? &s_utlb[i] : NULL;
}

static void fault(u32 va, u32 expevt, u32 vector)
{
    g_ccn.tea = va;
    g_ccn.pteh = (g_ccn.pteh & 0xFF) | (va & 0xFFFFFC00u);
    sh4_abort_exception(expevt, vector);
}

static u32 tlb_translate(u32 va, int access, int md);

u32 sh4_mmu_translate(u32 va, int access)
{
    int md = (g_sh4.sr & SR_MD) != 0, write = access == MMU_WRITE;
    if (va >= 0x80000000u && (va >> 29) != 6)
    {
        if (!md && !((va >> 26) == 0x38 && !(g_ccn.mmucr & MMUCR_SQMD)))
        {
            g_ccn.tea = va;
            sh4_abort_exception(write ? EXC_ADDR_WRITE : EXC_ADDR_READ, 0x100);
        }
        return va;
    }
    if ((va >> 26) == 0x1F && (g_ccn.ccr & 0x20)) return va;
    return tlb_translate(va, access, md);
}

u32 sh4_mmu_translate_sq(u32 va)
{
    int md = (g_sh4.sr & SR_MD) != 0;
    if (!md && (g_ccn.mmucr & MMUCR_SQMD))
    {
        g_ccn.tea = va;
        sh4_abort_exception(EXC_ADDR_WRITE, 0x100);
    }
    return tlb_translate(va & ~31u, MMU_WRITE, md);
}

static void tc_fill(TcEntry *c, const TlbEntry *e, u32 va, int md, u32 tag);
static int wince_refill(u32 va, int write);

int sh4_mmu_translate_quiet(u32 va, int access, u32 *pa)
{
    int md = (g_sh4.sr & SR_MD) != 0, write = access == MMU_WRITE;
    u32 tag, ptel, pr;
    TcEntry *c;
    if (!g_mmu_on || (md && va >= 0x80000000u && (va >> 29) != 6))
    {
        *pa = va;
        return 1;
    }
    if (va >= 0x80000000u && (va >> 29) != 6)
    {
        if ((va >> 26) != 0x38 || (g_ccn.mmucr & MMUCR_SQMD)) return 0;
        *pa = va;
        return 1;
    }
    if ((va >> 26) == 0x1F && (g_ccn.ccr & 0x20))
    {
        *pa = va;
        return 1;
    }
    tag = tc_tag(va, md);
    c = &s_tc[(va >> 10) & (TC_SIZE - 1)];
    if (c->tag != tag)
    {
        const TlbEntry *e = utlb_search(va, md);
        if (!e && wince_refill(va, write)) e = utlb_search(va, md);
        if (!e) return 0;
        tc_fill(c, e, va, md, tag);
    }
    ptel = c->ptel;
    pr = (ptel >> 5) & 3;
    if (access == MMU_FETCH)
    {
        if (!md && !(pr & 2)) return 0;
    }
    else if ((!md && !(pr & 2)) || (write && !(pr & 1)) || (write && !(ptel & PTEL_D)))
        return 0;
    *pa = c->phys | (va & 0x3FFu);
    return 1;
}

int sh4_mmu_translate_sq_quiet(u32 va, u32 *pa)
{
    int md = (g_sh4.sr & SR_MD) != 0;
    u32 tag, ptel, pr;
    TcEntry *c;
    if (!md && (g_ccn.mmucr & MMUCR_SQMD)) return 0;
    va &= ~31u;
    tag = tc_tag(va, md);
    c = &s_tc[(va >> 10) & (TC_SIZE - 1)];
    if (c->tag != tag)
    {
        const TlbEntry *e = utlb_search(va, md);
        if (!e) return 0;
        tc_fill(c, e, va, md, tag);
    }
    ptel = c->ptel;
    pr = (ptel >> 5) & 3;
    if ((!md && !(pr & 2)) || !(pr & 1) || !(ptel & PTEL_D)) return 0;
    *pa = c->phys | (va & 0x3FFu);
    return 1;
}

static void tc_fill(TcEntry *c, const TlbEntry *e, u32 va, int md, u32 tag)
{
    u32 m = page_mask(e->ptel), epr = (e->ptel >> 5) & 3, f = 0;
    if (e >= s_utlb && e < s_utlb + 64)
    {
        u32 i = (u32)(e - s_utlb), slot = (u32)(c - s_tc), k, n = s_tcCount[i];
        if (n <= TC_TRACK)
        {
            for (k = 0; k < n && s_tcSlots[i][k] != slot; k++) {}
            if (k == n)
            {
                if (n < TC_TRACK) s_tcSlots[i][n] = (u16)slot;
                s_tcCount[i] = (u8)(n + 1);
            }
        }
    }
    c->tag = tag;
    c->phys = ((e->ptel & 0x1FFFFC00u & ~m) | (va & m)) & ~0x3FFu;
    c->ptel = e->ptel;
    if (md || (epr & 2)) f |= MMU_TC_READ;
    if ((f & MMU_TC_READ) && (epr & 1) && (e->ptel & PTEL_D)) f |= MMU_TC_WRITE;
    if ((c->phys & 0x1C000000u) == 0x0C000000u) f |= MMU_TC_RAM;
    c->flags = f;
    c->tagRead = (f & (MMU_TC_READ | MMU_TC_RAM)) == (MMU_TC_READ | MMU_TC_RAM) ? tag : 0;
    c->tagWrite = (f & (MMU_TC_WRITE | MMU_TC_RAM)) == (MMU_TC_WRITE | MMU_TC_RAM) && !sh4_jit_ram_has_code(c->phys) ? tag : 0;
    c->ramDelta = (c->phys & 0x00FFFFFFu) - (va & ~0x3FFu);
}

int g_sh4_wince_tlb = 1;

static const u16 s_wceTlbMiss[40] = {
    0x5173, 0x5072, 0x4111, 0x8B23, 0x4119, 0x6213, 0x4129, 0x4101, 0x4108, 0x001E,
    0x6123, 0x932B, 0x4119, 0x2139, 0x4108, 0x011E, 0x5345, 0x4111, 0x8D31, 0x6023,
    0x5210, 0x710C, 0x2328, 0x892C, 0x4009, 0xC93C, 0x001E, 0xC801, 0x8927, 0x6103,
    0x70FF, 0xE3E3, 0x413D, 0x171D, 0x1701, 0x0038, 0x0009, 0x0009, 0x002B, 0x0009,
};
static void ldtlb_at(u64 now);
static u32 s_wceVbr = 1;
static u64 s_wceSlice, s_wceStep;
static int s_wceHandler;

static int kernel_word(u32 addr, u32 *v)
{
    if ((addr >> 30) != 2 || (addr & 3)) return 0;
    if ((addr & 0x1C000000u) == 0x0C000000u) *v = buf_r32(g_ram, addr & (RAM_SIZE - 1));
    else *v = mem_read32(addr);
    return 1;
}

static int wince_refill(u32 va, int write)
{
    u32 *bank, section, block, lock, mask, entry, k;
    if (!g_sh4_wince_tlb || (g_sh4.sr & (SR_BL | SR_RB)) || (va >> 31)) return 0;
    if (g_sh4.vbr != s_wceVbr)
    {
        u32 h = g_sh4.vbr + 0x400;
        s_wceVbr = g_sh4.vbr;
        s_wceHandler = (h >> 30) == 2 && mem_read16(h + 0x70) == 0x01FF;
        for (k = 0; k < 40 && s_wceHandler; k++)
            if (mem_read16(h + k * 2) != s_wceTlbMiss[k]) s_wceHandler = 0;
    }
    if (!s_wceHandler) return 0;
    bank = g_sh4.rbank;
    if (bank[7] != 0xFF000000u) return 0;
    if (!kernel_word(g_ccn.ttb + (va >> 25) * 4, &section)) return 0;
    if (!kernel_word(section + ((va >> 16) & 0x1FF) * 4, &block) || !(block >> 31)) return 0;
    if (!kernel_word(block, &lock) || !kernel_word(bank[4] + 20, &mask) || !(lock & mask)) return 0;
    if (!kernel_word(block + 12 + ((va >> 12) & 15) * 4, &entry) || !(entry & 1)) return 0;
    g_ccn.tea = va;
    g_ccn.pteh = (g_ccn.pteh & 0xFF) | (va & 0xFFFFFC00u);
    g_ccn.expevt = write ? EXC_TLB_MISS_WRITE : EXC_TLB_MISS_READ;
    g_ccn.ptea = entry >> 29;
    g_ccn.ptel = entry - 1;
    bank[0] = entry - 1;
    bank[1] = entry >> 29;
    bank[2] = lock;
    bank[3] = mask;
    {
        u64 now = sched_now();
        s_wceStep = now == s_wceSlice ? s_wceStep + 31 : 0;
        s_wceSlice = now;
        ldtlb_at(now + s_wceStep);
    }
    return 1;
}

static u32 tlb_translate(u32 va, int access, int md)
{
    int write = access == MMU_WRITE;
    u32 tag, pr, ptel, phys;
    TcEntry *c;
    tag = tc_tag(va, md);
    c = &s_tc[(va >> 10) & (TC_SIZE - 1)];
    if (c->tag != tag)
    {
        const TlbEntry *e = utlb_search(va, md);
        if (!e && wince_refill(va, write)) e = utlb_search(va, md);
        if (!e)
        {
            fault(va, write ? EXC_TLB_MISS_WRITE : EXC_TLB_MISS_READ, 0x400);
            return 0;
        }
        tc_fill(c, e, va, md, tag);
    }
    ptel = c->ptel;
    pr = (ptel >> 5) & 3;
    if (access == MMU_FETCH)
    {
        if (!md && !(pr & 2)) fault(va, EXC_PROT_READ, 0x100);
    }
    else
    {
        if ((!md && !(pr & 2)) || (write && !(pr & 1))) fault(va, write ? EXC_PROT_WRITE : EXC_PROT_READ, 0x100);
        if (write && !(ptel & PTEL_D)) fault(va, EXC_INITIAL_WRITE, 0x100);
    }
    phys = c->phys | (va & 0x3FFu);
    return phys;
}

u32 sh4_mmu_peek(u32 va, int *ok)
{
    u32 i;
    *ok = 1;
    if (!g_mmu_on || (va >= 0x80000000u && (va >> 29) != 6) || ((va >> 26) == 0x1F && (g_ccn.ccr & 0x20))) return va;
    {
        int md = (g_sh4.sr & SR_MD) != 0;
        u32 tag = tc_tag(va, md);
        TcEntry *c = &s_tc[(va >> 10) & (TC_SIZE - 1)];
        if (c->tag == tag) return c->phys | (va & 0x3FFu);
        int k = utlb_find(va, md);
        if (k >= 0)
        {
            tc_fill(c, &s_utlb[k], va, md, tag);
            return c->phys | (va & 0x3FFu);
        }
    }
    i = (u32)utlb_find(va, 1);
    if ((int)i >= 0)
    {
        u32 m = page_mask(s_utlb[i].ptel);
        return (s_utlb[i].ptel & 0x1FFFFC00u & ~m) | (va & m);
    }
    *ok = 0;
    return va;
}

int sh4_mmu_fetch_refill(u32 va)
{
    if (!g_mmu_on || va >= 0x80000000u || utlb_find(va, (g_sh4.sr & SR_MD) != 0) >= 0) return 0;
    return wince_refill(va, 0);
}

void sh4_mmu_code_changed(u32 off)
{
    u32 i;
    for (i = 0; i < TC_SIZE; i++)
    {
        TcEntry *c = &s_tc[i];
        if (!c->tag || !(c->flags & MMU_TC_RAM)) continue;
        if (off != ~0u && ((c->phys ^ off) & 0x00FFF000u)) continue;
        c->tagWrite = (c->flags & MMU_TC_WRITE) && !sh4_jit_ram_has_code(c->phys) ? c->tag : 0;
    }
}

void sh4_mmu_reset(void)
{
    memset(s_utlb, 0, sizeof(s_utlb));
    memset(s_itlb, 0, sizeof(s_itlb));
    utlb_index_all();
    g_mmu_on = 0;
    tc_flush();
}

void sh4_mmu_write_mmucr(u32 value)
{
    u32 i;
    if (value & MMUCR_TI)
    {
        for (i = 0; i < 64; i++) s_utlb[i].ptel &= ~PTEL_V;
        for (i = 0; i < 4; i++) s_itlb[i].ptel &= ~PTEL_V;
        utlb_index_all();
    }
    g_ccn.mmucr = value & ~MMUCR_TI;
    g_sh4.jitSqKey = (g_ccn.mmucr & MMUCR_SQMD) ? ~0u : 0x38u;
    if ((value & MMUCR_AT) != (u32)g_mmu_on)
    {
        LOG_INFO("[sh4] MMU %s (MMUCR %08X)", value & MMUCR_AT ? "on" : "off", value);
        g_mmu_on = (int)(value & MMUCR_AT);
        sh4_break_slice();
    }
    tc_flush();
}

void sh4_mmu_write_pteh(u32 value)
{
    g_ccn.pteh = value;
    g_sh4.jitAsidTag = value << 24;
}

static void tc_drop(u32 pteh, u32 ptel)
{
    u32 m = page_mask(ptel), va = pteh & ~m & 0xFFFFFC00u, n = (m + 1) >> 10, i;
    if (!(ptel & PTEL_V)) return;
    if (n >= TC_SIZE)
    {
        tc_flush();
        return;
    }
    for (i = 0; i < n; i++, va += 0x400u)
    {
        TcEntry *c = &s_tc[(va >> 10) & (TC_SIZE - 1)];
        if (c->tag && (((c->tag & 0xFFFFFFu) - 1) >> 1) == (va >> 10)) memset(c, 0, sizeof(*c));
    }
}

static u64 s_urcAt;

static int utlb_overlaps(u32 i)
{
    u64 m;
    u32 k;
    if (s_uBucket[i] < 0) return 0;
    if (s_uBucket[i] == UTLB_BUCKETS) return 1;
    m = (s_uBuckets[s_uBucket[i]] | s_uLarge) & ~(1ull << (63 - i));
    for (; m; m &= ~(1ull << (63 - k)))
    {
        k = utlb_first(m);
        if ((s_uVpn[i] & s_uMask[k]) == s_uVpn[k] || (s_uVpn[k] & s_uMask[i]) == s_uVpn[i]) return 1;
    }
    return 0;
}

static void tc_drop_entry(u32 i)
{
    const TlbEntry *e = &s_utlb[i];
    u32 n = s_tcCount[i], k;
    s_tcCount[i] = 0;
    if (n > TC_TRACK)
    {
        tc_drop(e->pteh, e->ptel);
        return;
    }
    if (!(e->ptel & PTEL_V)) return;
    {
        u32 m = page_mask(e->ptel), base = (e->pteh & ~m & 0xFFFFFC00u) >> 10, pages = (m + 1) >> 10;
        for (k = 0; k < n; k++)
        {
            TcEntry *c = &s_tc[s_tcSlots[i][k]];
            if (c->tag && ((((c->tag & 0xFFFFFFu) - 1) >> 1) - base) < pages) memset(c, 0, sizeof(*c));
        }
    }
}

static void ldtlb_at(u64 now)
{
    u32 urc = (g_ccn.mmucr >> 10) & 63, urb = (g_ccn.mmucr >> 18) & 63;
    TlbEntry *e;
    urc = (u32)((urc + (now - s_urcAt)) % (urb ? urb : 64));
    s_urcAt = now;
    g_ccn.mmucr = (g_ccn.mmucr & ~(63u << 10)) | (urc << 10);
    e = &s_utlb[urc];
    tc_drop_entry(urc);
    e->pteh = g_ccn.pteh;
    e->ptel = g_ccn.ptel;
    e->ptea = g_ccn.ptea;
    utlb_index(urc);
    if (utlb_overlaps(urc)) tc_drop(e->pteh, e->ptel);
}

void sh4_mmu_ldtlb(void)
{
    ldtlb_at(sched_now_exact());
}

static void array_assoc(TlbEntry *t, u32 n, u32 value, int withD)
{
    u32 i;
    for (i = 0; i < n; i++)
    {
        TlbEntry *e = &t[i];
        u32 m = page_mask(e->ptel);
        if (!(e->ptel & PTEL_V)) continue;
        if ((e->pteh ^ value) & ~m & 0xFFFFFC00u) continue;
        if (!(e->ptel & PTEL_SH) && ((e->pteh ^ g_ccn.pteh) & 0xFF)) continue;
        e->ptel = (e->ptel & ~PTEL_V) | (value & PTEL_V);
        if (withD) e->ptel = (e->ptel & ~PTEL_D) | ((value >> 7) & PTEL_D);
    }
}

u32 sh4_mmu_array_read(u32 addr)
{
    int utlb = (addr >> 24) >= 0xF6;
    TlbEntry *e = utlb ? &s_utlb[(addr >> 8) & 63] : &s_itlb[(addr >> 8) & 3];
    if (((addr >> 24) & 1) == 0)
        return (e->pteh & 0xFFFFFCFFu) | (e->ptel & PTEL_V) | (utlb ? (e->ptel & PTEL_D) << 7 : 0);
    if (addr & 0x00800000u) return e->ptea & 0xF;
    return e->ptel & (utlb ? 0x1FFFFDFFu : 0x1FFFFDFAu);
}

void sh4_mmu_array_write(u32 addr, u32 value)
{
    int utlb = (addr >> 24) >= 0xF6;
    TlbEntry *t = utlb ? s_utlb : s_itlb;
    TlbEntry *e = utlb ? &s_utlb[(addr >> 8) & 63] : &s_itlb[(addr >> 8) & 3];
    if (((addr >> 24) & 1) == 0)
    {
        if (addr & 0x80)
        {
            array_assoc(t, utlb ? 64 : 4, value, utlb);
            if (utlb) array_assoc(s_itlb, 4, value, 0);
        }
        else
        {
            e->pteh = value & 0xFFFFFCFFu;
            e->ptel = (e->ptel & ~PTEL_V) | (value & PTEL_V);
            if (utlb) e->ptel = (e->ptel & ~PTEL_D) | ((value >> 7) & PTEL_D);
        }
    }
    else if (addr & 0x00800000u)
        e->ptea = value & 0xF;
    else
        e->ptel = value & 0x1FFFFDFFu;
    utlb_index_all();
    tc_flush();
}

void sh4_mmu_state(StateIO *io)
{
    state_tag(io, "MMU ");
    STATE(io, s_utlb);
    STATE(io, s_itlb);
    STATE(io, g_mmu_on);
    if (io->loading)
    {
        utlb_index_all();
        tc_flush();
    }
}
