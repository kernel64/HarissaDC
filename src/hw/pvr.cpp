// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "pvr.h"

#include <stdlib.h>
#include <string.h>

#include "holly.h"
#include "maple.h"
#include "../core/log.h"
#include "../core/state.h"
#include "../core/sched.h"
#include "../mem/mem.h"

#define PVR_BASE 0x005F8000u
#define PVR_END  0x005FA000u
static u32 s_regs[(PVR_END - PVR_BASE) / 4];
#define REG(a) s_regs[((a) - PVR_BASE) >> 2]

#define ID              0x005F8000
#define REVISION        0x005F8004
#define SOFTRESET       0x005F8008
#define STARTRENDER     0x005F8014
#define FB_R_CTRL       0x005F8044
#define FB_R_SOF1       0x005F8050
#define FB_R_SOF2       0x005F8054
#define FB_R_SIZE       0x005F805C
#define SPG_HBLANK_INT  0x005F80C8
#define SPG_VBLANK_INT  0x005F80CC
#define SPG_CONTROL     0x005F80D0
#define PT_ALPHA_REF    0x005F811C
#define SPG_HBLANK      0x005F80D4
#define SPG_LOAD        0x005F80D8
#define SPG_VBLANK      0x005F80DC
#define VO_CONTROL      0x005F80E8
#define SPG_STATUS      0x005F810C
#define TA_OL_BASE      0x005F8124
#define TA_ISP_BASE     0x005F8128
#define TA_NEXT_OPB     0x005F8134
#define TA_ITP_CURRENT  0x005F8138
#define TA_LIST_INIT    0x005F8144
#define TA_YUV_TEX_BASE 0x005F8148
#define TA_YUV_TEX_CTRL 0x005F814C
#define TA_YUV_TEX_CNT  0x005F8150
#define TA_LIST_CONT    0x005F8160
#define TA_NEXT_OPB_INIT 0x005F8164

static int s_evLine, s_evRender;
static u32 s_lineCycles, s_lines, s_line, s_field;
static u32 s_frames;
static u32 s_nextLine;

static void spg_recalc(void)
{
    u32 pixelClock = (REG(FB_R_CTRL) & (1u << 23)) ? 27000000u : 13500000u;
    u32 hcount = (REG(SPG_LOAD) & 0x3FF) + 1;
    s_lines = ((REG(SPG_LOAD) >> 16) & 0x3FF) + 1;
    s_lineCycles = (u32)((u64)SH4_CLOCK_HZ * hcount / pixelClock);
    if (REG(SPG_CONTROL) & 0x10) s_lineCycles /= 2;
    if (s_lineCycles < 100) s_lineCycles = 100;
    if (s_line >= s_lines) s_line = 0;
}

static u32 lines_ahead(void)
{
    u64 r = sched_remaining(s_evLine);
    if (r == SCHED_NEVER) return 0;
    return (u32)((r + s_lineCycles - 1) / s_lineCycles);
}

static u32 cur_line(void)
{
    u32 a = lines_ahead();
    return s_nextLine >= a ? s_nextLine - a : 0;
}

static u32 next_event_line(u32 line)
{
    u32 vbIn = REG(SPG_VBLANK_INT) & 0x3FF, vbOut = (REG(SPG_VBLANK_INT) >> 16) & 0x3FF;
    u32 hb = REG(SPG_HBLANK_INT), hbLine = hb & 0x3FF, hbMode = (hb >> 12) & 3, next = s_lines;
    if (vbIn > line && vbIn < next) next = vbIn;
    if (vbOut > line && vbOut < next) next = vbOut;
    if (hbMode == 2) next = line + 1;
    else if (hbMode == 0 && hbLine > line && hbLine < next) next = hbLine;
    else if (hbMode == 1 && hbLine)
    {
        u32 m = (line / hbLine + 1) * hbLine;
        if (m < next) next = m;
    }
    return next;
}

static void spg_schedule(u32 line, u32 inLine)
{
    u32 next = next_event_line(line);
    s_nextLine = next;
    sched_after(s_evLine, inLine + (next - line - 1) * s_lineCycles);
}

