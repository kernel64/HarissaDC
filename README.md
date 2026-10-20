# HarissaDC

A Dreamcast emulator for the Xbox 360 (RGH / JTAG consoles).

HarissaDC is in **early development**; its builds are shared to get feedback from the community.
Many games work, some have graphical glitches or slow scenes, and some do not start
yet. Your reports are what will decide what gets fixed first.

## Download

Get the latest build from the [Releases](https://github.com/kernel64/HarissaDC/releases) page: the zip holds the
`HarissaDC` folder to copy to the console and this documentation.

Version française : [LISEZMOI.md](LISEZMOI.md).

## What you need

- An Xbox 360 that runs unsigned code (RGH or JTAG) with a homebrew launcher
  (Aurora, FreeStyle Dash, XeXMenu...).
- Your own Dreamcast games, dumped from your discs.
- No Dreamcast BIOS is needed: HarissaDC has its own.

## Installation

1. Extract the zip. You get a `HarissaDC` folder with `default.xex` and an empty
   `games` folder.
2. Copy the whole `HarissaDC` folder to the console, for example to
   `Hdd:\Emulators\HarissaDC` (with FTP or a USB stick).
3. Put your games in `HarissaDC\games` (sub-folders are fine, one per game is tidy).
4. Launch `default.xex` from your launcher (in Aurora: add the folder to the
   Homebrew paths, or browse to it with the file manager).

On the first start, choose your language. The game list then shows your games.

### Game formats

| Format | Notes |
|---|---|
| `.gdi` + track files | Keep the tracks next to the `.gdi`. |
| `.cue` + `.bin` | Redump dumps (GD-ROM layout with `REM HIGH-DENSITY AREA`). |
| `.chd` | CHD version 5 (`chdman createcd`). Older CHDs: convert with `chdman copy`. |
| `.iso` | A data track only. |
| `.7z` | One game per archive; extracted once into `cache\` on the first start. |

Multi-disc games: name the files `Game (Disc 1).gdi`, `Game (Disc 2).gdi`... and they
show as one entry. Change discs from the in-game menu.

## Controls

**Game list**: A play, Y settings, X about, LB / RB page up / down, BACK quit.

**In game**:

| Buttons | Action |
|---|---|
| BACK | Emulator menu (resume, reset, save / load state, change disc, settings, back to the list, quit) |
| BACK + RB | Quick save in the current slot |
| BACK + LB | Quick load from the current slot |
| BACK + D-pad left / right | Change the quick save slot |

Up to 4 controllers are supported (Dreamcast ports A to D). Every Dreamcast button,
the D-pad included, can be remapped per player in **Settings > Controls**: select a
button, press A, then press the Xbox input you want. Vibration (the Dreamcast
vibration pack) is on by default and can be turned off on the same page.

## Settings

- **Graphics**: presets (Original, Enhanced, Smooth, CRT), 4:3 or stretched, size and
  position of the picture, brightness, smoothing, FXAA, sharpening, blur, scanlines,
  CRT and LCD effects.
- **CPU / Region / Renderer**: take effect at the next start of a game.
- Settings are kept with **Save for all games** (or **Save for this game only** from the
  in-game menu).

## Box art

When the console has a network connection, the game list downloads box art
(from the libretro thumbnails) and keeps it in the `art` folder. **Settings > Box art
downloads** turns it off; **Get box art for all games** fetches everything at once.
The emulator also checks GitHub for a newer release when the list opens.

## Files HarissaDC creates

| Folder / file | Contents |
|---|---|
| `config\` | Settings (all games, and per game) |
| `saves\` | Save states and the virtual memory card (VMU) |
| `art\` | Box art cache |
| `cache\` | Games extracted from `.7z` archives |
| `harissadc.log` | Log of the last session (attach it to bug reports) |

## Known issues

- Some games have graphical glitches (wrong colours, things drawn in front of or
  behind others), for example in some menus of Daytona USA 2001.
- Some cutscenes and videos run slowly (for example in Jet Grind Radio) while the
  game itself is fine.
- Loading from `.chd` is slower than from `.gdi` / `.cue`.
- Vibration strength is an approximation of the Dreamcast pack.

## Reporting problems

Please open an issue on <https://github.com/kernel64/HarissaDC/issues> with:

- the game, its region and its format (`.gdi`, `.cue`, `.chd`...);
- what happens and where (a photo or a video helps a lot);
- the `harissadc.log` file from the emulator folder, taken right after the problem.

A save state (BACK + RB) made just before the problem is very welcome too.

## Credits

HarissaDC is made by Mohamed Aymen (kernel64). Hardware documentation from the
Dreamcast homebrew community; nullDC, Flycast and nulldc-360 were studied for
reference. The interface comes from Harissa64. Third-party components and their
notices are listed in `THIRD_PARTY_NOTICES.txt`.

Dreamcast is a trademark of SEGA. This project is not affiliated with SEGA. Only play
games you own.
