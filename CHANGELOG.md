# Changes

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
