#!/usr/bin/env python3
"""Fetch authentic League of Legends 2D assets for NeonMOBA.

Sources:
* Data Dragon (ddragon.leagueoflegends.com) — official static CDN:
  champion square icons, passive/ability icons, localized ability data.
* Community Dragon (raw.communitydragon.org) — raw in-game UI art:
  Summoner's Rift minimap, floating health-bar atlas, scoreboard atlas.

Outputs into ``projects/moba/assets``:
* ``lol/icons/champions/<Name>.png``      champion portraits
* ``lol/icons/spells/<file>.png``         ability + passive icons
* ``lol/icons/summoner/<file>.png``       summoner spell icons
* ``lol/ui/minimap.png``                  SR minimap
* ``lol/ui/healthbaricons.png``           floating health-bar atlas
* ``data/ddragon/<Name>.json``            raw champion ability data (zh_CN)
* ``data/abilities.json``                 compact per-champion ability table

Usage::

    python tools/fetch_lol_assets.py
"""

import json
import os
import ssl
import sys
import urllib.request

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", "projects", "moba", "assets"))
DD = "https://ddragon.leagueoflegends.com"
CD = "https://raw.communitydragon.org"
LOCALE = "zh_CN"

ROSTER = ["Garen", "Ashe", "Annie", "MasterYi", "Teemo", "Lux", "Darius",
          "Yasuo", "Zed", "Jinx", "Leona", "Riven", "Ahri", "Vayne"]

SUMMONERS = ["SummonerFlash", "SummonerDot", "SummonerHeal", "SummonerTeleport",
             "SummonerExhaust", "SummonerHaste", "SummonerSmite", "SummonerBarrier"]

_CTX = ssl.create_default_context()


def fetch(url, dest=None):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "neon-moba/1.0"})
        with urllib.request.urlopen(req, timeout=60, context=_CTX) as r:
            data = r.read()
    except Exception as exc:  # noqa: BLE001
        print("  ! %s -> %s" % (url, exc))
        return None
    if dest:
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as fh:
            fh.write(data)
    return data


def fetch_json(url):
    data = fetch(url)
    if data is None:
        return None
    try:
        return json.loads(data.decode("utf-8"))
    except Exception as exc:  # noqa: BLE001
        print("  ! json parse %s -> %s" % (url, exc))
        return None


def latest_version():
    v = fetch_json(DD + "/api/versions.json")
    return v[0] if v else "16.18.1"


def rel(path):
    return "assets/" + os.path.relpath(path, ROOT).replace("\\", "/")


def main():
    ver = latest_version()
    print("Data Dragon version:", ver)
    icons_dir = os.path.join(ROOT, "lol", "icons")
    data_dir = os.path.join(ROOT, "data")
    os.makedirs(os.path.join(data_dir, "ddragon"), exist_ok=True)
    abilities = {}

    for name in ROSTER:
        info = fetch_json("%s/cdn/%s/data/%s/champion/%s.json" % (DD, ver, LOCALE, name))
        if not info or "data" not in info:
            print("skip", name)
            continue
        champ = info["data"][name]
        with open(os.path.join(data_dir, "ddragon", name + ".json"), "w",
                  encoding="utf-8") as fh:
            json.dump(info, fh, ensure_ascii=False, indent=2)

        # champion portrait
        icon = os.path.join(icons_dir, "champions", name + ".png")
        fetch("%s/cdn/%s/img/champion/%s.png" % (DD, ver, name), icon)

        entry = {
            "id": champ.get("key", ""),
            "name": champ.get("name", name),
            "title": champ.get("title", ""),
            "portrait": rel(icon),
            "passive": {},
            "abilities": [],
        }
        p = champ.get("passive", {})
        if p:
            pf = p.get("image", {}).get("full", "")
            pdest = os.path.join(icons_dir, "spells", pf) if pf else None
            if pdest:
                fetch("%s/cdn/%s/img/passive/%s" % (DD, ver, pf), pdest)
            entry["passive"] = {"name": p.get("name", ""),
                                "desc": p.get("description", ""),
                                "icon": rel(pdest) if pdest else ""}

        for spell in champ.get("spells", []):
            img = spell.get("image", {}).get("full", "")
            sdest = os.path.join(icons_dir, "spells", img) if img else None
            if sdest:
                fetch("%s/cdn/%s/img/spell/%s" % (DD, ver, img), sdest)
            cds = spell.get("cooldown", [])
            costs = spell.get("cost", [])
            entry["abilities"].append({
                "key": ["Q", "W", "E", "R"][len(entry["abilities"])]
                       if len(entry["abilities"]) < 4 else "?",
                "name": spell.get("name", ""),
                "desc": spell.get("description", ""),
                "tooltip": spell.get("tooltip", ""),
                "cd": cds[0] if cds else 0,
                "cost": costs[0] if costs else 0,
                "costType": spell.get("costType", ""),
                "range": (spell.get("range") or [0])[0],
                "icon": rel(sdest) if sdest else "",
            })
        abilities[name] = entry
        print("ok", name, len(entry["abilities"]), "spells")

    for s in SUMMONERS:
        fetch("%s/cdn/%s/img/spell/%s.png" % (DD, ver, s),
              os.path.join(icons_dir, "summoner", s + ".png"))

    # Community Dragon in-game UI art.
    ui = os.path.join(ROOT, "lol", "ui")
    fetch("%s/latest/game/assets/maps/info/map11/2dlevelminimap_base_baron1.png" % CD,
          os.path.join(ui, "minimap.png"))
    fetch("%s/latest/game/assets/ux/floatinghealthbars/healthbaricons.png" % CD,
          os.path.join(ui, "healthbaricons.png"))
    fetch("%s/latest/game/assets/ux/scoreboard/scoreboardatlas.png" % CD,
          os.path.join(ui, "scoreboardatlas.png"))
    fetch("%s/latest/game/assets/ux/endofgame/eog_defeat_base.png" % CD,
          os.path.join(ui, "eog_defeat.png"))

    with open(os.path.join(data_dir, "abilities.json"), "w", encoding="utf-8") as fh:
        json.dump(abilities, fh, ensure_ascii=False, indent=2)
    print("wrote abilities.json for %d champions" % len(abilities))


if __name__ == "__main__":
    sys.exit(main())
