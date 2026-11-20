# Dreamcast cheat database

`dreamcast_cheats.txt` is libretro-database's Dreamcast cheats
(`cht/Sega - Dreamcast`, https://github.com/libretro/libretro-database), converted by
`tools/build_cheats.py`: one file, games keyed by the product numbers of the Redump list
(`metadat/redump/Sega - Dreamcast.dat`), each cheat's RetroArch entries as operations
(`src/hw/cheat.h`).

The source files are licensed under the Creative Commons Attribution-ShareAlike 4.0
International licence (CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/);
this converted version is distributed under the same licence. The emulator reads it as
data: it is not part of the program and its licence does not apply to the program.

The release zip puts it in `cheats\dreamcast_cheats.txt`, beside the emulator.
