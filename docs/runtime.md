# Dreamsy — Runtime and multiplayer

Part of the Dreamsy spec. Section numbers are shared across the spec files; [docs/README.md](README.md) maps each
section to its file.

## 4. Runtime & multiplayer

### 4.1 Scripting: Luau

- Luau 0.747 embedded in the C++ engine (native; interpreter-only on iOS and web), host API in `engine/src/script`. Contract: `format/LUAU_API.md`.
- Why Luau: sandboxed by design, fast, gradual types, and models are heavily trained on it.
- API is Dreamsy's own (not a Roblox clone), but follows the same proven shape:
  client / server / shared scripts, remote events + remote functions, replicated properties.
- Sandbox: no filesystem, no raw network, CPU + memory budgets per script, all I/O via services.

### 4.2 Server

The server is the headless engine (Linux build) running the same package with
`scripts/server` + `scripts/shared`. Clients never trust themselves.

Platform services exposed to server scripts:

| Service    | What                                                                  |
|------------|-----------------------------------------------------------------------|
| Rooms      | real-time sessions, matchmaking, join/leave, replication              |
| DataStore  | persistent per-player and per-world data, typed by `data/schema.luau` |
| Timers     | jobs that fire while players are offline ("build done in 4h")         |
| Async      | snapshot another player's world; run a deterministic sim against it   |
| Ledger     | server-only currency/inventory mutations (marketplace builds on this) |

`server.mode` picks the shape:
- `none` — single-player, runs locally.
- `rooms` — session games (obby, arena, party games).
- `persistent` — a shared world that outlives sessions.
- `async` — CoC-style: own base persistent, raids simulated against snapshots, replays.

Async/replay requires the deterministic path: fixed tick, seeded RNG from the platform,
fixed-point or carefully constrained float math in the sim layer. **OPEN:** how much of
physics we make deterministic vs. limiting async games to a sim-friendly subset.

### 4.3 Targets

> Superseded by §14.7. Since 2026-11-18 the app, share links, dev stack and deploy image use the C++ web player
> (`engine/apps/player`, embedding contract in `engine/apps/player/embed.js`); `engine-web` was removed. The table
> below is the history.

**Decision (2026-11-14):** web engine instead of a Godot fork (the fork was built, then deleted the same day). Games are Roblox-level at most
(sprite games, platformers, block games, clean racing), which web tech handles on phones; in
exchange we get one language across the stack, instant iteration, no native toolchain, play in
iPhone Safari with zero install, and trivial headless screenshots for the harness.

| Where | How |
|---|---|
| Web / iPhone Safari | `engine-web` directly |
| Mobile app | `engine-web` in a WebView (or expo-gl) inside the Expo app |
| Game servers | `engine-web/core` in Node (no renderer), server-authoritative |
| Harness playtest | headless Chromium: boot package, collect script errors, screenshot for Claude |

**2D first:** v1 engine implements the 2D path (orthographic renderer, rapier2d). 3D packages fail
with "not supported yet". The harness only builds 2D games until the 3D path ships.

### 4.4 Cloud topology

```
apps/mobile, apps/web ► services/api ◄── harness workers (claim jobs, upload versions)
                           │
                           └──► services/gameserver (orchestrator ─► headless engine fleet)
```

- **services/api** — ordinary backend, no engine. TypeScript (Hono) + Postgres + S3-compatible
  object storage. Owns users, games, immutable package versions, reference uploads, generation
  jobs (Postgres queue, leased to workers), event streams (SSE), quotas.
- **harness** — stateless workers that pull jobs from the API.
- **services/gameserver** — the room server (§4.5): Node + `ws`, one process, in-memory rooms. Prod: `gameserver`
  container on the same box; Caddy routes `api.dreamsy.dev/rooms*` (WebSocket) to it, everything else to the API.
  It owns the room protocol (`services/gameserver/src/protocol.ts`).
- **play.<domain>** — the C++ web player as static files (built in `deploy/Dockerfile`, published to Caddy by the
  compose `player` service).

### 4.5 Multiplayer v1 — rooms, 1v1 red vs blue, family play (2026-11-15)

Goal: "1v1 true red vs true blue, and a game can have many players, so I can share games with family".

**Decision: server-relayed state sync now, server-authoritative simulation later.** Every device runs the whole package
(client + server scripts, as single-player does today) and simulates its own world and avatar. The room server is
authoritative for everything *about* the match — membership, teams, host, shared state, score totals, the result — and
relays avatar samples (15 Hz) and remote events. Remote avatars are visual puppets interpolated 100 ms in the past.
Why not run the game on the server yet (decided with the TS engine, still true for E8): one VM runs client and server scripts
together, entity props have no replication layer, and phones need instant local movement (that means client prediction +
reconciliation for a server-simulated avatar). Relay gives zero-latency controls on phones, works for every existing game
and kit with a few lines (`room.replicate`, `room.finish`), and costs ~nothing on the server (no simulation). Trade-offs,
accepted for family/friends play: clients are trusted for their own position and claims (a modified client can cheat), no
physical interaction between players (puppets have no bodies), shared objects need a host decision (`room.isHost`).

