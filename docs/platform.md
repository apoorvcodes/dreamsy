# Dreamsy — Platform: clients, social, marketplace, leaderboards

Part of the Dreamsy spec. Section numbers are shared across the spec files; [docs/README.md](README.md) maps each
section to its file.

## 6. Clients

- **Mobile app (iOS first, Expo/React Native):** home/discover, play, create. Create = chat + live preview of the running
  game + tap-to-select entities to scope edits. Engine runs in a WebView: the C++ web player (§14.7 step 1) loaded with
  `?api=&game=&version=&token=&embed=1[&insets=]`, posting `ready / error / quit / runEnd` back (PWA: an iframe and
  `window.parent.postMessage`). Contract: `engine/apps/player/embed.js` and `engine/README.md` "Embedding the web
  player". Not ported from engine-web's page yet: `purchase` + the API store (the store is the free in-memory one), API
  leaderboards (`runEnd.rank`, ghosts), persisted saves, rooms/lobby (§4.5).
- **Web:** same flows, same player; share links are `play.<domain>/?public=<gameId>` (`/g/<gameId>` redirects there).
- App Store: games are interpreted Luau + data in a sandbox (same basis as Roblox / HTML5 mini-games, guideline 4.7).

## 11. Platform v1 — social, remix, marketplace (spec, 2026-11-15)

Shared contract for engine, API, app and harness. JSON camelCase; user routes need a bearer token
unless marked **public**.

### 11.1 Orientation (phone layout)
- Manifest `orientation = "portrait" | "landscape"` (default `"portrait"` for new games; existing
  games without it are `"landscape"`). Reference screen keeps the short side = 720: portrait is
  720×1280, landscape 1280×720.
- Engine letterboxes to the reference aspect, respects safe areas (notch/home bar) for UI anchors,
  and lays out touch controls per orientation. The app's Play screen locks to the game's orientation.

### 11.2 Feed, stats, thumbnails
- Every published game has stats: `plays`, `likes`, `remixes`, `trendingScore`.
- `GET /v1/feed?tab=trending|new|top|following&cursor=&limit=&multiplayer=1` → `{ games: FeedGame[], next }`.
  `FeedGame = { id, name, description, orientation, thumbnailUrl, players: {min, max}, multiplayer, creator: {id, handle}, stats: {plays, likes, remixes}, publishedAt, remixOf: {gameId, name} | null }`.
  `players` = the published version's `[players]` (`game_versions.players_min/max`, set on commit); `multiplayer` = max > 1;
  every game view has both (owner/group views: published, else latest). `?multiplayer=1` on feed/search/`GET /v1/games` filters.
- `GET /v1/search?q=&cursor=&multiplayer=1` → same shape (name/description/creator handle, ILIKE is fine for v1).
- Trending score = `(plays_7d + 3·likes_7d + 5·remixes_7d) / (hoursSincePublish + 2)^1.2`, recomputed on write or by a cheap periodic query — keep it simple.
- `POST /v1/games/:id/plays {version}` → counts a play (dedupe one per user per game per hour).
- `PUT /v1/games/:id/like` / `DELETE /v1/games/:id/like`; game views include `viewer: {liked, ownedPasses?}`.
- Thumbnails: the worker uploads `thumbnail` (JPEG, multipart field on `/complete`) taken from the harness playtest; stored per version. `GET /v1/games/:id/versions/:v/thumbnail` (public for the published version) — `thumbnailUrl` points at it.

### 11.3 Remix (fork)
- `POST /v1/games/:id/remix` → creates a new game (public by default, §11.8, so v1 goes live) owned by the caller whose version 1 is a
  copy of the source's published version (or latest, if the caller owns the source).
  `remixOf = {gameId, version}` stored; source `remixes` +1. Name "<source> (remix)".
- Remixes keep a credit line in their public view; store items are removed from the copy's manifest
  (sellers can't resell someone else's items) — the harness/worker never needs to know.

### 11.4 Creators & following
- `GET /v1/users/:id` → `{ user: {id, handle, bio, createdAt}, stats: {games, plays, likes, followers, following}, viewer: {following} }`.
- `GET /v1/users/:id/games` (published only, unless self). `PATCH /v1/me {handle?, bio?}` (handle unique, 3–20 chars `[a-z0-9_]`).
- `PUT|DELETE /v1/users/:id/follow`. Feed tab `following` = recent games from followed creators.

