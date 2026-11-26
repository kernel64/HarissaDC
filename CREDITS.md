# Credits

HarissaDC was written for the Xbox 360 after studying these emulators (studied, not copied):

- **nullDC4Wii** — BenoitAdam94 (https://github.com/BenoitAdam/nullDC4Wii), a fork of nullDCe (skmp)
  and nullDC (drk||Raziel, ZeZu).
- **nulldc-360** — gligli, GPLv3 (https://github.com/gligli/nulldc-360).
- **Flycast** — flyinghead and contributors, GPLv2 (https://github.com/flyinghead/flycast).

Code from other projects:

- **Harissa64** — Mohamed Aymen (https://github.com/kernel64/Harissa64), same author: the user
  interface (`xb_ui.cpp`, `xb_ui_atlas.h`), PNG decoding (`xb_png.cpp`), flags (`xb_flags.h`),
  languages (`xb_lang.cpp`, `tools/gen_lang.py`), the language screen, the display pass
  (`xb_display.cpp`), HTTPS (`xb_net.cpp`) and box art (`xb_art.cpp`, adapted to Dreamcast discs).
- **zlib 1.3.1** (inflate, CRC-32, adler32) — Jean-loup Gailly and Mark Adler, zlib license
  (`src/third_party/zlib/LICENSE`, unmodified). PNG decoding.
- **BearSSL 0.6** (TLS for the box art downloads) — Thomas Pornin, MIT license
  (`src/third_party/bearssl/LICENSE.txt`, unmodified).
- **LZMA SDK** (7z decoder) — Igor Pavlov, public domain.
- **SoundTouch 2.3.3** (audio time stretching below full speed) — Olli Parviainen, LGPL-2.1
  (`src/third_party/soundtouch/COPYING.TXT`), https://codeberg.org/soundtouch/soundtouch.
  Only the time-stretching files are used; local change: `override` removed from destructors
  (rejected by the Xbox 360 SDK's compiler).
- **CHD**: MAME's "Compressed Hunks of Data" format (Aaron Giles, BSD-3). HarissaDC's reader
  (`src/disc/chd.cpp`, FLAC decoder and CD ECC included) is its own code.
- **Box art**: the Redump list of Dreamcast discs from libretro-database
  (https://github.com/libretro/libretro-database) and pictures from libretro-thumbnails
  (https://thumbnails.libretro.com), downloaded by the console, never included.
- **rcheevos 12.5** (RetroAchievements' runtime and client) — RetroAchievements, MIT license
  (`src/third_party/rcheevos/LICENSE`), https://github.com/RetroAchievements/rcheevos. The copy
  comes from Harissa64's: `md5.c` renamed `rc_md5.c`, `xbox/` for the Xbox 360 SDK (a `windows.h`
  that includes `xtl.h`, `snprintf`), and memory values read in little-endian order on big-endian
  hosts (`rc_memref.c`). The disc hash (`src/hw/ra_dc.cpp`) follows its `rc_hash_dreamcast`.
- **Cheats**: libretro-database's Dreamcast cheat files (https://github.com/libretro/libretro-database,
  CC BY-SA 4.0), converted to `third_party/libretro-cheats/dreamcast_cheats.txt` by
  `tools/build_cheats.py`. The cheat formats (RetroArch's .cht entries, the Dreamcast code types)
  were studied from RetroArch's cheat manager and Flycast's cheat list (studied, not copied).
- **16:9 patches**: Flycast's table of widescreen game patches (`core/cheats.cpp`, GPL-2.0-or-later,
  https://github.com/flyinghead/flycast), codes by Esppiral, S4pph4rad, yzb37859365,
  Shenmue_Trilogy, Radaron, Virgin KLM, Joel, Zorlon, ELOTROLADO.NET, SEGARETRO.ORG and Sakuragi
  @ emutalk.net, gathered by flyinghead. Its Dreamcast and NAOMI / Atomiswave entries are
  converted to `src/hw/widescreen_patches.inc` and `widescreen_patches_arcade.inc` by
  `tools/build_widescreen.py`; the code applying them (`src/hw/widescreen.cpp`) is HarissaDC's
  own.
- **Inter** (interface font, as an atlas) — The Inter Project Authors, SIL Open Font License 1.1
  (`third_party/inter/OFL.txt`).
- **Noto Sans Arabic** (Arabic glyphs of the atlas) — The Noto Project Authors, SIL Open Font
  License 1.1 (`third_party/noto-arabic/OFL.txt`).
- The sharpening filter follows the idea of AMD FidelityFX CAS (MIT) and the CRT effects Timothy
  Lottes' CRT shader (public domain).
