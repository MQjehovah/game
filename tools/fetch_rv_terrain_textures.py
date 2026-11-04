#!/usr/bin/env python3
"""Fetch the 8 CC0 PBR layers used by the procedural RV terrain.

Downloads 2K Diffuse / nor_gl / arm maps from Poly Haven into
projects/rv/assets/textures/terrain/<layer>/ and writes the manifest
projects/rv/assets/data/terrain_layers.json.

Usage:
  python tools/fetch_rv_terrain_textures.py          # download missing files
  python tools/fetch_rv_terrain_textures.py --check  # verify only, non-zero on gaps
"""
import json
import sys
import time
from pathlib import Path
from urllib.request import Request, urlopen

HEADERS = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                         "AppleWebKit/537.36 (KHTML, like Gecko) "
                         "Chrome/124.0 Safari/537.36"}


def _open(url: str, timeout: int):
    return urlopen(Request(url, headers=HEADERS), timeout=timeout)

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/textures/terrain"
DATA = ROOT / "projects/rv/assets/data"

# layer order matters: the shader samples 0=soil, 1=rock, 2=snow, 3=extra.
# Poly Haven asset ids; the runner maps Diffuse/nor_gl/arm to our three maps.
LAYERS = [
    {"name": "grass",   "asset": "leafy_grass",        "uvScale": 0.22},
    {"name": "rock",    "asset": "gray_rocks",         "uvScale": 0.12},
    {"name": "snow",    "asset": "snow_02",            "uvScale": 0.18},
    {"name": "dirt",    "asset": "brown_mud_dry",      "uvScale": 0.20},
    {"name": "forest",  "asset": "forest_floor",       "uvScale": 0.24},
    {"name": "gravel",  "asset": "gravel_ground_01",   "uvScale": 0.16},
    {"name": "hardpan", "asset": "dry_mud_field_001",  "uvScale": 0.14},
    {"name": "cliff",   "asset": "cliff_side",         "uvScale": 0.10},
]

MAPS = {"albedo": "Diffuse", "normal": "nor_gl", "orm": "arm"}


def files_for(asset: str) -> dict:
    with _open(f"https://api.polyhaven.com/files/{asset}", 30) as r:
        return json.load(r)


def url_for(files: dict, kind: str) -> str:
    node = files[kind]["2k"]
    for ext in ("jpg", "png"):
        if ext in node:
            return node[ext]["url"]
    raise RuntimeError(f"no 2k jpg/png for {kind}")


def download(url: str, dest: Path, attempts: int = 4) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    last = None
    for i in range(attempts):
        try:
            with _open(url, 180) as r:
                data = r.read()
            if len(data) < 4096:
                raise RuntimeError(f"short download ({len(data)} bytes)")
            dest.write_bytes(data)
            return
        except Exception as e:  # transient CDN drops
            last = e
            time.sleep(1.5 * (i + 1))
    raise RuntimeError(f"failed to download {url}: {last}")


def check() -> int:
    missing = []
    for layer in LAYERS:
        for key in MAPS:
            p = OUT / layer["name"] / f"{key}.jpg"
            if not p.exists() or p.stat().st_size < 1024:
                missing.append(str(p.relative_to(ROOT)))
    if missing:
        print("missing terrain textures:")
        for m in missing:
            print("  " + m)
        return 1
    print(f"terrain textures OK ({len(LAYERS)} layers x {len(MAPS)} maps)")
    return 0


def main() -> int:
    if "--check" in sys.argv:
        return check()

    DATA.mkdir(parents=True, exist_ok=True)
    manifest = {"layers": []}
    for layer in LAYERS:
        name = layer["name"]
        dest = OUT / name
        if not (dest / "albedo.jpg").exists() or not (dest / "normal.jpg").exists() \
                or not (dest / "orm.jpg").exists():
            files = files_for(layer["asset"])
            for key, kind in MAPS.items():
                if not (dest / f"{key}.jpg").exists():
                    url = url_for(files, kind)
                    print(f"download {name}/{key}.jpg")
                    download(url, dest / f"{key}.jpg")
        manifest["layers"].append({
            "name": name,
            "albedo": f"{name}/albedo.jpg",
            "normal": f"{name}/normal.jpg",
            "orm": f"{name}/orm.jpg",
            "uvScale": layer["uvScale"],
        })
    (DATA / "terrain_layers.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"wrote {DATA / 'terrain_layers.json'}")
    return check()


if __name__ == "__main__":
    raise SystemExit(main())
