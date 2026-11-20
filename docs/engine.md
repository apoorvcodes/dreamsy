# Dreamsy — Engine (C++ 2D/2.5D)

Part of the Dreamsy spec. Section numbers are shared across the spec files; [docs/README.md](README.md) maps each
section to its file.

## 14. Engine v2 — native C++ 2D/2.5D engine with authoritative multiplayer (spec, 2026-11-17)

**Decision (2026-11-17):** Dreamsy is a **2D and 2.5D engine only**. 3D is dropped from scope (not "later").
The engine is rewritten in **C++** as one core that runs on the web (WebAssembly), iOS, Android and the game server.
Multiplayer moves from relay (§4.5) to **server-authoritative simulation with client prediction**. The TypeScript
engine (`engine-web/`) was the product and the reference implementation until the C++ engine reached parity (§14.8);
it was deleted 2026-11-18.

Why:
- **One simulation everywhere.** The server and every client run the same compiled core, which is what authoritative
  multiplayer, replays and async play (§4.2) need.
- **Size and startup.** `engine-web` shipped a 5.9 MB JS bundle + 0.7 MB Luau WASM. Target for the C++ web build: ≤ 1.5 MB
  gzipped including Luau, measured in CI.
- **Native apps without a WebView** (§14.7), from the same code.
- **Determinism.** Box2D v3 is deterministic across platforms; Rapier's compat build is not guaranteed to be.

What does **not** change: the game package format (§3) and the Luau API (`format/LUAU_API.md`) are the contract.
Kits (`kits`, ~41k lines of Luau) and generated games carry over unchanged. The harness keeps generating the
same packages.

### 14.1 Stack

| Need | Choice |
|---|---|
| Language | C++20, restricted style (§14.2) |
| Scripting | Luau (vendored `third_party/luau`), linked natively; WASM via Emscripten on the web |
| Physics | Box2D v3 (C17, cross-platform deterministic), replaces Rapier |
| Graphics / app / input | sokol (`sokol_gfx`, `sokol_app`): WebGL2 on the web (WebGPU later), Metal on iOS, GLES3 on Android |
| Audio | miniaudio (sfxr-style synth and chiptune music ported from `render/audio.ts`) |
| Images / fonts | stb_image, stb_truetype |
| Zip / data | miniz (zip), a small TOML reader for `dreamsy.toml`, a JSON parser (simdjson or yyjson) |
| Build | CMake + Ninja; Emscripten for web; Xcode / Android NDK via CMake toolchains |

All third-party code is vendored under `third_party/` (not modified; see CONTRIBUTING.md).

### 14.2 Code style

Clean, readable C++ that a TypeScript/Python developer can learn from. Mostly structs and free functions.

- Errors are values: `Result<T>` / `Status` with a message. Engine code never uses `throw`/`try`. Exceptions stay
  enabled in the compiler only because Luau raises script errors by unwinding through host calls, and destructors must run
  when it does. `-fno-rtti` for engine code.
- No raw `new`/`delete`. Ownership via `std::unique_ptr` or by value; `std::vector`, `std::string`, `std::span`.
- Entities and other engine objects are referenced by **generational handles** (`EntityId { index, generation }`), never
  by pointer across frames. A stale handle fails a check instead of reading freed memory.
- Per-frame scratch memory from an arena that resets each tick.
- No deep templates, no inheritance trees, no operator tricks. Virtual calls only at platform boundaries.
- Naming: types `PascalCase`, functions and variables `snake_case`, files `snake_case.cpp/.h`, namespace `dreamsy`.
- `clang-format` + `clang-tidy` in CI.
- `docs/CPP-FOR-TS-DEVS.md` explains the patterns we use, mapped to TypeScript/Python.

### 14.3 Layout

