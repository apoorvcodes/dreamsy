# Dreamsy engine v2 (C++)

The native 2D/2.5D engine: one C++20 core for web (WebAssembly), iOS, Android and headless game
servers. Design and roadmap: [docs/engine.md](../docs/engine.md) §14 (spec index: [docs/README.md](../docs/README.md)). New to C++? Start with `docs/cpp-for-ts-devs.md`.

Status: **E0 (scaffold), E1 (packages), E2 (scripting), E3 (world + physics) done; E4 (renderer) foundation done; E5 input + platform UI (keys, mouse, touch, on-screen controls, gestures, UI entities, dialogs, toasts, pause) done, audio pending; E7 2.5D minimal slice done (package `view`, `z`/`height`, blocks, raised tile maps, shadows; docs/engine.md §14.5); E6 parity done: the harness, the app and share links run on this engine and engine-web (the TypeScript engine) was deleted 2026-11-18 (tag `engine-web-final`).**
It loads and validates every game in `games/` (same verdicts as the TypeScript validator), reads/writes
Dreamsy packs (`.dpk`, §3.6), and runs games headless natively: entities, Box2D physics with 2.5D height,
tweens, sprite animation, particles, camera, tile maps, dialogs. Every 2D game in `games/` produces the same
script output as engine-web did (`tests/conformance/run_compare.py --all`, against golden reports recorded from it).
The harness playtests on this engine (`dreamsy-tool playtest`, the web player for screenshots; docs/harness.md §5.1) and the
app plays games in the web player (see [Embedding the web player](#embedding-the-web-player-the-app-share-links)).

## Build

Needs CMake ≥ 3.22, Ninja, and a C++20 compiler (Apple clang 15+ or clang/gcc on Linux).

```sh
cd engine
cmake --preset debug && cmake --build --preset debug   # debug + AddressSanitizer/UBSan
ctest --preset debug                                   # run the tests
./build/debug/dreamsy-tool smoke                       # Luau + Box2D check
./build/debug/dreamsy-tool validate ../games/examples/feature-demo
./build/debug/dreamsy-tool pack ../games/examples/feature-demo /tmp/fd.dpk   # also converts .zip builds
./build/debug/dreamsy-tool info /tmp/fd.dpk            # summary + content hash
./build/debug/dreamsy-tool run tests/scripts/basics    # run scripts headless (logs, errors, save)
./build/debug/dreamsy-tool check ../games/examples/feature-demo   # type-check scripts (format/dreamsy.d.luau)
./build/release/dreamsy-tool run ../games/examples/coin-dash --ticks 300 \
    --input "10-200:move_x=1;40:jump" --dump "10:player,#coin" --hash   # scripted input, entity dumps, state hashes
./build/release/dreamsy-tool run tests/scenes/z_platforms --ticks 240   # 2.5D from the package (view, z, height)
./build/release/dreamsy-tool playtest ../games/showcase/cloud-hopper --seconds 8 --auto-dialog first \
    --timeline '[{"t":0.3,"tap":[360,900]},{"t":0.5,"hold":["right"]},{"t":1,"press":["jump"]}]'   # harness playtest -> JSON
./build/release/dreamsy-tool eval --base ../games/examples/kit-tycoon --kits tycoon --script sim.luau --until out   # kit sims -> JSON
./build/release/dreamsy-tool run tests/scenes/z_jump --view topdown \
    --height player=16,crate=16,ledge=48 --zjump 50:player=300 --dump 10:player   # 2.5D test overrides
./build/debug/dreamsy-player                           # window (Metal): runs feature-demo (keys, mouse, Esc/P pause)
./build/debug/dreamsy-player ../games/examples/kit-tycoon --touch   # --touch: on-screen controls with the mouse
./build/debug/dreamsy-player --view iso                # force a view: side | topdown | oblique | iso (default: the package's); keys 1-4 / Tab; --smooth; --frames N; --demo
```

Other presets: `release`, `headless` (no window; for Linux CI), `web`, and `ios-sim`, `ios-device`,
`android-arm64` (see [Phones](#phones)).

Web (needs Emscripten; the repo uses 4.0.11 in `~/emsdk`):

```sh
source ~/emsdk/emsdk_env.sh
cmake --preset web && cmake --build --preset web
ctest --preset web                                     # tests run under Node
# build/web/dreamsy-player.html: serve the folder with any static server to view it
# (dreamsy-player.html#iso picks the view mode, #iso-smooth forces smooth mode)
NODE_PATH=../harness/node_modules node tools/web_screenshot.cjs /tmp/dreamsy-e4 540x960
NODE_PATH=../harness/node_modules node tools/web_touch_screenshot.cjs /tmp/dreamsy-e5
#   as a phone: real touch events tap through the dialog, push the joystick, hold A
# Another game in the web player (kit games need their kit packs embedded):
cmake --preset web -DDREAMSY_DEMO_PACKAGE_DIR=$PWD/../games/examples/kit-tycoon -DDREAMSY_WEB_EMBED_KITS=ON
#   headless Chromium screenshots of every view mode (Playwright page.route, no server)
# A game's own view, from its own build folder, holding keys on the way (see the script header):
DREAMSY_WEB_BUILD=build/web-mygame DREAMSY_SHOT_KEYS="d@0,Space@800" NODE_PATH=../harness/node_modules \
    node tools/web_screenshot.cjs /tmp/shots 540x960 game
```

### Embedding the web player (the app, share links)

`build/web` is a static site: `dreamsy-player.{html,js,wasm}` plus `kits/<kit>.dpk`. `scripts/dev.sh` serves it on
`:2005` (with `index.html` = `dreamsy-player.html`) and `deploy/Dockerfile` builds it into `/app/player`.
The contract lives in `apps/player/embed.js` (linked with `--pre-js`; the download starts before the WASM is ready):

| Query | Loads |
|---|---|
| `?public=<gameId>` (or a `/g/<gameId>` path) | the published version, `GET <api>/v1/public/games/<id>/package`; `<api>` = `?api=`, else `api.<domain>` on `play.<domain>`, else this host on `:8787` |
| `?api=<base>&game=<id>&version=<n>&token=<jwt>` | `GET <api>/v1/games/<id>/versions/<n>/package` with `Authorization: Bearer` (token from the query, else `localStorage.dreamsy_token`) |
| `?pkg=<url>[&kits=<base>]` | a `.dpk` or `.zip` by URL (dev and tools); kits from `<base>/<kit>.dpk` (default `kits/`) |
| nothing | the embedded demo (`/pkg`), except in embed mode (error "no game to load") |

Packages are told apart by content (zip signature), so URLs need no extension. Embed mode (`?embed=1`, inside a
React Native WebView, or `?public=`) posts messages to the host: a JSON string to `window.ReactNativeWebView.postMessage`
and, inside an iframe, the object to `window.parent.postMessage(msg, "*")`:

| Message | When |
|---|---|
| `{type:"ready"}` | the game is running |
| `{type:"error", message}` | the game didn't load (download, validation, start), or a script error after ready (at most one per 2 s, the same one at most every 30 s) |
| `{type:"quit"}` | pause menu Quit (outside embed mode Quit goes back) |
| `{type:"runEnd", reason, board?, value?}` | `leaderboard.finish` (`"finish"`, value = seconds) / `leaderboard.submit` (`"submit"`), `platform.runEnded(reason)` (`gameover` / `finish` / `quit`), or the room finished; at most one per 3 s |

`?insets=top,right,bottom,left` (CSS px) keeps the pause button, menu, touch controls and the game's UI anchors clear
of the host's safe area. Not supported yet (engine-web's dev page had them): `{type:"purchase"}` and the API store
(the store is the free in-memory one), API leaderboards (runs, ranks, ghosts; `runEnd` has no `rank`), saved games
(`save.*` lasts for the session; engine-web kept it in localStorage) and `{type:"save"}`,
`?room=` / `?spectate=` and the lobby (no room client until E8: multiplayer games run solo with the offline room),
`?touch=` (use `#touch`), `?budget=`. `?scene=<path>&params=<json>&save=<json>` start mid-game (harness playtests).

## Phones

The player runs natively on phones (docs/engine.md §14.7, groundwork for E9): Metal on iOS, GLES3 on
Android, both through sokol_app. Today it shows the E0 spinning square. Scripts run the same as
everywhere else; Luau is always the interpreter (Luau.CodeGen is never linked), which is what iOS
requires anyway. Platform files live in `platform/ios/` (Info.plist template, `ios.cmake`) and
`platform/android/` (Gradle project).

### iOS Simulator

Needs Xcode with an iOS Simulator runtime (tested: Xcode 15.4, iOS 17.5 simulator, Apple silicon).

```sh
cmake --preset ios-sim && cmake --build --preset ios-sim     # build/ios-sim/Dreamsy.app (arm64)
xcrun simctl boot "iPhone 15"                                 # or open -a Simulator
xcrun simctl install booted build/ios-sim/Dreamsy.app
xcrun simctl launch --console-pty booted com.dreamsy.player   # log goes to this terminal
xcrun simctl io booted screenshot /tmp/dreamsy-ios.png
ctest --preset ios-sim                                        # dreamsy-tests inside the booted simulator
xcrun simctl spawn booted "$PWD/build/ios-sim/dreamsy-tool" run "$PWD/tests/scripts/basics"
xcrun simctl shutdown booted
```

`dreamsy-tests` and `dreamsy-tool` are plain command-line binaries; `simctl spawn` runs them in the
simulator, where they can read the Mac's files (use absolute paths). Simulator builds need no signing.

### Your own iPhone (free Apple ID)

The `ios-device` preset generates an Xcode project; Xcode signs it.

1. Xcode > Settings > Accounts: add your Apple ID. It gets a free "Personal Team".
2. On the phone (iOS 16+): connect by cable, tap "Trust", then turn on Settings > Privacy & Security >
   Developer Mode and restart.
3. Pick a bundle ID nobody else has used (free teams cannot use `com.dreamsy.player`), and generate:
   ```sh
   cmake --preset ios-device -DDREAMSY_IOS_BUNDLE_ID=com.yourname.dreamsy -DDREAMSY_IOS_TEAM_ID=YOURTEAMID
   open build/ios-device/dreamsy_engine.xcodeproj
   ```
   The team ID is shown in Xcode > Settings > Accounts (or leave it out and pick the team in Xcode under
   the `dreamsy-player` target > Signing & Capabilities; re-running CMake resets that choice).
4. In Xcode pick the `dreamsy-player` scheme and your phone, press Run.
5. First launch only: on the phone, Settings > General > VPN & Device Management > trust your developer
   profile, then open Dreamsy.

Without the Xcode UI:

```sh
cmake --build --preset ios-device -- -allowProvisioningUpdates
xcrun devicectl list devices
xcrun devicectl device install app --device <id> build/ios-device/Release-iphoneos/Dreamsy.app
xcrun devicectl device process launch --device <id> com.yourname.dreamsy
```

Limits of a free Apple ID: the app stops launching after 7 days (run again from Xcode), at most 3 apps
per device. Xcode only deploys to iOS versions it knows: Xcode 15.4 goes up to iOS 17.x, so a phone on a
newer iOS needs a newer Xcode. To check the device build compiles without signing:
`cmake --build --preset ios-device -- CODE_SIGNING_ALLOWED=NO`.

### Android

Not built yet on this machine (no SDK/NDK). What to install:

- Android Studio (it brings the SDK, a JDK 17+ and Gradle support), or the command-line tools.
- SDK Manager: Android SDK Platform 35, Build-Tools 35, **NDK 27.2.12479018** (r27c), **CMake 3.22.1**.
- Python 3 on `PATH` (the engine build runs code generators) and Gradle 8.9+ if building from a shell.

Build the APK (NativeActivity, arm64-v8a, minSdk 26) with Gradle:

```sh
cd platform/android
gradle wrapper --gradle-version 8.10.2      # once; or just open this folder in Android Studio
./gradlew assembleDebug                      # app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.dreamsy.player/android.app.NativeActivity
adb logcat -s SOKOL                          # sokol logs to logcat (tag SOKOL)
```

Gradle runs `engine/CMakeLists.txt` with `-DDREAMSY_BUILD_TESTS=OFF` and builds only `dreamsy-player`, which on
Android is `libdreamsy-player.so` (sokol_app supplies `ANativeActivity_onCreate`; there is no Java code).
For a plain CMake build of the libraries, tool and tests (e.g. to `adb push` and run them under
`/data/local/tmp`):

```sh
export ANDROID_NDK_HOME=~/Library/Android/sdk/ndk/27.2.12479018
cmake --preset android-arm64 && cmake --build --preset android-arm64
```

The engine's own logging (`base/log.cpp`) writes to stderr, which Android drops; it needs a logcat sink before E9.

## Layout

Comments that cite `engine-web/...` ("same rules as engine-web/core/world.ts") name the TypeScript engine this one
replaced, where the behavior came from. It was deleted 2026-11-18; read any of those files with
`git show engine-web-final:engine-web/core/world.ts`.

| Path | What |
|---|---|
| `src/base/` | `Result`/`Status` errors, `Pool`/`Id` handles, `Arena`, `Rng` (bit-identical to the TS engine), `Hasher`, logging, checks |
| `src/pkg/` | packages: `Files` (packs, dirs), `pack.cpp` (.dpk), `catalog` (embedded `format/catalog.json` + kit list), `manifest` (TOML), `formats` (tilesets, levels, anims, sfx, music, SVG), `validate` (`load_package()`), `zip_import` (tools only) |
| `src/world/` | the entity world (E3): `world` (entities with catalog props and 2.5D `z`/`height`, scene tree, generational ids + never-reused script handles, prop coercion, transforms, view mode), `tilemap`, `tween`, `particles` |
| `src/physics/` | Box2D v3 (32 px = 1 m, Rapier's average friction/restitution) + engine-handled z: gravity, landing, z-range collision filtering (see `physics2d.h`) |
| `src/script/` | Luau host (§14.13): `vm` (sandbox, memory cap, CPU budget, tracebacks), `host` (scripts, require, kits, scheduler, scenes, non-entity ops), `host_world` (entity ops, physics events, tweens, anims, particles, camera, dialogs), `host_input` (pointer signals, swipes, tap_screen / tap_world / press, pause); prelude + kits embedded from engine-web at build time |
| `src/check/` | `dreamsy-tool check`: Luau analyzer over a package's scripts against `format/dreamsy.d.luau` (tools only) |
| `src/input/` | input (E5), platform-free: `input_state` (what scripts read per tick: axes, pressed/held, pointer grace, swipes; engine-web core/input.ts), `keyboard`, `gamepad` (mapping hook; polling not wired), `usage` (which controls a game reads), `touch_controls` (on-screen joystick / A B C / aim stick: layout, hit testing, visibility from reads) |
| `src/ui/` | platform UI (E5): `dialog` (ui.dialog / ui.choice state machine + input glue), `toasts`, `layout` (UI entity layout + hit testing, shared by drawing and taps), `overlay` (draws UI entities, toasts, dialogs, controls, pause button into the draw list; `dreamsy_overlay`), `shell` (the pause menu: Settings, Controls, Leaderboard, Quit) |
| `src/play/` | the running game in the player: `scene_draw` (world -> draw list), `platform_ui` (device events -> controls / pause / dialog / UI / world taps; draws the overlay) |
| `src/render/` | renderer (E4). CPU side, no GPU (`dreamsy_render`): `draw_list.h` (what E3 fills each frame), `sort` (depth order per view mode, shadows), `view` (letterbox, projection, camera, pixel snap), `image` (PNG/JPG stb_image, SVG nanosvg), `atlas` (stb_rect_pack pages), `textures` (package images on first use), `font` (bundled fonts, stb_truetype, layout), `geometry` (batched quads), `style` (manifest render settings). GPU side (`dreamsy_render_gpu`): `renderer` (sokol_gfx, hand-written Metal + GLSL ES 3.0 shaders) |
| `src/dmath/` | deterministic math: musl libm linked into every executable (see its README) |
| `src/platform/` | sokol implementation unit; web link settings |
| `platform/ios/` | iOS app bundle: `Info.plist.in`, `ios.cmake` (bundle ID, signing settings) |
| `platform/android/` | Android Gradle project: NativeActivity manifest loading `libdreamsy-player.so` |
| `src/dev/` | E0 smoke checks (Luau, Box2D), kept for `dreamsy-tool smoke` |
| `apps/bench/` | `dreamsy-bench`: CPU time per frame by stage (see [Performance](#performance)); packages in `tests/bench/` |
| `apps/tool/` | `dreamsy-tool`, the headless CLI; `playtest.cpp`: `playtest` (the harness's scripted playtests, JSON report) and `eval` (a Luau script in a package with kits, for kit balance sims) |
| `apps/player/` | `dreamsy-player`, the windowed engine; `demo_scene` draws a test scene from a package's assets until E3/E6 |
| `apps/player/embed.js` | the web player's embedding contract: package source from the query, messages to the host app (see [Embedding](#embedding-the-web-player-the-app-share-links)) |
| `tools/` | `web_screenshot.cjs` (screenshots of the web player), `web_touch_screenshot.cjs` (phone touch input), `make_play_page.py` (packs games for the web player) |
| `tests/` | doctest unit tests, one binary: `dreamsy-tests`; `tests/kits/` kit tests + shared helpers, `tests/api/` Luau API and every-game tests (see [Tests](#tests)) |
| `tests/conformance/` | `conformance.py`: C++ vs `format/src/validate.ts` on every game plus ~80 generated broken packages; `run_compare.py`: C++ vs golden reports recorded from engine-web (`golden/`, `--update` after an intentional change): `tests/scripts/*` (identical logs, errors, audio, save, time), physics scenarios with scripted input (identical script output + measured position/velocity drift), `--all` over every game |
| `tests/scripts/` | script-only test games (basics, modules, data/save, errors/budget, scenes/room/store/audio, math) |
| `tests/scenes/` | entity test scenes (`z_jump`: the 2.5D jump/land/ledge scene with test overrides; `z_platforms`: 2.5D from the package, compared with engine-web) |
| `fuzz/` | fuzz targets (pack, package, zip import); `-replay` builds run in ctest, libFuzzer builds in CI |

Third-party code lives in `../third_party/` (vendored, unmodified; each folder has `DREAMSY_VENDOR.md`).

## Renderer notes (E4)

- Draw order per view mode (docs/engine.md §14.5): layer, then `z_index` (a sub-layer, as in Godot), then
  side: submission order; topdown: y; oblique: y, z; iso: x+y, z; ties by submission order. Tiles and
  ground rects draw under upright items of the same layer/z_index, shadows between.
- One draw call per atlas page: images, glyphs, alpha and additive blending share batches
  (premultiplied colors; additive = alpha 0).
- Pixel-art styles (`pixel`) draw the world into a reference-size target and scale it up with nearest
  filtering; the UI is drawn on top at full resolution. Other styles draw directly (linear filtering).
- Clip rects (scissor) per batch: scroll lists and clipped panels cut text, rects and images exactly.
- Lighting2D: after the world pass, one quad multiplies the frame by the light level (ambient darkness,
  up to 64 radial Light2Ds with falloff and flicker, vignette); `emissive` items draw after it. The UI is
  never darkened.
- The UI pass covers the whole framebuffer (letterbox bars included) for the platform's controls.
- SVG: nanosvg gaps are patched before parsing (unitless bounding-box gradients, `<use>`, rect fills with
  `<pattern>`), and `<text>` is drawn with the bundled fonts (no transforms on text).
- Fonts: characters a bundled font lacks come from a DejaVu symbol subset (arrows, shapes, ★ ♥ ▶ ...);
  the pixel theme font is DejaVu Sans Mono Bold (engine-web's effective font, see docs/engine.md §14.10).
- Not supported yet: WebP images (clear error, drawn as a solid rect; no package uses WebP), emoji and
  scripts beyond the subsets, MSDF text (outlines are 8 offset copies), per-tile elevation inside one tile map (E7; today one raised TileMap2D per height level), render
  targets and post effects.
- 2.5D (E7): in topdown/oblique/iso a Sprite2D with `height` is a block (top face + shaded front face; two
  side faces in iso), a TileMap2D with `height` draws one block per tile. Blocks sort by footprint center;
  whatever stands on a block sorts right after it (`render/sort.h`). Drop shadows lie on the ground under
  the item: physics `ground_below()` gives the top of the block it is over.

## Tests

`dreamsy-tests` is one doctest binary. Besides the engine unit tests (`tests/*_test.cpp`) it holds the
ports of engine-web's test suite (2026-11-18): `tests/kits/<kit>_test.cpp` (every kit: behavior, battle
math, economy, content vs schemas, example games, balance sims) and `tests/api/*_test.cpp` (Luau API
behavior, plus `games_test.cpp`: every game in `games/` runs 300 ticks of scripted input with zero script
errors). Shared helpers are in `tests/kits/kit_test.h`: `start()` (a game from files), `run_luau()` (run a
Luau snippet inside a package and read what it saved as JSON, like the kits' TS harnesses), `simulate()`
(playtest timelines), a JSON view `J`, `schema_check()`, and `budget()` (the 250 ms per-call CPU budget,
x8 under AddressSanitizer).

doctest suites: `kit <kit>`, `api <name>`, plus two tags:
- `slow`: balance sims, generator sweeps and long end-to-end runs (minutes under ASan). ctest runs them as
  their own entry, `dreamsy-tests-slow` (label `slow`); the `debug` and `headless` test presets skip it,
  `release` and `web` run it. Under ASan: `ctest --test-dir build/debug -L slow` or
  `build/debug/dreamsy-tests -ts=slow`.
- `engine-gap`: ported tests the C++ engine doesn't pass yet (none today); excluded from ctest.

```sh
build/debug/dreamsy-tests -ts="kit rpg"                 # one kit (its slow tests: -ts=slow -tc="rpg*")
cmake --preset debug -DDREAMSY_TESTS_ONLY="rpg;games"    # build only some tests/{kits,api} files
```

## Rules

- No `throw`/`try` in engine code; errors are `Result`/`Status` (§14.2).
- No raw `new`/`delete`; reference long-lived objects by `Id<T>`, never by pointer across frames.
- Simulation code never reads the clock or OS randomness; use `Rng` and the tick counter (§14.4).
- Builds use `-ffp-contract=off -fno-fast-math` everywhere. The physics golden-hash test fails if
  determinism breaks on any platform.

## Performance

`dreamsy-bench` times one frame's CPU work per stage (`step`, `draw`, `sort`, `geometry`; no GPU):

```sh
./build/release/dreamsy-bench                          # tests/bench/stress: 5,000 animated sprites + 200 bodies
./build/release/dreamsy-bench tests/bench/scripted     # 2,000 entities each running onTick + a timer
./build/release/dreamsy-bench ../games/showcase/cloud-hopper --json
node build/web/dreamsy-bench.js                        # the same in WASM (web preset)
```

## Checks beyond unit tests

```sh
python3 tests/conformance/conformance.py              # needs `pnpm install` (format/)
python3 tests/conformance/run_compare.py               # scripts + physics scenarios (uses build/release if present)
python3 tests/conformance/run_compare.py --all         # every game in games/examples and games/showcase
python3 fuzz/make_corpus.py /tmp/corpus build/debug/dreamsy-tool
DREAMSY_FUZZ_SAVE=/tmp/last.bin ./build/debug/fuzz-package-replay --mutate 1000 /tmp/corpus/package
```

After a fuzz crash, `/tmp/last.bin` holds the input that caused it.
