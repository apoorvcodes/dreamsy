#!/usr/bin/env bash
# Start the whole Dreamsy dev stack and open the app in Expo Go.
#   ./scripts/dev.sh            (Ctrl+C stops everything)
#   ./scripts/dev.sh --reseed   (publish the showcase games again)
#   ./scripts/dev.sh --tunnel   (phone on any network: Metro over a Cloudflare quick tunnel)
#   ./scripts/dev.sh --prod     (app uses a hosted api/play.dreamsy.dev instead of this laptop; prod is down)
# Needs Emscripten 4.0.11 (EMSDK in the environment, or ~/emsdk): the engine is the C++ web player, built here.
set -euo pipefail
set -m   # job control: each background server gets its own process group, so cleanup kills the whole tree
cd "$(dirname "$0")/.."
ROOT=$(pwd)
# Networks with broken IPv6 + slow IPv4 (VPNs, venue Wi-Fi) make Node's 250ms connect race time out
# ("Connection error", Expo "UnexpectedServerError"). A longer window is harmless elsewhere.
export NODE_OPTIONS="${NODE_OPTIONS:-} --network-family-autoselection-attempt-timeout=2000"
LOGS="$ROOT/.dev-logs"; mkdir -p "$LOGS"
IP=$(ipconfig getifaddr en0 2>/dev/null || echo localhost)
TUNNEL=0; RESEED=0; LOCAL=1
for a in "$@"; do case "$a" in --tunnel) TUNNEL=1;; --reseed) RESEED=1;; --local) LOCAL=1;; --prod) LOCAL=0;; esac; done
PROD_API=https://api.dreamsy.dev; PROD_ENGINE=https://play.dreamsy.dev/
PIDS=()
cleanup() {
  echo; echo "stopping…"
  for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill -TERM -- "-$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
}
# Leftovers from an earlier run of this script (only our own servers; anything else → ask the user).
free_port() {
  local port=$1 pid
  for pid in $(lsof -ti tcp:"$port" -sTCP:LISTEN 2>/dev/null); do
    echo "  killing $(ps -o comm= -p "$pid" 2>/dev/null | xargs basename 2>/dev/null) on :$port (pid $pid)"
    kill "$pid" 2>/dev/null || true
  done
  sleep 0.5
  for pid in $(lsof -ti tcp:"$port" -sTCP:LISTEN 2>/dev/null); do kill -9 "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT INT TERM

step() { printf "\n\033[1;35m▸ %s\033[0m\n" "$*"; }
bg() { local name=$1; shift; ( "$@" 2>&1 | sed -u "s/^/[$name] /" >"$LOGS/$name.log" ) & PIDS+=($!); echo "  $name → .dev-logs/$name.log"; }
wait_http() { for _ in $(seq 1 60); do curl -sf "$1" >/dev/null && return 0; sleep 1; done; echo "timed out waiting for $1"; exit 1; }

step "Checks"
# Stop any previous run of this script (and everything it started) — only one stack at a time.
for pid in $(pgrep -f "scripts/dev.sh" || true); do
  if [ "$pid" != "$$" ] && [ "$pid" != "$PPID" ]; then echo "  stopping previous dev.sh (pid $pid)"; kill -TERM "$pid" 2>/dev/null || true; fi
done
pkill -f "expo start" 2>/dev/null && echo "  stopped previous Expo" || true
# Expo account: EXPO_TOKEN from the repo-root .env (access token from expo.dev/settings/access-tokens).
if [ -z "${EXPO_TOKEN:-}" ] && [ -f .env ]; then
  t=$(grep -E '^EXPO_TOKEN=' .env | tail -1 | cut -d= -f2- | tr -d '"' || true)
  [ -n "$t" ] && export EXPO_TOKEN="$t"
fi
(cd apps/mobile && npx expo whoami >/dev/null 2>&1) || { echo "Not signed in to Expo. Create a token at https://expo.dev/settings/access-tokens and add EXPO_TOKEN=... to the repo-root .env (or run: cd apps/mobile && npx expo login). Sign in to Expo Go on the phone with the same account."; exit 1; }
docker info >/dev/null 2>&1 || { echo "Docker isn't running. Start Docker Desktop, then rerun."; exit 1; }
[ -f services/api/.env ] || { echo "Missing services/api/.env (copy services/api/.env.example)."; exit 1; }
if [ -z "${EMSDK:-}" ]; then
  [ -f "$HOME/emsdk/emsdk_env.sh" ] || { echo "Emscripten not found: install emsdk 4.0.11 in ~/emsdk (engine/README.md) or set EMSDK."; exit 1; }
  set +u; source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1; set -u
fi
if [ "$LOCAL" = 1 ]; then
  printf 'EXPO_PUBLIC_API_URL=http://%s:8787\nEXPO_PUBLIC_ENGINE_URL=http://%s:2005/\n' "$IP" "$IP" > apps/mobile/.env.local
  echo "  app → this laptop ($IP)"
else
  printf 'EXPO_PUBLIC_API_URL=%s\nEXPO_PUBLIC_ENGINE_URL=%s\n' "$PROD_API" "$PROD_ENGINE" > apps/mobile/.env.local
  echo "  app → prod ($PROD_API, $PROD_ENGINE)"
fi
for port in 8787 8790 2005 3000 8081 8082 19000 19001 19002; do free_port $port; done   # API, rooms, engine, dashboard, Expo/Metro
pkill -f "harness.*src/main.ts worker" 2>/dev/null && echo "  stopped leftover harness worker" || true

step "Postgres"
docker compose -f services/api/docker-compose.yml up -d
for _ in $(seq 1 30); do docker compose -f services/api/docker-compose.yml exec -T postgres pg_isready -U dreamsy >/dev/null 2>&1 && break; sleep 1; done

step "Engine: C++ web player (engine/build/web; the first build takes a few minutes)"
(cd engine && { [ -f build/web/build.ninja ] || cmake --preset web >/dev/null; } && cmake --build --preset web --target dreamsy-player)
cp engine/build/web/dreamsy-player.html engine/build/web/index.html   # http://host:2005/ serves the player

step "Migrations"
pnpm -s -C services/api db:migrate

step "API (http://$IP:8787)"
bg api pnpm -s -C services/api start
wait_http http://localhost:8787/health

step "Room server (ws://$IP:8790/rooms)"
bg gameserver env API_URL=http://localhost:8787 pnpm -s -C services/gameserver start

if [ ! -f "$LOGS/.seeded" ] || [ "$RESEED" = 1 ]; then
  step "Publishing showcase games"
  pnpm -s -C services/api seed:showcase && touch "$LOGS/.seeded"
fi

step "Harness worker (builds games)"
bg worker pnpm -s -C harness worker

step "Web dashboard (http://localhost:3000)"
[ -f apps/web/.env.local ] || echo "  (no apps/web/.env.local — dashboard will offer guest sign-in only)"
bg web pnpm -s -C apps/web exec vite --port 3000 --strictPort

step "Engine (http://$IP:2005)"
# Static server for engine/build/web (wasm needs its MIME type for streaming compilation).
bg engine python3 -c 'import functools, http.server as h, sys
h.SimpleHTTPRequestHandler.extensions_map.update({".wasm": "application/wasm", ".js": "text/javascript", ".dpk": "application/octet-stream"})
h.test(HandlerClass=functools.partial(h.SimpleHTTPRequestHandler, directory=sys.argv[1]), port=2005, bind="0.0.0.0")' engine/build/web
wait_http http://localhost:2005/

# Public https URL from a Cloudflare quick tunnel's log (no account needed).
# NOTE: a quick tunnel makes that local port reachable from the internet while dev.sh runs.
tunnel_url() {
  local name=$1 url=""
  for _ in $(seq 1 60); do
    url=$(grep -oE 'https://[a-z0-9-]+\.trycloudflare\.com' "$LOGS/tunnel-$name.log" 2>/dev/null | head -1 || true)
    [ -n "$url" ] && { echo "$url"; return 0; }
    sleep 1
  done
  echo "tunnel for $name didn't come up (see .dev-logs/tunnel-$name.log)" >&2; exit 1
}

if [ "$TUNNEL" = 1 ]; then
  step "Tunnels (phone works on any network; these URLs are public while dev.sh runs)"
  command -v cloudflared >/dev/null || { echo "Install cloudflared: brew install cloudflared"; exit 1; }
  rm -f "$LOGS"/tunnel-*.log
  bg tunnel-metro cloudflared tunnel --no-autoupdate --url http://localhost:8081
  if [ "$LOCAL" = 1 ]; then   # prod API/engine are already public; only a local stack needs tunnels for them
    bg tunnel-api cloudflared tunnel --no-autoupdate --url http://localhost:8787
    bg tunnel-engine cloudflared tunnel --no-autoupdate --url http://localhost:2005
    API_PUBLIC=$(tunnel_url api); ENGINE_PUBLIC=$(tunnel_url engine)
    printf 'EXPO_PUBLIC_API_URL=%s\nEXPO_PUBLIC_ENGINE_URL=%s/\n' "$API_PUBLIC" "$ENGINE_PUBLIC" > apps/mobile/.env.local
    echo "  API     $API_PUBLIC"; echo "  engine  $ENGINE_PUBLIC"
  fi
  METRO_PUBLIC=$(tunnel_url metro)
  export EXPO_PACKAGER_PROXY_URL="$METRO_PUBLIC"
  echo "  metro   $METRO_PUBLIC"
fi

step "App — scan the QR code with your iPhone camera (Expo Go), then 'Continue as guest'"
cd apps/mobile && npx expo start --go --port 8081