### 11.5 Sharing (public)
- `GET /v1/public/games/:id` → FeedGame + `publishedVersion` (**public**, published only).
- `GET /v1/public/games/:id/package` → published package zip (**public**) so the web player can run
  it without an account. Rate-limit later.
- Share URL: `${WEB_URL}/g/<gameId>` → the C++ web player at play.<domain> (Caddy redirects `/g/:id` to
  `?public=<id>`), no login needed. App share sheet shares this URL.

### 11.6 Marketplace
- `GET /v1/marketplace?sort=popular|new|price&cursor=` → store items across all published games:
  `{ items: [{ gameId, gameName, itemId, kind, name, description, price, iconUrl?, sales }], next }`.
  Purchases still go through `POST /v1/games/:id/store/purchase` (in the game's context).
- `sales` counts purchases per item.

**Coins & pay-per-play (2026-11-15).** The Stars ledger (`wallets`, append-only `ledger`) is the
currency, shown to users as **coins**. 10,000 coins = $0.10 (1,000,000 = $10); no real money yet.
- Every account starts with **1,000,000 coins** (`STARTER_STARS`); migration `0011_pay_per_play` topped
  existing wallets up to 1,000,000 with a `grant` ledger entry and seeded the platform account `u_platform`.
- Starting someone else's game costs **`PLAY_PRICE` = 10,000 coins**, split **`CREATOR_SHARE` = 0.9**:
  creator 9,000, platform 1,000. Three ledger rows that sum to zero: `play` (player −10,000),
  `play_earning` (creator +9,000), `platform_fee` (`u_platform` +1,000), linked by `pass_id`.
  Store purchases split the same way (`purchase` / `sale` / `platform_fee`).
- A paid start opens a **30-minute play pass** (`play_passes`, `PLAY_PASS_MINUTES`) for (user, game);
  starts while it's valid are free and return the same pass (reloads, crashes, rematches, multiplayer rejoin).
- Free: the owner playing their own game; group members playing an unpublished draft shared to their group.
- `POST /v1/games/:id/play {version?}` → `{ charged, price, balance, pass: {id, expiresAt}, creatorEarned }`
  (`price` is always the play price; `creatorEarned` 0 unless charged); 402 `{error:"insufficient_coins", balance, price}`.
  One transaction; wallet rows locked in id order, so concurrent starts charge once and never go negative.
  It also counts the play (same 1/user/game/hour dedupe as `POST /plays`, which stays for old clients, free).
  `GET /v1/games/:id/play` is a side-effect-free quote: `{free, reason: "owner"|"draft"|"pass"|null, price, balance, pass|null}`.
- `GET /v1/me/earnings` → `{ totalCoins, todayCoins, paidPlays, usd: {total, today}, games: [{gameId, name, coins, plays}] }`
  (play earnings + store sales); also returned as `earnings` on `GET /v1/me/insights`.
  `GET /v1/me/wallet` → `wallet: {balance, currency: "coins", usd}`. Game views carry `playPrice`.
- v1 gate is client-side: the app calls `POST /play` before opening the player; package fetch does
  not check passes yet. Real-money top-up / cash-out is later.

**Create & remix prices + notifications (2026-11-15).**
- **Create:** a game's *first* build (`POST /v1/games/:id/generations` while the game has no
  versions) costs **`CREATE_PRICE` = 100,000 coins** → platform (two `create` ledger rows). Edits,
  tweaks and sprite edits of a built game are free. Response `{generation, charged}` (0 or 100,000;
  also `generation.charged` / `generation.refunded`); 402 `{error:"insufficient_coins", balance,
  price}`. If that build ends `failed` (final, incl. lease give-up) or `canceled` while the game still
  has no version, the charge is refunded once (`create_refund` rows; `generations.refunded_at`, set
  under the generation row lock). A checkpoint that committed v1 keeps the charge.
- **Remix:** `POST /v1/games/:id/remix` costs **`REMIX_PRICE` = 25,000 coins**, 100% to the original
  game's owner (`remix` −25,000 / `remix_earning` +25,000); free on your own game. Response
  `{game, charged, ownerEarned}`; same 402.
- Prices are exposed: game views `remixPrice` (next to `playPrice`); `GET /v1/me/quota` and
  `GET /v1/me/wallet` carry `createPrice` (top level and in `quota` / `wallet`) and `remixPrice`.
