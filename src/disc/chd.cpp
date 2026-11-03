// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "chd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/log.h"
#include "../core/prof.h"
#include "../core/thread.h"
#include "../third_party/lzma/LzmaDec.h"
#include "../third_party/zlib/zlib.h"

#define CD_SECTOR 2352
#define CD_SUBCODE 96

enum
{
    COMP_TYPE0 = 0, COMP_TYPE1, COMP_TYPE2, COMP_TYPE3,
    COMP_NONE = 4,
    COMP_SELF = 5,
    COMP_PARENT = 6,
    COMP_RLE_SMALL = 7, COMP_RLE_LARGE = 8,
    COMP_SELF_0 = 9, COMP_SELF_1 = 10,
    COMP_PARENT_SELF = 11, COMP_PARENT_0 = 12, COMP_PARENT_1 = 13
};

#define TAG(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))
#define CODEC_ZLIB TAG('z', 'l', 'i', 'b')
#define CODEC_LZMA TAG('l', 'z', 'm', 'a')
#define CODEC_CDZL TAG('c', 'd', 'z', 'l')
#define CODEC_CDLZ TAG('c', 'd', 'l', 'z')
#define CODEC_CDFL TAG('c', 'd', 'f', 'l')

struct ChdMapEntry
{
    u8 kind;
    u32 length;
    u64 offset;
};

struct ChdDecoder
{
    FILE *f;
    u8 *comp;
    u8 *work;
    u32 statCodec[4], statNone, statSelf;
    u32 decoded;
    u64 ticks;
};

#define CHD_SLOTS 32
#define CHD_AHEAD 12
enum { SLOT_FREE, SLOT_BUSY, SLOT_READY };
struct ChdSlot
{
    u32 hunk;
    int state;
    u32 used;
    u8 *data;
};

struct ChdFile
{
    char path[260];
    u64 fileSize;
    u32 codecs[4];
    u64 logicalBytes, mapOffset, metaOffset;
    u32 hunkBytes, unitBytes, hunkCount;
    ChdMapEntry *map;
    ChdDecoder dec;
    u8 *cache;
    u32 cacheHunk;
    Mutex *lock;
    Event *wake;
    Thread *worker;
    ChdDecoder bg;
    ChdSlot slots[CHD_SLOTS];
    u32 clock, aheadFrom, aheadTo, lastHunk;
    u64 waitTicks;
    volatile int quit;
};

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u64 be64(const u8 *p) { return ((u64)be32(p) << 32) | be32(p + 4); }
static u64 be48(const u8 *p) { return ((u64)((p[0] << 8) | p[1]) << 32) | be32(p + 2); }

static int seek64(FILE *f, u64 pos)
{
#ifdef _MSC_VER
    return _fseeki64(f, (__int64)pos, SEEK_SET);
#else
    return fseeko(f, (off_t)pos, SEEK_SET);
#endif
}

static int dec_read(const ChdFile *c, ChdDecoder *d, u64 pos, void *buf, u32 len)
{
    if (pos + len > c->fileSize || seek64(d->f, pos) != 0) return -1;
    return fread(buf, 1, len, d->f) == len ? 0 : -1;
}

static int read_at(ChdFile *c, u64 pos, void *buf, u32 len) { return dec_read(c, &c->dec, pos, buf, len); }

struct BitReader
{
    const u8 *p;
    u32 len, pos;
    u32 buf;
    int bits;
};

static void bits_init(BitReader *b, const u8 *p, u32 len)
{
    b->p = p;
    b->len = len;
    b->pos = 0;
    b->buf = 0;
    b->bits = 0;
}

static u32 bits_peek(BitReader *b, int n)
{
    if (!n) return 0;
    while (b->bits <= 24)
    {
        if (b->pos < b->len) b->buf |= (u32)b->p[b->pos] << (24 - b->bits);
        b->pos++;
        b->bits += 8;
    }
    return b->buf >> (32 - n);
}

static void bits_skip(BitReader *b, int n)
{
    b->buf <<= n;
    b->bits -= n;
}

static u32 bits_read(BitReader *b, int n)
{
    u32 v = 0;
    while (n > 0)
    {
        int k = n > 16 ? 16 : n;
        v = (v << k) | bits_peek(b, k);
        bits_skip(b, k);
        n -= k;
    }
    return v;
}

static int bits_overflow(const BitReader *b) { return b->pos - b->bits / 8 > b->len; }

#define HUFF_CODES 16
#define HUFF_MAXBITS 8

struct Huffman
{
    u8 len[HUFF_CODES];
    u16 lookup[1 << HUFF_MAXBITS];
};

