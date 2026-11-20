# dreamsy-mcp

Make 2D and 2.5D games with your own AI assistant. `dreamsy-mcp` gives any MCP client (Claude Code, Claude Desktop,
Cursor, …) the [Dreamsy](https://github.com/apoorvcodes/dreamsy) game engine as tools: start from one of 21 genre
kits, edit scenes and Luau scripts, draw pixel sprites, playtest headless, check and export a game you can share.

It runs on your machine. Your assistant's own model does the building, so there is no Dreamsy account, key or server.

Can't run a local process (ChatGPT, Claude.ai, a phone)? Use the hosted server instead: add
`https://dreamsy.dev/mcp` as a remote MCP server / custom connector. Same tools, a private id per game,
and `publish_game` gives a `dreamsy.dev/play` link.

## Install

```sh
claude mcp add dreamsy -- npx -y dreamsy-mcp
```

Needs Node 20+. The first run downloads the package and the engine binary for your machine; nothing to build.

**Claude Desktop** (`claude_desktop_config.json`), **Cursor** (`~/.cursor/mcp.json`), **Windsurf**
(`~/.codeium/windsurf/mcp_config.json`), **Gemini CLI** (`~/.gemini/settings.json`):

```json
{ "mcpServers": { "dreamsy": { "command": "npx", "args": ["-y", "dreamsy-mcp"] } } }
```

**VS Code** (`.vscode/mcp.json`):

```json
{ "servers": { "dreamsy": { "type": "stdio", "command": "npx", "args": ["-y", "dreamsy-mcp"] } } }
```

**Codex CLI** (`~/.codex/config.toml`):

```toml
[mcp_servers.dreamsy]
command = "npx"
args = ["-y", "dreamsy-mcp"]
```

Any other MCP client: run `npx -y dreamsy-mcp` as a stdio server. Then ask something like
“build a cozy frog platformer with dreamsy”.

Games are saved as folders in the directory the server starts in. Set `DREAMSY_GAMES_DIR` to keep them elsewhere:

```sh
claude mcp add -e DREAMSY_GAMES_DIR=$HOME/dreamsy-games dreamsy -- npx -y dreamsy-mcp
```

## Tools

| Tool | What |
|---|---|
| `dreamsy_options` | kits, art styles and style packs to start from |
| `create_game` | a new game from a kit's example, or empty |
| `dreamsy_guide` | the format, edit ops, entity catalog, Luau API, style and kit docs for a game |
| `game_outline`, `read_files`, `get_entities` | look at a game |
| `apply_ops`, `draw_sprite` | edit a game; a batch applies only if the package stays valid |
| `playtest` | run the game headless, with screenshots |
| `check_game` | the release check: valid package and a smoke run with no script errors |
| `export_game` | write a `.dpk` file you can play and share; also saves it to the Dreamsy cloud |
| `publish_game` | put the game in the public Dreamsy library and get a `dreamsy.dev/play/<id>` link anyone can open |

Games save to the Dreamsy cloud under a save key kept in `~/.dreamsy/config.json` (no account; the same key the Dreamsy
editor uses on this computer). Set `DREAMSY_CLOUD=0` to keep everything local.

## Platforms

Prebuilt engine binaries: macOS 12+ (Apple Silicon and Intel) and Linux (x64 and arm64, glibc 2.35+, e.g. Ubuntu
22.04+). On Windows, use WSL. Elsewhere, build `dreamsy-tool` from source (see the repo's `engine/README.md`) and set
`DREAMSY_TOOL` to its path.

Playtest screenshots use Chromium, downloaded automatically the first time a screenshot is needed (about 150 MB, once,
into Playwright's cache). Set `DREAMSY_AUTO_CHROMIUM=0` to skip it; playtests still report errors, scenes, controls and
sound without screenshots.

## License

[PolyForm Noncommercial 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0): free for personal, hobby,
research, educational and nonprofit use; no commercial use. Bundled third-party software and its licenses are listed in
`THIRD-PARTY-NOTICES.txt`.
