#!/usr/bin/env python3
import os
import sys
from xml.sax.saxutils import escape

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
XB = os.path.dirname(HERE)                                   # src/platform/xbox360
ROOT = os.path.abspath(os.path.join(XB, "..", "..", ".."))
FONT = os.path.join(ROOT, "third_party", "inter", "Inter-SemiBold.ttf")
TITLE_ID = 0x44430360

RED, GOLD, BLUE, PURPLE, GREEN, GREY = (214, 58, 44), (224, 160, 30), (46, 120, 210), (136, 80, 200), (40, 150, 90), (90, 98, 116)

A = [
    (1, "FIRST", 10, RED, "1",
     ("First Step", "Unlock a RetroAchievements achievement in any game.", "Unlocked a first RetroAchievements achievement."),
     ("Premier pas", "Débloquez un succès RetroAchievements dans un jeu.", "Premier succès RetroAchievements débloqué.")),
    (2, "COLLECT_10", 20, RED, "10",
     ("Collector I", "Unlock 10 RetroAchievements achievements.", "Unlocked 10 RetroAchievements achievements."),
     ("Collectionneur I", "Débloquez 10 succès RetroAchievements.", "10 succès RetroAchievements débloqués.")),
    (3, "COLLECT_50", 40, RED, "50",
     ("Collector II", "Unlock 50 RetroAchievements achievements.", "Unlocked 50 RetroAchievements achievements."),
     ("Collectionneur II", "Débloquez 50 succès RetroAchievements.", "50 succès RetroAchievements débloqués.")),
    (4, "COLLECT_100", 60, RED, "100",
     ("Collector III", "Unlock 100 RetroAchievements achievements.", "Unlocked 100 RetroAchievements achievements."),
     ("Collectionneur III", "Débloquez 100 succès RetroAchievements.", "100 succès RetroAchievements débloqués.")),
    (5, "COLLECT_250", 80, RED, "250",
     ("Collector IV", "Unlock 250 RetroAchievements achievements.", "Unlocked 250 RetroAchievements achievements."),
     ("Collectionneur IV", "Débloquez 250 succès RetroAchievements.", "250 succès RetroAchievements débloqués.")),
    (6, "COLLECT_500", 100, RED, "500",
     ("Collector V", "Unlock 500 RetroAchievements achievements.", "Unlocked 500 RetroAchievements achievements."),
     ("Collectionneur V", "Débloquez 500 succès RetroAchievements.", "500 succès RetroAchievements débloqués.")),
    (7, "POINTS_100", 20, GOLD, "100",
     ("Scorer I", "Earn 100 RetroAchievements points.", "Earned 100 RetroAchievements points."),
     ("Marqueur I", "Gagnez 100 points RetroAchievements.", "100 points RetroAchievements gagnés.")),
    (8, "POINTS_1000", 50, GOLD, "1K",
     ("Scorer II", "Earn 1,000 RetroAchievements points.", "Earned 1,000 RetroAchievements points."),
     ("Marqueur II", "Gagnez 1 000 points RetroAchievements.", "1 000 points RetroAchievements gagnés.")),
    (9, "POINTS_5000", 100, GOLD, "5K",
     ("Scorer III", "Earn 5,000 RetroAchievements points.", "Earned 5,000 RetroAchievements points."),
     ("Marqueur III", "Gagnez 5 000 points RetroAchievements.", "5 000 points RetroAchievements gagnés.")),
    (10, "GAMES_3", 20, BLUE, "3",
     ("Explorer I", "Unlock achievements in 3 different games.", "Unlocked achievements in 3 different games."),
     ("Explorateur I", "Débloquez des succès dans 3 jeux différents.", "Succès débloqués dans 3 jeux différents.")),
    (11, "GAMES_10", 50, BLUE, "10",
     ("Explorer II", "Unlock achievements in 10 different games.", "Unlocked achievements in 10 different games."),
     ("Explorateur II", "Débloquez des succès dans 10 jeux différents.", "Succès débloqués dans 10 jeux différents.")),
    (12, "GAMES_25", 80, BLUE, "25",
     ("Explorer III", "Unlock achievements in 25 different games.", "Unlocked achievements in 25 different games."),
     ("Explorateur III", "Débloquez des succès dans 25 jeux différents.", "Succès débloqués dans 25 jeux différents.")),
    (13, "MASTER_1", 80, PURPLE, "100%",
     ("Perfectionist I", "Unlock every achievement of a game.", "Unlocked every achievement of a game."),
     ("Perfectionniste I", "Débloquez tous les succès d'un jeu.", "Tous les succès d'un jeu débloqués.")),
    (14, "MASTER_3", 100, PURPLE, "x3",
     ("Perfectionist II", "Unlock every achievement of 3 games.", "Unlocked every achievement of 3 games."),
     ("Perfectionniste II", "Débloquez tous les succès de 3 jeux.", "Tous les succès de 3 jeux débloqués.")),
    (15, "SHENMUE", 30, GREEN, "SHEN",
     ("Shenmue", "Unlock 10 achievements in Shenmue.", "Unlocked 10 achievements in Shenmue."),
     ("Shenmue", "Débloquez 10 succès dans Shenmue.", "10 succès débloqués dans Shenmue.")),
    (16, "SONIC", 30, GREEN, "SA",
     ("Sonic Adventure", "Unlock 10 achievements in Sonic Adventure.", "Unlocked 10 achievements in Sonic Adventure."),
     ("Sonic Adventure", "Débloquez 10 succès dans Sonic Adventure.", "10 succès débloqués dans Sonic Adventure.")),
    (17, "SOULCALIBUR", 30, GREEN, "SC",
     ("Soulcalibur", "Unlock 10 achievements in Soulcalibur.", "Unlocked 10 achievements in Soulcalibur."),
     ("Soulcalibur", "Débloquez 10 succès dans Soulcalibur.", "10 succès débloqués dans Soulcalibur.")),
    (18, "CRAZY_TAXI", 30, GREEN, "CT",
     ("Crazy Taxi", "Unlock 10 achievements in Crazy Taxi.", "Unlocked 10 achievements in Crazy Taxi."),
     ("Crazy Taxi", "Débloquez 10 succès dans Crazy Taxi.", "10 succès débloqués dans Crazy Taxi.")),
    (19, "JET_SET_RADIO", 30, GREEN, "JSR",
     ("Jet Set Radio", "Unlock 10 achievements in Jet Set Radio (Jet Grind Radio).", "Unlocked 10 achievements in Jet Set Radio."),
     ("Jet Set Radio", "Débloquez 10 succès dans Jet Set Radio (Jet Grind Radio).", "10 succès débloqués dans Jet Set Radio.")),
    (20, "SAVE_STATE", 10, GREY, "SAVE",
     ("Quick Save", "Save a state in a game.", "Saved a state in a game."),
     ("Sauvegarde éclair", "Sauvegardez un état dans un jeu.", "Un état sauvegardé dans un jeu.")),
    (21, "CHEAT", 10, GREY, "CODE",
     ("Secret Code", "Turn on a cheat in a game.", "Turned on a cheat in a game."),
     ("Code secret", "Activez un code de triche dans un jeu.", "Un code de triche activé dans un jeu.")),
    (22, "FAVOURITE", 10, GREY, "FAV",
     ("My Favourites", "Add a game to your favourites.", "Added a game to your favourites."),
     ("Mes préférés", "Ajoutez un jeu à vos favoris.", "Un jeu ajouté à vos favoris.")),
    (23, "LANGUAGE", 10, GREY, "Aa",
     ("Polyglot", "Change the language of the menus.", "Changed the language of the menus."),
     ("Polyglotte", "Changez la langue des menus.", "Langue des menus changée.")),
]
TOTAL = 1000


