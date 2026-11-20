// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "cheat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/log.h"
#include "../mem/mem.h"

enum
{
    OP_SET = 1,
    OP_INC,
    OP_DEC,
    OP_IF_EQ,
    OP_IF_NE,
    OP_IF_LT,
    OP_IF_GT
};

struct CheatOp
{
    u8 kind, bits, bigEndian;
    u8 mask;
    u32 addr, value;
    u32 repeat, addAddr, addValue;
};

struct Cheat
{
    char name[96];
    int first, count;
    u8 on, supported;
};

struct CheatList
{
    Cheat *cheats;
    int count, cap;
    CheatOp *ops;
    int opCount, opCap;
    CheatMem mem;
};

#define RAM_MASK (RAM_SIZE - 1)

static u32 ram_read(u32 off, int size)
{
    u32 a = 0x8C000000u | (off & RAM_MASK);
    return size == 1 ? mem_read8(a) : size == 2 ? mem_read16(a) : mem_read32(a);
}

static void ram_write(u32 off, u32 value, int size)
{
    u32 a = 0x8C000000u | (off & RAM_MASK);
    if (size == 1) mem_write8(a, (u8)value);
    else if (size == 2) mem_write16(a, (u16)value);
    else mem_write32(a, value);
}

CheatList *cheat_new(void)
{
    CheatList *l = (CheatList *)calloc(1, sizeof(CheatList));
    if (l)
    {
        l->mem.read = ram_read;
        l->mem.write = ram_write;
    }
    return l;
}

void cheat_free(CheatList *l)
{
    if (!l) return;
    free(l->cheats);
    free(l->ops);
    free(l);
}

void cheat_set_mem(CheatList *l, const CheatMem *m)
{
    l->mem.read = m ? m->read : ram_read;
    l->mem.write = m ? m->write : ram_write;
}

static Cheat *add_cheat(CheatList *l, const char *name, int nameLen)
{
    Cheat *c;
    if (l->count == l->cap)
    {
        int cap = l->cap ? l->cap * 2 : 32;
        Cheat *n = (Cheat *)realloc(l->cheats, cap * sizeof(Cheat));
        if (!n) return NULL;
        l->cheats = n;
        l->cap = cap;
    }
    c = &l->cheats[l->count++];
    memset(c, 0, sizeof(*c));
    if (nameLen < 0) nameLen = (int)strlen(name);
    if (nameLen > (int)sizeof(c->name) - 1) nameLen = (int)sizeof(c->name) - 1;
    memcpy(c->name, name, nameLen);
    c->name[nameLen] = 0;
    c->first = l->opCount;
    c->supported = 1;
    return c;
}

static CheatOp *add_op(CheatList *l)
{
    CheatOp *o;
    if (l->opCount == l->opCap)
    {
        int cap = l->opCap ? l->opCap * 2 : 64;
        CheatOp *n = (CheatOp *)realloc(l->ops, cap * sizeof(CheatOp));
        if (!n) return NULL;
        l->ops = n;
        l->opCap = cap;
    }
    o = &l->ops[l->opCount++];
    memset(o, 0, sizeof(*o));
    o->repeat = 1;
    o->bits = 8;
    if (l->count) l->cheats[l->count - 1].count++;
    return o;
}

static const char *line_end(const char *p, const char *end)
{
    while (p < end && *p != '\n' && *p != '\r') p++;
    return p;
}

