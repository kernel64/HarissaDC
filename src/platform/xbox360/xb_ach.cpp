// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_ach.h"

#include <xtl.h>
#include <xbox.h>
#include <string.h>

#include "../../core/log.h"
#include "../../core/types.h"
#include "xb_ra.h"

struct AchSlot
{
    XUSER_ACHIEVEMENT ach;
    XOVERLAPPED ov;
};
static AchSlot s_slots[XB_ACH_COUNT + 1];
static u8 s_asked[XB_ACH_COUNT + 1];
static CRITICAL_SECTION s_lock;
static int s_ready, s_enabled;

void XbAchInit(int enabled)
{
    if (!s_ready)
    {
        InitializeCriticalSection(&s_lock);
        s_ready = 1;
    }
    s_enabled = enabled;
}

void XbAchUnlock(int id)
{
    DWORD user, r;
    AchSlot *s;
    if (!s_ready || !s_enabled || id < 1 || id > XB_ACH_COUNT) return;
    EnterCriticalSection(&s_lock);
    if (s_asked[id])
    {
        LeaveCriticalSection(&s_lock);
        return;
    }
    for (user = 0; user < 4; user++)
        if (XUserGetSigninState(user) != eXUserSigninState_NotSignedIn) break;
    if (user == 4)
    {
        LeaveCriticalSection(&s_lock);
        LOG_INFO("[ach] achievement %d: no profile signed in", id);
        return;
    }
    s_asked[id] = 1;
    s = &s_slots[id];
    memset(s, 0, sizeof(*s));
    s->ach.dwUserIndex = user;
    s->ach.dwAchievementId = (DWORD)id;
    r = XUserWriteAchievements(1, &s->ach, &s->ov);
    LeaveCriticalSection(&s_lock);
    LOG_INFO("[ach] achievement %d for profile %lu: %s (%lu)", id, user, r == ERROR_IO_PENDING || r == ERROR_SUCCESS ? "asked" : "refused", r);
}

void XbAchCheckRa(void)
{
    static const struct { int id; const char *title, *title2; } named[5] = {
        { XB_ACH_SHENMUE, "Shenmue", NULL },
        { XB_ACH_SONIC, "Sonic Adventure", NULL },
        { XB_ACH_SOULCALIBUR, "Soulcalibur", "Soul Calibur" },
        { XB_ACH_CRAZY_TAXI, "Crazy Taxi", NULL },
        { XB_ACH_JET_SET_RADIO, "Jet Set Radio", "Jet Grind Radio" },
    };
    XbRaTotals t;
    int i;
    XbRaTotalsGet(&t);
    if (t.unlocks >= 1) XbAchUnlock(XB_ACH_FIRST);
    if (t.unlocks >= 10) XbAchUnlock(XB_ACH_COLLECT_10);
    if (t.unlocks >= 50) XbAchUnlock(XB_ACH_COLLECT_50);
    if (t.unlocks >= 100) XbAchUnlock(XB_ACH_COLLECT_100);
    if (t.unlocks >= 250) XbAchUnlock(XB_ACH_COLLECT_250);
    if (t.unlocks >= 500) XbAchUnlock(XB_ACH_COLLECT_500);
    if (t.points >= 100) XbAchUnlock(XB_ACH_POINTS_100);
    if (t.points >= 1000) XbAchUnlock(XB_ACH_POINTS_1000);
    if (t.points >= 5000) XbAchUnlock(XB_ACH_POINTS_5000);
    if (t.games >= 3) XbAchUnlock(XB_ACH_GAMES_3);
    if (t.games >= 10) XbAchUnlock(XB_ACH_GAMES_10);
    if (t.games >= 25) XbAchUnlock(XB_ACH_GAMES_25);
    if (t.completed >= 1) XbAchUnlock(XB_ACH_MASTER_1);
    if (t.completed >= 3) XbAchUnlock(XB_ACH_MASTER_3);
    for (i = 0; i < 5; i++)
        if (XbRaUnlocksTitled(named[i].title) + (named[i].title2 ? XbRaUnlocksTitled(named[i].title2) : 0) >= 10) XbAchUnlock(named[i].id);
}