- **Notifications** (`notifications` table, migration `0012_notifications_create_price`): inserted in
  the same transaction as their cause via `notify(tx, userId, kind, title, body, data)`; never to
  yourself. Kinds: `play_earning` (one per paid pass: "You got paid" / "@p played G — +9K coins
  ($0.09)"), `remix_earning` ("@p remixed G — +25K coins ($0.25)"), `sale` ("@p bought I in G — +N
  coins"), `build_done` ("Your game is ready" / "G vN is playable"; on complete, and on the checkpoint
  that commits a new game's v1), `group_join` ("New family member" / "@p joined Group", to the other
  members).
  `GET /v1/me/notifications?cursor=&limit=` → `{items:[{id, kind, title, body, data, read,
  createdAt}], unread, next}`; `GET /v1/me/notifications/unread` → `{unread}`;
  `POST /v1/me/notifications/read {ids?, all?}` → `{unread}`.
- **Push:** `POST /v1/me/push-tokens {token, platform}` / `DELETE {token}` (`push_tokens`; a token
  belongs to its last registrant). After commit the API sends each new notification to the owner's
  Expo tokens (`src/push.ts`: `https://exp.host/--/api/v2/push/send`, ≤100/batch, 10 s timeout,
  fire-and-forget, errors swallowed) as `{to, title, body, data:{...data, kind, notificationId},
  sound:"default", badge: unread}`; tokens reported `DeviceNotRegistered` are deleted.

### 11.7 Safety
- `POST /v1/games/:id/report {reason}`; games with ≥3 distinct reports are hidden from feeds pending review (`hidden` flag).

### 11.8 Visibility (public by default, 2026-11-15)
- `games.visibility = "public" | "private"`, default `"public"`: "all games are public until you set it".
- **Public** ⇒ auto-publish: every committed version (worker checkpoint/complete, revert, a remix's copied
  v1) becomes the live one in the same transaction (`status = published`, `publishedVersion = v`,
  `publishedAt` set once, trending recomputed). Everyone plays the newest version.
- **Private** ⇒ never published; only the owner and groups it's shared with (§13) can play it.
- `PATCH /v1/games/:id {visibility}` (owner): `private` unpublishes; `public` publishes the latest version
  (an empty game just turns public and its first commit goes live). `POST /publish {version?}` = public +
  that version live (pin an older version; the next commit moves live to the newest again).
  `POST /unpublish` = private. `POST /v1/games {visibility?}` may create private up front.
- Owner views include `visibility`. `hidden` (moderation) stays separate: hidden games never show in feeds.
- Migration 0015: existing games `published` → public, everything else → private (nothing private gets
  exposed; old drafts are not auto-published).
