# Crédits

Ce projet s'appuie sur l'étude des émulateurs suivants :

- **nullDC4Wii** — BenoitAdam94 (https://github.com/BenoitAdam/nullDC4Wii), fork de nullDCe (skmp)
  et de nullDC (drk||Raziel, ZeZu). nullDCe n'étant pas sous MIT, étudié, non copié.
- **nulldc-360** — gligli, GPLv3 (https://github.com/gligli/nulldc-360). Étudié, non copié.
- **Flycast** — flyinghead et contributeurs, GPLv2 (https://github.com/flyinghead/flycast).
  Étudié, non copié.

Code repris :

- **Harissa64** — Mohamed Aymen (https://github.com/kernel64/Harissa64). Le propriétaire autorise
  la reprise sous MIT des parties écrites pour Harissa64 (pas de celles dérivées de code tiers).
- **Inter** (police de l'interface, sous forme d'atlas) — The Inter Project Authors, SIL Open Font
  License 1.1 (`third_party/inter/OFL.txt`).

- **zlib 1.3.1** (inflate, CRC-32, adler32 seulement, compilé avec `Z_SOLO`) — Jean-loup Gailly et
  Mark Adler, licence zlib (`src/third_party/zlib/LICENSE`, non modifié), copie reprise de
  Harissa64. Sert au décodage PNG (drapeaux de l'écran des langues).
- **BearSSL 0.6** (TLS des téléchargements de jaquettes) — Thomas Pornin, licence MIT
  (`src/third_party/bearssl/LICENSE.txt`, non modifié), copie reprise de Harissa64.
- **SoundTouch 2.3.3** (étirement temporel du son sous la pleine vitesse, `xb_audio.cpp`) —
  Olli Parviainen, LGPL-2.1 (`src/third_party/soundtouch/COPYING.TXT`), https://codeberg.org/soundtouch/soundtouch.
  Seuls les fichiers de l'étirement sont repris ; modification locale : `override` retiré des
  destructeurs (refusé par le compilateur VS2010 de la XDK). Même usage que Dolphin, PCSX2 et
  DuckStation, étudiés pour ce choix.
- **CHD** : le format « Compressed Hunks of Data » de MAME (Aaron Giles, BSD-3). Lecteur écrit
  pour HarissaDC (`src/disc/chd.cpp`, y compris le décodeur FLAC et l'ECC CD), sans code repris ; les
  petites images de test de Flycast (`ref/`, hors dépôt) servent seulement aux tests locaux.
- **Jaquettes** : liste Redump des disques Dreamcast de libretro-database
  (https://github.com/libretro/libretro-database) et images de libretro-thumbnails
  (https://thumbnails.libretro.com), téléchargées par la console, jamais incluses dans le dépôt.
- **Noto Sans Arabic** (glyphes arabes de l'atlas, repris de Harissa64) — The Noto Project Authors, SIL Open
  Font License 1.1 (`third_party/noto-arabic/OFL.txt`).

Code repris ou adapté de Harissa64 (même auteur, sous MIT ici) :

- `src/platform/xbox360/xb_ui.cpp`, `xb_ui_atlas.h` (UTF-8, droite à gauche, logo), `xb_png.cpp`
  (décodage PNG), `xb_flags.h` (drapeaux), `xb_lang.cpp`, `tools/gen_lang.py` (tables des textes,
  formes arabes jointes), l'écran des langues et la passe d'affichage (`xb_display.cpp`),
  `xb_net.cpp` (HTTPS) et `xb_art.cpp` (jaquettes, adapté aux disques Dreamcast).
- Le filtre de netteté suit l'idée d'AMD FidelityFX CAS (MIT) et les effets CRT le shader CRT de
  Timothy Lottes (domaine public), comme dans Harissa64.