static void spg_reschedule(void)
{
    u64 r = sched_remaining(s_evLine);
    u32 line = cur_line(), inLine;
    if (r == SCHED_NEVER) return;
    inLine = (u32)(r % s_lineCycles);
    if (!inLine) inLine = s_lineCycles;
    if (line >= s_lines) line = s_lines - 1;
    spg_schedule(line, inLine);
}

static void spg_line(void *, u32 late)
{
    u32 vbIn = REG(SPG_VBLANK_INT) & 0x3FF, vbOut = (REG(SPG_VBLANK_INT) >> 16) & 0x3FF;
    u32 hb = REG(SPG_HBLANK_INT), hbLine = hb & 0x3FF, hbMode = (hb >> 12) & 3;
    s_line = s_nextLine;
    if (s_line >= s_lines)
    {
        s_line = 0;
        if (REG(SPG_CONTROL) & 0x10) s_field ^= 1;
    }
    if (s_line == vbIn)
    {
        s_frames++;
        holly_raise(HOLLY_VBLANK_IN);
        maple_vblank();
    }
    if (s_line == vbOut) holly_raise(HOLLY_VBLANK_OUT);
    if (hbMode == 2 || (hbMode == 0 && s_line == hbLine) || (hbMode == 1 && hbLine && s_line % hbLine == 0))
        holly_raise(HOLLY_HBLANK);
    spg_schedule(s_line, s_lineCycles > late ? s_lineCycles - late : 1);
}

static u32 spg_status(void)
{
    u32 vb = REG(SPG_VBLANK), vbStart = vb & 0x3FF, vbEnd = (vb >> 16) & 0x3FF;
    u32 line = cur_line();
    u32 blank = vbStart > vbEnd ? (line >= vbStart || line < vbEnd) : (line >= vbStart && line < vbEnd);
    u32 vsync = line < 3;
    u32 hsync = sched_remaining(s_evLine) % s_lineCycles < s_lineCycles / 8;
    return (line & 0x3FF) | (s_field << 10) | (blank << 11) | (hsync << 12) | (vsync << 13);
}

u32 pvr_frame_count(void) { return s_frames; }

#define PCW_PARA_TYPE(w) ((w) >> 29)
#define PCW_LIST_TYPE(w) (((w) >> 24) & 7)
#define PCW_VOLUME(w)    (((w) >> 6) & 1)
#define PCW_COL_TYPE(w)  (((w) >> 4) & 3)
#define PCW_TEXTURE(w)   (((w) >> 3) & 1)
#define PCW_OFFSET(w)    (((w) >> 2) & 1)
#define PCW_UV16(w)      ((w) & 1)

enum { PARA_END_OF_LIST = 0, PARA_USER_CLIP = 1, PARA_OBJ_LIST_SET = 2, PARA_POLYGON = 4, PARA_SPRITE = 5, PARA_VERTEX = 7 };
enum { LIST_OPAQUE = 0, LIST_OPAQUE_MOD = 1, LIST_TRANS = 2, LIST_TRANS_MOD = 3, LIST_PUNCH = 4, LIST_NONE = 7 };

#define TA_MAX_WORDS (4u << 20) / 4
static u32 *s_taData;
static u32 s_taWords;
static u32 *s_taBuf[2], s_taCur;
static u32 s_taIsp;
static u32 s_doneWords, s_doneIsp, s_doneValid;
static const u32 *s_renderData;
static u32 s_renderWords;
static u32 s_fifo[16];
static u32 s_fifoWords;
static u32 s_paramWords;
static u32 s_list = LIST_NONE;
static u32 s_vertexWords = 8;
static TaStats s_stats, s_lastStats;
static u32 s_renders;

static void ta_reset(void)
{
    s_taWords = 0;
    s_fifoWords = 0;
    s_paramWords = 0;
    s_list = LIST_NONE;
    s_vertexWords = 8;
    memset(&s_stats, 0, sizeof(s_stats));
}

