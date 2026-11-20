# Dreamsy site

The public website and the game editor (Next.js).

```sh
pnpm -C apps/site dev        # http://127.0.0.1:3100
```

- `/`, `/library`, `/kits`, `/docs`: the marketing site.
- `/editor`: make and edit games. Builds run the harness on this machine (`harness/src/cli/editor.ts`) with the API key
  you add in the editor, which is kept in an httpOnly cookie and never saved to disk. Games are folders in
  `games/mine` (set `DREAMSY_GAMES_DIR` to change it). Needs `engine/build/release/dreamsy-tool`; the preview also needs
  the web player (`engine/build/web`). See docs/harness.md §5.5.

## Deploying

Set `SITE_URL` (for example `https://dreamsy.example`) so canonical links, Open Graph images, `robots.txt` and
`sitemap.xml` use the real domain; it falls back to `http://127.0.0.1:3100`. Regenerate the OG image and app icons
with `pnpm -C apps/site brand` after changing the logo (`components/Logo.tsx`) or the headline.

Library cards use pre-cropped 4:3 WebP screenshots at 480 and 720 pixels wide in `public/games`.
The Fly image prepares versioned player assets at build time. Next.js negotiates gzip for the
player JavaScript and WASM. For a local build
with the same asset URLs and caching, run `node apps/site/scripts/prepare-player.mjs engine/build/web`
from the repository root after building the engine's web player, then restart the site.

## Cloud save and the library

Every successful build, sprite or music edit and restore is also saved to the Dreamsy cloud (`services/cloud`, default
`https://dreamsy-cloud.tofu-develops.workers.dev`; override with `DREAMSY_CLOUD_URL`, turn off with `DREAMSY_CLOUD=0`).
Games belong to this computer's save key in `~/.dreamsy/config.json`, shared with the MCP server and the harness CLI, so
games made in any of them show up together. `~/.dreamsy/games.json` maps local game folders to cloud games.

The games page has two tabs: **Your games** (local, with cloud and published badges, plus cloud games not on this
computer) and **Library** (everything published, with search, the total, Download .dpk and Remix in editor). The
workspace's **Publish** button asks for a public creator name and link and an optional private email.
