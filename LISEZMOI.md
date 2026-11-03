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
| `.7z`, `.zip` | Un jeu par archive ; extrait une fois dans `cache\` au premier lancement. |

### Jeux sur plusieurs disques

Mettez les disques dans le même dossier avec le même nom suivi de `(Disc 1)`, `(Disc 2)`...
(`[Disc 2]` et `Disc 2` marchent aussi), dans n'importe quel format :

```
games\Resident Evil Code Veronica\Resident Evil Code Veronica (Disc 1).chd
games\Resident Evil Code Veronica\Resident Evil Code Veronica (Disc 2).chd
```

La liste n'affiche qu'une entrée, qui démarre le disque 1. Quand le jeu demande le disque
suivant, appuyez sur BACK, choisissez **Changer de disque**, puis le disque : le capot
s'ouvre et se referme comme sur une Dreamcast, et le jeu continue.

## Commandes

**Liste des jeux** : A jouer, Y réglages, X à propos, LB / RB page précédente /
suivante, LT / RT lettre précédente / suivante, B ajouter ou retirer un favori (les favoris
sont en tête de liste), BACK quitter.

**En jeu** :

| Boutons | Action |
|---|---|
| BACK | Menu de l'émulateur (reprendre, redémarrer, sauver / charger un état, changer de disque, réglages, retour à la liste, quitter) |
| BACK + RB | Sauvegarde rapide dans l'emplacement courant |
| BACK + LB | Chargement rapide de l'emplacement courant |
| BACK + croix gauche / droite | Changer d'emplacement de sauvegarde rapide |
| BACK maintenu (1,5 s) | Retour à la liste des jeux |

Jusqu'à 4 manettes (ports Dreamcast A à D). Chaque bouton Dreamcast, croix comprise,
se règle par joueur dans **Réglages > Commandes** : choisissez un bouton, appuyez sur
A, puis sur la touche Xbox voulue. La même page règle, par joueur, le stick qui sert de
stick analogique, sa zone morte, sa sensibilité et l'inversion des axes. La vibration
(kit vibration Dreamcast) est active par défaut et se désactive aussi sur cette page.

**Pistolet** : Ranger Mission, Sports Shooting USA, SEGA Clay Challenge et Extreme
Hunting 2 ont un pistolet automatiquement : le stick gauche vise, RT ou A tire, B tire hors
de l'écran (recharger), START est le Start du pistolet. Pour les autres jeux de tir,
ajoutez `lightgun=1` dans `harissadc.ini`.

## Réglages

- **Graphismes** : préréglages (Original, Amélioré, Lisse, CRT) ; format d'image **4:3**,
  **16:9** (la 3D est dessinée plus large, on voit plus de la scène sur les côtés) ou
  **16:9 étiré** ; taille et position de l'image, luminosité, V-Sync (off / on / stricte),
  filtre des textures (celui du jeu, au plus proche, bilinéaire), lissage, FXAA, netteté,
  flou, lignes de balayage, effets CRT et LCD. Le 16:9 et le filtre des textures demandent
  le rendu GPU.
- **CPU / Rendu** : pris en compte au prochain lancement d'un jeu.
- **Afficher les FPS** : non, images par seconde, + charge CPU et mémoire, + résolution.
- La région de la console (Japon, USA, Europe) est choisie d'après chaque disque, rien à
  régler.
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

## Compatibilité

[COMPATIBILITE.md](COMPATIBILITE.md) liste les jeux testés et leur état.

## Problèmes connus

- Les jeux Windows CE (leur disque affiche « Powered by Windows CE » : Sega Rally 2,
  Resident Evil 2, Virtua Cop 2...) démarrent mais sont encore bien trop lents pour être
  joués.
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

## Soutenir le projet

HarissaDC est gratuit et le restera. Il est développé sur mon temps libre : chaque jeu corrigé représente des heures de tests sur une vraie console. S'il vous fait passer de bons moments avec vos jeux Dreamcast et que vous souhaitez l'aider à aller plus loin, un don me permet d'y consacrer plus de temps : plus de jeux qui fonctionnent, plus de vitesse, de nouvelles fonctions.

Chaque montant compte, et merci.

### Faire un don en cryptomonnaie

| Monnaie | Réseau | Adresse |
|---|---|---|
| Tether (USDT) | TRON (TRC20) | `TMWW2XeV9kY1U9Cv4AeW9Z6oXnuxXyBut8` |
| Bitcoin (BTC) | Bitcoin | `16GjM9aBDiTEkMyzcMq4sMWpdacc4zWkty` |
| Ether (ETH) | Ethereum | `0xcb9b2f3563249c9508eae26af396f21926ec928c` |
| Solana (SOL) | Solana | `mFxWKAEYnV5oKwYwtzmGCNfvWLNyqfL2FBWH3qSLSHU` |

Envoyez chaque monnaie uniquement sur le réseau indiqué sur sa ligne : des fonds envoyés sur un autre réseau sont perdus. Vérifiez l'adresse avant d'envoyer : un transfert ne peut pas être annulé.

### Autres façons d'aider

- Mettez une étoile au dépôt sur GitHub : cela aide d'autres joueurs à le trouver.
- Signalez un bug dans les [issues](https://github.com/kernel64/HarissaDC/issues), avec une sauvegarde d'état si possible : c'est le moyen le plus rapide de faire corriger un jeu.
- Parlez de HarissaDC aux autres joueurs Xbox 360.

## Code source

Le code source est dans ce dépôt, sous licence GNU GPL version 2 uniquement (voir
`LICENSE`) ; les composants tiers gardent leur propre licence (`THIRD_PARTY_NOTICES.txt`).
Pour le compiler : [BUILDING.md](BUILDING.md) (en anglais).

## Crédits

HarissaDC est réalisé par Mohamed Aymen (kernel64). Documentation matérielle de la
communauté homebrew Dreamcast ; nullDC, Flycast et nulldc-360 ont été étudiés comme
références. L'interface vient de Harissa64. Les composants tiers et leurs mentions sont
listés dans `THIRD_PARTY_NOTICES.txt`.

Dreamcast est une marque de SEGA. Ce projet n'est pas affilié à SEGA. Ne jouez qu'aux
jeux que vous possédez.
