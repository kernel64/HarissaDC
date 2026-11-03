// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_pad.h"

#include <stdlib.h>
#include <string.h>

#include "../../core/config.h"
#include "../../core/log.h"

struct PadEvent
{
    DWORD first, last;
    WORD buttons;
    BYTE lt, rt;
    int axis;
    int value;
};

static PadEvent s_events[128];
static int s_count, s_active;
static LARGE_INTEGER s_start, s_freq;

static DWORD now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (DWORD)((t.QuadPart - s_start.QuadPart) * 1000 / s_freq.QuadPart);
}

int XbPadScript(const char *text)
{
    static const struct { const char *name; WORD bit; } names[] = {
        { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B }, { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y },
        { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK }, { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER },
        { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER }, { "DU", XINPUT_GAMEPAD_DPAD_UP }, { "DD", XINPUT_GAMEPAD_DPAD_DOWN },
        { "DL", XINPUT_GAMEPAD_DPAD_LEFT }, { "DR", XINPUT_GAMEPAD_DPAD_RIGHT }, { "LS", XINPUT_GAMEPAD_LEFT_THUMB },
        { "RS", XINPUT_GAMEPAD_RIGHT_THUMB } };
    const char *p = text;
    s_count = 0;
    s_active = 0;
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_start);
    while (p && *p && s_count < (int)(sizeof(s_events) / sizeof(s_events[0])))
    {
        PadEvent *e = &s_events[s_count];
        char name[16];
        int n = 0;
        unsigned i;
        memset(e, 0, sizeof(*e));
        while (*p == ' ') p++;
        while (*p && *p != '@' && *p != '=' && n < 15) name[n++] = *p++;
        name[n] = 0;
        if (*p == '=')
        {
            e->axis = !strcmp(name, "LX") ? 1 : !strcmp(name, "LY") ? 2 : 0;
            e->value = (int)strtol(p + 1, (char **)&p, 10);
        }
        else if (!strcmp(name, "LT"))
            e->lt = 255;
        else if (!strcmp(name, "RT"))
            e->rt = 255;
        else
            for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
                if (!strcmp(name, names[i].name)) e->buttons = names[i].bit;
        if (!e->buttons && !e->axis && !e->lt && !e->rt)
        {
            LOG_ERROR("[pad] script: bad event \"%s\"", name);
            s_count = 0;
            return -1;
        }
        if (*p != '@')
        {
            LOG_ERROR("[pad] script: event \"%s\" needs @milliseconds", name);
            s_count = 0;
            return -1;
        }
        e->first = e->last = (DWORD)strtoul(p + 1, (char **)&p, 10);
        if (*p == '-') e->last = (DWORD)strtoul(p + 1, (char **)&p, 10);
        if (*p == ',') p++;
        s_count++;
    }
    s_active = s_count > 0;
    if (s_active) LOG_INFO("[pad] scripted controller: %d events", s_count);
    return 0;
}

void XbPadGameStarts(void)
{
    static int done;
    if (done || !s_active) return;
    done = 1;
    QueryPerformanceCounter(&s_start);
    LOG_INFO("[pad] script clock starts with the game");
}

DWORD XbPadGet(DWORD user, XINPUT_STATE *state)
{
    DWORD now;
    int i;
    if (!s_active) return XInputGetState(user, state);
    memset(state, 0, sizeof(*state));
    if (user != 0) return ERROR_DEVICE_NOT_CONNECTED;
    now = now_ms();
    state->dwPacketNumber = now;
    for (i = 0; i < s_count; i++)
        if (now >= s_events[i].first && now <= s_events[i].last)
        {
            state->Gamepad.wButtons |= s_events[i].buttons;
            if (s_events[i].lt) state->Gamepad.bLeftTrigger = s_events[i].lt;
            if (s_events[i].rt) state->Gamepad.bRightTrigger = s_events[i].rt;
            if (s_events[i].axis == 1) state->Gamepad.sThumbLX = (SHORT)s_events[i].value;
            if (s_events[i].axis == 2) state->Gamepad.sThumbLY = (SHORT)s_events[i].value;
        }
    return ERROR_SUCCESS;
}

int XbPadSource(const XINPUT_GAMEPAD *g, int src)
{
    static const WORD bits[PADSRC_COUNT] = { 0, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                                            XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0,
                                            XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
                                            XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT };
    if (src == PADSRC_LT) return g->bLeftTrigger;
    if (src == PADSRC_RT) return g->bRightTrigger;
    if (src <= PADSRC_NONE || src >= PADSRC_COUNT) return 0;
    return (g->wButtons & bits[src]) ? 255 : 0;
}

int XbPadSources(const XINPUT_GAMEPAD *g, u32 mask)
{
    int src, best = 0;
    for (src = 1; src < PADSRC_COUNT; src++)
        if (mask & (1u << src))
        {
            int v = XbPadSource(g, src);
            if (v > best) best = v;
        }
    return best;
}

u32 XbPadPressed(const XINPUT_GAMEPAD *g)
{
    u32 mask = 0;
    int src;
    for (src = 1; src < PADSRC_COUNT; src++)
        if (XbPadSource(g, src) > 128) mask |= 1u << src;
    return mask;
}
