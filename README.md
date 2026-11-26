# HarissaDC

A Dreamcast emulator for the Xbox 360 (RGH / JTAG consoles).

HarissaDC is in **early development**; its builds are shared to get feedback from the community.
Many games work, some have graphical glitches or slow scenes, and some do not start
yet. Your reports are what will decide what gets fixed first.

## Download

Get the latest build from the [Releases](https://github.com/kernel64/HarissaDC/releases) page: the zip holds the
`HarissaDC` folder to copy to the console and this documentation.

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
| `.cdi` | DiscJuggler images. |
| `.7z`, `.zip` | One game per archive; extracted once into `cache\` on the first start. |

### Games on several discs

Put the discs in the same folder and give them the same name with `(Disc 1)`, `(Disc 2)`...
(`[Disc 2]` and `Disc 2` work too), in any format:

```
games\Resident Evil Code Veronica\Resident Evil Code Veronica (Disc 1).chd
games\Resident Evil Code Veronica\Resident Evil Code Veronica (Disc 2).chd
```

The list shows one entry, which starts disc 1. When the game asks for the next disc,
press BACK, choose **Change disc**, then the disc: the lid opens and closes as on a
Dreamcast, and the game goes on.

### A game as its own entry in the dashboard

To show a game in Aurora (or another launcher) as its own title, make a small shortcut
folder for it, next to your `HarissaDC` folder:

1. Create a folder, for example `Hdd:\Emulators\Ikaruga`.
2. Copy HarissaDC's `default.xex` into it.
3. Next to it, create a text file `harissadc.ini`:

   ```
   autoboot=1
   disc=Hdd1:\Emulators\HarissaDC\games\Ikaruga (Japan).chd
   datadir=Hdd1:\Emulators\HarissaDC
   exitto=dashboard
   ```

- `autoboot=1` and `disc=` start the game directly, without the game list. Write the full
  path with `Hdd1:\` for the hard disk (not `Hdd:\`), `Usb0:\` for a USB stick.
- `datadir=` uses your main HarissaDC folder for the settings, memory cards, save states,
  cheats, box art and BIOS: the shortcut and the main install share them. Keep this path
  short (32 characters at most). Without it, the shortcut folder keeps its own.
- `exitto=dashboard`: leaving the game (BACK held, **Back to the game list** or
  **Quit to the dashboard**)
  returns to the dashboard.

The cover and background of the entry are set in Aurora, as for any homebrew.
When you update HarissaDC, copy the new `default.xex` into each shortcut folder too.

## Controls

**Game list**: A play, Y settings, X about, LB / RB page up / down, LT / RT previous /
next letter, B add or remove a favourite (favourites are listed first), RS rename the game
(an empty name gives back its own), BACK quit.

**In game**:

| Buttons | Action |
|---|---|
| BACK | Emulator menu (resume, reset, save / load state, change disc, settings, back to the list, quit) |
| BACK + RB | Quick save in the current slot |
| BACK + LB | Quick load from the current slot |
| BACK + D-pad left / right | Change the quick save slot |
| BACK held (1.5 s) | Back to the game list |

Up to 4 controllers are supported (Dreamcast ports A to D). Every Dreamcast button,
the D-pad included, can be remapped per player in **Settings > Controls**: select a
button, press A, then press the Xbox input you want. The same page sets, per player,
which stick is the analog stick, its dead zone, sensitivity and inverted axes. A stick's
directions can also be given to any button, and **Stick as D-pad** plays the games made for
the D-pad (Dino Crisis...) with a stick. Vibration
(the Dreamcast vibration pack) is on by default and can be turned off there too.

**Light gun**: Ranger Mission, Sports Shooting USA, SEGA Clay Challenge and Extreme
Hunting 2 get a light gun by themselves: the left stick aims, RT or A fires, B fires off
the screen (reload), START is the gun's Start. For other shooting games, add
`lightgun=1` to `harissadc.ini`.

## Settings

The settings are sorted into pages, from the game list (Y) and from the in-game menu:

- **Picture**: aspect ratio (see **16:9** below), **Sides** (blurred or black, for a 4:3
  picture on a 16:9 setting), orientation, brightness, size and position of the picture,
  internal resolution (1.5x or 1x).
- **Effects**: presets (Original, Enhanced, Smooth, CRT), texture filter (as the game,
  nearest, bilinear), smoothing, FXAA, sharpening, blur, scanlines, CRT and LCD effects.
  16:9 and the texture filter need the GPU renderer.
- **Controls**: see above.
- **Interface**: language, FPS display (off, frame rate, a panel with speed, CPU, RAM and
  resolution), box art downloads.
- **Achievements**: RetroAchievements (see below).
- **Advanced**: CPU, renderer, BIOS (built-in, Sega's, or Sega's when present), V-Sync,
  audio margin, CD audio (follow the game, or real time), **Fewer frames when slow** (when a
  game falls behind, it keeps its real speed and shows fewer pictures instead of running in
  slow motion; automatic for the games that need it), memory card (one per game, or one
  shared). CPU, renderer and BIOS take
  effect at the next start of a game. Sega's BIOS is optional: copy your own
  (`dc_boot.bin` and `dc_flash.bin`) to the `HarissaDC\bios` folder, and games start with
  the Dreamcast's boot animation, as on the real console.
- The console region (Japan, USA, Europe) is chosen from each disc, nothing to set.
- Settings are kept with **Save for all games** (or **Save for this game only** from the
  in-game menu).

### 16:9

**Settings > Picture > Aspect ratio**:

- **4:3**: the Dreamcast's picture.
- **16:9**: for about 150 games, HarissaDC changes the game's own camera, so it shows a
  wider view at the TV's full resolution (1280x720); the game's 2D overlay (score, timer,
  menus) is stretched. Other games are shown 4:3, with the sides filled by a blurred copy of
  the picture (or black: **Sides**).
- **16:9 wider view**: for any 3D game, shows what the game draws beyond its 4:3 edges;
  when a game leaves its sides empty, the picture goes back to 4:3 by itself. Games that
  only draw what their 4:3 view shows can leave gaps or show objects popping in on the sides.
- **16:9 anamorphic**: for games that have their own 16:9 option (Wacky Races, Toy
  Commander...): turn it on in the game too.
- **16:9 stretched**: the 4:3 picture stretched over the whole screen.

The wider view and anamorphic need the GPU renderer. Like every setting, the choice can be
saved for one game only.

## Cheats

The in-game menu's **Cheats** page lists the codes known for the game (the Dreamcast
cheat database of libretro, in `cheats\dreamcast_cheats.txt`): A turns one on or off. Your
own codes go in the `cheats` folder, in a file named after the game, as a RetroArch `.cht`
file or a `.txt` list of codes.

## Achievements

- **RetroAchievements**: turn it on in **Settings > Achievements** and sign in with your
  retroachievements.org account (typed with the Xbox keyboard; only a login token is kept).
  Achievements unlock as you play (softcore), with a notification; the in-game menu lists
  the game's achievements, unlocked ones first. They pause while a cheat is on, until the
  game restarts.
- **Xbox 360 achievements**: HarissaDC has its own (23, 1000 G), earned with your
  RetroAchievements progress (first unlock, number of unlocks, points, games completed...).

> **Warning**: these achievements are written to your Xbox profile. Achievements from a
> homebrew title on a profile that is used online can get the account banned. If your
> profile goes online, do not use it with HarissaDC: use an offline profile.

## Box art

When the console has a network connection, the game list downloads box art
(from the libretro thumbnails, Dreamcast and Atomiswave) and keeps it in the `art`
folder. **Settings > Box art downloads** turns it off; **Get box art for all games**
fetches everything at once.

## Updates

When the game list opens and a newer release is out on GitHub, HarissaDC offers to install
it (A: Install, B: Not now). It downloads it, checks it, and replaces only `default.xex`
(the previous one kept as `default_previous.xex`) and the cheat database, then starts
again in the new version: save states, memory cards, settings, games and box art are kept.
If anything fails, nothing is changed. Shortcut folders (see above) are not updated: copy
the new `default.xex` into them.

## Files HarissaDC creates

| Folder / file | Contents |
|---|---|
| `config\` | Settings (all games, and per game), RetroAchievements login token |
| `saves\` | Save states, and the memory cards (VMU) in `saves\vmu\` (one per game) |
| `cheats\` | The cheat database, and your own cheat files |
| `art\` | Box art cache |
| `cache\` | Games extracted from `.7z` archives |
| `harissadc.log` | Log of the last session (attach it to bug reports) |

## Compatibility

[COMPATIBILITY.md](COMPATIBILITY.md) lists the games tested so far and how they run.

## Known issues

- Windows CE games (their discs show "Powered by Windows CE": Sega Rally 2,
  Resident Evil 2, Virtua Cop 2...) run at their real speed but show fewer distinct
  pictures in their heaviest scenes.
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

## Support the project

HarissaDC is free and always will be. It is built in my spare time: every game fixed means hours of tests on a real console. If it gives you good moments with your Dreamcast games and you would like to help it go further, a donation lets me spend more time on it: more games working, better speed, new features.

Any amount helps, and thank you.

### Donate in cryptocurrency

| Currency | Network | Address |
|---|---|---|
| Tether (USDT) | TRON (TRC20) | `TMWW2XeV9kY1U9Cv4AeW9Z6oXnuxXyBut8` |
| Bitcoin (BTC) | Bitcoin | `16GjM9aBDiTEkMyzcMq4sMWpdacc4zWkty` |
| Ether (ETH) | Ethereum | `0xcb9b2f3563249c9508eae26af396f21926ec928c` |
| Solana (SOL) | Solana | `mFxWKAEYnV5oKwYwtzmGCNfvWLNyqfL2FBWH3qSLSHU` |

Send each currency only on the network shown on its line: coins sent on another network are lost. Check the address before you send: a transfer can't be cancelled.

### Other ways to help

- Star the repository on GitHub: it helps other players find it.
- Report a bug in the [issues](https://github.com/kernel64/HarissaDC/issues), with a save state if you can: it is the fastest way to get a game fixed.
- Tell other Xbox 360 players about HarissaDC.

## Source code

The source code is in this repository, under the GNU GPL version 2 only (see `LICENSE`);
third-party components keep their own licenses (`THIRD_PARTY_NOTICES.txt`).
How to build it: [BUILDING.md](BUILDING.md).

## Credits

HarissaDC is made by Mohamed Aymen (kernel64). Hardware documentation from the
Dreamcast homebrew community; nullDC, Flycast and nulldc-360 were studied for
reference. The interface comes from Harissa64. Third-party components and their
notices are listed in `THIRD_PARTY_NOTICES.txt`.

Dreamcast is a trademark of SEGA. This project is not affiliated with SEGA. Only play
games you own.
