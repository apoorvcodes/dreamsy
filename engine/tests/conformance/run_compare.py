#!/usr/bin/env python3
"""Engine conformance: runs packages in the C++ engine and diffs the reports against golden files
recorded from engine-web, the TypeScript engine the C++ engine replaced (engine/tests/conformance/golden).

    python3 engine/tests/conformance/run_compare.py [--tool PATH] [--scripts|--games|--all] [--update]

The goldens were recorded with engine-web's headless runner before it was deleted, so they pin the
script-level behavior games were written against. `--update` rewrites the script-level lines of the
goldens from the C++ engine (for an intentional behavior change: review the diff before committing);
the entity dump rows are never rewritten, they stay engine-web's so drift keeps measuring parity.

Three suites:
  scripts  engine/tests/scripts/*: script-only games. Logs, errors, audio, save data and time must
           be identical, line for line.
  games    physics/entity scenarios (GAMES below): a package, a scripted input plan and entity
           dumps. Script-level output (logs, errors, audio, save, time) must be identical; entity
           positions/velocities are compared numerically and the drift is reported, because the
           engines use different physics (Rapier vs Box2D). This is the feel-parity check.
  all      every game in games/examples and games/showcase with a generic input plan: script
           output must be identical; player positions are compared like `games`.
With no flags: scripts + games. Exit 0 when every script-level report is identical (drift is
reported, not judged).
"""
import argparse
import difflib
import math
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT_TICKS, SEED, SCRIPT_BUDGET = "120", "7", "100"
# Entity games load kits; a debug + sanitizer build compiles them far slower than engine-web, so
# the CPU budget is generous for both engines (it isn't what these suites test).
GAME_BUDGET = "5000"

# A player holding right and jumping, then left, then up/down, pressing the buttons kits use.
GENERIC_INPUT = ";".join([
    "5:jump", "8:action",
    "20-140:move_x=1", "40:jump", "70:jump", "100:jump", "130:action",
    "141-260:move_x=-1", "160:jump", "190:action2", "220:jump",
    "261-360:move_y=1", "280:action", "300:jump",
    "361-460:move_y=-1", "380:action", "400:jump",
    "461-560:move_x=1", "470-500:action", "520:jump", "540:action2",
])

# name: (package dir, ticks, input plan, dump spec)
GAMES = {
    # walk right under the ledges and collect coin_3 (an Area2D: the tick it vanishes is compared),
    # then back left jumping into the ledges and right again
    "coin-dash": ("games/examples/coin-dash", 420,
                  "10-200:move_x=1;201-330:move_x=-1;331-420:move_x=1;215:jump;260:jump;300:jump;350:jump",
                  "2:player,#coin"),
    # dynamic player with friction 0, slimes, coins, tile map collisions, sfx on jumps
    "feature-demo": ("games/examples/feature-demo", 600,
                     "10-200:move_x=1;201-260:move_x=-1;261-600:move_x=1;30:jump;80:jump;130:jump;190:jump;280:jump;330:jump;400:jump;470:jump",
                     "10:player,#enemy"),
    # world map, enter level 1, run and jump (a script-side controller: no Box2D bodies)
    "kit-platformer": ("games/examples/kit-platformer", 600,
                       "20:jump;90-560:move_x=1;120:jump;160-175:jump;220:jump;300-320:jump;380:jump;450:jump",
                       "10:#player"),
    # the 2.5D test scene, planar only (its heights come from the C++ tests): walk into a crate
    "z_jump": ("engine/tests/scenes/z_jump", 300, "", "5:player"),
    # 2.5D from the package (view, z, height; slice E7): jump onto a step, take a raised coin,
    # walk off the ledge, bump a tower too tall to climb. Both engines run the z physics.
    "z_platforms": ("engine/tests/scenes/z_platforms", 240, "", "10:hero"),
    # free dynamics, no script control: bounce, friction, rotation, a stack, a kinematic pusher.
    # The honest Rapier-vs-Box2D number (the games above steer their bodies every tick).
    "physics_mix": ("engine/tests/scenes/physics_mix", 600, "",
                    "30:ball,box_tumble,capsule,stack_0,stack_2,stack_4,pusher,light"),
}


def default_tool():
    for preset in ("release", "debug"):
        p = ROOT / f"engine/build/{preset}/dreamsy-tool"
        if p.exists():
            return str(p)
    return str(ROOT / "engine/build/debug/dreamsy-tool")


GOLDEN = ROOT / "engine/tests/conformance/golden"
UPDATE = False


class Golden:
    """engine-web's report for one run, read from golden/<suite>/<name>.txt (shaped like a process result)."""
    def __init__(self, path):
        self.path = path
        self.returncode = 0 if path.exists() else 1
        self.stdout = path.read_text() if path.exists() else ""
        self.stderr = "" if path.exists() else f"no golden file {path.relative_to(ROOT)}"


def update_golden(path, cpp_stdout):
    """Script lines from the C++ run, dump rows kept from the golden (engine-web physics)."""
    old_dumps = [l for l in Golden(path).stdout.splitlines() if l.startswith("dump ")]
    script = [l for l in norm(cpp_stdout) if not l.startswith(("dump ", "hash "))]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(old_dumps + script) + "\n")


def run_both(tool, pkg, ticks, budget, plan="", dump="", suite="scripts", name=None):
    path = GOLDEN / suite / f"{name or Path(pkg).name}.txt"
    cpp_args = [tool, "run", str(pkg), "--ticks", str(ticks), "--seed", SEED, "--budget", budget]
    if suite == "scripts":
        cpp_args += ["--load-budget", SCRIPT_BUDGET]
    if plan:
        cpp_args += ["--input", plan]
    if dump:
        cpp_args += ["--dump", dump]
    cpp = subprocess.run(cpp_args, capture_output=True, text=True)
    if UPDATE and cpp.returncode == 0:
        update_golden(path, cpp.stdout)
    return Golden(path), cpp