```
engine/
  CMakeLists.txt
  src/
    base/       Result, handles, arenas, logging, deterministic math + RNG
    pkg/        package loading: zip/dir, manifest, scenes, tilesets, levels, anims, sfx/music, assets
    world/      entities, components, transforms, scene tree, tick loop
    physics/    Box2D v3 wrapper, collision layers, height (z) for 2.5D
    script/     Luau VMs, prelude, host API (the LUAU_API.md surface), CPU/memory budgets
    render/     sokol renderer: sprites, atlases, tilemaps, text, UI, particles, lighting, camera, 2.5D views
    audio/      miniaudio, sfx synth, music
    input/      keyboard, mouse, touch, gestures, on-screen controls
    net/        protocol, snapshots, prediction, reconciliation, interpolation
    platform/   web (Emscripten), iOS, Android, headless
  apps/
    player/     the game player (web/iOS/Android)
    server/     headless authoritative room simulation
    tool/       headless CLI: run a package, scripted playtests, state hashes, screenshots
  tests/        unit tests + the conformance runner (§14.8)
  fuzz/         fuzzers for package loading and the protocol
```

### 14.4 Simulation and determinism

- Fixed tick, 60 Hz. Rendering interpolates between ticks.
- Nothing in the simulation reads the wall clock or the OS RNG. Time comes from the tick counter; randomness from a
  seeded platform RNG (`base/rng`).
- Floats are allowed but constrained: no `-ffast-math`, `-ffp-contract=off`, and the simulation uses `base/math`
  (own `sin`/`cos`/`atan2`/`sqrt`-based helpers) instead of libm transcendentals, which differ across platforms.
  The Luau `math` library inside simulation VMs is overridden with the same functions.
- Box2D v3 with its cross-platform determinism settings.
- A **state hash** per tick (entities, components, physics, script-visible state) is available in debug builds and
  in the tool. Desync detection and the conformance suite use it.

This settles the §4.2 **OPEN** on deterministic physics: all of physics is deterministic.

### 14.5 2D and 2.5D

The world is 2D (`x`, `y`) plus an optional **height** `z`. Physics runs in the 2D plane (Box2D); `z` is handled by the
engine (gravity on `z`, jumping, falling off ledges, landing), in the style of classic beat-'em-ups and isometric games.
The camera is always orthographic.

View modes (manifest, per scene override):