static void ta_keep_list(void)
{
    if (!s_taWords || !s_taBuf[1]) return;
    s_doneWords = s_taWords;
    s_doneIsp = s_taIsp;
    s_doneValid = 1;
    s_taCur ^= 1;
    s_taData = s_taBuf[s_taCur];
}

static void ta_list_end(void)
{
    static const int bits[5] = { HOLLY_OPAQUE_END, HOLLY_OPAQUE_MOD_END, HOLLY_TRANS_END, HOLLY_TRANS_MOD_END, HOLLY_PUNCH_END };
    if (s_list < 5) holly_raise(bits[s_list]);
    s_stats.listEnds++;
    s_list = LIST_NONE;
}

static u32 ta_param_words(u32 pcw)
{
    switch (PCW_PARA_TYPE(pcw))
    {
    case PARA_POLYGON:
    {
        u32 list = s_list != LIST_NONE ? s_list : PCW_LIST_TYPE(pcw);
        if (list == LIST_OPAQUE_MOD || list == LIST_TRANS_MOD) return 8;
        return (PCW_COL_TYPE(pcw) == 2 && (PCW_OFFSET(pcw) || PCW_VOLUME(pcw))) ? 16 : 8;
    }
    case PARA_VERTEX:
        return s_vertexWords;
    default:
        return 8;
    }
}

static u32 vertex_words(u32 pcw, int sprite)
{
    u32 tex = PCW_TEXTURE(pcw), col = PCW_COL_TYPE(pcw);
    if (sprite) return 16;
    if (s_list == LIST_OPAQUE_MOD || s_list == LIST_TRANS_MOD) return 16;
    if (PCW_VOLUME(pcw)) return tex ? 16 : 8;
    if (tex && col == 1) return 16;
    return 8;
}

static void ta_param(const u32 *p)
{
    u32 pcw = p[0], type = PCW_PARA_TYPE(pcw);
    if (s_taWords + s_paramWords <= TA_MAX_WORDS)
    {
        u32 *d = s_taData + s_taWords, k;
        for (k = 0; k + 4 <= s_paramWords; k += 4)
        {
            d[k] = p[k];
            d[k + 1] = p[k + 1];
            d[k + 2] = p[k + 2];
            d[k + 3] = p[k + 3];
        }
        for (; k < s_paramWords; k++) d[k] = p[k];
        s_taWords += s_paramWords;
    }
    s_stats.params++;
    switch (type)
    {
    case PARA_END_OF_LIST:
        ta_list_end();
        break;
    case PARA_POLYGON:
    case PARA_SPRITE:
        if (s_list == LIST_NONE) s_list = PCW_LIST_TYPE(pcw);
        s_vertexWords = vertex_words(pcw, type == PARA_SPRITE);
        if (s_list == LIST_OPAQUE_MOD || s_list == LIST_TRANS_MOD) s_stats.modvols++;
        else if (type == PARA_SPRITE) s_stats.sprites++;
        else s_stats.polygons++;
        break;
    case PARA_VERTEX:
        s_stats.vertices++;
        break;
    default:
        break;
    }
}

static void ta_write_word(u32 w)
{
    s_fifo[s_fifoWords++] = w;
    if (s_fifoWords == 1) s_paramWords = ta_param_words(w);
    if (s_fifoWords == s_paramWords)
    {
        ta_param(s_fifo);
        s_fifoWords = 0;
    }
    if (s_fifoWords >= 16) s_fifoWords = 0;
}

static u8 s_yuvIn[384];
static u32 s_yuvFill, s_yuvDest, s_yuvX, s_yuvW, s_yuvBlocks;
static u32 s_yuvTotal;

u32 pvr_yuv_blocks(void)
{
    return s_yuvTotal;
}

static void yuv_init(void)
{
    u32 ctrl = REG(TA_YUV_TEX_CTRL);
    s_yuvW = ((ctrl & 0x3F) + 1) * 16;
    s_yuvBlocks = ((ctrl & 0x3F) + 1) * (((ctrl >> 8) & 0x3F) + 1);
    s_yuvDest = REG(TA_YUV_TEX_BASE) & (VRAM_SIZE - 1);
    s_yuvX = 0;
    s_yuvFill = 0;
    REG(TA_YUV_TEX_CNT) = 0;
}