def norm(text):
    # Memory addresses ("table: 0x...") differ between any two runs; everything else must match.
    return [re.sub(r"0x[0-9a-f]+", "0x…", line) for line in text.splitlines()]


def split_dumps(lines):
    script, dumps = [], {}
    for line in lines:
        if line.startswith("dump "):
            parts = line.split()
            dumps[(int(parts[1]), parts[2])] = [float(x) for x in parts[3:] if "=" not in x]
        elif not line.startswith("hash "):
            script.append(line)
    return script, dumps


def drift(a, b):
    """Position/velocity differences for dump rows present in both engines."""
    pos, vel, last = [], [], {}
    for key in sorted(set(a) & set(b)):
        va, vb = a[key], b[key]
        d = math.hypot(va[0] - vb[0], va[1] - vb[1])
        pos.append(d)
        last[key[1]] = (key[0], d)
        if len(va) >= 4 and len(vb) >= 4:
            vel.append(math.hypot(va[2] - vb[2], va[3] - vb[3]))
    return pos, vel, last, sorted(set(a) - set(b)), sorted(set(b) - set(a))


def report_diff(a, b, limit=40):
    lines = list(difflib.unified_diff(a, b, "golden (engine-web)", "c++", lineterm="", n=1))
    for line in lines[:limit]:
        print("   ", line)
    if len(lines) > limit:
        print(f"    … {len(lines) - limit} more diff lines")


def compare_scripts(tool, pkgs):
    failures = 0
    for p in pkgs:
        ts, cpp = run_both(tool, p, SCRIPT_TICKS, SCRIPT_BUDGET)
        if ts.returncode != 0 or cpp.returncode != 0:
            print(f"✗ {p.name}: ts exit {ts.returncode}, c++ exit {cpp.returncode}\n{ts.stderr[-1500:]}\n{cpp.stderr[-1500:]}")
            failures += 1
            continue
        a, b = norm(ts.stdout), norm(cpp.stdout)
        if a == b:
            print(f"✓ {p.name}  ({len(a)} lines identical)")
            continue
        failures += 1
        print(f"✗ {p.name}")
        report_diff(a, b)
    return failures


def compare_game(tool, name, pkg, ticks, plan, dump, suite="games"):
    ts, cpp = run_both(tool, pkg, ticks, GAME_BUDGET, plan, dump, suite, name)
    if ts.returncode != 0 or cpp.returncode != 0:
        print(f"✗ {name}: ts exit {ts.returncode}, c++ exit {cpp.returncode}\n{ts.stderr[-800:]}\n{cpp.stderr[-800:]}")
        return 1
    sa, da = split_dumps(norm(ts.stdout))
    sb, db = split_dumps(norm(cpp.stdout))
    pos, vel, last, only_ts, only_cpp = drift(da, db)
    max_pos = max(pos, default=0.0)
    mean_pos = sum(pos) / len(pos) if pos else 0.0
    same = sa == sb
    print(f"{'✓' if same else '✗'} {name}: script output {'identical' if same else 'DIFFERS'} ({len(sa)} lines); "
          f"{len(pos)} dump rows: position drift max {max_pos:.3g} px, mean {mean_pos:.3g} px; "
          f"velocity drift max {max(vel, default=0.0):.3g} px/s")
    if last:
        print("    at the last dump: " + ", ".join(f"{k} {d:.3g} px (tick {t})" for k, (t, d) in sorted(last.items())[:6]))
    if only_ts or only_cpp:
        print(f"    dump rows only in engine-web: {len(only_ts)} {only_ts[:4]}; only in c++: {len(only_cpp)} {only_cpp[:4]}")
    if not same:
        report_diff(sa, sb)
    return 0 if same else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", default=default_tool())
    ap.add_argument("--scripts", action="store_true", help="engine/tests/scripts only")
    ap.add_argument("--games", action="store_true", help="the physics/entity scenarios only")
    ap.add_argument("--all", action="store_true", help="every game in games/examples and games/showcase")
    ap.add_argument("--update", action="store_true", help="rewrite the goldens' script lines from the C++ engine")
    args = ap.parse_args()
    global UPDATE
    UPDATE = args.update
    print(f"tool: {args.tool}")
    failures = total = 0

    default = not (args.scripts or args.games or args.all)
    if args.scripts or default:
        pkgs = sorted(p for p in (ROOT / "engine/tests/scripts").iterdir() if p.is_dir())
        failures += compare_scripts(args.tool, pkgs)
        total += len(pkgs)
    if args.games or default:
        print("\nphysics/entity scenarios (drift = |golden engine-web - c++| over the dumped entities):")
        for name, (pkg, ticks, plan, dump) in GAMES.items():
            failures += compare_game(args.tool, name, ROOT / pkg, ticks, plan, dump)
            total += 1
    if args.all:
        print("\nall games, generic input (600 ticks):")
        for group in ("games/examples", "games/showcase"):
            for pkg in sorted(p for p in (ROOT / group).iterdir() if p.is_dir()):
                if 'dimension = "3d"' in (pkg / "dreamsy.toml").read_text():
                    print(f"- {pkg.name}: 3D, not supported by either engine (skipped)")
                    continue
                failures += compare_game(args.tool, pkg.name, pkg, 600, GENERIC_INPUT, "20:#player", "all")
                total += 1
    print(f"\n{total - failures}/{total} identical at script level")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