static int huff_import(Huffman *h, BitReader *b)
{
    u32 bitsAt[HUFF_MAXBITS + 1], counts[HUFF_MAXBITS + 2];
    int n = 0, i, l;
    while (n < HUFF_CODES)
    {
        int v = (int)bits_read(b, 4);
        if (v != 1) h->len[n++] = (u8)v;
        else
        {
            v = (int)bits_read(b, 4);
            if (v == 1) h->len[n++] = 1;
            else
            {
                int rep = (int)bits_read(b, 4) + 3;
                while (rep-- && n < HUFF_CODES) h->len[n++] = (u8)v;
            }
        }
    }
    memset(counts, 0, sizeof(counts));
    for (i = 0; i < HUFF_CODES; i++)
    {
        if (h->len[i] > HUFF_MAXBITS) return -1;
        counts[h->len[i]]++;
    }
    {
        u32 start = 0;
        for (l = HUFF_MAXBITS; l > 0; l--)
        {
            u32 next = (start + counts[l]) >> 1;
            if (l != 1 && next * 2 != start + counts[l]) return -1;
            bitsAt[l] = start;
            start = next;
        }
    }
    memset(h->lookup, 0, sizeof(h->lookup));
    for (i = 0; i < HUFF_CODES; i++)
    {
        u32 code, first, last, k;
        if (!h->len[i]) continue;
        code = bitsAt[h->len[i]]++;
        first = code << (HUFF_MAXBITS - h->len[i]);
        last = first | ((1u << (HUFF_MAXBITS - h->len[i])) - 1);
        for (k = first; k <= last; k++) h->lookup[k] = (u16)((i << 4) | h->len[i]);
    }
    return 0;
}

static int huff_decode(const Huffman *h, BitReader *b)
{
    u16 e = h->lookup[bits_peek(b, HUFF_MAXBITS)];
    bits_skip(b, e & 15);
    return e >> 4;
}

