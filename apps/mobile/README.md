# apps/mobile — Dreamsy app (iOS first)

Create (chat + live preview) and play. Expo + expo-router + TypeScript. See docs/platform.md §6.

## Setup

```sh
pnpm install                       # from the repo root
cp apps/mobile/.env.example apps/mobile/.env.local   # fill in the values
```

Native projects are generated (Continuous Native Generation). `ios/` and `android/` are
gitignored; never edit them by hand — change `app.config.ts` or a config plugin instead.

```sh
cd apps/mobile
npx expo run:ios                   # prebuild + build the dev client + install on simulator/device
npx expo start --dev-client        # JS server for an already-installed dev client
```

**Expo Go** works for everything except Apple/Google sign-in: `npx expo start --go` (or
`pnpm start:go`), scan the QR code, "Continue as guest". In Expo Go (`src/runtime.ts` `isExpoGo`)
the Google SDK is never required and the Apple/Google buttons are hidden (Apple tokens from Expo Go
carry Expo Go's audience, which the API rejects). Full stack steps: repo-root `README.md`.

Sign in with Apple and Google Sign-In need a dev-client build (`expo-dev-client`). Sign in with
Apple only fully works on a real device signed with a team that has the capability enabled.

Rerun `npx expo prebuild --clean` after changing `app.config.ts`, env vars it reads, or
native dependencies.

## Web app (PWA, https://pwa.dreamsy.dev)

The same app, built for the browser for people without the native app (phones and laptops):
`pnpm -C apps/mobile build:web` → `apps/mobile/dist` (SPA, prod API + engine URLs passed on the command
line, so `.env.local` doesn't leak in), served as static files (Caddy `pwa.dreamsy.dev` in `deploy/Caddyfile`;
prod is down and there is no deploy script for now). Shell, manifest and icons are in `public/`.

Web-only files (`*.web.ts(x)`, picked by Metro on web): `EngineView.web.tsx` (the player in an
`<iframe>`, engine messages via `postMessage`, keyboard focus), `auth/tokenStore.web.ts` (token in
localStorage), `auth/native.web.ts` (no Apple), `haptics.web.ts` (no-op), `dialogs.web.tsx`
(in-app `Alert.alert` + `Share.share` → share sheet or copy link), `playerWindow.web.ts` (desktop:
full-window player at the game's aspect + keys hint; phones: full screen / "turn sideways" for
landscape), `mouseScroll.web.ts` (drag-scroll shelves), `install.web.tsx` (install banner/row, iOS
"Add to Home Screen" sheet, after the first game), `app/scan.web.tsx` (no camera; entry points
hidden). Push is skipped on web (`src/push.ts`); the inbox still polls. On wide screens the app
sits in a 480 px column (`useAppDimensions`).

## Env

| Var | Used by | What |
|---|---|---|
| `EXPO_PUBLIC_API_URL` | JS | `services/api` base URL (LAN IP on device) |
| `EXPO_PUBLIC_ENGINE_URL` | JS | C++ web player page the player WebView loads (LAN IP, `:2005/`, served by `scripts/dev.sh`) |
| `EXPO_PUBLIC_WEB_URL` | JS | optional public web base for share links (`<base>/g/<id>`); falls back to the engine URL. Built in `src/social.ts` `shareUrl()` |
| `DREAMSY_IOS_BUNDLE_ID` | app.config | iOS bundle id |
| `DREAMSY_ANDROID_PACKAGE` | app.config | Android package (later) |
| `EXPO_PUBLIC_GOOGLE_IOS_CLIENT_ID` | JS | Google iOS OAuth client id |
| `EXPO_PUBLIC_GOOGLE_WEB_CLIENT_ID` | JS | Google web OAuth client id = idToken audience the API verifies |
| `GOOGLE_IOS_URL_SCHEME` | app.config | reversed iOS client id (`com.googleusercontent.apps.…`) |
| `EAS_PROJECT_ID` | app.config | EAS project id -> `extra.eas.projectId`; needed for Expo push tokens (push is skipped without it) |

## Layout

```
app/                 expo-router routes
  _layout.tsx        AuthProvider + protected stack (signed in -> tabs, else sign-in)
  sign-in.tsx        email code (default) / Google / Apple / guest
  onboarding.tsx     first run: handle + avatar
  avatar.tsx         edit avatar (modal)
  (tabs)/            Home (feed tabs + search), Create (my games, running builds + ETA),
                     Market (store items across games), Profile (own profile, coins wallet, earnings)
  discover/[id].tsx  game page: play, like, share, share with family, remix, creator, report (docs/platform.md §11);
                     leaderboards (board picker, Everyone / each of my groups, top 20, me, ghost badge; §13)
  user/[id].tsx      creator profile: follow, published games
  groups/index.tsx   Family & friends (docs/platform.md §13): my groups, start one (share sheet with the invite link),
                     join by code. Reached from Profile (row + menu) and the people icon on Home
  groups/[id].tsx    group: invite code + share, members, shared games (Play; drafts too), leave, delete /
                     new code (owner)
  join/[code].tsx    invite link dreamsy://join/<code> or https://dreamsy.dev/join/<code> (also from the QR
                     scanner). Joins when signed in; otherwise the code waits until sign-in + onboarding
  game/[id].tsx      with `room` (+ `spectate=1`), e.g. dreamsy://game/<id>?room=CODE from the web studio's QR: straight
                     into the player in that room (Join = play gate, Spectate free). Otherwise the
                     Create workspace: chat + preview, publish, round/ETA card, checkpoint reloads;
                     first build costs CREATE_PRICE coins (402 -> "Out of coins"), edits free
  notifications.tsx  inbox (earnings, build done, family joins): tap opens the game/group and marks read,
                     "Mark all read". Bell + unread badge on Home and Profile; hidden when the API has no
                     /v1/me/notifications (404)
  play/[id].tsx      full-screen player (params id, version, orientation?, room?, spectate?); locks to the
                     game's orientation and passes safe-area insets to the engine (`&insets=t,r,b,l`);
                     `room` joins that room (`&room=CODE`), `spectate=1` watches it (free, no POST /play)
  welcome.tsx        "Welcome to Dreamsy world" sheet after the first game (transparent modal): result line, Live now
                     (polled 20 s; Join / Spectate / Play), 6 trending games, Make your own game, Explore.
                     Showing it calls POST /v1/me/onboarding
  friends.tsx        Friends: search by handle + Add, requests (Accept/Decline, Cancel), friends with presence
                     (Online / In X lobby + Join / Playing X · 1v1 + Spectate), long-press to unfriend; polls
                     every 15 s while focused. Reached from Profile ("Friends" row) and Home's people menu;
                     hidden when the API has no /v1/me/friends (404)
src/
  api.ts             typed client for services/api; takes its bearer token from auth
  runtime.ts         isExpoGo
  social.ts          shareUrl/shareGame, number formatting, fallback gradients
  usePaged.ts        cursor pagination for feeds/search/market
  wallet.ts          useWallet() + walletChanged() (refetch after in-game purchases)
  coins.tsx          coin formatting, play gate, create/remix prices, "Out of coins" sheet
  notifications.ts   unread store, 30 s foreground poll, "You got paid" toast, game-invite banner, tap -> screen
  onboarding.tsx     first run: user.onboardedAt === null -> GET /v1/onboarding -> player with `onboarding=1`
                     (banner; runEnd / Quit / close / 120 s -> app/welcome.tsx). Skipped if /v1/onboarding or
                     /v1/live 404, or onboardedAt is missing (older API)
  live.tsx           GET /v1/live store (404 hides it), 20 s focus polling, Live now header + people cards
  friends.tsx        friends store + presence lines, join (play gate) / spectate (free) a room, open a game_invite
  push.ts            expo-notifications: permission + Expo token -> POST /v1/me/push-tokens, foreground
                     presentation, push taps. Skipped in Expo Go on Android and without a projectId
  toast.tsx          app-wide top toasts (<ToastHost /> in app/_layout.tsx)
  components/
    EngineView.tsx   the game player: WebView of the C++ web player in embed mode + message bridge
    GamePreview.tsx  inline 16:10 player card (workspace, discover detail)
  auth/              ALL auth logic; screens import only from '@/auth'
    index.ts         public surface: AuthProvider, useAuth, SignInCancelled
    AuthProvider.tsx context: signInWithApple/Google, continueAsGuest, signOut, getAccessToken
    native.ts        Apple + Google native SDKs -> identityToken / idToken (backend-agnostic)
    types.ts         AuthBackend interface (the swappable part)
    dreamsyApiBackend.ts  current backend: services/api /v1/auth/*, token in expo-secure-store
modules/
  dreamsy-engine/    notes on the player bridge (see its README)
```

## Auth flow

Screens use `useAuth()` from `@/auth` only.

1. **Email code** (default, `src/auth/dreamsyApiBackend.ts`): `POST /v1/auth/email/start {email}` emails a
   6-digit code (expires in 10 min; resend after 30 s), `POST /v1/auth/email/verify {email, code}` returns
   `{ user, token }`. No Supabase involved. Typing a username without `@` uses the demo bypass
   (`/v1/auth/demo`, usernames ending in `_demo`).
   **Google** (`src/auth/supabase.ts`, hidden unless the Supabase env vars are set): Supabase OAuth (PKCE) in the system auth sheet, redirecting
   to `dreamsy://auth-callback` (`exp://…/--/auth-callback` in Expo Go, so it works there too). The
   Supabase access token goes to `POST /v1/auth/supabase`, which verifies it against the project's
   JWKS and returns `{ user, token }`. **Apple**: native sheet -> `/v1/auth/apple` (dev builds only).
   **Guest**: `/v1/auth/guest`.
2. The Dreamsy token goes to the keychain (`expo-secure-store`). On launch `/v1/me` restores the
   session; a 401 signs out.
3. Signed in with `user.avatar == null` -> `app/onboarding.tsx` (handle, then avatar maker). Saving
   the avatar (`PATCH /v1/me {avatar}`) lands in the tabs.

## Design

Ink on paper (`src/theme.ts`): cream `#F3EFE6`, ink `#141414`, 2px outlines, Silkscreen pixel caps for
labels/buttons and Space Grotesk for text. Build screens from `src/ui` (Txt, InkButton, RoundButton,
Card, Header, Pills, ProgressBar, Field, Empty). Avatars are SVG busts from parts (`src/avatar`),
stored as `{ hat, head, eyes, mouth, extra, body }` indices; users without one get a stable avatar from their id.

## Engine view

`src/components/EngineView.tsx` renders a WebView of
`${EXPO_PUBLIC_ENGINE_URL}?api=${EXPO_PUBLIC_API_URL}&game=<id>&version=<n>&token=<jwt>&embed=1`.
The page is the C++ engine's web player (`engine/build/web`; contract in `engine/apps/player/embed.js` and
`engine/README.md` "Embedding the web player"). It downloads the package from the API (bearer token) and
posts JSON back via `window.ReactNativeWebView.postMessage` (in the PWA iframe: `window.parent.postMessage`):

| message | app does |
|---|---|
| `{type:"ready"}` | hide the spinner |
| `{type:"error", message}` | banner (load errors + script errors, rate-limited) |
| `{type:"quit"}` | pause-menu Quit: closes the Play screen |
| `{type:"purchase", itemId, balance}` | Coins purchase in the engine's sheet: wallet refetch (not sent by the C++ player yet: its store is the free in-memory one) |
| `{type:"runEnd", reason, board?, value?, rank?}` | `onRunEnd`; the first-run player moves on to the Welcome sheet (`rank` needs API leaderboards, not in the C++ player yet) |

`&insets=top,right,bottom,left` (full-screen player) keeps the engine's pause button and controls out of the notch.
`&room=` / `&spectate=` (friends: join / watch a room) are ignored until the C++ engine has a room client
(docs/engine.md §14.10 E8); multiplayer games run solo meanwhile.

The workspace preview is keyed by `latestVersion`, so a finished generation (or a restore)
reloads it. Production will point `EXPO_PUBLIC_ENGINE_URL` at a hosted build of the web player
(`deploy/Dockerfile` builds it into `/app/player`).
