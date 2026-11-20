# Changelog

## v0.1.0 — 2026-11-19 ✦ first public release

**Engine**
- Native C++20 engine for 2D and 2.5D games (side, top-down, oblique, isometric) replacing the TypeScript engine:
  Luau scripting, Box2D physics, one core for WebAssembly, iOS, Android and headless tools (`dreamsy-tool`).
- 21 genre and foundation kits with golden example games and headless tests.

**Builder (harness)**
- Router, agent with validated edit ops, SVG sprites, headless playtests with screenshots, final gate and art review.
- `compose_music`: a smaller model writes 3–5 chiptune tracks per game and reworks them on request.
- Saves every build to the Dreamsy cloud under a per-computer save key.

**Editor and site** (`apps/site`)
- Landing page, library, kit pages, docs, OG image and manifest.
- Editor: chat builds with a live timeline, game preview, Sprites (SVG editing), Music (in-browser chiptune player),
  files and versions, banners from real screenshots, Your games and Library tabs, publishing with creator credit.
- Bring your own Anthropic or OpenAI key (httpOnly cookie, never stored on disk).

**Cloud** (`services/cloud`)
- Cloudflare Worker + R2 + D1: accountless saves, content-addressed versions, publishing, remixes, library search, and
  hard cost and abuse limits.

**MCP**
- `npx -y dreamsy-mcp`: the engine as tools for Claude Code, Cursor, VS Code and other MCP clients, with prebuilt
  engines for macOS and Linux, cloud save and `publish_game`.

**License**
- PolyForm Noncommercial 1.0.0.
