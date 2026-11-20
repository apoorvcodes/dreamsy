# Maintaining Dreamsy

How the maintainers review, label, merge and release. Contributors start with [CONTRIBUTING.md](CONTRIBUTING.md).

## Review

- Nothing merges red, and nothing merges untested: the PR says what was run, and CI (`ci`, plus `engine` when the engine,
  format, kits or games change) is green.
- Read the diff of a first-time contributor's PR before approving its workflow run. Be extra careful with changes to
  `.github/workflows/`.
- Review in numbered points. For small leftovers, push a commit onto the contributor's branch rather than rewriting
  theirs.
- Close unverified, end-to-end agent PRs with a link to "AI-assisted contributions" in CONTRIBUTING.md.

## Merge method

Squash and merge only. The PR title becomes the commit message, so fix the title before merging. Merge with
`gh pr merge N --squash --match-head-commit <sha>` so nothing merges that changed after review.

## Labels

| Label | Use |
|---|---|
| `area:engine` | C++ engine, `dreamsy-tool`, web player |
| `area:site` | website and editor (`apps/site`) |
| `area:harness` | the AI builder and CLI (`harness`) |
| `area:mcp` | `dreamsy-mcp` and the hosted MCP endpoint |
| `area:cloud` | the Worker (`services/cloud`) |
| `area:kits` | genre and foundation kits, example games |
| `needs-repro` | a bug nobody can act on yet; ask a specific question |
| `scope: undecided` | a good idea, but not sure it belongs |
| `good first issue` | small, well-described, no deep context needed |
| `skip-changelog` | no CHANGELOG line needed (CI, docs typos, refactors) |
| `release` | the release PR |

The bug form's "Where" dropdown maps to the `area:` labels; apply them while triaging. Every new issue gets a label, a
milestone or `scope: undecided`, and a reply the same day. Close with a reason: fixed by #N, duplicate of #N, or won't
fix and why.

## Changelog

Every PR adds one line under `## Unreleased` in [CHANGELOG.md](CHANGELOG.md), written for users, or carries
`skip-changelog`. Reword it before merging if needed.

## Releases

SemVer, at 0.x for now: a patch release is fixes only, a minor release has features or behaviour changes.

1. Open a PR titled `Release X.Y.Z` with the `release` label. It is the only place versions change:
   - rename `## Unreleased` to `## vX.Y.Z — YYYY-MM-DD` and add a new empty `## Unreleased` above it;
   - bump `packages/mcp/package.json` if `dreamsy-mcp` changes, and refresh the lockfile.
2. After it merges, tag `main`: `git tag -a vX.Y.Z -m "vX.Y.Z" origin/main && git push origin vX.Y.Z`.
3. Create the GitHub Release from the tag, with that CHANGELOG section as the notes.
4. If `dreamsy-mcp` changed: `pnpm -C packages/mcp pack:all` and publish it (by hand for now; npm 2FA on).

Never move or delete a published tag; ship a patch release instead.

## Repository settings (owner checklist)

These need admin access and are not in the repo. Check them before launch and after any change to this file.

- [ ] **Private vulnerability reporting** on (Settings → Code security). SECURITY.md and the issue form link to it.
- [ ] **Secret scanning** and **push protection** on.
- [ ] **Dependabot alerts** and **Dependabot security updates** on (`.github/dependabot.yml` covers version updates).
- [ ] **Ruleset on `main`**: require a pull request, require the `ci` status check (`typecheck + test`), block force
      pushes and deletion; owners may bypass in an emergency. Don't require the path-filtered `engine` jobs: they never
      report on PRs that don't touch the engine.
- [ ] **Tag ruleset on `v*`**: block updates and deletion.
- [ ] **One merge method**: allow squash merging only; turn off merge commits and rebase merging. Turn on "Automatically
      delete head branches".
- [ ] **Actions**: require approval for workflows from all outside collaborators; default `GITHUB_TOKEN` permissions
      read-only.
- [ ] **Discussions** on (the issue form and CONTRIBUTING.md send questions there), with Q&A and Show and tell
      categories.
- [ ] **Labels** above created.
- [ ] **Topics**: `game-engine`, `gamedev`, `2d`, `luau`, `mcp`, `ai`, `cpp`, `webassembly`.
- [ ] **Website** link in the About box: https://dreamsy.dev.

Check rulesets with `gh api repos/apoorvcodes/dreamsy-mono/rulesets`.