static const char *skip_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static u32 hex_value(const char *p, int n)
{
    u32 v = 0;
    int i;
    for (i = 0; i < n; i++)
    {
        char c = p[i];
        v = (v << 4) | (u32)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

static u32 parse_u32(const char *p, const char *end, int hex)
{
    u32 v = 0;
    p = skip_space(p, end);
    if (!hex && p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        hex = 1;
        p += 2;
    }
    for (; p < end; p++)
    {
        char c = *p;
        if (hex && is_hex(c)) v = (v << 4) | hex_value(p, 1);
        else if (!hex && c >= '0' && c <= '9') v = v * 10 + (u32)(c - '0');
        else break;
    }
    return v;
}

static int ends_with_continued(const char *name)
{
    static const char tail[] = "(Continued)";
    size_t n = strlen(name), t = sizeof(tail) - 1;
    while (n && name[n - 1] == ' ') n--;
    return n >= t && !memcmp(name + n - t, tail, t);
}

struct ChtEntry
{
    char desc[96];
    u32 address, value, type, size, mask, repeat, addAddr, addValue;
    int bigEndian, handler, hasCode, seen;
};

static void cht_field(ChtEntry *e, const char *key, int keyLen, const char *v, const char *vend)
{
#define KEY(s) (keyLen == (int)sizeof(s) - 1 && !memcmp(key, s, sizeof(s) - 1))
    e->seen = 1;
    if (KEY("desc"))
    {
        int n = (int)(vend - v);
        if (n > (int)sizeof(e->desc) - 1) n = (int)sizeof(e->desc) - 1;
        memcpy(e->desc, v, n);
        e->desc[n] = 0;
    }
    else if (KEY("address")) e->address = parse_u32(v, vend, 0);
    else if (KEY("value")) e->value = parse_u32(v, vend, 0);
    else if (KEY("cheat_type")) e->type = parse_u32(v, vend, 0);
    else if (KEY("memory_search_size")) e->size = parse_u32(v, vend, 0);
    else if (KEY("address_bit_position")) e->mask = parse_u32(v, vend, 0);
    else if (KEY("repeat_count")) e->repeat = parse_u32(v, vend, 0);
    else if (KEY("repeat_add_to_address")) e->addAddr = parse_u32(v, vend, 0);
    else if (KEY("repeat_add_to_value")) e->addValue = parse_u32(v, vend, 0);
    else if (KEY("big_endian")) e->bigEndian = vend - v >= 4 && !memcmp(v, "true", 4);
    else if (KEY("handler")) e->handler = (int)parse_u32(v, vend, 0);
    else if (KEY("code")) e->hasCode = vend > v;
#undef KEY
}

static int cht_add(CheatList *l, const ChtEntry *e)
{
    CheatOp *o;
    int made = 0;
    if (!e->seen) return 0;
    if (!l->count || !ends_with_continued(e->desc) || l->cheats[l->count - 1].count == 0)
    {
        if (!add_cheat(l, e->desc[0] ? e->desc : "?", -1)) return 0;
        made = 1;
    }
    if (e->handler == 0 && e->hasCode)
    {
        l->cheats[l->count - 1].supported = 0;
        return made;
    }
    if (e->type < OP_SET || e->type > OP_IF_GT || e->size > 5)
    {
        l->cheats[l->count - 1].supported = e->type == 0 ? l->cheats[l->count - 1].supported : 0;
        return made;
    }
    o = add_op(l);
    if (!o) return made;
    o->kind = (u8)e->type;
    o->bits = (u8)(1u << e->size);
    o->mask = (u8)(e->mask ? e->mask : 0xFF);
    o->addr = e->address;
    o->value = e->value;
    o->repeat = e->repeat;
    o->addAddr = e->addAddr;
    o->addValue = e->addValue;
    o->bigEndian = (u8)e->bigEndian;
    if (o->addr >= RAM_SIZE) l->cheats[l->count - 1].supported = 0;
    return made;
}

int cheat_load_cht(CheatList *l, const char *text, u32 len)
{
    const char *p = text, *end = text + len;
    ChtEntry *entries = NULL;
    int n = 0, i, made = 0;
    while (p < end)
    {
        const char *e = line_end(p, end), *k = skip_space(p, end);
        if (e - k > 6 && !memcmp(k, "cheats", 6) && !(k[6] >= '0' && k[6] <= '9'))
        {
            const char *eq = (const char *)memchr(k, '=', e - k);
            if (eq)
            {
                const char *v = skip_space(eq + 1, e);
                if (v < e && *v == '"') v++;
                n = (int)parse_u32(v, e, 0);
            }
        }
        p = e + 1;
    }
    if (n <= 0 || n > 10000) return 0;
    entries = (ChtEntry *)calloc(n, sizeof(ChtEntry));
    if (!entries) return 0;
    for (i = 0; i < n; i++)
    {
        entries[i].repeat = 1;
        entries[i].handler = 1;
        entries[i].size = 3;
    }
    for (p = text; p < end;)
    {
        const char *e = line_end(p, end), *k = skip_space(p, end);
        if (e - k > 5 && !memcmp(k, "cheat", 5) && k[5] >= '0' && k[5] <= '9')
        {
            const char *q = k + 5, *eq, *key, *v, *vend;
            u32 idx = 0;
            while (q < e && *q >= '0' && *q <= '9') idx = idx * 10 + (u32)(*q++ - '0');
            eq = (const char *)memchr(q, '=', e - q);
            if (eq && *q == '_' && idx < (u32)n)
            {
                const char *kend = eq;
                key = q + 1;
                while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) kend--;
                v = skip_space(eq + 1, e);
                vend = e;
                while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t')) vend--;
                if (v < vend && *v == '"') v++;
                if (vend > v && vend[-1] == '"') vend--;
                cht_field(&entries[idx], key, (int)(kend - key), v, vend);
            }
        }
        p = e + 1;
    }
    for (i = 0; i < n; i++) made += cht_add(l, &entries[i]);
    free(entries);
    for (i = l->count - made; i < l->count; i++)
        if (!l->cheats[i].count) l->cheats[i].supported = 0;
    return made;
}

static int code_words(const char *p, const char *end, u32 *words, int max)
{
    int n = 0;
    for (;;)
    {
        p = skip_space(p, end);
        if (p >= end || *p == '#' || *p == ';') break;
        if (end - p < 8 || n == max) return 0;
        {
            int i;
            for (i = 0; i < 8; i++)
                if (!is_hex(p[i])) return 0;
        }
        if (p + 8 < end && p[8] != ' ' && p[8] != '\t' && p[8] != '#' && p[8] != ';') return 0;
        words[n++] = hex_value(p, 8);
        p += 8;
    }
    return n;
}

static int codes_to_ops(CheatList *l, const u32 *w, int n)
{
    int i = 0;
    while (i + 1 < n)
    {
        u32 type = w[i] >> 24, addr = w[i] & 0x00FFFFFF;
        CheatOp *o;
        if (type & 0xF0) return 0;
        if (type <= 2)
        {
            o = add_op(l);
            if (!o) return 0;
            o->kind = OP_SET;
            o->bits = (u8)(8 << type);
            o->mask = 0xFF;
            o->addr = addr;
            o->value = w[i + 1];
            i += 2;
        }
        else if (type == 0x0D)
        {
            static const u8 kinds[4] = { OP_IF_EQ, OP_IF_NE, OP_IF_GT, OP_IF_LT };
            u32 cmp = w[i + 1] >> 16;
            if (cmp > 3) return 0;
            o = add_op(l);
            if (!o) return 0;
            o->kind = kinds[cmp];
            o->bits = 16;
            o->mask = 0xFF;
            o->addr = addr;
            o->value = w[i + 1] & 0xFFFF;
            i += 2;
        }
        else return 0;
    }
    return i == n;
}

static void codes_finish(CheatList *l, const u32 *words, int nw)
{
    Cheat *c = &l->cheats[l->count - 1];
    if (!nw || (nw & 1) || !codes_to_ops(l, words, nw)) c->supported = 0;
}

int cheat_load_codes(CheatList *l, const char *text, u32 len)
{
    const char *p = text, *end = text + len;
    u32 words[256];
    int nw = 0, made = 0, have = 0;
    while (p < end)
    {
        const char *e = line_end(p, end), *k = skip_space(p, end), *ne = e;
        u32 lw[16];
        int n;
        while (ne > k && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
        p = e + 1;
        if (k >= ne || *k == '#' || *k == ';') continue;
        n = code_words(k, ne, lw, 16);
        if (n > 0)
        {
            if (have && nw + n <= (int)(sizeof(words) / sizeof(words[0])))
            {
                memcpy(words + nw, lw, n * sizeof(u32));
                nw += n;
            }
            continue;
        }
        if (have) codes_finish(l, words, nw);
        have = add_cheat(l, k, (int)(ne - k)) != NULL;
        made += have;
        nw = 0;
    }
    if (have) codes_finish(l, words, nw);
    return made;
}

static int product_key(const char *p, const char *e, char *out, int size)
{
    int n = 0;
    for (; p < e && *p; p++)
    {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) continue;
        if (n < size - 1) out[n++] = c;
    }
    out[n] = 0;
    if (n > 2 && out[0] == 'M' && out[1] == 'K' && out[2] >= '0' && out[2] <= '9')
    {
        memmove(out, out + 2, n - 1);
        n -= 2;
    }
    return n;
}

static int product_listed(const char *p, const char *e, const char *key)
{
    while (p < e)
    {
        const char *q = p;
        char k[32];
        while (q < e && *q != ',' && *q != ']') q++;
        if (product_key(p, q, k, sizeof(k)) && !strcmp(k, key)) return 1;
        if (q >= e || *q == ']') break;
        p = q + 1;
    }
    return 0;
}

static void db_ops(CheatList *l, const char *p, const char *e)
{
    while (p < e)
    {
        const char *q = (const char *)memchr(p, ';', e - p);
        u32 f[8] = { 0, 8, 0, 0, 1, 0, 0, 0xFF };
        int k = 0;
        const char *s = p;
        if (!q) q = e;
        while (s < q && k < 8)
        {
            const char *c = (const char *)memchr(s, ',', q - s);
            if (!c) c = q;
            f[k] = parse_u32(s, c, k == 2 || k == 3 || k == 5 || k == 6 || k == 7);
            k++;
            s = c + 1;
        }
        if (k >= 4 && f[0] >= OP_SET && f[0] <= OP_IF_GT && f[2] < RAM_SIZE &&
            (f[1] == 1 || f[1] == 2 || f[1] == 4 || f[1] == 8 || f[1] == 16 || f[1] == 32))
        {
            CheatOp *o = add_op(l);
            if (o)
            {
                o->kind = (u8)f[0];
                o->bits = (u8)f[1];
                o->addr = f[2];
                o->value = f[3];
                o->repeat = f[4];
                o->addAddr = f[5];
                o->addValue = f[6];
                o->mask = (u8)f[7];
            }
        }
        else if (l->count) l->cheats[l->count - 1].supported = 0;
        p = q + 1;
    }
}

int cheat_load_db(CheatList *l, const char *text, u32 len, const char *product)
{
    const char *p = text, *end = text + len;
    int in = 0, made = 0;
    char key[32];
    if (!product || !product_key(product, product + strlen(product), key, sizeof(key))) return 0;
    product = key;
    while (p < end)
    {
        const char *e = line_end(p, end), *k = skip_space(p, end);
        if (k < e && *k == '[')
        {
            const char *close = (const char *)memchr(k, ']', e - k);
            if (in && made) break;
            in = close && product_listed(k + 1, close + 1, product);
        }
        else if (in && k < e && *k != '#')
        {
            const char *bar = (const char *)memchr(k, '|', e - k);
            if (bar && add_cheat(l, k, (int)(bar - k)))
            {
                made++;
                db_ops(l, bar + 1, e);
                if (!l->cheats[l->count - 1].count) l->cheats[l->count - 1].supported = 0;
            }
        }
        p = e + 1;
    }
    return made;
}

int cheat_count(const CheatList *l) { return l ? l->count : 0; }
const char *cheat_name(const CheatList *l, int i) { return l && i >= 0 && i < l->count ? l->cheats[i].name : ""; }
int cheat_supported(const CheatList *l, int i) { return l && i >= 0 && i < l->count && l->cheats[i].supported; }
int cheat_enabled(const CheatList *l, int i) { return l && i >= 0 && i < l->count && l->cheats[i].on; }

void cheat_enable(CheatList *l, int i, int on)
{
    if (!l || i < 0 || i >= l->count) return;
    l->cheats[i].on = (u8)(on && l->cheats[i].supported);
}

int cheat_active(const CheatList *l)
{
    int i, n = 0;
    if (!l) return 0;
    for (i = 0; i < l->count; i++) n += l->cheats[i].on;
    return n;
}

static u32 swap_value(u32 v, int bits)
{
    if (bits == 16) return (u32)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF));
    if (bits == 32) return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
    return v;
}

