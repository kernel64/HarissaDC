# Changes

## 0.4.0 (2026-11-26)

**New**

- **True 16:9** (**Settings > Picture > Aspect ratio**):
  - **16:9**: for about 150 games (and the Atomiswave conversions of Knights of Valour and Faster than Speed), HarissaDC changes the game's own camera, so it shows a wider view at the TV's full resolution (1280x720). The game's 2D overlay (score, timer, menus) is stretched to 16:9. Other games are shown 4:3, with the sides filled by a blurred copy of the picture instead of black bars (**Sides**: Blurred or Black).
  - **16:9 wider view**: for any 3D game, shows what the game draws beyond its 4:3 edges. When a game leaves its sides empty, the picture goes back to 4:3 by itself.
  - **16:9 anamorphic**: for games that have their own 16:9 option (Wacky Races, Toy Commander...).
  - The choice can be made for one game only, like every setting.
- **Automatic updates**: when HarissaDC starts and a newer release is out, it offers to download and install it (A: Install, B: Not now). Only the emulator and the cheat database are replaced: your save states, memory cards, settings, games and box art are kept. This works from this version on, for the releases that follow.
- **Rename games** in the game list (RS), for example discs whose title shows up garbled.
- **Games as their own dashboard entries**: a shortcut folder with `datadir=` and `exitto=dashboard` in its `harissadc.ini` uses the main install's saves, settings and BIOS, and quits back to the dashboard (#39).
- **CD audio in real time** (**Settings > Advanced > CD audio**), for games whose CD music must keep its own pace.
- **Fewer frames when slow** (**Settings > Advanced**): when a game falls behind, it keeps its real speed and shows fewer pictures, instead of running in slow motion. It is on by default for the games that need it (all Windows CE games, Wacky Races, Draconus).

**Faster**

- **Windows CE games** now run at their real speed: **Urban Chaos** about 59 fps (30 before), **Tomb Raider: The Last Revelation** 59.5 (40), **Spirit of Speed 1937** about 56 (32), **Sega Rally 2**'s races and **Resident Evil 2** at 60 most of the time. They show fewer distinct pictures in their heaviest scenes.
- **Wacky Races**: its hub and races at 60 fps (41 to 50 before).
- **Alone in the Dark: The New Nightmare**: about 54 fps in play (45 before).

**Fixed**