| `view` | Projection | Depth order | Typical games |
|---|---|---|---|
| `side` | x right, y up | layer, then z-index | platformers, runners (today's 2D) |
| `topdown` | x right, y down | layer, then y | top-down RPGs, shooters, tycoon |
| `oblique` | top-down, z draws upward | layer, then y, then z | Zelda-like, beat-'em-ups |
| `iso` | 2:1 isometric diamond | layer, then x+y, then z | city builders, tactics, farming |

Features:
- Depth sorting by view mode; sprites can set an anchor (feet) and a sort offset.
- Height: per-entity `z` and `height`, drop shadows on the ground, collisions only between overlapping `z` ranges.
- Isometric tilemaps with per-tile elevation and stacked tiles.
- Parallax layers.
- Normal-mapped sprite lighting on top of today's Light2D (lights get a height).
- Later: sprite stacking (layered slices) and billboards.

Format changes for 2.5D are in §3 (§3.1 manifest `view`, §3.3 scene `view`, entity `z`/`height`/`z_velocity`/
`z_gravity_scale`, Sprite2D `anchor`), landed in E7 (2026-11-17, minimal slice):
- Both engines simulate height identically (engine-web through Rapier contact/intersection hooks; conformance:
  `engine/tests/scenes/z_platforms` in `run_compare.py`). engine-web previews every view flat; the C++ renderer draws
  them.
- Ground support uses the mover's **center** (its feet), not its whole footprint: a body stands on a block while its
  center is over it and steps off a ledge when its center leaves. This also keeps depth sorting a simple key sort.
- Depth sorting with blocks (`render/sort.h`): blocks (Sprite2D / tiles with `height`) sort by their footprint center;
  an item standing on raised ground is moved right after the block it stands on, with its shadow. In iso, long sprite
  blocks are split into square cells so the center key stays correct.
- Drop shadows fall on the ground under the item (a block's top when over one), not always on the floor.
- Scripts: `e.onGround`, `e.groundZ` (Body2D, read-only), `camera.view()`, `camera.screenDirToWorld(dir)` (joystick →
  world direction; rotates in iso); `camera.screenToWorld` / `worldToScreen` and world taps project through the view.
- Not yet (rest of E7): per-tile elevation grids and stacked tiles inside one TileMap2D (today: one raised map per
  height level), iso tilesets with art for each face, sort offsets, normal-mapped lighting, parallax, moving
  platforms carrying riders, kit updates.

### 14.6 Multiplayer v2 — server-authoritative

**Model:** the server simulates; clients send inputs, predict their own player, and interpolate everything else.
Built for co-op and party games up to 8 players (platformers, top-down, racing, arenas). The design leaves room for a
`rollback` mode later (fast 1v1 fighting/duels), which the determinism of §14.4 makes possible.

- **Two VM roles.** The server runs `scripts/server` + `scripts/shared`; each client runs `scripts/client` +
  `scripts/shared`. Single-player runs both roles in one process (as today) without a network.
- **Inputs.** Clients send numbered input frames (buttons, sticks, taps; ≤ 60/s, batched) to the server.
- **Snapshots.** The server sends snapshots of replicated state at 20–30 Hz: quantized, delta-compressed against the
  last acknowledged snapshot, binary.
- **Replication.** Entities in replicated scenes replicate their transform and physics state by default. Script
  properties replicate only when declared (Luau API, specified in `format/LUAU_API.md` in slice E8).
- **Ownership.** An entity can be owned by a player. The owner's client predicts it.
- **Prediction + reconciliation.** The owning client simulates its own entities immediately with local input. When a
  snapshot arrives, it rewinds to the server state and replays unacknowledged inputs. Non-owned entities interpolate
  ~100 ms behind.
- **Remote events** (`remoteEvent`) stay, now delivered through the server.
- **Interest management.** Later: clients receive only nearby entities.
- **Reconnect.** Same seat within 60 s; the server sends a full snapshot.
- **Relay compatibility.** v1 relay games (`room.replicate`, §4.5) keep working in the C++ engine as `net.model = "relay"`
  until they are rebuilt.

**Transport:** binary over WebSocket first (works everywhere). Native clients get UDP/WebTransport later.

**Topology:** the existing Node room server (`services/gameserver`) keeps lobbies, codes, quick match, presence and
spectators. When a match starts, it assigns the room to a simulation process (`engine/apps/server`), which loads the
package and runs the match. One simulation process hosts many rooms.

### 14.7 Targets

| Where | How |
|---|---|
| Web / phone browsers | `engine/apps/player` via Emscripten (WebGL2, WebGPU later) |
| Mobile app, step 1 | the WASM build inside the existing WebView (smaller and faster than today) |
| Mobile app, step 2 | native: an Expo native module that hosts the engine view (iOS Metal, Android GLES3); WebView retired |
| Game servers | `engine/apps/server`, Linux, headless |
| Harness playtests | `engine/apps/tool` headless for logic playtests; screenshots from the web build in headless Chromium until the tool renders offscreen |

App Store: games remain interpreted Luau + data in a sandbox (§6, guideline 4.7).

### 14.8 Migration and parity

- **Done 2026-11-18: `engine-web` removed** (last version: git tag `engine-web-final`). The harness, the app, share
  links, the dev stack and the deploy image use the C++ engine; engine-web's test suite was ported to C++
  (`engine/tests/kits`, `engine/tests/api`, the headless playtest of every game).
- **Conformance suite:** `engine/tests/conformance/run_compare.py` diffs the C++ engine against golden reports recorded
  from engine-web before it was deleted (`golden/`: script-only tests, physics scenarios, every game with a scripted
  input plan). Script output must be identical; entity positions are compared numerically against engine-web's
  physics as drift. `--update` rewrites the script lines for an intentional behavior change (never the dump rows).
  `conformance.py` keeps diffing the C++ loader against the TypeScript validator in `format/`.
- What the C++ engine does not do yet that engine-web did: the online room client (multiplayer games run solo until
  E8), the API store and leaderboards in the web player, saves across sessions on the web (§6 gaps).
- Physics behaves differently (Rapier → Box2D). Kits with physics balance sims are re-tuned where needed.

### 14.9 Safety

Packages and scripts are untrusted (AI- and user-generated).
- Luau sandbox (no filesystem, no network), per-script CPU and memory budgets, as today.
- Every size and count read from a package is bounds-checked; limits on textures, entities, audio length, zip size.
- Fuzzers for package loading and the network protocol run in CI.
- CI runs the tests under AddressSanitizer and UndefinedBehaviorSanitizer.
- The server never trusts client inputs beyond their declared shape and rate.

### 14.10 Slices

Each slice ends with tests passing and is built one at a time (docs/README.md).

| Slice | Delivers | Done when |
|---|---|---|
| E0 Scaffold | `engine/` CMake, vendored deps, test runner, CI with sanitizers, native + web "hello", `docs/CPP-FOR-TS-DEVS.md` | builds on macOS, Linux, web; CI green |
| E1 Packages | Dreamsy pack (§3.6) + dir loading, manifest, scenes, tilesets, levels, anims, sfx/music, asset index, full validator port (same issue codes as the TS validator); fuzzers | loads every package in `games/`; conformance with format/src/validate.ts |
| E2 Scripting | §14.13: native Luau, client/server VMs, prelude + host API core, require/kits, budgets, errors with traces, deterministic math, `dreamsy-tool check` (type checking against `dreamsy.d.luau`) | script-only examples run headless; every game type-checks |
| E3 World + physics (2.5D-native) | entities with `x, y, z` + `height` from day one, view modes in the data model, tick loop, Box2D v3 in the ground plane + engine-handled z (gravity, jumps, ledges), collision layers, state hash | coin-dash runs headless and passes conformance; a z-jump test scene behaves identically on every platform |
| E4 Renderer (2.5D-native) | sokol: batched sprites, atlases, tilemaps, text, UI, camera, particles, lighting; depth sorting per view mode (§14.5), drop shadows; web build | examples look right in a browser; side/topdown/oblique/iso test scenes sort correctly |
| E5 Input, audio, save, shell | touch/keys/gestures, on-screen controls, audio, save, platform menu, dialogs | feature-demo works on a phone browser |
| E6 Parity | full host API, all kits, conformance suite green; harness playtests and app WebView switch to the C++ build | §14.8 parity. **Done 2026-11-18:** harness (§5.1: `dreamsy-tool playtest` / `eval`, browser shots from the C++ web player), app WebView + share links (embedding contract, §6), engine-web's tests ported, conformance against goldens; engine-web deleted |
| E7 2.5D content | iso tilemaps with elevation and stacked tiles, normal-mapped lighting, parallax, §3 format fields, kit updates (topdown/rpg/farming get oblique/iso) | an iso and an oblique example game. **Minimal slice landed 2026-11-17:** §3 fields (view, z/height/z_velocity/z_gravity_scale, Sprite2D anchor) in both validators, z physics in both engines, blocks/raised tile maps/shadows in the C++ renderer, script helpers, docs + harness prompt, and a Claude-built oblique showcase game (§14.5) |
| E8 Netcode v2 | §14.6, `engine/apps/server`, gameserver integration, `LUAU_API.md` replication API | mp-duel and racing versus run authoritative |
| E9 Native mobile | Expo native module, iOS + Android builds | showcase games run natively on both |
| E10 Full engine | remaining §14.12 items not landed in E4–E9 | §14.12 quality bars met |
| Later | rollback mode, UDP/WebTransport, interest management, offscreen screenshots | |

Parity (E6) is the floor, not the goal. Each slice from E4 on includes the §14.12 features for its area.

2.5D is built into the world model (E3) and renderer (E4) from the start, not added later: every entity
has `z` and `height`, and depth sorting follows the scene's view mode. E7 adds the 2.5D content features.

**E4 decisions (2026-11-17):** the renderer consumes a plain draw list (`engine/src/render/draw_list.h`)
filled by the world; `z_index` is a sub-layer under §14.5's depth keys (layer, z_index, then y / x+y / z);
alpha and additive blending share batches via premultiplied alpha; pixel-art styles render into a
reference-size target with nearest upscaling; shaders are hand-written MSL + GLSL ES 3.0 until custom
materials need sokol-shdc; bundled fonts are Press Start 2P (E6: DejaVu Sans Mono), Nunito, Inter, Source
Serif 4 (OFL; Latin-1 subsets); WebP is deferred (clear error) because no package uses it and libwebp costs ~100 KB.

**E5 input + platform UI decisions (2026-11-17):** script-visible input follows engine-web exactly
(`engine/src/input/input_state.h`: pressed for the one tick after an edge, live held, pointer visible one tick
after release, swipes ≥ 40 px within 0.6 s with px/s velocity, everything hidden while a blocking dialog is
open, `recentlyRead` = 45 ticks). The platform UI is drawn by the engine into the draw list, not as HTML:
UI entities, toasts, dialogs, on-screen controls and the pause button/menu share one layout module
(`engine/src/ui/layout.h`) for drawing and hit testing. On-screen controls keep engine-web's sizes in CSS px
(points), converted to reference px per window (E6: placed in the whole window, see below). Headless
`tap_screen` keeps engine-web's `uiHit` rules (global z_index, no safe area); device taps use the drawn
stacking order.

**E6 visual parity decisions (2026-11-17):** `engine/tools/parity.mts` screenshots every game in both engines
at the reference size and the same game time (manual stepping, `?manual=1` + `window.__dreamsy.step`) with
the same scripted input and scores the difference; that is the parity check for looks. The pixel theme font is
DejaVu Sans Mono Bold, not Press Start 2P: engine-web's pixel stack names fonts no device has, so every game
was laid out with the system monospace (0.6 em per character) and Press Start 2P (1 em) overflowed panels. A
DejaVu Sans Bold symbol subset is the fallback for characters the bundled fonts lack (▶ ▼ ★ ♥ → …). SVGs are
pre-processed before nanosvg (unitless bounding-box gradients, `<use>`, rect `<pattern>` fills) and `<text>` is
drawn with the bundled fonts. The renderer has per-batch clip rects (scissor), Lighting2D (a multiply pass after
the world, emissive items after it) and a UI pass over the whole window, so the pause button, pause menu and
touch controls sit in the window like engine-web's container. The pause menu is engine-web's full shell
(Settings, Controls, local Leaderboard, Quit); settings persist under engine-web's localStorage key on the web.

### 14.11 Open questions

- **OPEN:** WebGPU from the start or after WebGL2 parity (default: after).
- **OPEN:** whether the harness moves screenshots to the native tool (offscreen rendering) or keeps headless Chromium.
- **OPEN:** remove the 3D entity types from the format at E7 (format version bump) or immediately.
- **OPEN:** whether the engine/format/kits are released as open source, and under which license (MIT or Apache-2.0).

### 14.12 Full engine scope (not a toy)

The target is a complete, production 2D/2.5D engine: everything a team would expect to ship a commercial 2D game,
built natively for 2D (no 3D scene graph underneath).

**Rendering**
- Batched sprite renderer: one draw call per atlas + material; textures packed into atlases at load (stb_rect_pack).
- Materials and custom shaders: GLSL written once, cross-compiled to Metal / GLES3 / WebGL2 / WGSL with `sokol-shdc`.
- Render layers, render targets, and a post-processing stack (bloom, color grading LUT, vignette, pixelate/CRT, distortion).
- Pixel-perfect mode (integer scaling, snapped camera) and smooth mode.
- 2D lighting: point/spot/ambient lights, normal maps, shadows cast by occluders.
- Crisp text at any size (MSDF fonts), 9-slice sprites, lines/shapes/debug draw.

**Animation**
- Frame animations, tweens and easing (as today).
- 2D skeletal animation: bones, skinned meshes, IK, in Dreamsy's own JSON format.
- Animation state machines with transitions and events.

**World**
- Scene tree, prefabs (instancing with overrides), tags/groups, scene transitions, entity pooling.
- Chunked tilemaps for large worlds; rule-based autotiling, animated tiles, collision generated from tiles; iso + elevation.
- Native pathfinding: A* on grids, flow fields for crowds, navmesh for non-grid levels.

**Physics** (Box2D v3)
- Bodies, shapes, joints, sensors, ray and shape casts, continuous collision.
- Character controller helpers: slopes, one-way platforms, moving platforms, coyote time.

**Audio**
- Mixer with buses (music, sfx, UI), ducking, 2D positional audio.
- Streaming music (Ogg Vorbis via stb_vorbis) plus today's synthesized sfx and chiptune music.

**Input and UI**
- Action mapping over keyboard, mouse, touch and gamepad; rebinding; gestures; on-screen controls.
- Retained UI with flex-style layout, theming, scrolling, focus navigation (gamepad/keyboard), safe areas.
- Text input with IME on web and mobile. UTF-8 everywhere, font fallback; complex-script shaping **OPEN** (HarfBuzz).

**Platform services**
- Versioned saves, localization string tables, app lifecycle (pause/resume), orientation, haptics, asset cache.

**Tooling** (dev builds only, compiled out of release)
- Hot reload of scripts and assets.
- Dev overlay (Dear ImGui): entity inspector, physics debug draw, profiler (per-system CPU, draw calls, memory), network
  graph, log console. Tracy profiler hooks.
- Input recording and deterministic replay; state-hash diffing between runs and engines.
- Asset packer at publish time (atlases, MSDF fonts, audio), cached by content hash.

**Quality bars** (measured in CI or on devices)
- 60 fps with 5,000 animated sprites + 200 physics bodies on an iPhone 12 and a mid-range Android, native and web.
- Web engine download ≤ 1.5 MB gzipped; first frame ≤ 1 s after download on a mid-range phone.
- Clean under AddressSanitizer + UndefinedBehaviorSanitizer; fuzzers run nightly.
- Identical state hashes on macOS, Linux, iOS, Android and web for the conformance suite.
- Every Luau-facing API documented in `format/LUAU_API.md`.

### 14.13 Scripting (E2)

The Luau API contract (`format/LUAU_API.md`) is unchanged: every kit and game runs as is. The host improves:

- **Native Luau.** Linked as C++, not WASM-in-JS: no JS↔WASM call cost per host call. Luau native code
  generation where the platform allows JIT-style code (desktop, Android, servers); the interpreter on iOS
  and the web.
- **Same prelude, native host.** The Luau prelude (`engine/src/script/prelude.luau`, plain Luau calling numbered host
  ops; moved from engine-web/core/prelude.ts unchanged) runs on ops implemented in C++. The op numbers live in
  `src/script/ops.h` and are substituted into the prelude at build time.
- **VMs.** Single-player (`server.mode = "none"`) keeps one VM for all scripts, as engine-web did: client and
  server scripts share module tables, and games rely on that. Server-authoritative multiplayer (§14.6) runs a
  server VM (`scripts/server` + `scripts/shared`) and a client VM (`scripts/client` + `scripts/shared`). Each
  script runs in its own sandbox thread.
- **OPEN: fast vectors.** Luau's native `vector` type would make vector math allocation-free, but it can't tell
  `Vec2` from `Vec3` (`tostring`, prop encoding and type checks depend on that). Decide after profiling E3.
- **Budgets.** Per-callback CPU deadline via the interrupt callback (a script stuck in a loop is killed with
  a clear error, and `pcall` can't swallow it); memory cap per VM.
- **Errors developers and models can act on.** `scripts/client/player.luau:12: attempt to index nil with
  'position'` plus a stack trace with script paths. Errors in callbacks are reported once with the callback's
  origin.
- **Deterministic math.** `math.sin/cos/tan/atan2/exp/log/pow` use the engine's deterministic
  implementations (§14.4); `math.random`/`random()` use the seeded platform RNG.
- **Type checking before play.** `dreamsy-tool check <package>` runs Luau's analyzer over every script against
  `engine/script/dreamsy.d.luau` (typed declarations of the whole API, kept in sync with `LUAU_API.md`) and
  reports `path:line: message`. The harness runs it before playtests, so wrong field names, missing
  arguments and nil mistakes are fixed before a game ever boots. Non-strict scripts get warnings; `--!strict`
  scripts get errors. (Built 2026-11-17: declarations in `format/dreamsy.d.luau`, Entity props generated from the
  catalog; Luau's old solver, since the new one crashes in Luau 0.747. Lenient by design: possibly-nil values,
  mixed list literals and untyped `{}` tables are warnings even in strict scripts, because they rarely fail at
  runtime. Every valid game in the repo checks with zero errors. `--strict` analyzes every script strictly.)
- **Per-script profiling.** CPU time per script and callback, shown in the dev overlay (§14.12).
- **Hot reload** (dev builds): a changed script re-runs without restarting the game.
