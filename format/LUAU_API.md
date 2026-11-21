# Dreamsy Luau API — v0 contract

The scripting surface every game script may use. The harness teaches this to Claude verbatim;
the engine (`engine/src/script`: the prelude `prelude.luau` + the C++ host) must implement exactly this. Change both together.

Luau rules: `--!strict` recommended. Sandboxed — no `io`, `os` (except `os.clock`), `debug`,
`loadstring`, `getfenv`/`setfenv`, filesystem or network. A script's top-level code and the modules it loads
share a 2,000 ms budget by default, and each callback gets 250 ms. Harness balance sims can raise the callback
budget with `Game.load(pkg, { budgetMs })` or `simulate(..., { budgetMs })`, which also raises the load budget
if `budgetMs` exceeds it.
Reserved words can't be field or method names with dot syntax: `goto` (Luau), plus the Lua keywords
(`end`, `then`, `local`, `function`, `repeat`, …) — use `t["goto"]` or pick another name (`goTo`, `moveTo`).

## Script kinds

| Location | Runs on | When |
|---|---|---|
| `scripts/client/*.luau` attached to an entity | each player's device | once per entity, at spawn; `script.entity` = that entity |
| `scripts/server/*.luau` attached to an entity | the server (or locally when `server.mode = "none"`) | same |
| `scripts/client|server/*.luau` not attached | client / server | once at game start; `script.entity` is nil; **keeps running across `loadScene`** (the session) |
| `scripts/shared/*.luau` | wherever required | modules only: `return` a table |

`require("../shared/config")` — paths are relative to the requiring script, no extension.
Engine kits (`dreamsy.toml` `kits = ["rpg"]`): `require("@kit/rpg")`, `require("@kit/rpg/battle")` — see each kit's KIT.md.
A kit's dependencies (its `kit.json` `"requires"`, e.g. rpg → core, progression) are enabled automatically, so a game listing
only `"rpg"` may also `require("@kit/core")`. Kit module top-level code runs as session code (its `onTick`/timers survive scenes).
Modules load once per game and survive scene changes, so a shared module table is the easy way
to carry state (score, lives, unlocked levels) between scenes.
In single-player (`server.mode = "none"`) client and server scripts run in the same process.

## Screen, camera, time (2D)

- Reference screen from `dreamsy.toml` `orientation`: **portrait = 720×1280**, **landscape = 1280×720** (absent = landscape).
  Every phone shows exactly this area (letterboxed with the theme background), so lay out levels and HUDs for it.
  World and UI share units: at Camera2D zoom 1, one world pixel = one UI pixel.
- Portrait: the bottom ~300 px is where thumbs and the on-screen joystick/buttons are (bottom-left stick,
  bottom-right buttons) — keep important action and HUD out of the bottom corners; put HUD at the top.
- Top-level UI anchors stay inside the phone's safe area (notch, home indicator) automatically.
  The engine's pause button sits in the top-right corner: keep ~100×100 px there free of HUD.
