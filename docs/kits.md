# Dreamsy — Genre kits

Part of the Dreamsy spec. Section numbers are shared across the spec files; [docs/README.md](README.md) maps each
section to its file.

## 12. Genre kits + big-game pipeline (spec, 2026-11-15)

Target: every generated game offers **1–2 hours of play**. Opus designs and fills content; it does
not re-implement core systems per game.

### 12.1 Kits
- A kit is a tested, versioned Luau library shipped with the engine: `kits/<kit>/`
  - `init.luau` (+ submodules) — the API, required by games as `require("@kit/<kit>")`
    (submodules `require("@kit/<kit>/battle")`). Read-only to games; sandboxed like game scripts.
  - `KIT.md` — the API reference the harness injects into Opus's prompt when the game uses the kit
    (systems, data formats, example snippets, extension points, do/don't).
  - `schemas/*.json` — JSON Schemas for the kit's content files (e.g. `data/creatures/*.json`); the
    harness validates content against them.
  - `example/` — a small complete game package using the kit (also a golden test).
  - tests — headless core simulation tests in `engine/tests/kits/<kit>_test.cpp` (C++ doctest; ported from
    `engine-web/test/kits/<kit>.test.ts`, 2026-11-18).
- Manifest: `kits = ["rpg"]` (array; validated against the shipped kit list).
  - `kit.json` (optional) — `{ "requires": ["core", "progression"] }`: dependencies are enabled automatically when a game
    lists the kit (engine `expandKits` / `kitDependencies()`; format `registerKitDependencies` for the validator; the
    harness reads `kits/*/kit.json` itself).
- Kits are **data-driven**: content lives in the game package (`data/<kit-area>/*.json`, tilemaps,
  sprites); kits expose hooks/events for custom behaviour.
- v1 kits: `rpg` (overworld grid movement, NPCs/interaction/signs/doors/warps, dialog trees, quests,
  inventory, shops, party, turn-based battles with types/moves/status/XP, creature catching, saving),
  `platformer` (tight controller, enemies library, checkpoints, collectibles, world map/level select,
  bosses), `topdown` (Zelda-like action-adventure: combat, keys/doors, dungeons, items/abilities that
  gate areas, hearts), `tower-defense` (paths, waves, towers/upgrades, economy), `tycoon` (idle/
  incremental: generators, upgrades, prestige, offline progress).

### 12.2 Big-game pipeline (harness)
1. **Director** (Opus, high effort): writes the design bible in `notes.md` + structured
   `data/design/*.json` (world graph: regions/maps/connections/gates; progression & level curve; content
   lists — creatures, items, enemies, NPCs, quests — as specs; milestone plan of 10–20 rounds).
2. **Content builders** (parallel, Sonnet): the director calls `build_content {kind, items:[spec…]}`;
   the harness fans out to N workers, each with the bible + kit schema + its slice of specs, writing
   only its own ids/paths (sprites via draw_sprite, data files, maps); results are schema-validated
   and merged; failures are retried or reported back.
3. **Assembler** (Opus): wires content into scenes/maps, tunes, playtests, art-director review.
4. **Progression check** (static + simulated): world graph reachability with gates (every required
   area reachable in order; no softlocks), tilemap walkability between warps, kit-provided headless
   simulations (e.g. battle balance across the level curve). Failures go back to the director.
5. Rounds continue milestone by milestone (checkpoint per round) until the plan is done.