def icon(colour, text, path):
    """A 64x64 rounded tile in the category's colour with a short white text."""
    S = 4
    im = Image.new("RGBA", (64 * S, 64 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    top = tuple(min(255, int(c * 1.12)) for c in colour)
    bot = tuple(int(c * 0.62) for c in colour)
    grad = Image.new("RGBA", (1, 64 * S))
    for y in range(64 * S):
        k = y / (64 * S - 1)
        grad.putpixel((0, y), tuple(int(top[i] + (bot[i] - top[i]) * k) for i in range(3)) + (255,))
    grad = grad.resize((64 * S, 64 * S))
    mask = Image.new("L", (64 * S, 64 * S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 64 * S - 1, 64 * S - 1), radius=12 * S, fill=255)
    im.paste(grad, (0, 0), mask)
    d.rounded_rectangle((2 * S, 2 * S, 62 * S - 1, 62 * S - 1), radius=10 * S, outline=(255, 255, 255, 70), width=S)
    size = 34 * S
    while True:
        f = ImageFont.truetype(FONT, size)
        box = d.textbbox((0, 0), text, font=f)
        if box[2] - box[0] <= 50 * S or size <= 8 * S:
            break
        size -= S
    w, h = box[2] - box[0], box[3] - box[1]
    x, y = (64 * S - w) / 2 - box[0], (64 * S - h) / 2 - box[1]
    d.text((x + S, y + 2 * S), text, font=f, fill=(0, 0, 0, 110))
    d.text((x, y), text, font=f, fill=(255, 255, 255, 255))
    im.resize((64, 64), Image.LANCZOS).save(path)


BG, BG2, ACCENT = (18, 19, 26), (28, 30, 40), (234, 76, 58)
BODY = [(176, 78), (158, 150), (120, 196), (34, 222)]
PEPPER = [((255, 98, 72, 255), (190, 30, 26, 255)),       # body
          ((255, 236, 228, 110), (255, 236, 228, 70)),    # highlight
          ((120, 206, 92, 255), (46, 142, 64, 255))]      # stem and calyx


def bezier(p, t):
    p = [np.array(q, dtype=np.float64) for q in p]
    if len(p) == 3:
        return (1 - t) ** 2 * p[0] + 2 * (1 - t) * t * p[1] + t ** 2 * p[2]
    return (1 - t) ** 3 * p[0] + 3 * (1 - t) ** 2 * t * p[1] + 3 * (1 - t) * t ** 2 * p[2] + t ** 3 * p[3]


def sweep(draw, k, pts, radius, steps=400, t0=0.0, t1=1.0):
    for i in range(steps + 1):
        t = t0 + (t1 - t0) * i / steps
        c = bezier(pts, t)
        r = radius(t)
        if r > 0:
            draw.ellipse([(c[0] - r) * k, (c[1] - r) * k, (c[0] + r) * k, (c[1] + r) * k], fill=255)


def pepper_masks(size):
    n = size * 4
    k = n / 256.0
    shapes = []
    img = Image.new("L", (n, n), 0)
    sweep(ImageDraw.Draw(img), k, BODY, lambda t: 36.0 * (1.0 - t) ** 0.8 * min(1.0, 0.75 + t * 2.5) + 3.0 * (1 - t) + 2.0)
    shapes.append(img)
    img = Image.new("L", (n, n), 0)
    sweep(ImageDraw.Draw(img), k, [(158, 96), (146, 140), (118, 176), (70, 198)], lambda t: 6.0 * np.sin(np.pi * t) ** 0.7, t0=0.02, t1=0.98)
    shapes.append(img)
    img = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(img)
    sweep(d, k, [(144, 86), (174, 54), (210, 84)], lambda t: 7.0 + 6.0 * abs(np.sin(np.pi * 1.5 * t + 0.4)) * np.sin(np.pi * t) ** 0.4)
    sweep(d, k, [(158, 74), (176, 64), (194, 74)], lambda t: 9.0)
    sweep(d, k, [(176, 64), (182, 34), (214, 18)], lambda t: 7.5 - 2.5 * t)
    shapes.append(img)
    return [np.asarray(s.resize((size, size), Image.LANCZOS), dtype=np.float64) / 255.0 for s in shapes]


def comp(dst, rgb, alpha, x, y):
    h, w = alpha.shape
    H, W = dst.shape[:2]
    x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + w, W), min(y + h, H)
    if x0 >= x1 or y0 >= y1:
        return
    a = alpha[y0 - y:y1 - y, x0 - x:x1 - x][..., None]
    c = rgb[y0 - y:y1 - y, x0 - x:x1 - x]
    dst[y0:y1, x0:x1] = dst[y0:y1, x0:x1] * (1 - a) + c * a


def title_icon(path):
    S = 512
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float64)
    t = yy / (S - 1)
    img = np.array(BG2)[None, None, :] * (1 - t[..., None]) + np.array(BG)[None, None, :] * t[..., None]
    dd = np.hypot(xx - S / 2, yy - S * 0.46) / (S * 0.55)
    g = np.exp(-dd * dd * 1.6)[..., None] * 0.34
    img = img * (1 - g) + np.array(ACCENT)[None, None, :] * g
    m = sum(pepper_masks(256)) > 0.05
    ys, xs = np.nonzero(m)
    x0, y0, x1, y1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
    size = int(round(S * 0.86 * 256.0 / max(x1 - x0, y1 - y0)))
    k = size / 256.0
    px, py = int(round(S / 2 - (x0 + x1) / 2 * k)), int(round(S / 2 - (y0 + y1) / 2 * k))
    masks = pepper_masks(size)
    sh = Image.fromarray((masks[0] * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(size * 0.035))
    comp(img, np.zeros((size, size, 3)), np.asarray(sh, dtype=np.float64) / 255.0 * 0.55, px, py + int(size * 0.045))
    tt = np.linspace(0.0, 1.0, size)[:, None, None]
    for msk, (top, bot) in zip(masks, PEPPER):
        col = np.array(top, dtype=np.float64)[None, None, :] * (1 - tt) + np.array(bot, dtype=np.float64)[None, None, :] * tt
        comp(img, np.broadcast_to(col[..., :3], (size, size, 3)), msk * (col[..., 3] / 255.0), px, py)
    Image.fromarray(np.clip(np.round(img), 0, 255).astype(np.uint8), "RGB").resize((64, 64), Image.LANCZOS).save(path)


def gameconfig():
    L = []
    w = L.append
    w('<?xml version="1.0" encoding="utf-8" standalone="no"?>')
    w('<!-- HarissaDC title data, compiled by the XDK\'s spac into spa.bin and embedded in the XEX')
    w('     as the section named after the title ID (%08X, see harissadc.vcxproj AdditionalSections).' % TITLE_ID)
    w('     Dashboards (Aurora, the stock dashboard) read the title name (X_STRINGID_TITLENAME), the')
    w('     title image (X_IMAGEID_GAME, 64x64 PNG) and the achievements from it.')
    w('     Written by tools/gen_achievements.py: edit the table there, not this file. -->')
    w('<XboxLiveSubmissionProject Version="2.0.60117.01" xmlns="http://www.xboxlive.com/xlast">')
    w('  <GameConfigProject name="HarissaDC" titleName="HarissaDC" projectVersion="1.00.0000.0" schemaVersion="1.08.0000.0" '
      'classification="BaseGame" titleId="0x%08X" multiplayer="false" titleType="1" arn="0x0">' % TITLE_ID)
    w('    <ProductInformation>')
    w('      <Platform name="Xbox 360" id="2" />')
    w('    </ProductInformation>')
    w('    <Privileges />')
    w('    <LocalizedStrings defaultLocale="en-US" nextId="400">')
    w('      <SupportedLocale locale="en-US" />')
    w('      <SupportedLocale locale="fr-FR" />')

    def s(sid, name, en, fr=None, presence=False):
        w('      <LocalizedString id="%d" friendlyName="%s">' % (sid, name))
        w('        <Translation locale="en-US">%s</Translation>' % escape(en))
        w('        <Translation locale="fr-FR">%s</Translation>' % escape(fr if fr is not None else en))
        if presence:
            w('        <Presence />')
        w('      </LocalizedString>')

    s(32768, "X_STRINGID_TITLENAME", "HarissaDC")
    s(1, "STR_PLAYING", "Playing", "En jeu", presence=True)
    s(32512, "X_STRINGID_GAMETYPE", "Game Type", "Type de partie")
    s(32513, "X_STRINGID_RANKED", "Ranked Match", "Partie classée")
    s(32514, "X_STRINGID_STANDARD", "Player Match", "Partie libre")
    for aid, key, cred, col, txt, en, fr in A:
        s(100 + aid, "ACH_%s_NAME" % key, en[0], fr[0])
        s(200 + aid, "ACH_%s_DESC" % key, en[2], fr[2])
        s(300 + aid, "ACH_%s_HOWTO" % key, en[1], fr[1])
    w('    </LocalizedStrings>')
    w('    <Presence>')
    w('      <PresenceMode contextValue="0" stringId="1" friendlyName="STR_PLAYING" />')
    w('    </Presence>')
    w('    <Properties nextId="1" />')
    w('    <Contexts nextId="0x00000000">')
    w('      <Context stringId="32512" friendlyName="GAME_TYPE" id="0x0000800A" defaultValue="0">')
    w('        <ContextValue stringId="32513" value="0" friendlyName="RANKED" />')
    w('        <ContextValue stringId="32514" value="1" friendlyName="STANDARD" />')
    w('      </Context>')
    w('    </Contexts>')
    w('    <Achievements nextId="%d">' % (max(a[0] for a in A) + 1))
    for aid, key, cred, col, txt, en, fr in A:
        w('      <Achievement id="%d" friendlyName="%s" titleStringId="%d" descriptionStringId="%d" unachievedStringId="%d" '
          'showUnachieved="true" achievementType="Other" cred="%d" imageId="%d" />' % (aid, key, 100 + aid, 200 + aid, 300 + aid, cred, aid))
    w('    </Achievements>')
    w('    <Images>')
    for aid, key, cred, col, txt, en, fr in A:
        w('      <Image id="%d" friendlyName="IMG_%s">' % (aid, key))
        w('        <Path>art\\ach\\%02d.png</Path>' % aid)
        w('      </Image>')
    w('      <Image id="32768" friendlyName="X_IMAGEID_GAME">')
    w('        <Path>art\\icon.png</Path>')
    w('      </Image>')
    w('    </Images>')
    w('    <GameModes defaultValue="0" />')
    w('    <StatsViewMasters />')
    w('    <StatsViews nextViewId="1" />')
    w('    <GamerPictures />')
    w('  </GameConfigProject>')
    w('</XboxLiveSubmissionProject>')
    return "\r\n".join(L) + "\r\n"


def header():
    L = ["// Generated by tools/gen_achievements.py - do not edit.",
         "// The ids of HarissaDC's Xbox 360 achievements (title/harissadc.gameconfig).",
         "#ifndef XB_ACH_IDS_H", "#define XB_ACH_IDS_H", ""]
    for aid, key, cred, col, txt, en, fr in A:
        L.append("#define XB_ACH_%-14s %2d   // %s (%d G)" % (key, aid, en[0], cred))
    L += ["#define XB_ACH_COUNT %d" % len(A), "", "#endif", ""]
    return "\r\n".join(L)


def main():
    check = "--check" in sys.argv
    total = sum(a[2] for a in A)
    if total != TOTAL:
        sys.exit("ERROR: the gamerscore adds up to %d, not %d" % (total, TOTAL))
    if sorted(a[0] for a in A) != list(range(1, len(A) + 1)):
        sys.exit("ERROR: ids must be 1..N without holes")
    out = {os.path.join(XB, "title", "harissadc.gameconfig"): gameconfig().encode("utf-8"),
           os.path.join(XB, "xb_ach_ids.h"): header().encode("utf-8")}
    for path, data in out.items():
        old = open(path, "rb").read() if os.path.isfile(path) else None
        if old != data:
            if check:
                sys.exit("ERROR: %s is not up to date: run tools/gen_achievements.py" % path)
            open(path, "wb").write(data)
    if not check:
        adir = os.path.join(XB, "title", "art", "ach")
        os.makedirs(adir, exist_ok=True)
        for aid, key, cred, col, txt, en, fr in A:
            icon(col, txt, os.path.join(adir, "%02d.png" % aid))
        title_icon(os.path.join(XB, "title", "art", "icon.png"))
    print("achievements OK: %d achievements, %d G" % (len(A), total))


if __name__ == "__main__":
    main()
