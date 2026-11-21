# Dreamsy — Generation (the harness)

Part of the Dreamsy spec. Section numbers are shared across the spec files; [DREAMSY.MD](../DREAMSY.MD) maps each
section to its file.

## 5. Generation

### 5.1 Harness

- Runs in the cloud. Input: user prompt, reference images, current package version, user
  selection (e.g. tapped entity IDs). Output: validated diff-op batches.
- **Opus 5.5** orchestrates: game design, scene construction, all Luau logic, asset requests.
- **Smaller models** handle cheap, frequent work: small edits, naming, metadata, moderation pre-checks.
- Loop: plan → emit ops → validate (schema, typecheck, headless boot test, scripted smoke play)
  → fix → commit version → push to client for hot reload.
- **Playtests run on the C++ engine (2026-11-18):** the builder's `playtest` tool and the final gate (8 s smoke
  run, zero script errors) use `dreamsy-tool playtest <pkg> --timeline <json>` (headless, `harness/src/engine/playtest.ts`):
  timeline steps `{t, hold?, release?, press?, tap?: [x,y], swipe?: {from,to,seconds?}}` in game time, taps through
  `Host::tap_screen`; the JSON report has script errors (script/line/count), last logs, toasts, entity count,
  sfx counts + music, the avatar's position per second, dialog lines shown (auto-advanced) and the one open at
  the end, final save data, the scenes visited, and the controls the game read. Options: `--seconds --scene
  --params --save --clock --auto-dialog --budget --seed` (start mid-game). Screenshots come from the C++ web
  player in headless Chromium (`harness/src/engine/browser.ts`: Playwright `page.route`, no server, `?manual=1` +
  `window.__dreamsy.step`, `?scene=&params=&save=`), at game times, so 2.5D games are seen as players see them.
  Kit balance simulators (`kits/<kit>/harness.ts`) run Luau through `dreamsy-tool eval`.
- **OPEN:** harness language. Leaning TypeScript (Claude Agent SDK; shares types with web app).

### 5.2 Assets

Opus directs, specialized models make. Opus does not hand-build meshes except simple/procedural ones.

- **2D** (sprites, textures, UI, icons): Gemini image model, with style locked per game from
  the reference images.
- **3D**: Gemini generates reference views → image-to-3D (Hunyuan3D / Trellis self-hosted,
  or Tripo / Meshy API) → cleanup (decimate, UV, compress) → glTF. Characters → auto-rig.
- Audio: **OPEN**.
- Every asset gets `.meta.toml` provenance. Assets are content-addressed and reusable across games.

### 5.3 Cost

Every creator generating is the business risk. Per-user generation quota, small models for
iteration, Opus for builds and hard logic, aggressive caching of assets and plans.

### 5.4 MCP server: bring your own model (2026-11-18)

`harness/src/mcp/server.ts` exposes the engine to any MCP client (Claude Code, Claude Desktop, Cursor, ...). The
client's model is the builder, on the user's own subscription or key; the server holds no model key and runs locally.
- Tools: `dreamsy_options` (kits, styles, packs) · `create_game` (from a kit template or empty) · `dreamsy_guide`
  (the harness system prompt for that game: format, ops, catalog, Luau API, style, pack art, kit docs) ·
  `game_outline` · `read_files` · `get_entities` · `apply_ops` · `draw_sprite` · `playtest` (headless run +
  screenshots) · `check_game` (the final gate) · `export_game` (`.dpk`). Prompt: `build_game`.
- `read_files`, `get_entities`, `apply_ops` and `draw_sprite` are the agent's own tool definitions
  (`agent/tools/`), so the harness and MCP clients edit games the same way.
