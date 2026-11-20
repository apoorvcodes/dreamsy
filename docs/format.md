# Dreamsy — Game package format

Part of the Dreamsy spec. Section numbers are shared across the spec files; [docs/README.md](README.md) maps each
section to its file.

## 3. Game package format (v0)

A game is a directory (authoring form), shipped as a Dreamsy pack (`.dpk`, §3.6), content-addressed and versioned.

```
game/
  dreamsy.toml               manifest
  scenes/*.scene.json        entity tree, flat map keyed by stable id
  scripts/
    client/*.luau            runs on each player's device
    server/*.luau            runs on the authoritative server only
    shared/*.luau            modules required by both
  data/*.luau|json           persistent data shapes (player + world), versioned
  data/tilesets/*.json       TileMap2D tilesets (§3.5)
  data/levels/*.txt          TileMap2D grids, one text row per tile row (§3.5)
  data/sfx/<id>.json         synthesized sound effects (sfxr-style params; LUAU_API.md)
  data/music/<id>.json       chiptune songs (≤ 4 channels, note text; LUAU_API.md)
  data/anims/<name>.json     sprite animation sets (frame lists, fps, loop; Sprite2D `anims`)
  assets/
    <id>.png|glb|ogg|ttf     binary assets
    <id>.meta.json           kind, file, provenance (model, prompt, seed, references), license
```

Implementation: `format/` (`@dreamsy/format`): schemas, entity catalog, styles, validator,
ops, deterministic zip, CLI (`pnpm -C format cli validate <dir>`). Golden examples:
`games/examples/coin-dash` (2D pixel, single-player), `games/examples/block-tower` (3D blocky, rooms),
`games/examples/tile-quest` (2D pixel, TileMap2D levels + multiple scenes), `games/examples/feature-demo`
(portrait pixel: sprite anims, tweens, particles, camera, save, dialogs, collision layers, text styles).
`format/catalog.json` is the generated catalog the engine loader and harness consume.

### 3.1 Manifest (`dreamsy.toml`)

```toml
format = "0"
id = "g_01J..."            # platform-assigned
name = "Tiny Clash"
dimension = "2d"           # "2d" | "3d"
orientation = "portrait"   # "portrait" (720×1280) | "landscape" (1280×720); absent = landscape (§11.1)
view = "oblique"           # 2.5D view (§14.5), 2D games only: "side" | "topdown" | "oblique" | "iso"; absent = "side"
entry_scene = "scenes/main.scene.json"
kits = ["rpg"]             # optional engine kits (§12.1); format/src/kits.ts KITS

[players]
min = 1
max = 8                    # > 1 requires server.mode != "none"

[server]
mode = "rooms"             # "none" | "rooms" | "persistent" | "async"
tick_rate = 30
deterministic = false      # true required for mode = "async"

[avatar]
enabled = true             # requires a SpawnPoint in the entry scene

[physics]                  # optional; ≤ 16 named collision layers for Body2D/Area2D/TileMap2D
layers = ["player", "enemy", "pickup", "world"]   # `layer` / `mask` props; unset = every layer

[ui]                       # theme for the engine-owned pause/settings menu; all optional,
primary = "#5e7ed6"        # defaults derived from style.palette / preset
panel = "pixel"            # "pixel" | "rounded" | "sharp" | "outlined"
font = "pixel"             # "pixel" | "rounded" | "bold" | "serif"

[[store]]                  # Stars items; see LUAU_API.md "Store"
id = "vip"
kind = "pass"              # "pass" | "consumable"
name = "VIP"
price = 100

[style]
preset = "pixel"           # "pixel" | "blocky" | "clean" | "toon" | "custom"
grid = 16                  # pixel only
palette = ["#1a1c2c", "#ffcd75"]
notes = "..."              # required for custom
references = ["style_ref"] # image asset ids
```

Strict: unknown keys are errors (catches model typos).

### 3.2 Art styles

A preset (`format/src/styles.ts`) drives three things at once:
- **harness**: how Claude builds (e.g. `blocky`/`clean` = primitives first, no image-to-3D → cheap)
- **asset pipeline**: style prompt appended to every generation
- **engine**: render hints (texture filter, pixel snap, base resolution, shading, outline)

`pixel` is 2D-only; others work in 2D and 3D.

### 3.3 Scenes