static u16 crc16(const u8 *p, u32 n)
{
    u16 crc = 0xFFFF;
    u32 i;
    int k;
    for (i = 0; i < n; i++)
    {
        crc ^= (u16)(p[i] << 8);
        for (k = 0; k < 8; k++) crc = (u16)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static int read_map(ChdFile *c)
{
    u32 i;
    c->map = (ChdMapEntry *)calloc(c->hunkCount ? c->hunkCount : 1, sizeof(ChdMapEntry));
    if (!c->map) return -1;
    if (!c->codecs[0])
    {
        u8 *raw = (u8 *)malloc(c->hunkCount * 4 + 1);
        if (!raw || read_at(c, c->mapOffset, raw, c->hunkCount * 4))
        {
            free(raw);
            return -1;
        }
        for (i = 0; i < c->hunkCount; i++)
        {
            c->map[i].kind = COMP_NONE;
            c->map[i].length = c->hunkBytes;
            c->map[i].offset = (u64)be32(raw + i * 4) * c->hunkBytes;
        }
        free(raw);
        return 0;
    }
    {
        u8 head[16], *comp, *raw;
        u32 mapBytes, lastSelf = 0, rep = 0;
        u64 cur, lastParent = 0;
        int lengthBits, selfBits, parentBits, last = 0;
        Huffman h;
        BitReader b;
        u16 crc;
        if (read_at(c, c->mapOffset, head, 16)) return -1;
        mapBytes = be32(head);
        cur = be48(head + 4);
        crc = (u16)((head[10] << 8) | head[11]);
        lengthBits = head[12];
        selfBits = head[13];
        parentBits = head[14];
        if (mapBytes > c->fileSize || lengthBits > 32 || selfBits > 32 || parentBits > 32) return -1;
        comp = (u8 *)malloc(mapBytes + 4);
        raw = (u8 *)calloc(c->hunkCount ? c->hunkCount : 1, 12);
        if (!comp || !raw || read_at(c, c->mapOffset + 16, comp, mapBytes))
        {
            free(comp);
            free(raw);
            return -1;
        }
        bits_init(&b, comp, mapBytes);
        if (huff_import(&h, &b))
        {
            LOG_ERROR("[chd] bad map code");
            free(comp);
            free(raw);
            return -1;
        }
        for (i = 0; i < c->hunkCount; i++)
        {
            if (rep) { raw[i * 12] = (u8)last; rep--; continue; }
            {
                int v = huff_decode(&h, &b);
                if (v == COMP_RLE_SMALL)
                {
                    raw[i * 12] = (u8)last;
                    rep = 2 + huff_decode(&h, &b);
                }
                else if (v == COMP_RLE_LARGE)
                {
                    raw[i * 12] = (u8)last;
                    rep = 2 + 16 + (huff_decode(&h, &b) << 4);
                    rep += huff_decode(&h, &b);
                }
                else
                    raw[i * 12] = (u8)(last = v);
            }
        }
        for (i = 0; i < c->hunkCount; i++)
        {
            u8 *e = raw + i * 12;
            u64 offset = cur;
            u32 length = 0, hcrc = 0;
            switch (e[0])
            {
            case COMP_TYPE0: case COMP_TYPE1: case COMP_TYPE2: case COMP_TYPE3:
                length = bits_read(&b, lengthBits);
                cur += length;
                hcrc = bits_read(&b, 16);
                break;
            case COMP_NONE:
                length = c->hunkBytes;
                cur += length;
                hcrc = bits_read(&b, 16);
                break;
            case COMP_SELF:
                lastSelf = bits_read(&b, selfBits);
                offset = lastSelf;
                break;
            case COMP_PARENT:
                offset = bits_read(&b, parentBits);
                lastParent = offset;
                break;
            case COMP_SELF_1:
                lastSelf++;
            case COMP_SELF_0:
                e[0] = COMP_SELF;
                offset = lastSelf;
                break;
            case COMP_PARENT_SELF:
                e[0] = COMP_PARENT;
                offset = lastParent = ((u64)i * c->hunkBytes) / c->unitBytes;
                break;
            case COMP_PARENT_1:
                lastParent += c->hunkBytes / c->unitBytes;
            case COMP_PARENT_0:
                e[0] = COMP_PARENT;
                offset = lastParent;
                break;
            default:
                free(comp);
                free(raw);
                LOG_ERROR("[chd] bad hunk kind %u in the map", e[0]);
                return -1;
            }
            e[1] = (u8)(length >> 16); e[2] = (u8)(length >> 8); e[3] = (u8)length;
            e[4] = (u8)(offset >> 40); e[5] = (u8)(offset >> 32); e[6] = (u8)(offset >> 24);
            e[7] = (u8)(offset >> 16); e[8] = (u8)(offset >> 8); e[9] = (u8)offset;
            e[10] = (u8)(hcrc >> 8); e[11] = (u8)hcrc;
            c->map[i].kind = e[0];
            c->map[i].length = length;
            c->map[i].offset = offset;
        }
        if (bits_overflow(&b) || crc16(raw, c->hunkCount * 12) != crc)
        {
            LOG_ERROR("[chd] the map is damaged (CRC)");
            free(comp);
            free(raw);
            return -1;
        }
        free(comp);
        free(raw);
    }
    return 0;
}

static void *sz_alloc(ISzAllocPtr, size_t size) { return size ? malloc(size) : NULL; }
static void sz_free(ISzAllocPtr, void *p) { free(p); }
static const ISzAlloc s_alloc = { sz_alloc, sz_free };

static voidpf z_alloc(voidpf, uInt items, uInt size) { return malloc((size_t)items * size); }
static void z_free(voidpf, voidpf p) { free(p); }

static int inflate_raw(const u8 *src, u32 srcLen, u8 *dst, u32 dstLen)
{
    z_stream z;
    int r;
    memset(&z, 0, sizeof(z));
    z.zalloc = z_alloc;
    z.zfree = z_free;
    if (inflateInit2(&z, -15) != Z_OK) return -1;
    z.next_in = (Bytef *)src;
    z.avail_in = srcLen;
    z.next_out = dst;
    z.avail_out = dstLen;
    r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return (r == Z_STREAM_END || r == Z_OK || r == Z_BUF_ERROR) && z.avail_out == 0 ? 0 : -1;
}

static int lzma_raw(const u8 *src, u32 srcLen, u8 *dst, u32 dstLen)
{
    CLzmaDec dec;
    Byte props[5];
    SizeT outLen = dstLen, inLen = srcLen;
    ELzmaStatus status;
    u32 dict = 1u << 12;
    SRes r;
    while (dict < dstLen) dict <<= 1;
    props[0] = (2 * 5 + 0) * 9 + 3;
    props[1] = (Byte)dict; props[2] = (Byte)(dict >> 8); props[3] = (Byte)(dict >> 16); props[4] = (Byte)(dict >> 24);
    LzmaDec_Construct(&dec);
    if (LzmaDec_Allocate(&dec, props, 5, &s_alloc) != SZ_OK) return -1;
    LzmaDec_Init(&dec);
    r = LzmaDec_DecodeToBuf(&dec, dst, &outLen, src, &inLen, LZMA_FINISH_END, &status);
    LzmaDec_Free(&dec, &s_alloc);
    return r == SZ_OK && outLen == dstLen ? 0 : -1;
}

struct FlacBits
{
    const u8 *p;
    u32 len, pos;
    u64 buf;
    int bits;
};

static u32 fb_read(FlacBits *b, int n)
{
    u32 v;
    if (!n) return 0;
    while (b->bits < n)
    {
        b->buf = (b->buf << 8) | (b->pos < b->len ? b->p[b->pos] : 0);
        b->pos++;
        b->bits += 8;
    }
    b->bits -= n;
    v = (u32)(b->buf >> b->bits) & (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1));
    return v;
}

static s32 fb_sread(FlacBits *b, int n)
{
    u32 v = fb_read(b, n);
    if (n && n < 32 && (v >> (n - 1))) v |= ~0u << n;
    return (s32)v;
}

static u32 fb_unary(FlacBits *b)
{
    u32 q = 0;
    while (!fb_read(b, 1))
    {
        if (++q > 1u << 20 || b->pos > b->len + 8) return q;
    }
    return q;
}

static void fb_align(FlacBits *b) { b->bits -= b->bits & 7; }

static int flac_residual(FlacBits *b, int blockSize, int order, s32 *out)
{
    int method = (int)fb_read(b, 2), porder, parts, p, i = order;
    int pbits = method == 0 ? 4 : 5, escape = method == 0 ? 15 : 31;
    if (method > 1) return -1;
    porder = (int)fb_read(b, 4);
    parts = 1 << porder;
    if ((blockSize >> porder) < order) return -1;
    for (p = 0; p < parts; p++)
    {
        int n = (blockSize >> porder) - (p == 0 ? order : 0), param = (int)fb_read(b, pbits), k;
        if (param == escape)
        {
            int raw = (int)fb_read(b, 5);
            for (k = 0; k < n; k++) out[i++] = fb_sread(b, raw);
        }
        else
            for (k = 0; k < n; k++)
            {
                u32 v = (fb_unary(b) << param) | fb_read(b, param);
                out[i++] = (s32)(v >> 1) ^ -(s32)(v & 1);
            }
    }
    return 0;
}

static int flac_subframe(FlacBits *b, int blockSize, int bps, s32 *out)
{
    int type, wasted = 0, i, order;
    if (fb_read(b, 1)) return -1;
    type = (int)fb_read(b, 6);
    if (fb_read(b, 1))
    {
        wasted = 1;
        while (!fb_read(b, 1)) wasted++;
        bps -= wasted;
    }
    if (type == 0)
    {
        s32 v = fb_sread(b, bps);
        for (i = 0; i < blockSize; i++) out[i] = v;
    }
    else if (type == 1)
        for (i = 0; i < blockSize; i++) out[i] = fb_sread(b, bps);
    else if (type >= 8 && type <= 12)
    {
        order = type - 8;
        for (i = 0; i < order; i++) out[i] = fb_sread(b, bps);
        if (flac_residual(b, blockSize, order, out)) return -1;
        for (i = order; i < blockSize; i++)
            switch (order)
            {
            case 1: out[i] += out[i - 1]; break;
            case 2: out[i] += 2 * out[i - 1] - out[i - 2]; break;
            case 3: out[i] += 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3]; break;
            case 4: out[i] += 4 * out[i - 1] - 6 * out[i - 2] + 4 * out[i - 3] - out[i - 4]; break;
            }
    }
    else if (type >= 32)
    {
        s32 coef[32];
        int precision, shift, j;
        order = (type & 31) + 1;
        for (i = 0; i < order; i++) out[i] = fb_sread(b, bps);
        precision = (int)fb_read(b, 4) + 1;
        if (precision == 16) return -1;
        shift = fb_sread(b, 5);
        if (shift < 0) return -1;
        for (j = 0; j < order; j++) coef[j] = fb_sread(b, precision);
        if (flac_residual(b, blockSize, order, out)) return -1;
        for (i = order; i < blockSize; i++)
        {
            s64 sum = 0;
            for (j = 0; j < order; j++) sum += (s64)coef[j] * out[i - 1 - j];
            out[i] += (s32)(sum >> shift);
        }
    }
    else
        return -1;
    if (wasted)
        for (i = 0; i < blockSize; i++) out[i] <<= wasted;
    return 0;
}