static void yuv_block8(const u8 *uv, const u8 *y, u32 dest)
{
    u32 row, col;
    for (row = 0; row < 8; row++)
    {
        u32 off = (dest + row * s_yuvW * 2) & (VRAM_SIZE - 1);
        for (col = 0; col < 8; col += 2)
        {
            u32 u = uv[(row >> 1) * 8 + (col >> 1)], v = uv[64 + (row >> 1) * 8 + (col >> 1)];
            buf_w32(g_vram, (off + col * 2) & (VRAM_SIZE - 1) & ~3u,
                    u | ((u32)y[row * 8 + col] << 8) | ((v | ((u32)y[row * 8 + col + 1] << 8)) << 16));
        }
        vram_mark(off);
        vram_mark((off + 15) & (VRAM_SIZE - 1));
    }
}

static void yuv_macroblock(void);

static void yuv_byte(u8 b)
{
    s_yuvIn[s_yuvFill++] = b;
    if (s_yuvFill < 384) return;
    s_yuvFill = 0;
    yuv_macroblock();
}

static void yuv_macroblock(void)
{
    yuv_block8(s_yuvIn, s_yuvIn + 128, s_yuvDest);
    yuv_block8(s_yuvIn + 4, s_yuvIn + 192, s_yuvDest + 16);
    yuv_block8(s_yuvIn + 32, s_yuvIn + 256, s_yuvDest + s_yuvW * 16);
    yuv_block8(s_yuvIn + 36, s_yuvIn + 320, s_yuvDest + s_yuvW * 16 + 16);
    s_yuvTotal++;
    s_yuvDest += 32;
    s_yuvX += 16;
    if (s_yuvX >= s_yuvW)
    {
        s_yuvX = 0;
        s_yuvDest += s_yuvW * 2 * 15;
    }
    if (++REG(TA_YUV_TEX_CNT) >= s_yuvBlocks)
    {
        yuv_init();
        LOG_DEBUG("[pvr] YUV conversion done");
        holly_raise(HOLLY_YUV_DONE);
    }
}

static void yuv_write(u32 value, int size)
{
    int i;
    if (REG(TA_YUV_TEX_CTRL) & (1u << 24))
    {
        static int warned;
        if (!warned++) LOG_WARN("[pvr] YUV422 input to the YUV converter is not emulated");
        return;
    }
    for (i = 0; i < size; i++) yuv_byte((u8)(value >> (i * 8)));
}

void pvr_ta_dma(u32 dst, u32 src, u32 len)
{
    u32 i;
    if (dst & 0x00800000u)
    {
        i = 0;
        if (!(REG(TA_YUV_TEX_CTRL) & (1u << 24)))
            while (s_yuvFill == 0 && i + 384 <= len && !((src + i) & 3))
            {
                u32 avail, k;
                const u32 *p = (const u32 *)mem_word_ptr(src + i, &avail);
                if (!p || avail < 384) break;
                for (k = 0; k < 96; k++)
                {
                    u32 w = p[k];
                    s_yuvIn[k * 4] = (u8)w;
                    s_yuvIn[k * 4 + 1] = (u8)(w >> 8);
                    s_yuvIn[k * 4 + 2] = (u8)(w >> 16);
                    s_yuvIn[k * 4 + 3] = (u8)(w >> 24);
                }
                yuv_macroblock();
                i += 384;
            }
        for (; i < len; i += 4) yuv_write(mem_read32(src + i), 4);
    }
    else
    {
        while (len >= 32 && !(src & 31))
        {
            u32 avail;
            const u32 *p = (const u32 *)mem_word_ptr(src, &avail);
            if (!p || avail < 32) break;
            pvr_ta_fifo(p, 8);
            src += 32;
            len -= 32;
        }
        for (i = 0; i < len; i += 4) ta_write_word(mem_read32(src + i));
    }
}

void pvr_ta_stats(TaStats *out) { *out = s_lastStats; }