- Clients: a Public/Private switch replaces Publish/Unpublish ("Public: everyone can play the newest
  version" / "Private: only you (and groups you share with)"); "Make vN live" pins an older version of a
  public game. Clients fall back to the old publish UI if the API omits `visibility`.

## 13. Leaderboards, ghosts, groups (spec, 2026-11-15)

Step 1 of competitive/social play. Real-time rooms (1v1 red/blue, up to 8 players) come after, on the §4.2 server.

### 13.1 Model
- **Board**: per game, named (default `"fastest"`), `order = "asc"` (lower is better: times) or `"desc"` (scores).
  A player's **best** per (game, board) is what ranks; every submit is a **run**. Runs keep the game `version`.
- **Scopes**: `public` (everyone), `group:<groupId>` (members of a family/friends group), `me` (my runs).
- **Ghost**: optional recording of a run: the tracked entity's position + flip + animation frame sampled at 10 Hz in
  *game time*, plus the scene id: `{ v: 1, hz: 10, scene, frames: [[x, y, flipX(0|1), frame], ...] }` (≤ 5 min ≈
  3000 frames; ≤ 256 KB JSON). Stored only for a player's best run per board (older ghosts dropped).
- Times are measured by the engine in game time (paused time and time-scale excluded); client-reported for now
  (server-authoritative later with rooms). API rejects absurd values (< 1 s or > 6 h for asc boards).

### 13.2 Luau API (`leaderboard`, client scripts)
```luau
leaderboard.start(board?: string, opts?: { track: Entity? })   -- begin a timed run; ghost tracks opts.track or the entity tagged "player"
leaderboard.finish(board?: string)                               -- stop timer, submit time (+ ghost); shows the results card
leaderboard.submit(board: string, value: number, opts?: { order: "asc"|"desc" })  -- explicit score/time submit (no ghost)
leaderboard.cancel(board?: string)                               -- abandon (death/restart) without submitting
leaderboard.elapsed(board?: string): number                      -- seconds of the running timer (for a HUD)
leaderboard.raceGhost(which: "best" | "pb" | runId)              -- spawn a translucent replay of that ghost in the current scene
leaderboard.result  -- Signal(rank: number, best: number, isPersonalBest: boolean) after a submit resolves
-- implemented extras: finish() returns the time; leaderboard.running(board?): boolean; raceGhost(which, board?);
-- submit order defaults to "desc"; cancel() with no board cancels every running timer. Board names ^[a-z0-9_]{1,40}$ (API).
-- result `best` = the board record (#1 value), as the API returns it.
-- Ghost `frame` = index into the tracked sprite's anim-set textures (deduplicated, file order); 0 without anims.
```
Engine-owned UI: a results card after `finish` (your time, rank, PB, top 5 public + your group, "Race the #1 ghost",
"Play again") and a **Leaderboard** entry in the platform pause menu. Host interface `LeaderboardHost` (like
`StoreHost`): `MemoryLeaderboardHost` for Node/playtests/dev, API-backed in the dev page / app WebView.

### 13.3 API
```
POST /v1/games/:id/runs                    {board, order?, version, value, ghost?} → {run, rank, best, personalBest}
GET  /v1/games/:id/leaderboards/:board     ?scope=public|me|group:<id>&limit=20 → {board, order, entries:[{rank, user, value, runId, hasGhost, version, at}], me?: {rank, value, runId}}
GET  /v1/runs/:runId/ghost                 → ghost JSON (public if the game is visible to the caller)
POST /v1/groups {name}                     → {group, inviteCode}        GET /v1/me/groups
POST /v1/groups/join {code}                → {group}                     GET /v1/groups/:id (members)
POST /v1/groups/:id/leave                  DELETE /v1/groups/:id (owner)
POST /v1/groups/:id/games {gameId}         share a game (even a draft) with the group; GET /v1/groups/:id/games
GET  /v1/games/:id/leaderboards            → {boards:[{board, order, players, best, updatedAt}]}   (boards with runs; dashboard picker)
POST /v1/groups/:id/invite-code            → {inviteCode}   (owner; rotates, old links stop working)
DELETE /v1/groups/:id/games/:gameId        unshare (sharer, game owner or group owner)
```
Implementation notes (services/api `routes/leaderboards.ts`, `routes/groups.ts`, migration `0009_leaderboards_groups`):
- Board names `^[a-z0-9_]{1,40}$`; the first submit fixes a board's order (a different `order` later → 409). Values:
  asc 1 s…6 h, desc |v| ≤ 1e12. `version` ≤ the game's latest. ≤ 30 submits/min/user (in-process) → 429.
- `rank` on submit: a PB → rank of the new best (ties: earlier best first); otherwise where this run would place
  among the other players' bests. `best` = the board's #1 value. Ghost blobs live at `ghosts/<runId>.json`; when a
  new PB lands the previous best's ghost is deleted. Ghost shape is validated (`v: 1`, `frames` of 4 numbers) → 400; > 256 KB → 413.
- `scope=me` lists my runs (best first; `rank` = position among them). `group:<id>` requires membership (404).
- Groups: invite codes are 8 chars from `A–Z 2–9` minus look-alikes, case-insensitive. Joining is idempotent; a full
  group → 409. The owner leaving hands the group to the longest-standing member; the last member out deletes it.
  Only your own game (drafts too) or a published game can be shared.
- Shared-draft visibility: `GET /v1/games/:id` (returns the owner-style view + `shared: true`), version / package /
  thumbnail / assets / plays / likes and leaderboards accept members of any group the game is shared with
  (`canAccessGame` / `canAccessVersion` in services.ts). Store purchases still need the published version.
Groups: ≤ 50 members, invite code is 8 chars (rotatable by owner). Group members can play games shared to the group
even when unpublished (draft visibility check extends to "shared with one of my groups").

#### Spin-off royalties (2026-11-15)
Four parties: platform (Dreamsy), creator, spin-off creator, player. A paid play of a **spin-off** (a remix) still costs 10,000:
platform 1,000 · the spin-off's creator 8,000 · the **direct parent's** owner 1,000 royalty (`royalty` ledger kind + "royalty"
notification). One level only: a spin-off of a spin-off pays the first spin-off's creator, not the grandparent. No royalty when the
parent is your own game (you keep 9,000). `SPINOFF_ROYALTY` (default 1000) is taken from the creator's 90% share.

### 13.4 Friends, invites, presence (2026-11-15)
Mutual friends (not follows), invites into a live room, and "what is my friend doing" from the room server. Spectating and
the engine/app sides (keypad, `room=`/`spectate=1` URL params, invite panel, Friends screen) are specced in the same
contract; this section is the API part.

```
POST   /v1/friends/requests {handle}            → 201 {request:{id(=other user id), user, status:"pending", direction:"outgoing"}}
                                                  | 200 {friend} when they had already asked you (auto-accept)
                                                  | 404 unknown handle | 409 already friends | 400 self
POST   /v1/friends/requests/:userId/accept      → {friend}          (404 no request from them, 409 already friends)
POST   /v1/friends/requests/:userId/decline     → {ok}              (404 no request from them)
DELETE /v1/friends/:userId                      → {ok}              unfriend, cancel my request, or drop theirs (404 none)
GET    /v1/me/friends                           → {friends:[{user:{id,handle,avatar}, since, presence}], incoming:[{user, at}], outgoing:[{user, at}]}
GET    /v1/users/search?q=<prefix>              → {users:[{id, handle, avatar, friendStatus:"none"|"friends"|"incoming"|"outgoing"}]}
POST   /v1/invites {userId, gameId, roomCode}   → {ok}              friends only (403), ≤ 10/min per sender (429)
```
`Presence = {status: "offline"|"online"|"in_room"|"in_match", gameId?, gameName?, roomCode?, mode?: "duel"|"party",
players?, max?, spectatable?}`. "online" = connected to the room server's lobby in the last 60 s.

- **Model**: `friendships(user_a, user_b, status "pending"|"accepted", requested_by, created_at, accepted_at)`, one row per
  pair with ids sorted (`user_a < user_b`, primary key), so a request and its reverse are the same row. Migration `0013_friends`.
  Requests insert first (`ON CONFLICT DO NOTHING`), then lock the row: a repeat request is idempotent (201, no new
  notification); a request to someone who already asked you accepts it.
- **Presence** comes from the room server: `GET $GAMESERVER_INTERNAL_URL/internal/presence?users=id1,id2` with
  `x-internal-token: $WORKER_TOKEN` → `{presence: {[userId]: Presence}}`; 1.5 s timeout; any failure or an unknown
  id → `offline`. Default URL `http://gameserver:8790` (set in deploy/docker-compose.yml). `Deps.presence` swaps the
  fetcher (tests). Friends are sorted in_room, in_match, online, offline, then by handle.
- **Search**: handle prefix, case-insensitive, leading `@` ignored, `%`/`_` matched literally, excludes the caller, ≤ 20,
  ordered by handle. Empty `q` → `[]`.
- **Notifications** (existing notify + Expo push, §11.6): `friend_request` ("Friend request" / "@x wants to be friends"),
  `friend_accept` ("New friend" / "@x accepted your friend request"), both with `data.fromUserId`; `game_invite`
  ("Game invite" / "@x invited you to play <game name>", `data {gameId, version?, roomCode, fromUserId, spectate: false,
  expiresAt}`; `expiresAt` = now + 10 min, ISO). `version` = the request's optional `version`, else the game's published
  version, else absent.
- **Invites**: the game must be visible to the inviter (own, published, or shared with one of their groups; 404 otherwise).
  `roomCode` is upper-cased, 4–12 letters/digits. The rate limit counts the sender's `game_invite` notifications in the
  last 60 s (DB, so it holds across API instances).


#### Onboarding + live (2026-11-15)
New accounts land in the onboarding game (Tidal Dash, `ONBOARDING_GAME_ID`) for a free first play; on the engine's `runEnd` host message (leaderboard finish/submit, Luau `platform.runEnded`, match end) the app/studio shows "Welcome to Dreamsy world" with `GET /v1/live` (people online + what they're playing, Join/Spectate) and featured games, then `POST /v1/me/onboarding`. The package format (§3) is unchanged; `platform.runEnded` is a runtime API addition (format/LUAU_API.md).