- **Dragon Riders**: the notes were drawn on white boxes (#40).
- **Sonic Adventure 2**: sound effects that kept repeating, and dark lines under Sonic's feet when he falls.
- **Alone in the Dark**: sounds that looped.
- **Toy Commander** and **Re-Volt**: no music when started with Sega's BIOS (#45).
- **Sound**: after a slowdown, the sound no longer speeds up for a long time or cuts in small slices (#43).
- **Headhunter**: noise in the city sky (#49).
- **Spirit of Speed 1937**: the title, loading and pause screens (#47).
- **DX Game of Life**: its menus were drawn in a quarter of the screen.
- **Box art**: the main thumbnail server lost every Dreamcast picture; HarissaDC now uses its GitHub copy (#44), and the Atomiswave conversions and more games (`.7z` and `.zip` ones before their first start) now get their cover.

**Known issues**

- **Army Men: Sarge's Heroes**: its videos may freeze (#42).
- Unreal Tournament and Dragon Riders stay below 60 fps in their heaviest scenes.

## 0.3.0 (2026-11-20)

**Warning about achievements**: HarissaDC gives its own Xbox 360 achievements, written to
your Xbox profile. Achievements from a homebrew title on a profile that is used online can
get the account banned. If your profile goes online, do not use it with HarissaDC: use an
offline profile.

**New**

- Major rendering update, for every game: both renderers (the GPU renderer and the software
  one) were checked against each other on hundreds of test scenes, and the differences found
  were fixed: depth tests, polygon edges, clipping, shadows (modifier volumes), transparent
  and cut-out polygons, fog (the background's too), shading, bump mapping, render to texture
  (pictures that games draw into a texture), split screens and sprites. Many glitches in
  many games are gone.
- Achievements: RetroAchievements (off by default) in **Settings > Achievements**: sign in
  with the Xbox keyboard, achievements unlock as you play (softcore), with their badge and a
  notification; the in-game menu lists the game's achievements, unlocked ones first.
  HarissaDC also has its own Xbox 360 achievements (23, 1000 G), earned with your
  RetroAchievements progress (see the warning above). Achievements pause while a cheat is on.
- Cheats: the Dreamcast cheat database (libretro) is included in the `cheats` folder; turn
  cheats on and off from the in-game menu. Your own `.cht` or `.txt` files go in the same
  folder.
- Settings sorted into pages: Picture, Effects, Controls, Interface, Achievements and
  Advanced, from the game list and from the in-game menu.
- CDI disc images (DiscJuggler).
- Each game has its own memory card (`saves\vmu\`). **Settings > Advanced > Memory card**
  keeps one shared card instead.
- Controls: the sticks' directions can be given to any Dreamcast button or to the D-pad, and
  **Stick as D-pad** plays games made for the D-pad (Dino Crisis) with a stick.
- **Internal resolution** (Picture page): 1.5x (default) or 1x.
- The FPS display can show a panel with FPS, speed, CPU, RAM and resolution.
- Sega's BIOS (optional): copy your own Dreamcast BIOS (`dc_boot.bin` and `dc_flash.bin`)
  to the `HarissaDC\bios` folder and choose **Sega's** or **Sega's when present** in
  **Settings > Advanced > BIOS**: games then start with the Dreamcast's boot animation, as on
  the real console. Discs of every region start with it, and save states load whatever BIOS
  made them. HarissaDC's built-in BIOS stays the default: no BIOS file is needed.
- **Advanced**: audio margin.
- Consoles with a debugger attached (development tools) get a warning screen instead of a
  game frozen at its start.

**Faster**

- CPU emulation: Dragon Riders runs about 14% faster in its heavy scenes (about 44 fps,
  38 before), Unreal Tournament's split screen about 12% faster (about 35 fps, 32 before);
  other games gain too.
- Draconus: its forest now runs at full speed (about 26 fps before; the game shows fewer
  pictures, at its own pace).
- Ikaruga's logo runs at full speed; Grandia II's opening and first forest are faster.

**Fixed**

- Rendering in every game: see the rendering update above.
- Dragon Riders: after the first dialogue, the hero walked on the spot against the bed.
- Draconus: a dark blue sky and background instead of the fog.
- Wacky Races: black triangles over the menu panels.
- Unreal Tournament: the split screen showed only one player.
- Vigilante 8 2nd Offense: black panels in the menus.
- Yu Suzuki Game Works: black picture in its arcade games.
- Berserk: the ground and rocks behind the soldiers were wrongly shadowed.
- Rayman 2: dark wedges on the ground.
- Evil Twin no longer stops at its start.
- Loading a save state in a second game of the same session could freeze it.
- Some settings (vibration, V-Sync, texture filter, language...) could be lost when saved.

**Known issues**

- Windows CE games (Sega Rally 2, Worms World Party, Resident Evil 2...) are still slow in
  places.
- Unreal Tournament and Dragon Riders are below 60 fps in their heavy scenes.

## 0.2.2 (2026-11-06)

**Faster**

- Windows CE games: Sega Rally 2's races run at about 45-50 fps (about 36 before).
- Extreme Hunting 2 and Dirty Pigskin Football now run at full speed (about 40 and 31 fps
  before).
- Samurai Spirits Tenkaichi Kenkakuden: fights run at about 30-45 fps (about 20 before).
- Games that draw many polygons spend less time preparing each picture.

**Fixed**

- Samurai Spirits Tenkaichi Kenkakuden: the referee and parts of the background were drawn
  over the fighters.
- Berserk: in the intro, the horses fell over for a moment and the characters' limbs shook.
- Shenmue: the music suddenly became much louder a few seconds into a scene; a patch of sky
  flickered on the ground by the stones outside the house.
- Sound in every game: notes now fade in and out with the timing of the Dreamcast's sound
  chip (some ambient sounds rose late, then fell again).

**Known issues**

- Windows CE games (Sega Rally 2, Worms World Party, Resident Evil 2...) are still slow in
  places.
- Wacky Races runs its races at about 28 fps.

## 0.2.1 (2026-11-05)

**Fixed**

- Soulcalibur was slow in its fights: the game drew 20 pictures a second instead of 60
  (its frame timing was disturbed). It now runs at full speed.
- Sound lag: the sound was heard about half a second after the picture in every game. It
  now follows the picture closely (about 0.1 s), and stays in step after a slowdown.
- Capcom vs. SNK turned its picture on its side: the automatic orientation now only applies
  to the arcade conversions made for an upright screen. A new Orientation setting in
  Graphics (Auto, Normal, 90°, 270°) lets you choose it yourself.
- Extreme Hunting 2: the shot now lands under the sight (it landed far above it). The game
  now runs in 480p, as on the arcade machine it comes from. Start it without loading a save
  state made with an older version.
- Archives with long folder names: a cache extracted with the old version is extracted
  again automatically at the next start.

**Known issues**

- Windows CE games (Sega Rally 2, Worms World Party, Resident Evil 2...) are still slow in
  places: they run below full speed in their heaviest scenes.

## 0.2.0 (2026-11-04)

The source code is now published in this repository (GPL-2.0-only).

**New**

- Windows CE games: Sega Rally 2 races run at about 30-45 fps (it was a few frames a
  second), Resident Evil 2 plays its intro movies and reaches its title screen.
- Atomiswave conversions (`.zip` / `.7z` of a GDI) start, Metal Slug 6, The King of Fighters
  XI and others among them; `.zip` and `.7z` archives are extracted at the first start.
- Light gun: Ranger Mission, Sports Shooting USA, SEGA Clay Challenge and Extreme Hunting 2
  get their gun by themselves (left stick to aim, RT or A to fire). Other shooting games:
  `lightgun=1`.
- When the emulation runs below full speed the sound is stretched instead of cut.
- Games drawn for an upright screen are turned automatically (Animal Basket).
- Game list: LT / RT jump to the previous / next letter, B marks a favourite (listed
  first), the box art has a glow of its own colours.
- Box art: more covers found (other regions tried), a cover that could not be downloaded is
  tried again later, "Get box art for all games" stops at once without a network.
- Movies (Resident Evil 3, GTA 2) play at full speed.

- Music from the disc's audio tracks (CD audio): Quake III Arena, Unreal Tournament and
  the other games that play their music from the disc now have it.
- 16:9: the 3D is drawn wider (more of the scene on the sides). The aspect ratio is one
  setting: 4:3, 16:9 or 16:9 stretched.
- Settings: V-Sync (off / on / strict), texture filter (as the game, nearest, bilinear),
  FPS display with CPU load, memory and resolution; per player: stick dead zone,
  sensitivity and inverted axes.
- Hold BACK for 1.5 s to go back to the game list.
- The console region is chosen from each disc (the Region setting is gone).

**Fixed**

- Stereo was reversed (left and right swapped) in every game.
- House of the Dead 2 ran at 14 fps while loading and on its title screen: it now plays
  at full speed (games that keep asking the drive for its state no longer slow down).
- Marvel vs. Capcom showed a black screen: its screens are drawn now.
- Marvel vs. Capcom 2: characters sometimes showed cut and mixed pieces.
- Quake III Arena's menus ran at about 30 fps, Code: Veronica's title at 45: both
  run at full speed (games that wait by reading a timer no longer slow down).
- Code: Veronica's movies ran at 15 fps, now about 30.
- Games start faster (the waiting loop of the disc's boot program ran slowly).
- Save states made with earlier versions (0.1.0-dev) still load.
- The console froze in the second game started from the list (Crazy Taxi 2, Daytona USA)
  once the sound began to stretch.
- Wacky Races stayed on a black screen after its intro.
- Dirty Pigskin Football: the text of its play windows was not visible.
- GTA 2 stopped while loading its levels; it now runs at full speed.
- Resident Evil Code: Veronica no longer hangs on its memory card screen.

**Changed**

- The Graphics page is in a clearer order (picture, timing, look); presets set the
  texture filter too; options that need the GPU renderer are hidden with the software
  one.

## 0.1.0-dev (2026-10-20)

First build shared with the community.