const u32 *pvr_ta_data(u32 *words)
{
    if (s_renderData)
    {
        *words = s_renderWords;
        return s_renderData;
    }
    *words = s_taWords;
    return s_taData;
}

static void ta_alloc(void)
{
    if (!s_taBuf[0]) s_taBuf[0] = (u32 *)malloc(TA_MAX_WORDS * 4);
    if (!s_taBuf[1]) s_taBuf[1] = (u32 *)malloc(TA_MAX_WORDS * 4);
    if (!s_taData) s_taData = s_taBuf[s_taCur];
}

PvrRenderFn g_pvr_render;
void (*g_pvr_render_wait)(void);
u32 g_pvr_palette_gen;

void pvr_regs_copy(u32 *out)
{
    memcpy(out, s_regs, sizeof(s_regs));
}

void pvr_debug_set(const u32 *regs, const u32 *ta, u32 words)
{
    memcpy(s_regs, regs, sizeof(s_regs));
    if (!(REG(PT_ALPHA_REF) & 0xFF)) REG(PT_ALPHA_REF) = 0xFF;
    ta_alloc();
    if (!s_taData) return;
    if (words > TA_MAX_WORDS) words = TA_MAX_WORDS;
    memcpy(s_taData, ta, words * 4);
    s_taWords = words;
}

int g_pvr_gun_hpos = 286;

void pvr_light_gun(u32 x, u32 y)
{
    static u32 flip;
    u32 vbOut = (REG(SPG_VBLANK_INT) >> 16) & 0x3FF, line, h;
    int interlaced = (REG(SPG_CONTROL) >> 4) & 1;
    line = interlaced ? y / 2 + vbOut : (REG(0x005F80F0) & 0x3FF) + y;
    line ^= (flip >> 1) & 1;
    h = (x + (u32)g_pvr_gun_hpos) ^ (flip & 1);
    flip++;
    REG(0x005F80C4) = ((line & 0x3FF) << 16) | (h & 0x3FF);
}

u32 pvr_reg(u32 addr)
{
    return (addr >= PVR_BASE && addr < PVR_END) ? REG(addr & ~3u) : 0;
}

static void start_render(void)
{
    u32 param = REG(0x005F8020) & 0x7FFFFFu, bytes;
    s_renderData = s_taData;
    s_renderWords = s_taWords;
    if (param != s_taIsp && s_doneValid && param == s_doneIsp)
    {
        s_renderData = s_taBuf[s_taCur ^ 1];
        s_renderWords = s_doneWords;
    }
    bytes = s_renderWords * 4;
    if (g_pvr_render) g_pvr_render();
    s_renderData = NULL;
    s_renders++;
    s_lastStats = s_stats;
    s_lastStats.renders = s_renders;
    sched_after(s_evRender, bytes >= 10500 ? 1500000 : 450000 + bytes * 100);
}

static void render_done(void *, u32)
{
    if (g_pvr_render_wait) g_pvr_render_wait();
    holly_raise(HOLLY_RENDER_DONE_ISP);
    holly_raise(HOLLY_RENDER_DONE_TSP);
    holly_raise(HOLLY_RENDER_DONE_VIDEO);
}

u32 pvr_read(u32 addr, int size)
{
    u32 a = addr & ~3u, v;
    if (a < PVR_BASE || a >= PVR_END) return 0;
    switch (a)
    {
    case ID: v = 0x17FD11DB; break;
    case REVISION: v = 0x11; break;
    case SPG_STATUS: v = spg_status(); break;
    default: v = REG(a); break;
    }
    return reg_lane_read(v, addr, size);
}

