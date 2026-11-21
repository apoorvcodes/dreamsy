#!/usr/bin/env python3
"""Validator conformance: the C++ loader must report the same issues as format/src/validate.ts.

Runs both validators on
  - every package in games/examples and games/showcase, and
  - broken packages generated here by mutating real games (one mutation per rule),
then compares the sets of (severity, code, file, id). Messages may differ in wording.

    python3 engine/tests/conformance/conformance.py [--tool engine/build/debug/dreamsy-tool] [--keep]

Exit code 0 when they agree. Needs node + pnpm deps installed in format/.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GAMES = [ROOT / "games/examples", ROOT / "games/showcase"]

# Differences that are intended (docs/engine.md §14): the C++ engine rejects 3D games.
CPP_ONLY_CODES = {"manifest.dimension_unsupported"}


# ---------------------------------------------------------------- package files as a dict

def read_pkg(d: Path) -> dict:
    out = {}
    for p in sorted(d.rglob("*")):
        if p.is_file() and p.name != ".DS_Store":
            out[p.relative_to(d).as_posix()] = p.read_bytes()
    return out


def write_pkg(files: dict, d: Path):
    for rel, data in files.items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data if isinstance(data, bytes) else data.encode())


def js(files, path):
    return json.loads(files[path])


def put_js(files, path, value):
    files[path] = json.dumps(value, indent=1, ensure_ascii=False).encode()


def toml_edit(files, old, new):
    text = files["dreamsy.toml"].decode().replace("\r\n", "\n")
    assert old in text, (old, text[:200])
    files["dreamsy.toml"] = text.replace(old, new, 1).encode()


def toml_append(files, extra):
    files["dreamsy.toml"] = files["dreamsy.toml"] + ("\n" + extra + "\n").encode()


def scene_edit(path):
    """Decorator-style helper: mutate the scene JSON at `path` with fn(scene)."""
    def wrap(fn):
        def run(files):
            s = js(files, path)
            fn(s)
            put_js(files, path, s)
        return run
    return wrap


FD = "scenes/main.scene.json"  # feature-demo entry scene


def ent(s, eid):
    return s["entities"][eid]


# ---------------------------------------------------------------- mutations (base package, fn)

def m(base, fn):
    return (base, fn)


MUTATIONS = {
    # ---- manifest
    "manifest_missing": m("feature-demo", lambda f: f.pop("dreamsy.toml")),
    "manifest_bad_toml": m("feature-demo", lambda f: toml_append(f, "name = ")),
    "manifest_unknown_key": m("feature-demo", lambda f: toml_edit(f, 'format = "0"', 'format = "0"\ncolour = "red"')),
    "manifest_name_too_long": m("feature-demo", lambda f: toml_edit(f, 'name = "Feature Demo"', 'name = "' + "x" * 81 + '"')),
    "manifest_bad_format": m("feature-demo", lambda f: toml_edit(f, 'format = "0"', 'format = "1"')),
    "manifest_bad_entry_pattern": m("feature-demo", lambda f: toml_edit(f, 'entry_scene = "scenes/main.scene.json"', 'entry_scene = "main.json"')),
    "manifest_entry_missing": m("feature-demo", lambda f: toml_edit(f, 'entry_scene = "scenes/main.scene.json"', 'entry_scene = "scenes/nope.scene.json"')),
    "manifest_players": m("feature-demo", lambda f: (toml_edit(f, "min = 1\nmax = 1", "min = 3\nmax = 2"), toml_edit(f, 'mode = "none"', 'mode = "rooms"'))),
    "manifest_multiplayer_needs_server": m("feature-demo", lambda f: toml_edit(f, "max = 1", "max = 4")),
    "manifest_async": m("feature-demo", lambda f: toml_edit(f, 'mode = "none"', 'mode = "async"')),
    "manifest_tick_rate": m("feature-demo", lambda f: toml_edit(f, 'mode = "none"', 'mode = "none"\ntick_rate = 500')),
    "manifest_custom_style": m("feature-demo", lambda f: toml_edit(f, 'preset = "pixel"', 'preset = "custom"')),
    "manifest_unknown_kit": m("feature-demo", lambda f: toml_edit(f, 'entry_scene', 'kits = ["rpg", "nope", "rpg"]\nentry_scene')),
    "manifest_grid_warning": m("feature-demo", lambda f: toml_edit(f, 'preset = "pixel"', 'preset = "clean"')),
    "manifest_layers_dup": m("feature-demo", lambda f: toml_edit(f, 'layers = ["player", "enemy", "pickup", "world"]', 'layers = ["player", "player"]')),
    "manifest_bad_color": m("feature-demo", lambda f: toml_edit(f, 'primary = "#3b5dc9"', 'primary = "blue"')),
    "manifest_store": m("feature-demo", lambda f: toml_append(f, '[[store]]\nid = "vip"\nkind = "pass"\nname = "VIP"\nprice = 100\nicon = "asset:nope"\n\n[[store]]\nid = "vip"\nkind = "pass"\nname = "VIP"\nprice = 100')),
    "manifest_store_invalid": m("feature-demo", lambda f: toml_append(f, '[[store]]\nid = "Bad"\nkind = "gift"\nname = ""\nprice = 0')),
    "manifest_style_refs": m("feature-demo", lambda f: toml_edit(f, 'preset = "pixel"', 'preset = "pixel"\nreferences = ["nope"]')),
    "manifest_3d": m("block-tower", lambda f: None),
    "manifest_split_header": m("feature-demo", lambda f: toml_edit(f, "[style]", "[\nstyle]")),
    "manifest_toml11": m("feature-demo", lambda f: toml_edit(f, "[players]\nmin = 1\nmax = 1", 'players = {\n  min = 1,\n  max = 1,\n}\nversion_note = "\\e"')),
    "manifest_date": m("feature-demo", lambda f: toml_edit(f, 'name = "Feature Demo"', 'name = 1979-05-27')),
    # ---- package
    "package_unknown_file": m("coin-dash", lambda f: f.update({"README.md": b"hi"})),
    # ---- assets
    "asset_bad_id": m("coin-dash", lambda f: f.update({"assets/Bad.meta.json": f["assets/coin.meta.json"]})),
    "asset_meta_json": m("coin-dash", lambda f: f.update({"assets/coin.meta.json": b"{nope"})),
    "asset_meta_invalid": m("coin-dash", lambda f: f.update({"assets/coin.meta.json": b'{"kind": "image", "file": "coin.png", "extra": 1}'})),
    "asset_bad_extension": m("coin-dash", lambda f: f.update({"assets/coin.meta.json": b'{"kind": "audio", "file": "coin.png"}'})),
    "asset_file_missing": m("coin-dash", lambda f: f.pop("assets/coin.png")),
    "asset_svg_script": m("feature-demo", lambda f: f.update({"assets/owl.svg": b'<svg viewBox="0 0 1 1"><script>x</script></svg>'})),
    "asset_svg_handler": m("feature-demo", lambda f: f.update({"assets/owl.svg": b'<svg viewBox="0 0 1 1"><rect onclick="x()"/></svg>'})),
    "asset_svg_href": m("feature-demo", lambda f: f.update({"assets/owl.svg": b'<svg viewBox="0 0 1 1"><image href="http://x/y.png"/></svg>'})),
    "asset_svg_url": m("feature-demo", lambda f: f.update({"assets/owl.svg": b'<svg viewBox="0 0 1 1"><rect fill="url(\'#g\')"/></svg>'})),
    "asset_svg_ok_url": m("feature-demo", lambda f: f.update({"assets/owl.svg": b'<?xml version="1.0"?>\n<svg viewBox="0 0 1 1"><rect fill="url(#g)"/></svg>'})),
    "asset_svg_no_viewbox": m("feature-demo", lambda f: f.update({"assets/owl.svg": b'<svg width="1"></svg>'})),
    "asset_svg_not_svg": m("feature-demo", lambda f: f.update({"assets/owl.svg": b'<html></html>'})),
    "asset_no_meta": m("coin-dash", lambda f: f.update({"assets/extra.png": b"x"})),
    # ---- scripts
    "script_bad_path": m("coin-dash", lambda f: f.update({"scripts/other/x.luau": b""})),
    "script_literals": m("coin-dash", lambda f: f.update({"scripts/client/extra.luau":
        b'loadScene("scenes/nope.scene.json")\nloadScene("main")\nlocal x = require("@kit/rpg")\nrequire("@kit/nope/x")\n'
        b'require("@nope")\nsfx.play("boom")\nmusic.play(\'song\')\nfoo.loadScene("scenes/main.scene.json")\nmyloadScene("x")\n'})),
    # ---- data
    "tileset_bad_path": m("feature-demo", lambda f: f.update({"data/tilesets/Bad.json": b"{}"})),
    "tileset_invalid": m("feature-demo", lambda f: f.update({"data/tilesets/world.json": b'{"tiles": {"G": {"sprite": "nope", "solid": "yes"}}}'})),
    "tileset_keys": m("feature-demo", lambda f: f.update({"data/tilesets/extra.json": b'{"tiles": {"ab": {"color": "#ffffff"}, ".": {"solid": true}, "X": {"sprite": "asset:nope"}, "Y": {"sprite": null}}}'})),
    "tileset_json": m("feature-demo", lambda f: f.update({"data/tilesets/world.json": b"[1,2"})),
    "sfx_invalid": m("feature-demo", lambda f: f.update({"data/sfx/coin.json": b'{"wave": "piano", "freq": 9}'})),
    "sfx_long": m("feature-demo", lambda f: f.update({"data/sfx/coin.json": b'{"sustain": 2, "decay": 3, "attack": 1}'})),
    "sfx_zero": m("feature-demo", lambda f: f.update({"data/sfx/coin.json": b'{"sustain": 0, "decay": 0}'})),
    "sfx_bad_path": m("feature-demo", lambda f: f.update({"data/sfx/Bad.json": b"{}"})),
    "music_invalid": m("feature-demo", lambda f: f.update({"data/music/theme.json": b'{"channels": [{"notes": "C4 H9 -"}], "bpm": 10}'})),
    "music_multiple": m("feature-demo", lambda f: f.update({"data/music/theme.json": b'{"steps": 4, "channels": [{"notes": "C4 - . E4 G4"}], "order": [0, 3]}'})),
    "music_ok_string": m("feature-demo", lambda f: f.update({"data/music/theme.json": b'{"steps": 2, "channels": [{"notes": " C4  -\\n. Bb3 "}, {"notes": ["A4", ".", "-", "C#5"], "wave": "noise"}]}'})),
    "anims_invalid": m("feature-demo", lambda f: f.update({"data/anims/fox.json": b'{"Run": {}, "jump": {"frames": [], "fps": 100}, "idle": {"frames": ["asset:nope"], "next": "gone"}}'})),
    "anims_empty": m("feature-demo", lambda f: f.update({"data/anims/fox.json": b'{}'})),
    "anims_not_object": m("feature-demo", lambda f: f.update({"data/anims/fox.json": b'[]'})),
    "anims_bad_path": m("feature-demo", lambda f: f.update({"data/anims/Bad.json": b"{}"})),
    "data_txt_path": m("feature-demo", lambda f: f.update({"data/levels/Bad Name.txt": b"..", "data/x.txt": b".."})),
    # ---- scenes
    "scene_json": m("coin-dash", lambda f: f.update({"scenes/main.scene.json": b'{"format": "0",'})),
    "scene_invalid": m("coin-dash", scene_edit("scenes/main.scene.json")(lambda s: s.update({"extra": 1, "format": "1"}))),
    "scene_entity_invalid": m("coin-dash", scene_edit("scenes/main.scene.json")(lambda s: ent(s, "camera").update({"colour": 1, "tags": ["Bad"]}))),
    "scene_unknown_file": m("coin-dash", lambda f: f.update({"scenes/notes.txt": b"x"})),
    "scene_dup_ids": m("tile-quest", lambda f: (lambda a, b: put_js(f, "scenes/win.scene.json", {**b, "entities": {**b["entities"], next(iter(a["entities"])): {"type": "Node2D"}}}))(js(f, "scenes/level_1.scene.json"), js(f, "scenes/win.scene.json"))),
    "scene_root_missing": m("coin-dash", scene_edit("scenes/main.scene.json")(lambda s: s.update({"root": "nope"}))),
    "scene_tree": m("coin-dash", scene_edit("scenes/main.scene.json")(lambda s: (
        ent(s, "world")["children"].extend(["camera", "ghost", "world"]),
        ent(s, "ground").setdefault("children", []).append("ledge_1"),
        s["entities"].update({"orphan": {"type": "Node2D"}, "BadId": {"type": "Node2D"}}),
        ent(s, "world")["children"].append("BadId")))),
    "entity_types": m("feature-demo", scene_edit(FD)(lambda s: (
        s["entities"].update({"thing": {"type": "Gizmo"}, "cube": {"type": "Node3D"}}),
        ent(s, "world")["children"].extend(["thing", "cube"]),
        ent(s, "map").update({"children": ["sign"]}),
        ent(s, "world")["children"].remove("sign")))),
    "props_invalid": m("feature-demo", scene_edit(FD)(lambda s: (
        ent(s, "player").setdefault("props", {}).update({"position": [1, "2"], "rotation": "a", "z_index": 1.5, "colour": 1}),
        ent(s, "sky").setdefault("props", {}).update({"modulate": "red", "blend": "screen", "texture": "asset:nope"}),
        ent(s, "hills").setdefault("props", {}).update({"texture": "coin", "visible": 1}),
        ent(s, "cam").setdefault("props", {}).update({"follow": "nobody", "zoom": 1e9}),
        ent(s, "sign").setdefault("props", {}).update({"text": "x" * 5000})))),
    "script_missing": m("feature-demo", scene_edit(FD)(lambda s: ent(s, "player").update({"scripts": ["scripts/client/nope.luau"]}))),
    "tilemap_invalid": m("feature-demo", lambda f: (f.update({"data/levels/level.txt": f["data/levels/level.txt"] + b"\nZZQ~\xc3\xa9.\n\n"}),)),
    "tilemap_refs": m("feature-demo", scene_edit(FD)(lambda s: ent(s, "map")["props"].update({"tileset": "tiles.json", "data": "file:data/levels/none.txt"}))),
    "tilemap_inline": m("feature-demo", scene_edit(FD)(lambda s: ent(s, "map")["props"].update({"tileset": "data/tilesets/none.json", "data": "\n\n"}))),
    "tilemap_huge": m("feature-demo", scene_edit(FD)(lambda s: ent(s, "map")["props"].update({"data": ("G" * 1100 + "\n") * 3}))),
    "anims_refs": m("feature-demo", scene_edit(FD)(lambda s: (
        ent(s, "player_sprite")["props"].update({"anim": "fly"}),
        ent(s, "slime_sprite")["props"].update({"anims": "data/anims/none.json"}),
        ent(s, "slime_2_sprite")["props"].update({"anims": "anims.json"}),
        ent(s, "coin_1_sprite").setdefault("props", {}).update({"anims": "", "anim": "spin"})))),
    "physics_layers": m("feature-demo", scene_edit(FD)(lambda s: ent(s, "player")["props"].update({"layer": " ghost ", "mask": "world, nope,,"}))),
    "no_camera": m("feature-demo", scene_edit(FD)(lambda s: (s["entities"].pop("cam"), ent(s, "world")["children"].remove("cam")))),
    "no_spawn": m("feature-demo", lambda f: toml_append(f, "[avatar]\nenabled = true")),
    # zod counts code points (51 emoji = 102 UTF-16 units passes .max(100)); prop maxLength counts UTF-16 units.
    "utf8_name_ok": m("feature-demo", scene_edit(FD)(lambda s: ent(s, "player").update({"name": "🦊" * 51}))),
    "script_path_in_entity": m("feature-demo", scene_edit(FD)(lambda s: ent(s, "player").update({"scripts": ["scripts/shared/state.luau"]}))),
    "asset_kind_refs": m("feature-demo", lambda f: (
        f.update({"assets/beep.meta.json": b'{"kind": "audio", "file": "beep.ogg"}', "assets/beep.ogg": b"OggS",
                  "data/anims/extra.json": b'{"beep": {"frames": ["asset:beep"]}}',
                  "data/tilesets/extra.json": b'{"tiles": {"B": {"sprite": "asset:beep"}}}'}))),
    "music_bad_path": m("feature-demo", lambda f: f.update({"data/music/Bad.json": b"{}"})),
    "style_dimension": m("block-tower", lambda f: toml_edit(f, 'preset = "blocky"', 'preset = "pixel"')),
    "mesh_texture_ignored": m("block-tower", lambda f: (lambda s: (
        s["entities"].update({"thing": {"type": "Mesh3D", "props": {"mesh": "asset:a", "texture": "asset:b"}}}),
        s["entities"][s["root"]].setdefault("children", []).append("thing"),
        put_js(f, "scenes/main.scene.json", s)))(js(f, "scenes/main.scene.json"))),
    "utf16_prop_length": m("feature-demo", scene_edit(FD)(lambda s: ent(s, "player_sprite")["props"].update({"anim": "🦊" * 33}))),
    "utf8_name_long": m("feature-demo", scene_edit(FD)(lambda s: ent(s, "player").update({"name": "🦊" * 101}))),
    # ---- 2.5D (E7): view, z / height / z_velocity / z_gravity_scale, Sprite2D anchor
    "view_ok": m("feature-demo", lambda f: (toml_edit(f, 'entry_scene', 'view = "oblique"\nentry_scene'),
        scene_edit(FD)(lambda s: (s.update({"view": "iso"}),
            ent(s, "player")["props"].update({"z": 12, "height": 24, "z_velocity": 0, "z_gravity_scale": 0.5}),
            ent(s, "player_sprite")["props"].update({"height": 0, "anchor": "bottom"}),
            ent(s, "map")["props"].update({"z": 0, "height": 32})))(f))),
    "view_invalid": m("feature-demo", lambda f: toml_edit(f, 'entry_scene', 'view = "isometric"\nentry_scene')),
    "view_scene_invalid": m("feature-demo", scene_edit(FD)(lambda s: s.update({"view": "3d"}))),
    "view_3d": m("block-tower", lambda f: (toml_edit(f, 'entry_scene', 'view = "iso"\nentry_scene'),
        (lambda s: (s.update({"view": "oblique"}), put_js(f, "scenes/main.scene.json", s)))(js(f, "scenes/main.scene.json")))),
    "props_25d_invalid": m("feature-demo", scene_edit(FD)(lambda s: (
        ent(s, "player")["props"].update({"height": -3, "z": "up", "z_gravity_scale": True}),
        ent(s, "player_sprite")["props"].update({"z_velocity": 5, "anchor": "feet"}),
        ent(s, "cam").setdefault("props", {}).update({"z": 4})))),
}


def make_mutants(tmp: Path) -> list:
    bases = {d.name: d for g in GAMES for d in g.iterdir() if (d / "dreamsy.toml").exists()}
    out = []
    for name, (base, fn) in MUTATIONS.items():
        files = read_pkg(bases[base])
        fn(files)
        d = tmp / name
        write_pkg(files, d)
        out.append(d)
    return out


def issue_set(issues):
    return {(i["severity"], i["code"], i.get("file", "") or "", i.get("id", "") or "") for i in issues}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", default=str(ROOT / "engine/build/debug/dreamsy-tool"))
    ap.add_argument("--keep", action="store_true", help="keep the generated packages")
    args = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="dreamsy-conformance-"))
    try:
        real = sorted(d for g in GAMES for d in g.iterdir() if (d / "dreamsy.toml").exists())
        mutants = make_mutants(tmp)
        dirs = real + mutants

        ts_out = subprocess.run(["node", "--import", "tsx", str(ROOT / "engine/tests/conformance/ts_validate.mts"), *map(str, dirs)],
                                cwd=ROOT / "format", capture_output=True, text=True)
        if ts_out.returncode != 0:
            print(ts_out.stderr)
            sys.exit("TS validator failed to run")
        ts = {Path(r["dir"]): r for r in map(json.loads, ts_out.stdout.splitlines())}

        failures = 0
        codes_seen = set()
        for d in dirs:
            cpp_run = subprocess.run([args.tool, "validate", str(d), "--json"], capture_output=True, text=True)
            try:
                cpp = json.loads(cpp_run.stdout)
            except json.JSONDecodeError:
                print(f"✗ {d.name}: C++ tool crashed (exit {cpp_run.returncode})\n{cpp_run.stderr[-2000:]}")
                failures += 1
                continue
            t = ts[d]
            if "crash" in t:
                print(f"! {d.name}: TS validator threw: {t['crash']} (skipped)")
                continue
            a = issue_set(t["issues"])
            b = {i for i in issue_set(cpp["issues"]) if i[1] not in CPP_ONLY_CODES}
            codes_seen |= {i[1] for i in a}
            label = d.name if d in mutants else d.relative_to(ROOT).as_posix()
            # Equal issue sets imply equal verdicts (ok = no errors), apart from the C++-only codes.
            if a == b:
                print(f"✓ {label}  ({len(a)} issues)")
                continue
            failures += 1
            print(f"✗ {label}")
            for i in sorted(a - b):
                msg = next((x["message"] for x in t["issues"] if (x["severity"], x["code"], x["file"], x["id"]) == i), "")
                print(f"    only TS : {i}  {msg[:160]}")
            for i in sorted(b - a):
                msg = next((x["message"] for x in cpp["issues"] if (x["severity"], x["code"], x.get("file", ""), x.get("id", "")) == i), "")
                print(f"    only C++: {i}  {msg[:160]}")

        print(f"\n{len(dirs) - failures}/{len(dirs)} packages agree; {len(codes_seen)} distinct issue codes exercised")
        sys.exit(1 if failures else 0)
    finally:
        if args.keep:
            print(f"packages kept in {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
