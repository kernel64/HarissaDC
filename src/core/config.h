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
    PADSRC_LS, PADSRC_RS, PADSRC_DUP, PADSRC_DDOWN, PADSRC_DLEFT, PADSRC_DRIGHT,
    PADSRC_LSUP, PADSRC_LSDOWN, PADSRC_LSLEFT, PADSRC_LSRIGHT, PADSRC_RSUP, PADSRC_RSDOWN, PADSRC_RSLEFT, PADSRC_RSRIGHT,
    PADSRC_COUNT
};
enum { PAD_STICK_LEFT = 0, PAD_STICK_RIGHT = 1, PAD_STICK_NONE = 2 };
u32 config_stick_dirs(int x, int y);
int config_pad_stick_dpad(const u32 map[PADIN_COUNT]);
void config_pad_set_stick_dpad(u32 map[PADIN_COUNT], int stick);
const char *config_pad_source_key(int src);
const char *config_pad_source_name(int src);
const char *config_pad_input_name(int in);
#define PAD_PLAYERS 4
void config_pad_defaults(u32 map[PADIN_COUNT], int *stick);

enum { BIOS_HLE = 0, BIOS_REAL = 1, BIOS_AUTO = 2 };

enum { CONFIG_WIDE_OFF = 0, CONFIG_WIDE_AUTO = 1, CONFIG_WIDE_REVEAL = 2, CONFIG_WIDE_ANAMORPHIC = 3 };

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
    int gunX, gunY;
    int gunHpos;
    int mmuSq;
    int renderWait;
    int fbReadback;
    int cable;
    int taSkip;
    int audioMs;
    int audioStretch;
    int cdRealtime;
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
    int countdown;
    int slowCpu;
    int palShader;
    int exactDepth;
    int hugeDepth;
    int transDepth;
    int fog;
    int aicaTrace;
    int soundLag;
    int sceneDump;
    int gpuDump, gpuDumpCount;
    int sceneDumpCount, sceneDumpEvery;
    int gpuDisplay;
    int wavDump, wavDumpAt;
    int showFps;
    int vsync;
    int texFilter;
    int renderScale;
    int artAll;
    int updateCheck;
    int rumble;
    int vmuShared;
    int lightGun;
    int rotate;
    int artDownload;
    int aspect;
    int widescreen;
    int sideFill;
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
    char renderTest[CONFIG_PATH_MAX];
    char cheats[256];
    int cheatOn;
    int achievements;
    int raTest;
    int raToast;
    int achAt;
    int noNet;
    char disc[CONFIG_PATH_MAX];
    char dataDir[CONFIG_PATH_MAX];
    int exitToDash;
    char dumpBlocks[CONFIG_PATH_MAX];
    int dumpBlocksAt;
    int jitTrail;
    int jitMap;
};

void config_defaults(Config *c);
int config_parse_text(Config *c, const char *text);
int config_write(const Config *c, int withLastGame, char *out, int size);

#endif