void pvr_write(u32 addr, u32 value, int size)
{
    u32 a = addr & ~3u;
    if (a < PVR_BASE || a >= PVR_END) return;
    value = reg_lane_write(REG(a), addr, value, size);
    if (a >= 0x005F9000u || a == 0x005F8108u) g_pvr_palette_gen++;
    switch (a)
    {
    case ID:
    case REVISION:
    case SPG_STATUS:
        return;
    case SOFTRESET:
        REG(a) = value;
        if (value & 1)
        {
            ta_keep_list();
            ta_reset();
        }
        return;
    case STARTRENDER:
        start_render();
        return;
    case TA_LIST_INIT:
        if (value & 0x80000000u)
        {
            ta_keep_list();
            s_taIsp = REG(TA_ISP_BASE) & 0x7FFFFFu;
            if (s_doneIsp == s_taIsp) s_doneValid = 0;
            ta_reset();
            REG(TA_ITP_CURRENT) = REG(TA_ISP_BASE);
            REG(TA_NEXT_OPB) = REG(TA_NEXT_OPB_INIT);
        }
        return;
    case TA_YUV_TEX_BASE:
    case TA_YUV_TEX_CTRL:
        REG(a) = value;
        yuv_init();
        return;
    case TA_YUV_TEX_CNT:
        return;
    case TA_LIST_CONT:
        if (value & 0x80000000u) s_list = LIST_NONE;
        return;
    case FB_R_CTRL:
    case SPG_LOAD:
    case SPG_CONTROL:
        REG(a) = value;
        spg_recalc();
        spg_reschedule();
        return;
    case SPG_VBLANK_INT:
    case SPG_HBLANK_INT:
        REG(a) = value;
        spg_reschedule();
        return;
    }
    REG(a) = value;
}

int g_pvr_ta_skip;
static int s_skipHalf;

void pvr_ta_sq(const u32 *words)
{
    if (!g_pvr_ta_skip && s_fifoWords == 0)
    {
        u32 n = ta_param_words(words[0]);
        if (n == 8)
        {
            s_paramWords = 8;
            ta_param(words);
            return;
        }
    }
    pvr_ta_fifo(words, 8);
}

void pvr_ta_fifo(const u32 *words, u32 n)
{
    u32 i;
    if (g_pvr_ta_skip && n == 8 && s_fifoWords == 0)
    {
        if (s_skipHalf)
        {
            s_skipHalf = 0;
            return;
        }
        if (PCW_PARA_TYPE(words[0]) == PARA_VERTEX)
        {
            s_skipHalf = s_vertexWords == 16;
            return;
        }
    }
    if (n == 8 && s_fifoWords == 0)
    {
        s_paramWords = ta_param_words(words[0]);
        if (s_paramWords == 8)
        {
            ta_param(words);
            return;
        }
        memcpy(s_fifo, words, 32);
        s_fifoWords = 8;
        return;
    }
    if (n == 8 && s_fifoWords == 8 && s_paramWords == 16)
    {
        memcpy(s_fifo + 8, words, 32);
        ta_param(s_fifo);
        s_fifoWords = 0;
        return;
    }
    for (i = 0; i < n; i++) ta_write_word(words[i]);
}

u32 pvr_area4_read(u32 addr, int size)
{
    static u32 warned;
    (void)size;
    if (warned < 8 && ++warned) LOG_WARN("[pvr] read from TA area %08X%s", addr, warned == 8 ? " (no more warnings)" : "");
    return 0;
}

void pvr_area4_write(u32 addr, u32 value, int size)
{
    u32 region = (addr >> 24) & 3;
    if (region == 0 || region == 2)
    {
        if ((addr & 0x00800000u) == 0)
        {
            if (size == 4) ta_write_word(value);
            return;
        }
        yuv_write(value, size);
        return;
    }
    {
        int mode32 = (region == 1 ? holly_reg(SB_LMMODE0) : holly_reg(SB_LMMODE1)) & 1;
        u32 dst = (mode32 ? 0x05000000u : 0x04000000u) | (addr & 0x00FFFFFFu);
        if (size == 4) mem_write32(dst, value);
        else if (size == 2) mem_write16(dst, (u16)value);
        else mem_write8(dst, (u8)value);
    }
}

