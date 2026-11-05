# Changes

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
