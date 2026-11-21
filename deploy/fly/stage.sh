#!/bin/sh
# Stage what the Fly image needs that isn't in git: the Linux engine binary, the web player and the seed games.
set -e
cd "$(dirname "$0")/../.."
A=deploy/fly/artifacts
rm -rf "$A" && mkdir -p "$A/web/kits" "$A/seed/games"
cp packages/mcp/prebuilt/linux-x64/dreamsy-tool "$A/dreamsy-tool"
cp engine/build/web/dreamsy-player.html engine/build/web/dreamsy-player.js engine/build/web/dreamsy-player.wasm "$A/web/"
if [ -f engine/build/web/dreamsy-player.data ]; then
  cp engine/build/web/dreamsy-player.data "$A/web/"
fi
cp engine/build/web/kits/*.dpk "$A/web/kits/"
if [ -d games/mine ]; then
  for g in games/mine/*/; do cp -R "${g%/}" "$A/seed/games/"; done
  [ -d games/mine/.editor ] && cp -R games/mine/.editor "$A/seed/games/.editor"
  python3 - "$PWD/games/mine" > "$A/seed/links.json" <<'PY'
import json, os, sys
src = sys.argv[1]
try:
    links = json.load(open(os.path.expanduser("~/.dreamsy/games.json")))
except Exception:
    links = {}
print(json.dumps({"/data/games/" + p[len(src) + 1:]: cid for p, cid in links.items() if p.startswith(src + "/")}))
PY
fi
echo "staged: $(du -sh "$A" | cut -f1)"