int pvr_read_framebuffer(u32 *out, int *w, int *h)
{
    u32 ctrl = REG(FB_R_CTRL), size = REG(FB_R_SIZE), base = REG(FB_R_SOF1);
    u32 depth = (ctrl >> 2) & 3, xWords = (size & 0x3FF) + 1, lines = ((size >> 10) & 0x3FF) + 1;
    u32 modulus = (size >> 20) & 0x3FF, bpp = depth == 2 ? 3 : depth == 3 ? 4 : 2;
    u32 width = xWords * 4 / bpp, x, y;
    if (!(ctrl & 1)) return 0;
    if (width > 1024) width = 1024;
    if (lines > 1024) lines = 1024;
    for (y = 0; y < lines; y++)
    {
        u32 line = base + y * (xWords + modulus - 1) * 4;
        for (x = 0; x < width; x++)
        {
            u32 off = line + x * bpp, px;
            u8 *v = g_vram;
            switch (depth)
            {
            case 0: case 1:
            {
                u32 o = vram_offset64(off & 0x7FFFFE);
                u16 c = buf_r16(v, o);
                if (depth == 0)
                    px = ((c >> 10 & 31) << 19) | ((c >> 5 & 31) << 11) | ((c & 31) << 3);
                else
                    px = ((c >> 11 & 31) << 19) | ((c >> 5 & 63) << 10) | ((c & 31) << 3);
                break;
            }
            case 2:
                px = (u32)buf_r8(v, vram_offset64(off & 0x7FFFFF)) |
                     ((u32)buf_r8(v, vram_offset64((off + 1) & 0x7FFFFF)) << 8) |
                     ((u32)buf_r8(v, vram_offset64((off + 2) & 0x7FFFFF)) << 16);
                break;
            default:
                px = buf_r32(v, vram_offset64(off & 0x7FFFFC)) & 0x00FFFFFF;
                break;
            }
            out[y * width + x] = px;
        }
    }
    *w = (int)width;
    *h = (int)lines;
    return 1;
}

void pvr_init(void)
{
    ta_alloc();
    s_evLine = sched_register("spg_line", spg_line, NULL);
    s_evRender = sched_register("render_done", render_done, NULL);
}

void pvr_reset(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    REG(SPG_LOAD) = (524u << 16) | 857u;
    REG(SPG_VBLANK_INT) = (21u << 16) | 260u;
    REG(SPG_VBLANK) = (21u << 16) | 260u;
    REG(SPG_HBLANK_INT) = 0x31D << 16;
    REG(SPG_CONTROL) = 0x10;
    REG(PT_ALPHA_REF) = 0xFF;
    s_line = 0;
    s_field = 0;
    s_frames = 0;
    spg_recalc();
    ta_reset();
    s_doneValid = 0;
    s_taIsp = 0;
    yuv_init();
    memset(&s_lastStats, 0, sizeof(s_lastStats));
    s_renders = 0;
    s_nextLine = 1;
    sched_after(s_evLine, s_lineCycles);
}

void pvr_state(StateIO *io)
{
    state_tag(io, "PVR ");
    STATE(io, s_regs);
    if (!io->loading) s_line = cur_line();
    STATE(io, s_lineCycles); STATE(io, s_lines); STATE(io, s_line); STATE(io, s_field); STATE(io, s_frames);
    if (io->loading) s_nextLine = s_line + lines_ahead();
    STATE(io, s_taWords);
    if (s_taWords > TA_MAX_WORDS)
    {
        io->error = 1;
        return;
    }
    state_io(io, s_taData, s_taWords * 4);
    if (io->loading)
    {
        s_doneValid = 0;
        s_taIsp = REG(TA_ISP_BASE) & 0x7FFFFFu;
    }
    STATE(io, s_fifo); STATE(io, s_fifoWords); STATE(io, s_paramWords); STATE(io, s_list); STATE(io, s_vertexWords);
    STATE(io, s_stats); STATE(io, s_lastStats); STATE(io, s_renders);
    STATE(io, s_yuvIn); STATE(io, s_yuvFill); STATE(io, s_yuvDest); STATE(io, s_yuvX); STATE(io, s_yuvW); STATE(io, s_yuvBlocks);
    if (io->loading && !(REG(PT_ALPHA_REF) & 0xFF)) REG(PT_ALPHA_REF) = 0xFF;
    if (io->loading) g_pvr_palette_gen++;
}
