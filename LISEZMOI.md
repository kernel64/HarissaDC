# HarissaDC

Un émulateur Dreamcast pour Xbox 360 (consoles RGH / JTAG).

HarissaDC est **en développement** ; ses versions sont partagées pour recueillir les retours de la
communauté. Beaucoup de jeux fonctionnent, certains ont des défauts graphiques ou des
scènes lentes, et quelques-uns ne démarrent pas encore. Vos retours décideront de ce
qui sera corrigé en premier.

## Téléchargement

La dernière version est sur la page [Releases](https://github.com/kernel64/HarissaDC/releases) : le zip contient
le dossier `HarissaDC` à copier sur la console et cette documentation.

English version: [README.md](README.md).

## Ce qu'il faut

- Une Xbox 360 qui lance du code non signé (RGH ou JTAG) avec un lanceur homebrew
  (Aurora, FreeStyle Dash, XeXMenu...).
- Vos propres jeux Dreamcast, copiés depuis vos disques.
- Pas besoin de BIOS Dreamcast : HarissaDC a le sien.

## Installation

1. Décompressez le zip. Vous obtenez un dossier `HarissaDC` avec `default.xex` et un
   dossier `games` vide.
2. Copiez tout le dossier `HarissaDC` sur la console, par exemple dans
   `Hdd:\Emulators\HarissaDC` (par FTP ou avec une clé USB).
3. Mettez vos jeux dans `HarissaDC\games` (les sous-dossiers sont acceptés, un par jeu
   c'est plus propre).
4. Lancez `default.xex` depuis votre lanceur (dans Aurora : ajoutez le dossier aux
   chemins Homebrew, ou ouvrez-le avec le gestionnaire de fichiers).

Au premier lancement, choisissez votre langue. La liste affiche ensuite vos jeux.

### Formats de jeux

| Format | Remarques |
|---|---|
| `.gdi` + fichiers des pistes | Gardez les pistes à côté du `.gdi`. |
| `.cue` + `.bin` | Copies Redump (GD-ROM avec `REM HIGH-DENSITY AREA`). |
| `.chd` | CHD version 5 (`chdman createcd`). Anciens CHD : convertir avec `chdman copy`. |
| `.iso` | Une seule piste de données. |
| `.7z` | Un jeu par archive ; extrait une fois dans `cache\` au premier lancement. |

Jeux à plusieurs disques : nommez les fichiers `Jeu (Disc 1).gdi`, `Jeu (Disc 2).gdi`...
ils apparaissent comme un seul jeu. On change de disque depuis le menu en jeu.

## Commandes

**Liste des jeux** : A jouer, Y réglages, X à propos, LB / RB page précédente /
suivante, BACK quitter.

**En jeu** :

| Boutons | Action |
|---|---|
| BACK | Menu de l'émulateur (reprendre, redémarrer, sauver / charger un état, changer de disque, réglages, retour à la liste, quitter) |
| BACK + RB | Sauvegarde rapide dans l'emplacement courant |
| BACK + LB | Chargement rapide de l'emplacement courant |
| BACK + croix gauche / droite | Changer d'emplacement de sauvegarde rapide |

Jusqu'à 4 manettes (ports Dreamcast A à D). Chaque bouton Dreamcast, croix comprise,
se règle par joueur dans **Réglages > Commandes** : choisissez un bouton, appuyez sur
A, puis sur la touche Xbox voulue. La vibration (kit vibration Dreamcast) est active
par défaut et se désactive sur la même page.

## Réglages

- **Graphismes** : préréglages (Original, Amélioré, Lisse, CRT), 4:3 ou étiré, taille
  et position de l'image, luminosité, lissage, FXAA, netteté, flou, lignes de balayage,
  effets CRT et LCD.
- **CPU / Région / Rendu** : pris en compte au prochain lancement d'un jeu.
- Les réglages se gardent avec **Enregistrer pour tous les jeux** (ou **Enregistrer
  pour ce jeu seulement** depuis le menu en jeu).

## Jaquettes

Quand la console a une connexion réseau, la liste télécharge les jaquettes (depuis
les vignettes libretro) et les garde dans le dossier `art`. **Réglages > Télécharger
les jaquettes** les désactive ; **Jaquettes de tous les jeux** les récupère toutes
d'un coup. L'émulateur vérifie aussi sur GitHub si une nouvelle version existe.

## Fichiers créés par HarissaDC

| Dossier / fichier | Contenu |
|---|---|
| `config\` | Réglages (tous les jeux, et par jeu) |
| `saves\` | Sauvegardes d'état et carte mémoire virtuelle (VMU) |
| `art\` | Jaquettes |
| `cache\` | Jeux extraits des archives `.7z` |
| `harissadc.log` | Journal de la dernière session (à joindre aux rapports de bug) |

## Problèmes connus

- Certains jeux ont des défauts graphiques (couleurs fausses, éléments dessinés
  devant ou derrière d'autres), par exemple dans certains menus de Daytona USA 2001.
- Certaines cinématiques et vidéos sont lentes (par exemple dans Jet Grind Radio)
  alors que le jeu lui-même est fluide.
- Le chargement depuis un `.chd` est plus lent que depuis un `.gdi` / `.cue`.
- La force de la vibration est une approximation du kit Dreamcast.

## Signaler un problème

Ouvrez une issue sur <https://github.com/kernel64/HarissaDC/issues> avec :

- le jeu, sa région et son format (`.gdi`, `.cue`, `.chd`...) ;
- ce qui se passe et à quel endroit (une photo ou une vidéo aide beaucoup) ;
- le fichier `harissadc.log` du dossier de l'émulateur, pris juste après le problème.

Une sauvegarde d'état (BACK + RB) faite juste avant le problème est aussi la bienvenue.

## Crédits

HarissaDC est réalisé par Mohamed Aymen (kernel64). Documentation matérielle de la
communauté homebrew Dreamcast ; nullDC, Flycast et nulldc-360 ont été étudiés comme
références. L'interface vient de Harissa64. Les composants tiers et leurs mentions sont
listés dans `THIRD_PARTY_NOTICES.txt`.

Dreamcast est une marque de SEGA. Ce projet n'est pas affilié à SEGA. Ne jouez qu'aux
jeux que vous possédez.
