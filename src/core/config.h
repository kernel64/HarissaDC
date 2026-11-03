// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_CONFIG_H
#define EMU_CONFIG_H

#include "types.h"

#define CONFIG_PATH_MAX 256

enum { PADIN_A, PADIN_B, PADIN_X, PADIN_Y, PADIN_START, PADIN_L, PADIN_R, PADIN_UP, PADIN_DOWN, PADIN_LEFT, PADIN_RIGHT, PADIN_COUNT };
enum
{
    PADSRC_NONE, PADSRC_A, PADSRC_B, PADSRC_X, PADSRC_Y, PADSRC_LB, PADSRC_RB, PADSRC_LT, PADSRC_RT, PADSRC_START,
    PADSRC_LS, PADSRC_RS, PADSRC_DUP, PADSRC_DDOWN, PADSRC_DLEFT, PADSRC_DRIGHT, PADSRC_COUNT
};
const char *config_pad_source_key(int src);
const char *config_pad_source_name(int src);
const char *config_pad_input_name(int in);
#define PAD_PLAYERS 4
void config_pad_defaults(u32 map[PADIN_COUNT], int *stick);

enum { BIOS_HLE = 0, BIOS_REAL = 1, BIOS_AUTO = 2 };

struct Config
{
    int logLevel;
    int biosMode;
    int region;
    char biosDir[CONFIG_PATH_MAX];
    char gamesDir[CONFIG_PATH_MAX];
    int mountDrives;
    int showLog;
    int diag;
    int exitAfter;
    int saveStateAt;
    int loadState;
    int traceField;
    int sampler;
    int taSkip;
    int audioMs;
    int audioStretch;
    int slice;
    int selfTest;
    int toneHz;
    int useJit;
    int gpuRender;
    int soundThread;
    int armJit;
    int fastFpu;
    int jitSplit;
    int jitSkips;
    int fpuBench;
    char xinput[512];
    char lang[8];
    char uiLang[8];
    int fastMem;
    int sh4Clock;
    int idleSkip;
    int transSort;
    int modVols;
    int transDepth;
    int fog;
    int aicaTrace;
    int soundLag;
    int sceneDump;
    int sceneDumpCount, sceneDumpEvery;
    int gpuDisplay;
    int showFps;
    int vsync;
    int texFilter;
    int artAll;
    int updateCheck;
    int rumble;
    int lightGun;
    int rotate;
    int artDownload;
    int aspect;
    int widescreen;
    int smooth;
    int fxaa;
    int sharpen;
    int blur;
    int screen;
    int brightness;
    int zoom;
    int posX, posY;
    u32 padMap[PAD_PLAYERS][PADIN_COUNT];
    int padStick[PAD_PLAYERS];
    int padDead[PAD_PLAYERS];
    int padRange[PAD_PLAYERS];
    int padInvert[PAD_PLAYERS];
    char lastGame[CONFIG_PATH_MAX];
    int autoBoot;
    char disc[CONFIG_PATH_MAX];
    char dumpBlocks[CONFIG_PATH_MAX];
    int dumpBlocksAt;
    int jitMap;
};

void config_defaults(Config *c);
int config_parse_text(Config *c, const char *text);
int config_write(const Config *c, int withLastGame, char *out, int size);

#endif