```json
{
  "format": "0",
  "root": "world",
  "entities": {
    "world":   { "type": "Node2D", "children": ["player", "coin_1"] },
    "player":  { "type": "Body2D", "props": { "position": [32, 144], "body": "dynamic" },
                 "scripts": ["scripts/client/player.luau"], "tags": ["player"] },
    "coin_1":  { "type": "Area2D", "props": { "position": [96, 104] }, "tags": ["coin"] }
  }
}
```

- Ids: `lowercase_snake`, stable forever, **unique across the whole package** (so a tap-selection
  or an op never needs a scene path).
- Tree = ordered `children` lists only; parent is derived, never stored (can't disagree).
- Types + typed props come from the catalog (`format/src/catalog.ts`): Folder, Node2D, Sprite2D,
  Body2D, Area2D, Camera2D, Text2D, TileMap2D, Particles2D, Light2D, Lighting2D, SpawnPoint2D, Node3D, Mesh3D, Body3D, Area3D, Camera3D,
  Light3D, Environment3D, SpawnPoint3D, Sound, UIText, UIButton, UIPanel.
  (2026-11-15 polish, additive: Light2D/Lighting2D 2D lighting; `emissive`/`blend` on Sprite2D/Text2D/Particles2D;
  Sprite2D `color` = alias of `modulate`; UIPanel `layout = "stack"`/`gap`/`padding`/`tint`/`radius`/`clip`; UIText `valign`.
  The validator only warns about a missing camera when the entry scene has world entities.)
- Prop values: numbers, bools, strings, `[x,y]`, `[x,y,z]`, `"#rrggbb(aa)"`, enums,
  `"asset:<id>"`, entity id refs. Unset = catalog default.
- Scripts attached to an entity run per-entity (`script.entity`). Client/server scripts not
  attached to anything run once at game start. Shared scripts are modules only.
- **2.5D (E7, 2026-11-17, additive).** Scene files may set a top-level `"view"` (same values as the manifest's) that
  overrides the manifest `view` for that scene; `view` in a 3D game is an error (`manifest.view_dimension`,
  `scene.view_dimension`). Props appended to the catalog (after each type's other props, so older prop indices never
  move): `z` (number, default 0, px above the ground, + up, relative to the parent) and `height` (number ≥ 0, default
  0, vertical extent in px) on Node2D, Sprite2D, Body2D, Area2D, Text2D, TileMap2D; `z_velocity` (px/s, + up, default 0)
  and `z_gravity_scale` (default 1) on Body2D; Sprite2D `anchor` (`"center"` | `"bottom"`, default `"center"`: which
  point of the image sits on `position`). Semantics (§14.5, LUAU_API.md "Height & 2.5D views"): bodies collide only
  when their z ranges overlap (height 0 = flat = collides with everything); moving bodies fall with z gravity and land
  on the highest static/kinematic top or raised solid tile under their center; topdown/oblique/iso have no planar
  gravity; in those views a Sprite2D with `height` > 0 draws as a block, a TileMap2D with `height` > 0 draws each tile
  as a block (its solid tiles are platforms). Packages without these fields behave exactly as before.
- Many scenes per game: `entry_scene` starts; scripts switch with `loadScene(path, params)`. A switch tears
  down the scene's entities, attached-script instances and physics; unattached scripts (the "session")
  and loaded modules keep running. The validator errors on literal `loadScene("...")` targets that don't exist.

### 3.4 Edit ops

The harness and in-app edits speak one op language (`format/src/ops.ts`), JSON, addressed by id:

```
set_manifest {key, value}         add_scene {scene, root}         remove_scene {scene}
add_entity {parent, id, type, props?, scripts?, tags?, index?}
set_props {id, props}  (null resets)   update_entity {id, type?, name?, scripts?, tags?}
remove_entity {id}  (subtree)     move_entity {id, parent, index?}
write_file {path, content}        edit_file {path, old, new}  (exact, single match; text files:
                                  notes.md, scripts/**.luau, data/**.luau|json, data/levels/**.txt)
delete_file {path}                put_asset {id, blob, meta}  (bytes from side channel)
remove_asset {id}
```

A batch applies all-or-nothing; failures name the op index. The result is then validated
(schema, tree integrity, prop types, asset/entity refs, manifest rules); issues are written
to be fed straight back to the model. The API runs the same validator on every uploaded package.
Still TODO: Luau compile/typecheck and headless boot test (harness, needs engine).
Versions are immutable; undo = revert (new version with an old package).

### 3.5 Tile maps

**TileMap2D** (2D, no children) draws and collides a whole level grid as one entity. Props: transform2d
(`position` = top-left corner of cell (0,0), `z_index` = layer), `tile_size` (px, default 16),
`tileset` (`"data/tilesets/<name>.json"`), `data` (the grid), `collision` (default true).

Tileset — `data/tilesets/<name>.json`, one entry per single printable-ASCII character:

```json
{
  "tiles": {
    "G": {"sprite": "asset:tile_grass", "solid": true},
    "B": {"sprite": "asset:tile_brick", "solid": true, "tags": ["breakable"]},
    "=": {"color": "#a76f4a", "solid": true},
    "^": {"sprite": "asset:tile_spike", "tags": ["spike"]},
    "c": {"color": "#f4f4f4"}
  }
}
```

`sprite` (`"asset:<id>"` image, stretched to the tile) and/or `color` (fill without sprite, tint with one;
neither = invisible); `solid` (default false); `tags` (lowercase_snake). `"."` and `" "` are always empty.

Grid — text, one line per tile row (top row first), one character per tile. Either inline
(`"data": "....\n.^..\nGGGG"`, small maps) or `"data": "file:data/levels/<name>.txt"` (anything bigger;
plain text, so rows diff and `edit_file` cleanly). Short rows are padded with empty tiles; trailing
blank lines are ignored; max 1024×256 tiles. Every character must be defined in the tileset (or be empty).
Engine: chunked merged meshes; solid tiles merged into row-run rectangles (seam-free floors);
`tileTouched` / `setTile` / `tileAt` in LUAU_API.md.

### 3.6 Dreamsy pack (`.dpk`) — transport and runtime format (2026-11-17)

**Decision:** packages travel and load as Dreamsy packs, not zips. Zip puts its index at the end,
deflates every file (including PNG/OGG that are already compressed), stores timestamps (so the same
content gives different bytes), and can't be read in place. A pack is built for the engine:

- **Index first.** Header + index come before the data, so one small (range) read gives the whole file
  list; the engine can parse `dreamsy.toml` and scenes before the media has arrived.
- **Uncompressed, 16-byte aligned.** Media is already compressed; text is compressed by HTTP
  (brotli/gzip) in transit, which beats zip because it shares context across files (e.g. 34 KB zip →
  20 KB gzip'd pack). The engine needs no decompressor and reads files in place from the pack buffer.
- **Content-addressed.** Every entry carries its SHA-256; the package's identity is the SHA-256 of
  the index. Enables integrity checks, de-duplication across versions and incremental updates
  (fetch only entries whose hash changed).
- **Canonical.** Sorted paths, fixed offsets, zero padding, no timestamps: the same files always give
  the same bytes. Readers accept only the canonical layout.

Layout (little-endian):

```
header, 80 bytes
  0   magic          "DREAMSY" 0x1A
  8   u32 version    1
  12  u32 flags      0 (reserved)
  16  u32 entry_count
  20  u32 index_size
  24  u64 data_offset = align16(80 + index_size)
  32  u64 data_size
  40  u8[32] index_sha256      (= the package content hash)
  72  u8[8]  reserved (0)
index, entry_count entries, paths strictly ascending (byte order), normalized (§3, no "..")
  u16 path_len, path (UTF-8), u8 compression (0 = none; others reserved), u8 reserved (0),
  u64 offset (from data_offset, multiple of 16), u64 size, u8[32] sha256
zero padding to data_offset
data: entries in index order, each at its offset, zero-padded to 16 bytes; the file ends there
```

Limits (reader): ≤ 20,000 entries, ≤ 64 MB per file, ≤ 200 MB total, paths ≤ 1024 bytes.
Reader/writer: `engine/src/pkg/pack.cpp`; `dreamsy-tool pack <dir|zip> <out.dpk>` (validates first),
`dreamsy-tool unpack`, `dreamsy-tool info` (prints the content hash). The C++ engine never reads zips;
the tool converts legacy zip builds. TS reader/writer: `format/src/pack.ts` (`Package.fromPack`, `toPack`,
`contentHash`; pure-TS synchronous SHA-256 so it runs in browsers), byte-identical with the C++ writer (cross-checked
in `format/test/pack.test.ts` against `dreamsy-tool` for every 2D example/showcase game); the format CLI packs to
`.dpk` when the output ends in `.dpk` and reads `.dpk` in `validate`/`unpack`/`apply`. **Still TODO:** the API and
harness still move zips (the players read both); switch them to `.dpk`.