static u32 op_read(const CheatList *l, const CheatOp *o, u32 addr)
{
    int bytes = o->bits >= 8 ? o->bits / 8 : 1;
    u32 v;
    if (bytes > 1 && (addr & (bytes - 1)))
    {
        int i;
        v = 0;
        for (i = bytes - 1; i >= 0; i--) v = (v << 8) | l->mem.read(addr + i, 1);
    }
    else v = l->mem.read(addr, bytes);
    return o->bigEndian ? swap_value(v, o->bits) : v;
}

static void op_write(const CheatList *l, const CheatOp *o, u32 addr, u32 v)
{
    int bytes = o->bits >= 8 ? o->bits / 8 : 1;
    if (o->bigEndian) v = swap_value(v, o->bits);
    if (bytes > 1 && (addr & (bytes - 1)))
    {
        int i;
        for (i = 0; i < bytes; i++) l->mem.write(addr + i, (v >> (8 * i)) & 0xFF, 1);
    }
    else l->mem.write(addr, v, bytes);
}

static u32 size_mask(int bits) { return bits >= 32 ? 0xFFFFFFFFu : bits >= 16 ? 0xFFFFu : 0xFFu; }

void cheat_apply(CheatList *l)
{
    int c;
    if (!l) return;
    for (c = 0; c < l->count; c++)
    {
        const Cheat *ch = &l->cheats[c];
        int i, run = 1;
        if (!ch->on) continue;
        for (i = ch->first; i < ch->first + ch->count; i++)
        {
            const CheatOp *o = &l->ops[i];
            u32 m = size_mask(o->bits), addr = o->addr, v = o->value, r;
            int bytes = o->bits >= 8 ? o->bits / 8 : 1;
            if (!run)
            {
                run = 1;
                continue;
            }
            if (o->kind >= OP_IF_EQ)
            {
                u32 mem = op_read(l, o, addr) & m;
                v &= m;
                run = o->kind == OP_IF_EQ ? mem == v : o->kind == OP_IF_NE ? mem != v : o->kind == OP_IF_LT ? v < mem : v > mem;
                continue;
            }
            for (r = 0; r < o->repeat; r++)
            {
                if (addr >= RAM_SIZE) break;
                if (o->bits < 8)
                {
                    u32 old = l->mem.read(addr, 1);
                    l->mem.write(addr, (old & ~(u32)o->mask) | (v & o->mask), 1);
                }
                else if (o->kind == OP_SET) op_write(l, o, addr, v & m);
                else
                {
                    u32 cur = op_read(l, o, addr);
                    op_write(l, o, addr, (o->kind == OP_INC ? cur + v : cur - v) & m);
                }
                v = (v + o->addValue) & m;
                addr += o->addAddr * bytes;
            }
        }
    }
}

void cheat_states_write(const CheatList *l, char *out, int size)
{
    int i, n = 0;
    if (size <= 0) return;
    out[0] = 0;
    for (i = 0; l && i < l->count; i++)
    {
        char num[16];
        int k;
        if (!l->cheats[i].on) continue;
        k = sprintf(num, n ? ",%d" : "%d", i);
        if (n + k >= size) break;
        memcpy(out + n, num, k + 1);
        n += k;
    }
}

void cheat_states_read(CheatList *l, const char *text)
{
    const char *p = text;
    int i;
    if (!l) return;
    for (i = 0; i < l->count; i++) l->cheats[i].on = 0;
    while (p && *p)
    {
        if (*p >= '0' && *p <= '9')
        {
            int v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            cheat_enable(l, v, 1);
        }
        else p++;
    }
}
