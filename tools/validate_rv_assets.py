#!/usr/bin/env python3
"""Validate RV prefab/scene references before a build or commit.

Checks:
  * every prefab/scene JSON parses
  * `gltf:assets/...` mesh keys resolve to an existing .gltf
  * `assets/...` texture/model/script strings resolve inside projects/rv
  * every glTF buffer/image URI resolves next to the glTF

Usage:
  python tools/validate_rv_assets.py [--project projects/rv]
Exit code 0 = all references found, 1 = missing/broken references.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

ASSET_EXTS = (".png", ".jpg", ".jpeg", ".gltf", ".glb", ".bin", ".json", ".tga")
SKIP_PREFIXES = ("http://", "https://", "data:")


def walk_strings(node):
    if isinstance(node, str):
        yield node
    elif isinstance(node, dict):
        for value in node.values():
            yield from walk_strings(value)
    elif isinstance(node, list):
        for value in node:
            yield from walk_strings(value)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", default="projects/rv")
    args = parser.parse_args()

    root = Path(args.project).resolve()
    if not root.is_dir():
        print(f"project not found: {root}")
        return 1

    errors: list[str] = []
    scanned_json = 0
    checked_refs = 0

    json_files = sorted(root.glob("assets/prefabs/*.json"))
    json_files += sorted(root.glob("assets/scenes/*.json"))
    for path in json_files:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:
            errors.append(f"{path.relative_to(root)}: invalid JSON: {exc}")
            continue
        scanned_json += 1
        for value in walk_strings(data):
            if value.startswith(SKIP_PREFIXES):
                continue
            if value.startswith("gltf:"):
                rel = value[len("gltf:"):]
            elif value.startswith("assets/") and value.endswith(ASSET_EXTS):
                rel = value
            else:
                continue
            checked_refs += 1
            target = (root / rel).resolve()
            if not target.is_file():
                errors.append(f"{path.relative_to(root)}: missing {rel}")

    # Runtime code references that are not expressed in a prefab JSON.
    runtime_assets = [
        "assets/textures/grass_blade.png",  # procedural terrain scatter
    ]
    for rel in runtime_assets:
        checked_refs += 1
        if not (root / rel).is_file():
            errors.append(f"runtime: missing {rel}")

    # glTF buffers/images are referenced relative to the .gltf file itself.
    for gltf in sorted((root / "assets/models").rglob("*.gltf")):
        try:
            data = json.loads(gltf.read_text(encoding="utf-8"))
        except Exception as exc:
            errors.append(f"{gltf.relative_to(root)}: invalid glTF JSON: {exc}")
            continue
        for group in ("buffers", "images"):
            for item in data.get(group, []):
                uri = item.get("uri")
                if not uri or uri.startswith(SKIP_PREFIXES):
                    continue
                if not (gltf.parent / uri).is_file():
                    errors.append(
                        f"{gltf.relative_to(root)}: missing {group} uri {uri}")

    print(f"scanned {scanned_json} JSON files, {checked_refs} asset references")
    if errors:
        print(f"FAIL: {len(errors)} problem(s)")
        for err in errors[:80]:
            print("  " + err)
        if len(errors) > 80:
            print(f"  ... and {len(errors) - 80} more")
        return 1
    print("OK: all referenced RV assets exist")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