static int flac_decode(const u8 *src, u32 srcLen, u8 *dst, u32 samples)
{
    static const int fixedSizes[16] = { 0, 192, 576, 1152, 2304, 4608, 0, 0, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
    FlacBits b;
    u32 done = 0;
    s32 *ch[2];
    int ok = -1;
    ch[0] = (s32 *)malloc(65536 * sizeof(s32) * 2);
    if (!ch[0]) return -1;
    ch[1] = ch[0] + 65536;
    memset(&b, 0, sizeof(b));
    b.p = src;
    b.len = srcLen;
    while (done < samples)
    {
        int bsCode, srCode, assign, sizeCode, blockSize, i, c;
        if (fb_read(&b, 14) != 0x3FFE || b.pos > srcLen) goto out;
        fb_read(&b, 2);
        bsCode = (int)fb_read(&b, 4);
        srCode = (int)fb_read(&b, 4);
        assign = (int)fb_read(&b, 4);
        sizeCode = (int)fb_read(&b, 3);
        fb_read(&b, 1);
        {
            u32 first = fb_read(&b, 8);
            int extra = 0;
            while (extra < 7 && (first & (0x80 >> extra))) extra++;
            for (i = 1; i < extra; i++) fb_read(&b, 8);
        }
        if (bsCode == 6) blockSize = (int)fb_read(&b, 8) + 1;
        else if (bsCode == 7) blockSize = (int)fb_read(&b, 16) + 1;
        else blockSize = fixedSizes[bsCode];
        if (srCode == 12) fb_read(&b, 8);
        else if (srCode == 13 || srCode == 14) fb_read(&b, 16);
        fb_read(&b, 8);
        if (blockSize <= 0 || blockSize > 65536 || (sizeCode != 0 && sizeCode != 4)) goto out;
        if (assign == 1 || assign >= 8)
        {
            int side = assign == 8 ? 1 : assign == 9 ? 0 : assign == 10 ? 1 : -1;
            if (assign > 10) goto out;
            for (c = 0; c < 2; c++)
                if (flac_subframe(&b, blockSize, 16 + (c == side), ch[c])) goto out;
            for (i = 0; i < blockSize; i++)
            {
                s32 a = ch[0][i], s = ch[1][i];
                if (assign == 8) ch[1][i] = a - s;
                else if (assign == 9) ch[0][i] = a + s;
                else if (assign == 10)
                {
                    s32 mid = (a << 1) | (s & 1);
                    ch[0][i] = (mid + s) >> 1;
                    ch[1][i] = (mid - s) >> 1;
                }
            }
        }
        else
            goto out;
        fb_align(&b);
        fb_read(&b, 16);
        for (i = 0; i < blockSize && done < samples; i++, done++)
        {
            u8 *o = dst + done * 4;
            o[0] = (u8)(ch[0][i] >> 8); o[1] = (u8)ch[0][i];
            o[2] = (u8)(ch[1][i] >> 8); o[3] = (u8)ch[1][i];
        }
    }
    ok = 0;
out:
    free(ch[0]);
    return ok;
}

int chd_flac_decode(const u8 *src, u32 len, u8 *dst, u32 samples) { return flac_decode(src, len, dst, samples); }

static u8 s_eccF[256], s_eccB[256];

static void ecc_tables(void)
{
    int i;
    if (s_eccF[1]) return;
    for (i = 0; i < 256; i++)
    {
        int j = (i << 1) ^ ((i & 0x80) ? 0x11D : 0);
        s_eccF[i] = (u8)j;
        s_eccB[i ^ j] = (u8)i;
    }
}

static void ecc_compute(const u8 *src, int majorCount, int minorCount, int majorMult, int minorInc, u8 *dst)
{
    int size = majorCount * minorCount, major, minor;
    for (major = 0; major < majorCount; major++)
    {
        int index = (major >> 1) * majorMult + (major & 1);
        u8 a = 0, b = 0;
        for (minor = 0; minor < minorCount; minor++)
        {
            u8 t = src[index];
            index += minorInc;
            if (index >= size) index -= size;
            a ^= t;
            b ^= t;
            a = s_eccF[a];
        }
        a = s_eccB[s_eccF[a] ^ b];
        dst[major] = a;
        dst[major + majorCount] = (u8)(a ^ b);
    }
}

void chd_cd_ecc_generate(u8 *sector)
{
    ecc_tables();
    ecc_compute(sector + 0xC, 86, 24, 2, 86, sector + 0x81C);
    ecc_compute(sector + 0xC, 52, 43, 86, 88, sector + 0x8C8);
}

static const u8 s_sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

static int cd_decompress(const ChdFile *c, ChdDecoder *d, u32 codec, const u8 *src, u32 len, u8 *dst)
{
    u32 frames = c->hunkBytes / CHD_FRAME_BYTES, i;
    u8 *sectors = d->work;
    if (codec == CODEC_CDFL)
    {
        if (flac_decode(src, len, sectors, frames * CD_SECTOR / 4)) return -1;
        for (i = 0; i < frames; i++)
        {
            memcpy(dst + i * CHD_FRAME_BYTES, sectors + i * CD_SECTOR, CD_SECTOR);
            memset(dst + i * CHD_FRAME_BYTES + CD_SECTOR, 0, CD_SUBCODE);
        }
        return 0;
    }
    {
        u32 eccBytes = (frames + 7) / 8, lenBytes = c->hunkBytes < 65536 ? 2 : 3, head = eccBytes + lenBytes, baseLen;
        if (len < head) return -1;
        baseLen = ((u32)src[eccBytes] << 8) | src[eccBytes + 1];
        if (lenBytes > 2) baseLen = (baseLen << 8) | src[eccBytes + 2];
        if (head + baseLen > len) return -1;
        if (codec == CODEC_CDLZ ? lzma_raw(src + head, baseLen, sectors, frames * CD_SECTOR)
                                : inflate_raw(src + head, baseLen, sectors, frames * CD_SECTOR))
            return -1;
        for (i = 0; i < frames; i++)
        {
            u8 *s = dst + i * CHD_FRAME_BYTES;
            memcpy(s, sectors + i * CD_SECTOR, CD_SECTOR);
            memset(s + CD_SECTOR, 0, CD_SUBCODE);
            if (src[i / 8] & (1 << (i % 8)))
            {
                memcpy(s, s_sync, sizeof(s_sync));
                chd_cd_ecc_generate(s);
            }
        }
    }
    return 0;
}

ChdFile *chd_open(const char *path)
{
    u8 h[124];
    ChdFile *c = (ChdFile *)calloc(1, sizeof(ChdFile));
    int i;
    if (!c) return NULL;
    strncpy(c->path, path, sizeof(c->path) - 1);
    c->dec.f = fopen(path, "rb");
    if (!c->dec.f)
    {
        LOG_ERROR("[chd] cannot open %s", path);
        free(c);
        return NULL;
    }
#ifdef _MSC_VER
    _fseeki64(c->dec.f, 0, SEEK_END);
    c->fileSize = (u64)_ftelli64(c->dec.f);
#else
    fseeko(c->dec.f, 0, SEEK_END);
    c->fileSize = (u64)ftello(c->dec.f);
#endif
    if (read_at(c, 0, h, 16) || memcmp(h, "MComprHD", 8))
    {
        LOG_ERROR("[chd] %s: not a CHD file", path);
        chd_close(c);
        return NULL;
    }
    if (be32(h + 12) != 5)
    {
        LOG_ERROR("[chd] %s: CHD version %u (only version 5 is read; chdman copy makes one)", path, be32(h + 12));
        chd_close(c);
        return NULL;
    }
    if (read_at(c, 0, h, sizeof(h)))
    {
        chd_close(c);
        return NULL;
    }
    for (i = 0; i < 4; i++) c->codecs[i] = be32(h + 16 + i * 4);
    c->logicalBytes = be64(h + 32);
    c->mapOffset = be64(h + 40);
    c->metaOffset = be64(h + 48);
    c->hunkBytes = be32(h + 56);
    c->unitBytes = be32(h + 60);
    {
        static const u8 zero[20] = { 0 };
        if (memcmp(h + 104, zero, 20))
        {
            LOG_ERROR("[chd] %s needs a parent CHD (not supported)", path);
            chd_close(c);
            return NULL;
        }
    }
    if (c->hunkBytes == 0 || c->hunkBytes > (1u << 20) || c->unitBytes == 0)
    {
        LOG_ERROR("[chd] %s: bad hunk size %u", path, c->hunkBytes);
        chd_close(c);
        return NULL;
    }
    c->hunkCount = (u32)((c->logicalBytes + c->hunkBytes - 1) / c->hunkBytes);
    c->dec.comp = (u8 *)malloc(c->hunkBytes);
    c->dec.work = (u8 *)malloc(c->hunkBytes);
    c->cache = (u8 *)malloc(c->hunkBytes);
    c->cacheHunk = 0xFFFFFFFFu;
    if (!c->dec.comp || !c->dec.work || !c->cache || read_map(c))
    {
        LOG_ERROR("[chd] %s: cannot read the hunk map", path);
        chd_close(c);
        return NULL;
    }
    return c;
}

void chd_close(ChdFile *c)
{
    int i;
    if (!c) return;
    if (prof_tick_rate() && (c->dec.decoded || c->bg.decoded))
    {
        double ms = 1000.0 / (double)prof_tick_rate();
        LOG_INFO("[chd] hunks decoded: %u by the reader (%.0f ms), %u ahead (%.0f ms); reader waited %.0f ms",
                 c->dec.decoded, c->dec.ticks * ms, c->bg.decoded, c->bg.ticks * ms, c->waitTicks * ms);
    }
    if (c->worker)
    {
        c->quit = 1;
        event_signal(c->wake);
        thread_join(c->worker);
    }
    if (c->wake) event_destroy(c->wake);
    if (c->lock) mutex_destroy(c->lock);
    for (i = 0; i < CHD_SLOTS; i++) free(c->slots[i].data);
    if (c->bg.f) fclose(c->bg.f);
    free(c->bg.comp);
    free(c->bg.work);
    if (c->dec.f) fclose(c->dec.f);
    free(c->map);
    free(c->dec.comp);
    free(c->dec.work);
    free(c->cache);
    free(c);
}

u32 chd_hunk_bytes(const ChdFile *c) { return c->hunkBytes; }
u32 chd_hunk_count(const ChdFile *c) { return c->hunkCount; }

static int hunk_decode_one(const ChdFile *c, ChdDecoder *d, u32 n, u8 *out)
{
    int depth;
    for (depth = 0; depth < 8; depth++)
    {
        const ChdMapEntry *e;
        if (n >= c->hunkCount) return -1;
        e = &c->map[n];
        switch (e->kind)
        {
        case COMP_TYPE0: case COMP_TYPE1: case COMP_TYPE2: case COMP_TYPE3:
        {
            u32 codec = c->codecs[e->kind];
            if (e->length > c->hunkBytes || dec_read(c, d, e->offset, d->comp, e->length)) return -1;
            d->statCodec[e->kind]++;
            if (codec == CODEC_CDZL || codec == CODEC_CDLZ || codec == CODEC_CDFL)
            {
                if (c->hunkBytes % CHD_FRAME_BYTES) return -1;
                return cd_decompress(c, d, codec, d->comp, e->length, out);
            }
            if (codec == CODEC_ZLIB) return inflate_raw(d->comp, e->length, out, c->hunkBytes);
            if (codec == CODEC_LZMA) return lzma_raw(d->comp, e->length, out, c->hunkBytes);
            LOG_ERROR("[chd] codec %c%c%c%c not supported", (char)(codec >> 24), (char)(codec >> 16), (char)(codec >> 8), (char)codec);
            return -1;
        }
        case COMP_NONE:
            d->statNone++;
            return dec_read(c, d, e->offset, out, c->hunkBytes);
        case COMP_SELF:
            d->statSelf++;
            if ((u32)e->offset == n) return -1;
            n = (u32)e->offset;
            continue;
        default:
            LOG_ERROR("[chd] hunk %u comes from a parent CHD (not supported)", n);
            return -1;
        }
    }
    return -1;
}

static int hunk_decode(const ChdFile *c, ChdDecoder *d, u32 n, u8 *out)
{
    u64 t0 = prof_ticks();
    int r = hunk_decode_one(c, d, n, out);
    d->ticks += prof_ticks() - t0;
    d->decoded++;
    return r;
}

int chd_read_hunk(ChdFile *c, u32 n, u8 *out) { return hunk_decode(c, &c->dec, n, out); }

static ChdSlot *slot_find(ChdFile *c, u32 hunk)
{
    int i;
    for (i = 0; i < CHD_SLOTS; i++)
        if (c->slots[i].state != SLOT_FREE && c->slots[i].hunk == hunk) return &c->slots[i];
    return NULL;
}

static ChdSlot *slot_take(ChdFile *c)
{
    ChdSlot *best = NULL;
    int i;
    for (i = 0; i < CHD_SLOTS; i++)
    {
        ChdSlot *s = &c->slots[i];
        if (s->state == SLOT_FREE) return s;
        if (s->state == SLOT_READY && (!best || s->used < best->used)) best = s;
    }
    return best;
}

static void read_ahead_main(void *arg)
{
    ChdFile *c = (ChdFile *)arg;
    while (!c->quit)
    {
        event_wait(c->wake, 200);
        mutex_lock(c->lock);
        while (!c->quit)
        {
            ChdSlot *s = NULL;
            u32 h;
            int ok;
            for (h = c->aheadFrom; h < c->aheadTo; h++)
                if (!slot_find(c, h)) break;
            if (h >= c->aheadTo || !(s = slot_take(c))) break;
            s->hunk = h;
            s->state = SLOT_BUSY;
            s->used = ++c->clock;
            mutex_unlock(c->lock);
            ok = hunk_decode(c, &c->bg, h, s->data) == 0;
            mutex_lock(c->lock);
            s->state = ok ? SLOT_READY : SLOT_FREE;
        }
        mutex_unlock(c->lock);
    }
}

int chd_start_read_ahead(ChdFile *c, int hwThread)
{
    int i;
    if (c->worker) return 0;
    c->lastHunk = 0xFFFFFFFFu;
    c->bg.f = fopen(c->path, "rb");
    c->bg.comp = (u8 *)malloc(c->hunkBytes);
    c->bg.work = (u8 *)malloc(c->hunkBytes);
    for (i = 0; i < CHD_SLOTS; i++) c->slots[i].data = (u8 *)malloc(c->hunkBytes);
    c->lock = mutex_create();
    c->wake = event_create();
    for (i = 0; i < CHD_SLOTS; i++)
        if (!c->slots[i].data) break;
    if (!c->bg.f || !c->bg.comp || !c->bg.work || i < CHD_SLOTS || !c->lock || !c->wake ||
        !(c->worker = thread_start(read_ahead_main, c, hwThread)))
    {
        LOG_WARN("[chd] no read-ahead (out of memory)");
        return -1;
    }
    return 0;
}

static int read_frame_shared(ChdFile *c, u32 hunk, u32 offset, u8 *out)
{
    mutex_lock(c->lock);
    for (;;)
    {
        ChdSlot *s = slot_find(c, hunk);
        if (s && s->state == SLOT_READY)
        {
            memcpy(out, s->data + offset, CD_SECTOR);
            s->used = ++c->clock;
            break;
        }
        if (s || !(s = slot_take(c)))
        {
            u64 t0 = prof_ticks();
            mutex_unlock(c->lock);
            thread_yield();
            mutex_lock(c->lock);
            c->waitTicks += prof_ticks() - t0;
            continue;
        }
        s->hunk = hunk;
        s->state = SLOT_BUSY;
        s->used = ++c->clock;
        mutex_unlock(c->lock);
        {
            int ok = hunk_decode(c, &c->dec, hunk, s->data) == 0;
            mutex_lock(c->lock);
            s->state = ok ? SLOT_READY : SLOT_FREE;
            if (!ok)
            {
                mutex_unlock(c->lock);
                return -1;
            }
        }
    }
    if (hunk != c->lastHunk)
    {
        c->lastHunk = hunk;
        c->aheadFrom = hunk + 1;
        c->aheadTo = hunk + 1 + CHD_AHEAD < c->hunkCount ? hunk + 1 + CHD_AHEAD : c->hunkCount;
        mutex_unlock(c->lock);
        event_signal(c->wake);
        return 0;
    }
    mutex_unlock(c->lock);
    return 0;
}

int chd_read_frame(ChdFile *c, u32 frame, u8 *out)
{
    u32 perHunk = c->hunkBytes / CHD_FRAME_BYTES, hunk;
    if (!perHunk) return -1;
    hunk = frame / perHunk;
    if (c->worker) return read_frame_shared(c, hunk, (frame % perHunk) * CHD_FRAME_BYTES, out);
    if (hunk != c->cacheHunk)
    {
        if (chd_read_hunk(c, hunk, c->cache))
        {
            c->cacheHunk = 0xFFFFFFFFu;
            return -1;
        }
        c->cacheHunk = hunk;
    }
    memcpy(out, c->cache + (frame % perHunk) * CHD_FRAME_BYTES, CD_SECTOR);
    return 0;
}

int chd_metadata(ChdFile *c, const char *tag, int index, char *out, int size)
{
    u32 want = TAG(tag[0], tag[1], tag[2], tag[3]);
    u64 at = c->metaOffset;
    int guard = 0;
    while (at && guard++ < 4096)
    {
        u8 h[16];
        u32 len;
        if (read_at(c, at, h, 16)) return -1;
        len = be32(h + 4) & 0xFFFFFF;
        if (be32(h) == want && index-- == 0)
        {
            u32 n = len < (u32)size - 1 ? len : (u32)size - 1;
            if (read_at(c, at + 16, out, n)) return -1;
            out[n] = 0;
            return (int)n;
        }
        at = be64(h + 8);
    }
    return -1;
}

void chd_stats(const ChdFile *c, u32 perCodec[4], u32 *none, u32 *self)
{
    int i;
    for (i = 0; i < 4; i++) perCodec[i] = c->dec.statCodec[i] + c->bg.statCodec[i];
    *none = c->dec.statNone + c->bg.statNone;
    *self = c->dec.statSelf + c->bg.statSelf;
}
