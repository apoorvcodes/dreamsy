#!/bin/sh
set -e
mkdir -p /data/games /data/dreamsy
if [ -z "$(ls -A /data/games 2>/dev/null)" ] && [ -d /app/deploy/fly/artifacts/seed/games ]; then
  cp -R /app/deploy/fly/artifacts/seed/games/. /data/games/
  [ -f /app/deploy/fly/artifacts/seed/links.json ] && [ ! -f /data/dreamsy/games.json ] && cp /app/deploy/fly/artifacts/seed/links.json /data/dreamsy/games.json
  echo "seeded games: $(ls /data/games | wc -l)"
fi
# Hosted MCP (/mcp); it must not see the editor's save key.
mkdir -p /data/mcp
(cd /app/harness && while true; do
  env -u DREAMSY_SAVE_KEY DREAMSY_MCP_DIR=/data/mcp DREAMSY_MCP_HOST=127.0.0.1 DREAMSY_MCP_TRUST_FLY=1 DREAMSY_MCP_PORT=8090 node --import tsx src/mcp/http.ts || true
  echo "dreamsy mcp exited; restarting"
  sleep 2
done) &

cd /app/apps/site
exec node_modules/.bin/next start -p "${PORT:-8080}" -H 0.0.0.0