- Camera2D `position` (or its `follow` target) is the **center** of the view. With no camera, the view's top-left is (0,0).
- Keep HUD/menus clear of the play area (anchor them to edges; don't cover the level).
- Touch controls appear only for what scripts read: `input.axis("move_…")` → joystick, `input.pressed/held("jump"|"action"|"action2")`
  → A/B/C buttons. Tap-only games (pointer / `tapped` / UIButton) show no controls.
- `screen.size()` → the reference size as a Vec2 (720×1280 / 1280×720); `camera.viewSize()` → visible world size
  (reference size / zoom) — use these instead of hard-coding 720/1280 in kits.
- `onTick` runs at a fixed **60 Hz**. `random()` → float in [0,1), `random(max)` → [0,max), `random(min,max)` → [min,max); `math.random` is seeded too.
- `input.axis("move_y")` is +1 for **down** (screen direction). `input.pointer()` is in UI pixels.
- Area `bodyEntered` reports bodies only. Hidden bodies (`visible = false`) don't collide.
- Using a destroyed entity raises "entity was destroyed". Audio is recorded but not played yet.

## Globals

```luau
script.entity : Entity?                       -- owning entity (attached scripts)
entity(id: string) : Entity?                  -- by id (current scene)
entitiesTagged(tag: string) : { Entity }
spawn(type: string, props: {[string]: any}?, parent: Entity?) : Entity   -- runtime entity, auto id
onTick(fn: (dt: number) -> ()) : Connection  -- every tick (60 Hz); :disconnect() stops it (also from inside fn)
after(seconds: number, fn: () -> ()) : Timer  -- one-shot timer; Timer:cancel() before it fires = never fires
every(seconds: number, fn: () -> ()) : Timer  -- repeating; Timer:cancel()
screen.size() : Vec2                          -- reference screen (portrait 720×1280 / landscape 1280×720)
random(a: number?, b: number?) : number       -- [0,1) | random(max): [0,max) | random(min,max): [min,max); seeded, deterministic on server
time() : number                               -- game seconds since start (scaled by game.timeScale)
clock.now() : number                          -- wall clock, unix seconds (daily rewards, offline progress); fixed in playtests
isEntity(v: any) : boolean                    -- true for Entity proxies (also destroyed ones); never throws
print(...)                                    -- goes to dev console / harness logs
vec2(x, y) : Vec2      vec3(x, y, z) : Vec3   -- + - * / , .x .y .z, :length(), :normalized()
color(hex: string) : Color
```

## Package data reader

Read the game's own files (content-driven games, kits). Read-only, cached; `.json` → tables, `.txt` / `.luau` / `.md` → string.

```luau
data.list(dir: string) : { string }     -- sorted package paths under dir, recursive: data.list("data/creatures")
data.read(path: string) : any           -- data.read("data/creatures/fox.json") → table (a fresh copy each call)
data.exists(path: string) : boolean
-- files.list / files.read / files.exists are the same functions
```

Paths are package paths (`data/…`, `scenes/…`); `..` and absolute paths are errors. Kits read their content
(`data/<area>/*.json`, `data/rooms/*.txt`, …) through this reader — don't generate `scripts/shared/*_content.luau` copies.

## Time scale & session code

```luau
game.timeScale = 0.3     -- 0..4 (default 1): slow-mo; 0 = hit-stop. Scales onTick dt, time(), timers, tweens, physics, anims, particles.
                         -- Dialog typing, camera shake decay and the pause menu run in real time.
game.session(fn)         -- run fn as session code: onTick/every/after/:connect created inside survive scene changes and the
                         -- death of the calling script's entity (kits use this for global systems created from entity scripts)
```

Ownership rule: callbacks belong to the script that registered them (attached script → dies with its entity / scene;
unattached script, module top level, or `game.session` → lives for the whole game). Kit functions called from an entity
script register under that entity, so long-lived kit systems should wrap registration in `game.session`.

## Scenes

A game can have many `scenes/*.scene.json` (title, levels, win screen). Entity ids stay unique across all of them.

```luau
loadScene(path: string, params: {[string]: any}?)   -- e.g. loadScene("scenes/level_2.scene.json", { level = 2 })
sceneParams() : {[string]: any}                      -- params of the current scene ({} if none)
currentScene() : string                              -- "scenes/level_1.scene.json"
game.sceneLoaded:connect(fn(path: string))           -- after a scene's attached scripts have started
```

- The switch happens at the **end of the current tick**; code after `loadScene` still runs in the old scene.
  If called several times in one tick, the last call wins.
- Teardown: every entity of the old scene, every attached script instance (its `onTick`s, timers and
  connections) and all physics. **Unattached scripts keep running** (they are the session); their connections
  to old-scene entities are dropped, their `onTick`/timers/`game.sceneLoaded` connections stay.
- New scene: entities are created, attached scripts run (they can read `sceneParams()` at top level), then
  `game.sceneLoaded` fires. It also fires once for the entry scene after all scripts have started.
- Nothing else carries over automatically: keep state in session-script locals or a shared module.

## Tile maps (TileMap2D)

A level as a grid of characters instead of hundreds of entities. Cells are **0-based**: col 0 is the left
column, row 0 the top row; the map's `position` is the top-left corner of cell (0, 0).

```luau
map:tileAt(worldPos: Vec2) : string?           -- tile character, nil if empty or outside the map
map:tagsAt(worldPos: Vec2) : { string }        -- the tile's tags ({} if none)
map:getTile(col: number, row: number) : string?
map:setTile(col: number, row: number, char: string?)   -- nil or "." clears; char must be in the tileset
map:worldToCell(worldPos: Vec2) : (number, number)     -- col, row (may be outside the map)
map:cellToWorld(col: number, row: number) : Vec2       -- world position of the cell's center
map:isSolid(col: number, row: number) : boolean         -- false outside the map
map:tileDef(char: string) : {char, solid, tags, sprite?, color?}?   -- the tileset entry
map.tileTapped:connect(fn(col: number, row: number, char: string?))  -- tap/click on the map (char nil = empty cell); `tapped` fires too
map.cols, map.rows : number                    -- read-only grid size
map.tileTouched:connect(fn(other: Entity, char: string, col: number, row: number))
```

- `tileTouched` fires when a moving body (dynamic/kinematic Body2D) **starts** touching a tile that is solid
  or has tags: solid tiles count when the body rests against them, non-solid tagged tiles (spikes, coins,
  goals) when the body overlaps them. It fires again only after the body leaves that cell and comes back.
- `setTile` updates drawing and collision immediately (e.g. breaking a brick, collecting a coin tile).
- Bodies get one `bodyEntered(map)` / `bodyExited(map)` per map, not per tile.

## Height & 2.5D views

The world is always 2D (`x`, `y` on the ground, +y down) plus a **height** `z` (+ up). `dreamsy.toml`
`view` picks how it is drawn (a scene file may override it with a top-level `"view"`):

| `view` | Looks like | Draw order (inside a z_index) | Planar gravity | For |
|---|---|---|---|---|
| `"side"` (default) | x right, y down | tree order | yes (980 px/s² down) | platformers, runners |
| `"topdown"` | straight down; z lifts things up the screen | y | none | top-down RPGs, shooters |
| `"oblique"` | top-down at an angle: z draws upward, blocks show their front | y, then z | none | Zelda-likes, beat-'em-ups, 2.5D platformers |
| `"iso"` | 2:1 isometric diamond: world (x, y) draws at (x − y, (x + y) / 2 − z) | x + y, then z | none | city builders, tactics, iso adventures |

Props (catalog): `z` (height above the ground, px, relative to the parent like `position`) and `height` (vertical
extent, px) on Node2D, Sprite2D, Body2D, Area2D, Text2D and TileMap2D; Body2D also has `z_velocity` (px/s, + up) and
`z_gravity_scale` (1). Sprite2D `anchor = "bottom"` stands an image on its position (its feet).

- **Jumping.** Dynamic/kinematic Body2Ds fall with z gravity (980 px/s² × `z_gravity_scale`) and land on the ground.
  Jump with `if input.pressed("jump") and me.onGround then me.z_velocity = 420 end` (420 → ~90 px high, 0.86 s in the air).
  Move with `me.velocity` as usual (x, y on the ground); z is separate.
- **Platforms / blocks.** A static (or kinematic) Body2D with a `height` is a block: things only collide with it while
  their z ranges overlap (`a.z < b.z + b.height` and `b.z < a.z + a.height`), so a body walks into its side like a wall,
  and once it jumps higher than the top it passes over and **lands on top** (z = block z + height). A body stands on a
  block while its **center** is over the block's footprint; walking off the edge (a ledge) makes it fall. Things with
  `height = 0` are flat and collide with everything, as in plain 2D — give moving bodies a height (e.g. 24–32).
- **Raised terrain.** A TileMap2D with a `height` makes every solid tile a block of that height (one map per level of
  height: e.g. ground map `height = 0` with `collision = false`, a `height = 32` map for cliffs/walls you can jump onto,
  a `height = 64` map above it). Area2D `z`/`height` work the same way: a coin floating at `z = 40` is only collected
  by a jump.
- **Reading it.** `e.onGround` (Body2D, read-only): resting on the floor or a block top, not rising. `e.groundZ` (Body2D,
  read-only): height of the ground under its center (a block's top, else 0). `e.z` / `e.z_velocity` read and write.
  Falling to the floor (`e.z == 0 and e.onGround`) works as "fell in the water/lava" in games with a deadly floor.
- **Drawing.** In topdown/oblique/iso a **Sprite2D with `height` > 0 is drawn as a solid block** of that height over its
  footprint (`size`, centered on its position): top face = its texture or color, sides shaded darker — give a platform's
  Body2D a Sprite2D child with the same `size` and `height`. Sprites with height 0 are upright billboards: characters,
  trees, coins (use `anchor = "bottom"`). Raised things get a **drop shadow** on the ground (or block top) below them.
  Depth sorting is automatic (table above); something standing on a block draws after it.
- **Floors** are TileMap2Ds (drawn flat on the ground; in iso as diamonds), or ground art with a lower `z_index`
  (e.g. -10) so everything standing on it draws on top. A full-screen floor Sprite2D at the same z_index would be
  depth-sorted like a standing object.
- **Laying out levels.** Positions are always ground coordinates. topdown/oblique: what you see is (x, y − z) — design
  on a grid, rows further down the screen are closer to the viewer; a block of footprint w×d and height h covers the
  screen from y − d/2 − h to y + d/2. iso: lay the level out on a square grid in world x, y (e.g. tile_size 32); +x
  runs down-right and +y down-left on screen, so the camera and HUD math go through `camera.worldToScreen`.
  The camera follows ground positions (Camera2D `follow` works as usual).
- **Controls in iso.** The joystick is in screen directions; turn it into a world direction with
  `camera.screenDirToWorld(vec2(input.axis("move_x"), input.axis("move_y")))` (unchanged in the other views).
  `camera.screenToWorld` / `worldToScreen` (and world taps) go through the view's projection on the ground (z = 0).
  `camera.view()` returns the scene's view mode.

```luau
-- oblique hero: move on the ground, jump onto blocks
local me = script.entity
onTick(function(dt)
	local dir = camera.screenDirToWorld(vec2(input.axis("move_x"), input.axis("move_y")))
	me.velocity = dir * 160
	if input.pressed("jump") and me.onGround then me.z_velocity = 420; sfx.play("jump") end
end)
```

The engine simulates heights exactly and draws every view (heights, blocks, drop shadows, depth order), in the
player and in playtest screenshots.

## Sprite animation

Frame lists live in `data/anims/<name>.json` and attach to a Sprite2D with `anims`:

```json
{ "idle": { "frames": ["asset:fox_idle_1", "asset:fox_idle_2"], "fps": 4 },
  "run":  { "frames": ["asset:fox_run_1", "asset:fox_run_2", "asset:fox_run_3"], "fps": 10 },
  "hurt": { "frames": ["asset:fox_hurt"], "fps": 8, "loop": false, "next": "idle" } }
```

`frames` (1..128 `"asset:<id>"` images, all the same size), `fps` 0.5..60 (10), `loop` (true), `next` (non-loop: play this when done).
Sprite2D props: `anims = "data/anims/fox.json"`, `anim = "idle"` (plays at spawn), `auto_flip = true` (face the direction of
horizontal movement — the sprite's Body2D parent's velocity, else its own motion; draw art facing **right**), `flip_h`, `flip_v`.

```luau
e:play(name: string, restart: boolean?)  -- on a Sprite2D with anims, or on any entity: its first descendant sprite with anims
                                          -- (call every tick freely: same animation keeps playing; restart = true starts over)
e:stop()                                  -- freeze on the current frame
e.anim : string                           -- current animation ("" = none)
e.animFinished:connect(fn(name: string))  -- non-looping animation reached its last frame (fires on the sprite and on the entity play() was called on)
```

## Tweens

```luau
tween(e: Entity, props: {[string]: any}, seconds: number, easing: string?) : Tween
tween(e, props, seconds, { easing: string?, delay: number?, loop: boolean?, yoyo: boolean? }) : Tween
Tween:cancel()                       -- stops where it is (done does not fire)
Tween.done:connect(fn())             -- reached the end (never for loop = true)
```

- Any number / Vec2 / Color prop: `position`, `scale`, `rotation`, `modulate`, `zoom`, `offset`, `size`, `color`, `font_size`, …
  e.g. `tween(coin, { position = coin.position + vec2(0, -40), modulate = color("#ffffff00") }, 0.4, "quad_out")`.
- Easings: `linear`, `quad_in|out|in_out` (default `quad_out`), `cubic_*`, `sine_*`, `expo_in|out`, `back_in|out|in_out`,
  `elastic_out`, `bounce_out`. `yoyo` goes there and back (2× seconds); `loop` repeats forever (with `yoyo`: ping-pong — bobbing, pulsing).
- A new tween on the same entity+prop replaces the old one's control of that prop. Tweens die with their entity.
- Tweening a Body2D's `position` teleports it each tick (use for kinematic platforms / UI, not dynamic bodies).

## Particles (Particles2D)

A Particles2D entity emits `rate` particles/s while `emitting` (set `emitting = false` for bursts only). Particles live in world
space (a moving emitter leaves a trail). Props: `rate`, `lifetime` (+`lifetime_random`), `direction` (deg, 0 = right, -90 = up),
`spread` (deg, 360 = all around), `speed` (+`speed_random`), `gravity` (Vec2 px/s²), `damping`, `start_color` → `end_color`,
`start_size` → `end_size` (px), `shape` (`"square"`|`"circle"`), `texture` (optional image), `emission_size` (Vec2 area), `max_particles`.

```luau
e:burst(n: number?)         -- emit n (default 16) at once: explosions, coin sparkle, dust on landing
e:clearParticles()
e.particleCount : number    -- read-only
```

Recipes: dust puff `{emitting=false, speed=60, spread=160, direction=-90, lifetime=0.4, start_size=6, end_size=0, gravity=vec2(0,200)}` +
`burst(8)`; sparkle `{shape="circle", start_color=color("#ffe066"), end_color=color("#ffffff00"), speed=90, spread=360}`;
trail: child of the player, `rate=40, speed=10, lifetime=0.5`.

## Camera (2D)

Camera2D props: `follow` (entity), `zoom`, `smoothing` (0 = locked to the target, 0.5 soft, 0.9 floaty), `deadzone` (Vec2 box the
target moves in freely), `limit_min` / `limit_max` (world rect the view never leaves — set them to the level bounds).

```luau
camera.shake(intensity: number, seconds: number?)        -- world px amplitude, decays (default 0.3 s); stronger replaces weaker
camera.zoomTo(zoom: number, seconds: number?, easing: string?) : Tween   -- tween of the current Camera2D's zoom
camera.current() : Entity?                               -- the current Camera2D
camera.center() : Vec2                                   -- view center in world px (after smoothing/limits, without shake)
camera.snap()                                            -- jump to the target now (after teleporting the player)
camera.viewSize() : Vec2                                 -- visible world size: reference size / zoom
camera.screenToWorld(p: Vec2) : Vec2                     -- UI/screen px (e.g. input.pointer()) → world
camera.worldToScreen(p: Vec2) : Vec2
camera.view() : string                                   -- "side" | "topdown" | "oblique" | "iso" (this scene)
camera.screenDirToWorld(dir: Vec2) : Vec2                -- joystick direction → world direction (iso rotates it)
```

Shake and smoothing are deterministic (seeded) and identical in headless playtests.

## Lighting (dark scenes)

Add one `Lighting2D` to a scene to darken it, and `Light2D`s to light it up (torches, lamps, lanterns, glowing pickups):

- `Lighting2D`: `ambient` (color of unlit areas, e.g. `"#0b0d1a"` night blue, `"#000000"` cave), `darkness` 0..1
  (0.85; 1 = unlit areas are exactly `ambient`), `vignette` 0..1 (darker screen edges). Hide it (`visible = false`)
  or tween `darkness` to 0 for daylight. No Lighting2D = no darkness (Light2Ds then do nothing).
- `Light2D`: `radius` (px), `color`, `energy` (0..8; tween it to fade), `falloff` (1 linear, 2 soft, 4 tight core),
  `flicker` 0..1 + `flicker_speed` (torches 0.15, candles 0.3), `enabled`. Make it a **child of the player** for a
  light that follows (`spawn("Light2D", { radius = 220 }, player)`). Up to 64 lights are lit at once (nearest to the view).
- `emissive = true` on Sprite2D / Text2D / Particles2D draws it above the darkness at full brightness (eyes in the dark,
  lit windows, sparks, labels). UI is never darkened.
- `blend = "add"` on Sprite2D / Particles2D: additive (glows, fire, light shafts); also works without Lighting2D.
- Headless playtests keep lights as plain entities (props, tweens and parenting work; nothing is drawn).

```luau
-- night falls over 3 s, the player's lantern flickers on
tween(entity("night"), { darkness = 0.9 }, 3, "sine_in_out")
spawn("Light2D", { radius = 200, color = color("#ffd28a"), flicker = 0.15 }, entity("player"))
```

## Save data

Per-game persistent storage (device; the app may sync it to the player's account later). Survives `loadScene`, restarts and relaunches.

```luau
save.get(key: string, default: any?) : any     -- a copy; nil/default if unset
save.set(key: string, value: any)              -- nil removes; value = nil | boolean | number | string | plain tables of those
save.remove(key: string)
save.keys() : { string }
```

Max 64 KB (JSON) per game; larger saves are a script error. Not Vec2/Color/Entity (store `{x=…, y=…}`).
Typical: `best = math.max(save.get("best", 0), score); save.set("best", best)`, unlocked levels, settings-like choices.
Headless playtests start with empty save data (or preloaded `save` data, see "Playtests") and report the final table.

## Dialog (platform-styled, themed by [ui])

```luau
ui.dialog(lines: { string | {text: string, speaker: string?, portrait: string?} } | string, opts?) : Dialog
  -- opts: { speaker: string?, portrait: "asset:<id>"?, speed: number? (chars/s, 40; 0 = instant),
  --         position: "bottom" | "top" (bottom), block: boolean? (true: game input paused while open) }
ui.choice(prompt: string | {text, speaker?, portrait?}, options: { string }, fn: ((index: number) -> ())?, opts?) : Dialog
Dialog.done:connect(fn(index: number?))   -- after the last line (choice: 1-based option index)
Dialog:close()                            -- remove without firing done
ui.dialogOpen() : boolean
```

- Players advance with a tap anywhere, or jump/action; the first press finishes the typewriter, the next shows the next line.
  Choices: tap an option, or up/down + jump/action. Dialogs opened while one is showing queue up.
- `block = true` (default): scripts see no input while the dialog is open (no accidental jumps); the world keeps running.
- Dialogs close on `loadScene`. Headless playtests advance them with `press: ["jump"]` (or `autoDialog`) and report the lines shown.
- An open dialog always gets taps first — before any UIPanel/UIButton, including full-screen modal panels.

## Collision layers

Declare up to 16 names in `dreamsy.toml`: `[physics] layers = ["player", "enemy", "pickup", "world"]`. Body2D, Area2D and
TileMap2D get `layer` (the one layer it is on) and `mask` (comma-separated layers it collides with / detects), e.g. player
`layer = "player", mask = "world,pickup,enemy"`, enemies `layer = "enemy", mask = "world,player"` (enemies pass through each other),
coin Area2D `layer = "pickup", mask = "player"`. Unset = on every layer / collides with everything (the default).
Two things touch only if each one's mask includes the other's layer. Scripts may change `e.layer` / `e.mask` at runtime
(e.g. invincibility frames); unknown names are errors.

## Text style

Text2D, UIText (and UIButton `font`): `font` = `"default"` (the [ui] font) | `"pixel"` | `"rounded"` | `"bold"` | `"serif"`,
`outline` (color) + `outline_size` (px; 2-4 keeps text readable over busy art), `shadow` (color) + `shadow_offset` (Vec2).
UIText without outline/shadow keeps a subtle default shadow.

## Entity

Props from the catalog are plain fields: `e.position`, `e.text`, `e.modulate`, `e.visible`, …
(vec props read/write as `Vec2`/`Vec3`, colors as `Color` or `"#rrggbb"`, assets as `"asset:<id>"`).
Setting a prop to `nil` resets it to its catalog default (`e.modulate = nil` → white, `button.icon = nil` → no icon,
`cam.limit_min = nil` → no limit).

Sprite2D without a `texture` draws a solid rect of `size` in `modulate` (`color` is an alias: `spawn("Sprite2D",
{ size = vec2(64, 8), color = color("#44dd66") })` for health bars, walls, cards, grid cells).

Text2D `align` anchors the text to `position`: `"center"` (default) = centered on it, `"left"` = text starts at it,
`"right"` = text ends at it; always vertically centered on it. Text2D has no tap area — tap a Sprite2D behind it.

Taps on overlapping things: the topmost visible listener wins (higher `z_index`, then later in the tree). Stacked
TileMap2Ds: the topmost visible map that listens (`tapped`/`tileTapped`/`tappedAt`) and has the point inside its grid
wins, even on an empty cell of that map (`char` = nil).

```luau
e.id : string            e.type : string          e.name : string
e.parent : Entity?       e:children() : { Entity }
e:hasTag(tag) : boolean  e:addTag(tag)            e:removeTag(tag)
e:destroy()
e.destroyed : boolean    -- true after destroy / scene change (safe on dead entities; other props raise "entity was destroyed")
e:clone(parent: Entity?) : Entity
-- physics bodies (Body2D/Body3D)
e.velocity : Vec2|Vec3   e.onFloor : boolean (read-only)
e.onGround : boolean     e.groundZ : number      -- Body2D, read-only, 2.5D (see "Height & 2.5D views")
e:applyImpulse(v)
-- signals (connect returns a Connection with :disconnect())
e.bodyEntered:connect(fn(other: Entity))     -- Area*/Body*
e.bodyExited:connect(fn(other: Entity))
e.pressed:connect(fn())                      -- UIButton
e.tapped:connect(fn(player: Player?))        -- any visible entity, touch/click
e.tappedAt:connect(fn(localPos: Vec2))       -- where on it: (0,0) = top-left of its `size` box (TileMap2D: of cell (0,0)),
                                             -- in its own unscaled units — one board sprite instead of a hit sprite per cell:
                                             -- local col, row = math.floor(p.x / cell), math.floor(p.y / cell)
```

## Input (client)

Platform-mapped: on-screen joystick + buttons on phones, keyboard/gamepad elsewhere.

```luau
input.axis("move_x" | "move_y" | "look_x" | "look_y") : number   -- -1..1
input.pressed("jump" | "action" | "action2") : boolean           -- this frame
input.held("jump" | "action" | "action2") : boolean
input.pointer() : Vec2?                                            -- screen position while touching (and 1 tick after a quick tap)
input.pointerDown:connect(fn(pos: Vec2))   -- finger/mouse down anywhere (UI px), fired immediately — never missed between frames
input.pointerUp:connect(fn(pos: Vec2))
input.swipe:connect(fn(dir: "left"|"right"|"up"|"down", velocity: Vec2))   -- ≥ 40 px within 0.6 s; velocity in px/s
```

`input.axis("look_x" / "look_y")` (twin-stick aim): phones show a second stick bottom-right (the buttons move above it).
Use `pointerDown`/`swipe` for flick/drag games instead of polling `input.pointer()` in `onTick`.

## UI (client)

```luau
ui.toast(text: string, seconds: number?)
ui.showScreen(rootEntityId: string)  ui.hideScreen(rootEntityId: string)   -- UIPanel trees
ui.theme() : {primary, accent, background, text: string ("#rrggbb"), panel: string, font: string}   -- resolved [ui] theme
```

UI props (UIText / UIButton / UIPanel): `scale` (Vec2, visual scale around the center — tween it for pops/bounces; layout and hit
size are unchanged). UIPanel `input_transparent = true`: taps pass through the panel (its buttons still work) — HUD frames over the
play area; UIText is input-transparent by default (set `false` + `tapped` for tappable text). UIText `valign = "top" | "center" | "bottom"`.

UIPanel children use their own `anchor` + `offset` inside the panel — unless the panel has **`layout = "stack"`**: then its
visible UI children are placed top to bottom in tree order, `padding` px from the top and `gap` px apart (12); a child's
anchor only picks its column (left / center / right), `offset.x` nudges it and `offset.y` adds extra space above it. Hidden
children take no space; changing a child's size/visibility re-stacks the rest. UIPanel `scroll = true`: vertical scroll list
(content height = lowest child, so pair it with `layout = "stack"` for lists), clipped, drag/flick on touch, wheel on desktop;
buttons inside fire on release so drags scroll. Other UIPanel props: `radius` (corner px; -1 = from the [ui] panel style),
`clip = true` (clip children to the panel), `image` + `tint` (multiplies the image: recolor one panel image). UIButton `icon = "asset:<id>"` (+ `icon_size`, `icon_position = "left" | "top"`; `text = ""`
for icon-only). `input.pointer()` keeps updating while a finger is on UI.

## Playtests (harness tools)

`simulate(pkg, timeline, seconds, opts)` (Node, fast) and `playtestBrowser(pkg, timeline, shotTimes, opts)` (real renderer)
share the timeline: steps `{ t, hold?, release?, press?, tap?: [x, y], swipe?: { from, to, seconds? } }` in **game time**
(the browser page is stepped tick by tick, so taps and screenshots are reproducible however slow the machine).
Both start mid-game with `opts.scene` + `opts.sceneParams`, preloaded `opts.save` data and `opts.clock` (unix seconds);
`simulate` also takes `autoDialog = "first" | "last" | n` (advance dialogs, answer choices) and `budgetMs`.
`tools/screenshot.ts --scene … --params '{"level":3}' --save '{"coins":50}' --clock 1767225600 --tap x,y@sec`.

## Players & avatar

```luau
players.local : Player?                -- this device's player (`players["local"]` in code: `local` is a keyword); nil while spectating
players.all() : { Player }             -- everyone in the room, room order (offline: just players.local)
players.get(id: string) : Player?      players.onTeam("red" | "blue") : { Player }
players.fromEntity(e: Entity) : Player?   -- e or an ancestor is my avatar (tag "player" / replicated) or a remote avatar
players.joined:connect(fn(p: Player))   players.left:connect(fn(p: Player))
-- joined fires for every player at start (me included), then for late joiners; left when someone disconnects
Player.id : string    Player.name : string    Player.team : "red" | "blue" | nil (nil offline)
Player.isLocal : boolean    Player.isHost : boolean    Player.avatar : Entity?   -- remote: its puppet (see room.replicate)
```

## Networking (multiplayer)

Multiplayer games set `[players] max` (2 = 1v1, ≤ 8) and `[server] mode = "rooms"`. The engine owns the lobby (quick match
1v1, create room → 6-letter code + share link, join with code, ready/start, play solo), the red/blue team HUD and the winner
card — games never build that UI. **v1 model (relayed state sync):** every device runs the whole package (client *and*
server scripts) and simulates its own world; the room server relays avatar samples and events and owns membership, teams,
the host, shared state, scores and the match result. So: each player moves their own avatar; anything shared (pickups,
round timer, who scored) is decided on the **room host's** device (`room.isHost`) and announced with remote events.
Offline (single player, playtests, harness) the same API works: one local player, `room.active = false`, `room.isHost = true`,
events stay in-process, `room.finish` fires `room.ended` locally.

Remote events — declare them in a shared module so every script sees the same names (letters, digits, `_ . : -`):

```luau
local hit = remoteEvent("hit")             -- fire-and-forget
hit:fireServer(...)                        -- → the room host's onServer(player = sender, ...) (offline: in-process)
hit:fireClient(player, ...)                -- → that player's onClient
hit:fireAll(...)                           -- → every player's onClient, including this device
hit:onServer(fn(player, ...))   hit:onClient(fn(...))

local buy = remoteFunction("buy")          -- request/response
buy:invoke(...) : ...                      -- offline / on the host only in multiplayer v1 (use a remoteEvent + fireClient reply)
buy:onInvoke(fn(player, ...) -> ...)
```
Args: nil, booleans, numbers, strings, Vec2, Players and tables of those (≤ 4 KB JSON); Entities can't be sent (send a
position or a Player). Events are delivered at the start of the next tick, in order.

Rooms:

```luau
room.active : boolean        -- in a multiplayer room
room.code : string?          room.mode : "duel" | "party" | nil      room.max, room.min : number
room.isHost : boolean        room.host() : Player     room.match : number (rematches bump it)     room.seed : number
room.replicate(e: Entity, opts: { tint: boolean? }?)
  -- send this entity (my avatar) to the others at 15 Hz: position, rotation, flip_h, anim, visible, scene.
  -- Every other device shows a puppet: a visual copy of ITS OWN replicated avatar (same package) with scripts and
  -- physics stripped (Body2D/Area2D → Node2D), interpolated 100 ms behind, plus a name tag in the team color.
  -- tint (default true): plain white sprites become exactly the team color (#ff2a2a red / #2a6bff blue), other sprites
  -- get a team tint — on my avatar too. Puppets are hidden while their player is in another scene.
room.avatarSpawned:connect(fn(p: Player, puppet: Entity))   -- decorate / add an Area2D for hits
room.setState(key: string, value) ; room.state(key) : any ; room.stateChanged:connect(fn(key, value))   -- shared, last write wins
room.addScore(target: Player | "red" | "blue", n: number) ; room.score(target) : number ; room.scores() : {[string]: number}
room.scoreChanged:connect(fn(target: Player | "red" | "blue", total: number))   -- server-authoritative totals (shown in the HUD)
room.finish(winner: Player | "red" | "blue" | "draw")   -- end the match; the first call in the room wins, later ones are ignored
room.ended:connect(fn(r: { winner: Player?, team: "red"|"blue"|nil, draw: boolean, reason: "finish"|"forfeit", scores }))
room.teamColor(team) : Color
```
A 1v1 whose opponent leaves ends as a forfeit for the one who stayed. Rematch = a fresh game start with the same room.

**Spectators** (`room.isSpectator == true`, `room.spectators : number` = how many watch). Anyone can watch a room (lobby
or running, up to 16 per room) from a friend's presence or a `?room=<code>&spectate=1` link. A spectating device runs the
whole package too, but has **no local player**: `players.local == nil`, `room.isHost == false`, and `players.all()` lists
only the players. The engine guards the common cases so games written for players don't crash:
- entities tagged `"player"` in a scene stay hidden (no physics) and **their attached scripts don't run** — put avatar
  logic (`room.replicate`, movement, `players.local.team`) in the avatar's own script;
- `room.replicate(e)` hides `e`; `fireServer` / `fireClient` / `fireAll`, `room.setState`, `room.addScore` and
  `room.finish` do nothing (the players' devices decide everything); `remoteFunction:invoke` errors;
- puppets appear for every player; a Camera2D with `follow` follows the watched player's puppet (HUD name chips switch it).
Other scripts that read `players.local` must check it: `local me = players["local"]; if not me then return end`.

## Platform menu, theme, audio

**Platform menu (engine-owned, every game):** a pause button (top-right) opens Pause → Resume,
Restart, Leaderboard (games that use `leaderboard`), Settings (music / sfx volume, mute, vibration, touch-control size),
Controls help, Quit.
Games do NOT build their own pause/settings/volume UI. They theme it via `dreamsy.toml`:

```toml
[ui]
primary = "#5e7ed6"      # buttons, highlights
accent = "#e8c27a"       # selection, important text
background = "#2a3170"   # panel fill (alpha added by engine)
text = "#f2ece6"
panel = "pixel"          # "pixel" | "rounded" | "sharp" | "outlined"
font = "pixel"           # "pixel" | "rounded" | "bold" | "serif"
```

```luau
game.paused:connect(fn())      game.resumed:connect(fn())     -- e.g. pause your own timers
game.restartRequested:connect(fn())   -- default: reload the entry scene; connect to customize
settings.get("music") / settings.get("sfx") : number 0..1    -- read-only for games
```

**Sound effects** — `data/sfx/<id>.json` (id = lowercase_snake), synthesized (sfxr-style), tiny and editable.
Every key is optional; unknown keys are errors. Total length attack + sustain + decay must be > 0 and ≤ 5 s.

| key | range | default | meaning |
|---|---|---|---|
| `wave` | `"square"` `"saw"` `"triangle"` `"sine"` `"noise"` | `"square"` | noise = white noise low-passed at `freq` |
| `attack` | 0..2 s | 0 | fade in |
| `sustain` | 0..2 s | 0.1 | hold at full volume |
| `decay` | 0..3 s | 0.2 | fade out |
| `freq` | 20..8000 Hz | 440 | start pitch (noise: brightness) |
| `slide` | -4..4 octaves | 0 | pitch change over the whole sound (+ rises, − falls) |
| `vibrato` | 0..12 semitones | 0 | vibrato depth |
| `vibrato_speed` | 0..40 Hz | 6 | vibrato rate |
| `duty` | 0.05..0.95 | 0.5 | square pulse width (0.125 = thin) |
| `volume` | 0..1 | 0.5 | |

Examples: jump `{"wave":"square","duty":0.25,"freq":280,"slide":1.2,"sustain":0.02,"decay":0.16}`,
coin `{"freq":988,"slide":0.5,"sustain":0.05,"decay":0.22}`, hurt `{"wave":"saw","freq":420,"slide":-1.5,"decay":0.3}`,
explosion `{"wave":"noise","freq":1200,"slide":-2,"decay":0.6}`.
Typical set: jump, land, coin, hit, hurt, explode, powerup, select, confirm.

**Music** — `data/music/<id>.json`, a tiny chiptune tracker:
`{ "bpm": 120, "steps": 16, "channels": [ { "wave": "square", "volume": 0.4,
  "notes": ["C4", "-", "E4", "-", "G4", ".", "E4", "-", ...] } ], "loop": true }`
(`"-"` hold, `"."` rest; up to 4 channels; patterns may be chained with `"order"`).

- `bpm` 40..240 (default 120); one step = a 16th note. `steps` 1..256 per pattern (default 16 = one 4/4 bar).
- `channels`: 1..4 of `{ "wave", "volume" 0..1 (0.4), "duty" 0.05..0.95 (0.5), "notes" }`. `notes` is an array
  or one whitespace-separated string: `"C4 - E4 . G#3 Bb2 - ."`. Notes `C0`..`B8` with optional `#`/`b`;
  `"-"` holds the previous note one more step; `"."` is silence. Noise channels use the note as brightness
  (low = kick, high = hat).
- Each channel's notes are `steps × N` long: tokens `0..steps-1` are pattern 0, the next `steps` pattern 1, …
  `order` (optional) lists pattern indices to play, e.g. `[0, 0, 1, 2]`; default = every pattern once.
  Max song length 4096 steps. `loop` (default true).
- `music.play` with the song already playing does nothing (call it at the top of each level script);
  music keeps playing across `loadScene`. `opts.fade` / `music.stop(fade)` are seconds.
- `sfx.play` opts: `volume` 0..1 (1), `pitch` 0.1..8 multiplier (1). A missing id is a script error.
- In headless playtests nothing is audible; calls are recorded and reported (sfx id → count, music played).
- Pause menu: the simulation freezes (no ticks, timers or physics) until Resume.

```luau
sfx.play(id: string, opts: {volume: number?, pitch: number?}?)
music.play(id: string, opts: {fade: number?}?)   music.stop(fade: number?)
```

## Store (Stars)

Items are declared in `dreamsy.toml` `[[store]]` (`id`, `kind = "pass"|"consumable"`, `name`, `description`, `price` in Stars, optional `icon = "asset:<id>"`).
The platform shows the purchase sheet; scripts never handle money.

```luau
store.items() : { {id: string, kind: string, name: string, description: string, price: number, icon: string?} }
store.owns(itemId: string) : boolean          -- passes owned by the local player (cached, instant)
store.prompt(itemId: string)                  -- opens the platform purchase sheet; result arrives via store.purchased
store.purchased:connect(fn(itemId: string, purchaseId: string))
store.pending() : { {itemId: string, purchaseId: string} }   -- consumables bought but not yet granted
store.consume(purchaseId: string)             -- call AFTER granting a consumable (e.g. +50 gems); idempotent
```

Gate content with passes (`if store.owns("vip") then ... end`): extra levels, areas, skins, perks.
On start, grant any `store.pending()` consumables (a purchase may have finished while the game was closed).
Kids play Dreamsy: no random paid rewards (loot boxes), show the price clearly, never block core
progress behind a purchase.

## Leaderboards & ghosts

Every game with a clear finish or a score gets online leaderboards for free. The engine times runs in **game time**
(pause and `game.timeScale` respected), records a **ghost** of the player, submits, and shows the results card
(time, rank, PB badge, top 5, "Race the #1 ghost", "Play again" → `game.restartRequested` / entry scene). The pause
menu gets a **Leaderboard** entry (top 10). Games never build leaderboard UI or talk to servers.

```luau
leaderboard.start(board: string?, opts: { track: Entity? }?)   -- begin a timed run (board default "fastest"); calling again restarts it
leaderboard.finish(board: string?) : number?    -- stop, submit the time (+ ghost), show the results card; returns seconds
leaderboard.cancel(board: string?)              -- abandon without submitting (game over, back to menu); no board = all runs
leaderboard.elapsed(board: string?) : number    -- seconds on the running timer (0 if none) — for an in-game HUD clock
leaderboard.running(board: string?) : boolean
leaderboard.submit(board: string, value: number, opts: { order: "asc"|"desc" }?)  -- explicit score (default "desc" = higher is better), no ghost
leaderboard.raceGhost(which: "best" | "pb" | runId, board: string?)  -- spawn a translucent replay of that run's ghost now
leaderboard.result:connect(fn(rank: number, best: number, isPersonalBest: boolean))  -- after a submit resolves (next tick); best = the board record (#1 value)
```

- Timed games (race, level, speedrun, puzzle clear): `leaderboard.start("<board>")` when the run really begins (after any
  countdown), `leaderboard.finish("<board>")` at the goal, `leaderboard.cancel("<board>")` on game over / quit. Deaths with
  respawn keep the clock running. Platform Restart cancels running timers automatically.
- Score games (endless runner, arcade): `leaderboard.submit("score", score)` once at game over (not every frame).
- Boards: lowercase letters, digits and `_` (≤ 40 chars). One board per track/level: `"track_" .. id`, `"level_" .. n`. Times are
  lower-is-better ("asc"); `submit` defaults to higher-is-better ("desc").
- The ghost follows `opts.track` or the entity tagged `"player"` (its sprite's position, flip and animation frame, 10 Hz,
  ≤ 5 min), only in the scene where the run started. Ghost replays draw the player's sprite at 45% alpha with no
  collisions or scripts, starting when spawned; they disappear on scene change, run end, or when the replay ends.
  The results card's "Race the #1 ghost" restarts the game and spawns that ghost at your next `leaderboard.start`.
- A board with no runs, offline play and playtests all work: the engine falls back to a local in-memory leaderboard.

**Run ended** — tell the host app a run is over (it may show "what to play next"; e.g. a new player's welcome sheet).
`leaderboard.finish` / `leaderboard.submit` and a multiplayer match end (`room.ended`) already do this; call
`platform.runEnded` at game over / round end in games without a leaderboard. Never stops or changes the game.

```luau
platform.runEnded(reason: string?)   -- "gameover" (default) | "finish" | "quit"; other strings count as "gameover"
```

## Services (server, slice 2+)

```luau
data.player(p: Player) : {[string]: any}   -- persistent, shaped by data/schema.luau; :save()
data.world() : {[string]: any}
sound.play(assetId: string, at: Vec2|Vec3?)
```

## Not available

Arbitrary HTTP, file access, require of anything outside `scripts/` other than `@kit/` modules, threads beyond
`coroutine`, reading other players' private data on the client.
