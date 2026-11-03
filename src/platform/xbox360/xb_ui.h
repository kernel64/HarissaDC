// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef XB_UI_H
#define XB_UI_H

#include <xtl.h>

#define UI_WIDTH 1280
#define UI_HEIGHT 720
#define UI_SAFE_X 64
#define UI_SAFE_Y 36

#define UI_COL_BG_TOP     D3DCOLOR_ARGB(255, 18, 19, 26)
#define UI_COL_BG_BOTTOM  D3DCOLOR_ARGB(255, 10, 11, 15)
#define UI_COL_PANEL      D3DCOLOR_ARGB(244, 27, 29, 38)
#define UI_COL_PANEL_TOP  D3DCOLOR_ARGB(244, 33, 35, 46)
#define UI_COL_CARD       D3DCOLOR_ARGB(255, 34, 36, 47)
#define UI_COL_LINE       D3DCOLOR_ARGB(26, 255, 255, 255)
#define UI_COL_TEXT       D3DCOLOR_ARGB(255, 243, 244, 247)
#define UI_COL_TEXT2      D3DCOLOR_ARGB(255, 164, 168, 182)
#define UI_COL_TEXT3      D3DCOLOR_ARGB(255, 112, 116, 132)
#define UI_COL_ACCENT     D3DCOLOR_ARGB(255, 240, 112, 32)
#define UI_COL_ACCENT_HI  D3DCOLOR_ARGB(255, 255, 150, 70)
#define UI_COL_SELECT     D3DCOLOR_ARGB(64, 240, 112, 32)
#define UI_COL_SHADOW     D3DCOLOR_ARGB(150, 0, 0, 0)
#define UI_COL_DIM        D3DCOLOR_ARGB(150, 4, 5, 8)

enum { UI_REGULAR = 0, UI_BOLD = 1 };
enum { UI_LEFT = 0, UI_CENTER = 1, UI_RIGHT = 2 };

void UiBegin(IDirect3DDevice9 *dev);
void UiEnd(void);
void UiSuspend(void);
void UiResume(void);
float UiTime(void);

void UiRoundRect(float x, float y, float w, float h, float r, D3DCOLOR top, D3DCOLOR bottom);
void UiRect(float x, float y, float w, float h, D3DCOLOR color);
void UiRoundRing(float x, float y, float w, float h, float r, float thick, D3DCOLOR color);
void UiShadow(float x, float y, float w, float h, float r, float blur, D3DCOLOR color);
void UiCircle(float cx, float cy, float r, D3DCOLOR color);
void UiBackdrop(void);
void UiDim(void);
void UiPanel(float x, float y, float w, float h, float r);
D3DCOLOR UiAlpha(D3DCOLOR c, float a);

IDirect3DTexture9 *UiImageCreate(IDirect3DDevice9 *dev, const unsigned char *rgba, int w, int h);
void UiImageRelease(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex);
void UiImage(IDirect3DTexture9 *tex, float x, float y, float w, float h, float r, D3DCOLOR color);

float UiText(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align);
float UiTextShadowed(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align);
float UiTextWidth(int font, float size, const char *text);
void UiTextFit(int font, float size, float maxW, const char *text, char *out, int outSize);
int UiTextWrap(int font, float size, float maxW, int maxLines, const char *text, char *out, int outSize);
#define UI_CH_LCHEVRON "\xE2\x80\xB9"
#define UI_CH_RCHEVRON "\xE2\x80\xBA"
#define UI_CH_ELLIPSIS "\xE2\x80\xA6"
#define UI_CH_BULLET   "\xE2\x80\xA2"
#define UI_CH_MIDDOT   "\xC2\xB7"

float UiLogo(float x, float y, float size);
float UiBrand(float x, float cy, float size);

float UiFooter(float x, float cy, const char *hints);
float UiFooterWidth(const char *hints);
void UiScreenFooter(const char *hints);

void UiToast(const char *text, float age, float life);
void UiFpsBadge(const char *text);

struct UiInput
{
    WORD held, prev;
    DWORD repeatAt, holdStart;
};
void UiInputInit(UiInput *in);
WORD UiInputPoll(UiInput *in);
#define UI_BTN_LT 0x0400
#define UI_BTN_RT 0x0800

#endif