- Games are folders under `DREAMSY_GAMES_DIR` (default: the server's working directory); paths outside it are refused.
- Not exposed: `build_content` and `generate_image` (they need our model keys), the sprite library and memory.
- The tools live in `mcp/tools.ts` (`createServer(options)`); `mcp/server.ts` is the stdio entry (`npx dreamsy-mcp`).

**Hosted (2026-11-19): `https://dreamsy.dev/mcp`.** `mcp/http.ts` serves the same tools over Streamable HTTP for
clients that can't start a local process (ChatGPT, Claude.ai, phones). It runs as its own process on the site's Fly
machine (always on: ChatGPT gives up during a cold start); Next.js rewrites `/mcp` to it. No sign-in:
- Stateless (2026-11-20): ChatGPT opens a new connection for every tool call, so nothing lives in an MCP session.
  `create_game` returns an unguessable game id (`lantern-0k3f9x2m1q`); every other tool takes it, from any connection.
  Whoever has the id has the game. Each game has its own anonymous cloud save key (`/data/mcp/meta/<id>.json`), so
  `export_game` saves to the cloud and `publish_game` returns the `dreamsy.dev/play` link. The process never sees the
  editor's `DREAMSY_SAVE_KEY`.
- ChatGPT probes `server/discover` (MCP 2026-07-28) first; the server answers with a legacy 400 so it falls back to
  `initialize`. ChatGPT also needs tool annotations and rejects tuple (array-form `items`) schemas.
- Published games are final (2026-11-20): `apply_ops`, `draw_sprite` and `publish_game` refuse them; `remix_game`
  copies one into a new editable game id.
- Limits: 20 new games per IP per day, 8 requests in flight, 8 MB per request (413 closes the connection); games
  unused for 24 h are deleted. Cloud uploads also hit the worker caps, and since they all come from the Fly machine's
  IP, the worker's per-IP daily upload cap is a global cap for hosted MCP.
- `test/mcp-http.test.ts` drives it like ChatGPT (a new connection per call) against a fake cloud.

### 5.5 Site editor: builds on the user's key (2026-11-19)

The website (`apps/site`, Next.js) has an editor at `/editor` that builds games without `services/api`:

- Games are folders in `games/mine` (or `DREAMSY_GAMES_DIR`); editor state (build history, versions as `.dpk`, thumbnail)
  lives in `<games>/.editor/<id>/`.
- Published games are final (2026-11-20): the hosted editor is shared and has no accounts, so once a game is published
  its build, file, rename, restore and publish routes answer 409 and the workspace offers Remix (a new editable copy
  through the library's remix). The cloud worker enforces it too: a game publishes once. The dashboard opens on the
  library, where a game's picture and its Play button open `/play/<slug>`.
- A build is one `harness/src/cli/editor.ts` child process started by a Next route handler. Input `{ dir, prompt, images }`
  on stdin; output is JSON lines (`event` per progress event, then one `result`); on success the package replaces the
  folder's files. The model keys come only from `DREAMSY_EDITOR_{ANTHROPIC,OPENAI,GEMINI}_KEY` in the child's environment.
- Keys are stored per browser in httpOnly, SameSite=Strict cookies; the server reads them to start a build and never writes
  them to disk. Every successful build is snapshotted with `dreamsy-tool pack`, and a version can be restored with
  `dreamsy-tool unpack`. The preview is the C++ web player (`engine/build/web`) served at `/player/` and pointed at
  `/api/games/<id>/pack` with `?pkg=`.

Cloud save and publishing (`services/cloud`, Cloudflare Worker + R2 + D1, 2026-11-19): games are saved per browser with a
random save key (no accounts), every version is a content-addressed `.dpk` in R2, and publishing gives a public slug with
an optional author name and link (public) and email (private). See `services/cloud/README.md`. Builds upload by
default: `harness/src/cloud.ts` saves each successful build (editor and `local`) when `DREAMSY_CLOUD_URL` and a save key
are set; a failed upload never fails the build (event `cloud.failed`, the local copy stays). The editor, the MCP server
(`export_game` saves, `publish_game` publishes) and the CLI share one save key per computer in `~/.dreamsy/config.json`
and map local folders to cloud games in `~/.dreamsy/games.json`. The Worker also caps each network at 300 MB of new data
a day and each save key at 2 GB, on top of the global 500 GB / 3M writes budget.

Music (2026-11-19): the builder calls `compose_music` (`harness/src/agent/tools/music.ts`), which asks a smaller model (Haiku)
for several original chiptune tracks in `data/music/*.json` (docs/format.md, LUAU_API.md "Music"), one per role (title, level,
battle/boss, victory and game-over jingles), validates each with `parseMusic`, and retries once with the errors. Asking to
change the music reworks the same ids. The MCP guide carries the same composer guidance for clients without a composer. The
site editor plays songs and sound effects with a Web Audio port of the original synth (`apps/site/lib/audio.ts`); the C++
engine does not play audio yet.