- **Rooms**: 6-char codes (no 0/O/1/I/L); `party` rooms up to `[players] max` (≤ 8), teams balanced red/blue (players can
  switch in the lobby), host starts when everyone is ready (`[players] min`); late joiners drop into the running match.
  **Quick match**: a per-game queue pairs two players into a `duel` room (first red, second blue) that starts at once;
  rematch = both press Rematch. A duel ends as a forfeit when one side leaves. Host = first player, moves on leave.
- **Auth**: the API's HS256 user token in the first message (`{t:"hello", token, name?}`), verified with the same
  `JWT_SECRET`; the web player gets a guest token (`POST /v1/auth/guest`) when signed out, so share links work for family
  without accounts. Player ids are per connection (two tabs of one account = two players). Display name: typed in the
  lobby, else the API handle (`GET /v1/me`).
- **Limits**: 8 KB/message (socket closes > 16 KB), 40 msg/s sustained + 80 burst (excess dropped with `rate_limited`;
  > 400 drops → close 4008), event args ≤ 4 KB, state values ≤ 2 KB, ≤ 64 state keys, 20 s heartbeat.
- **Protocol** (`services/gameserver/src/protocol.ts`, moved from `engine-web/net/protocol.ts` 2026-11-18; JSON over one WebSocket): client `hello · create · join · spectate · quick · cancel ·
  ready · team · start · leave · ping` and in a match `st (avatar sample) · ev (event → host | others | player id) · kv ·
  score · end`; server `welcome · room · queued · start · left · error · pong` and `st (+ server ts) · ev · kv · score ·
  ended`.
- **C++ engine (2026-11-18): no room client yet.** `engine/src/script` has only the offline room (engine-web's
  NetSession without a link: `room.*` works locally, `room.finish` ends the run, no lobby), so in the web player and the
  app multiplayer v1 games run solo until E8 (§14.10, which brings authoritative netcode, §14.6). The app's `room` /
  `spectate` query params are ignored meanwhile; the gameserver keeps running and its tests drive it at the protocol level.
- **Engine (engine-web, retired)**: `core/net.ts` (NetSession: Luau `players` / `remoteEvent` / `room` on a `NetLink`), `net/client.ts`
  (RoomClient over WebSocket), `net/interp.ts` (clock sync + snapshot interpolation), `net/loopback.ts` (in-memory room for
  tests/harness), `net/lobby.ts` (engine-owned lobby, HUD chips, winner card). Dev page: games with `server.mode = "rooms"`
  and `players.max > 1` open the lobby first (`?room=<code>` joins, `?solo=1` skips, tools never see it); the room server
  URL is derived from the API (`https://api.x` → `wss://api.x/rooms`, local `:8787` → `:8790`) or `?rooms=`.
  Share link: `https://play.dreamsy.dev/?public=<gameId>&room=<code>` (published games; dev: the current URL + `room`).
- **Luau API**: `format/LUAU_API.md` "Players & avatar" + "Networking (multiplayer)". Example: `games/examples/mp-duel`
  (Orb Duel: host spawns orbs, first claim wins, first to 5); the racing kit races versus in a room (KIT.md "Versus").
- **v2**: server-authoritative simulation (engine core per room in Node, input upload + prediction/reconciliation,
  replicated entity props — the §4.2 design), reconnect/resume to the same seat, cross-device physical contact,
  spectators, persistent matchmaking (skill, regions), several gameserver processes (rooms pinned by code), match
  results to the API (`runs`/leaderboards from rooms), moderation of display names.
- **Spectators + presence (2026-11-15)**: `spectate {code}` watches a room (lobby or running): not a player, not counted
  in max, ≤ 16 per room (`LIMITS.maxSpectators`), receives everything players receive (`room`/`start` carry
  `spectator: true`; late spectators get the current `kv` + `score` right after `start`), can't send `st/ev/kv/score/ready/
  team/start/end` (error code `spectator`). `RoomInfo.spectators` = count. Engine: `?room=<code>&spectate=1`, no local
  player (`players.local == nil`, `room.isSpectator`), "player"-tagged entities hidden with their scripts skipped, no touch
  controls, SPECTATING HUD chip + player chips that switch the followed puppet. Presence: the room server tracks each
  userId (in_match > in_room > online, online until 60 s after the last connection closes) and answers
  `GET /internal/presence?users=a,b` (header `x-internal-token` = `WORKER_TOKEN`) → `{presence:{[userId]: Presence}}` for
  services/api's friends list. Lobby: room codes are typed on an on-screen keypad (no system keyboard; paste; auto-join
  at 6), the name field stays above the keyboard (visualViewport), and the room screen has "Invite friends"
  (`GET /v1/me/friends`, `POST /v1/invites`; signed-in users with an API game only).
