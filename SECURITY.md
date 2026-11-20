# Security

## Reporting a vulnerability

Please **don't open a public issue**. Report it privately through
[GitHub security advisories](https://github.com/apoorvcodes/dreamsy/security/advisories/new) with steps to reproduce.
We aim to reply within a few days and will credit you in the fix unless you'd rather stay anonymous.

## Scope

Especially interesting:

- **The cloud Worker** (`services/cloud`): ways around save-key ownership, the upload limits or the storage budget,
  reading another owner's games, or getting private publish data (emails).
- **The editor** (`apps/site`): reading or leaking API keys (kept in httpOnly cookies, never written to disk), path
  traversal in the game file routes, unsafe SVG getting through the sprite editor.
- **The engine** (`engine`): crashes or memory errors from a malicious `.dpk` package (packages are untrusted input; the
  engine is fuzzed, see engine/README.md).
- **`dreamsy-mcp`**: escaping the games directory or the save key in `~/.dreamsy`.

## Supported versions

Fixes land on `main` and in the latest `dreamsy-mcp` release on npm.
