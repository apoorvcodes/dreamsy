# Contributing to Dreamsy ✦

Thanks for wanting to help make tiny games easier to make. Bug reports, kits, example games, docs and engine fixes are
all welcome.

## Before you start

- Read [docs/README.md](docs/README.md): the spec index (principles, layout, roadmap, known gaps) and the map of the spec
  in `docs/`, which is the source of truth. Section numbers (§N) are global across the spec files.
- For anything bigger than a small fix, open an issue first so we can agree on the approach.
- Questions go to [Discussions](https://github.com/apoorvcodes/dreamsy-mono/discussions), not issues.
- Be kind: this project follows the [Code of Conduct](CODE_OF_CONDUCT.md).

## Setup

You need Node with pnpm 10 (`corepack enable`), CMake, Ninja and a C++20 compiler.

```sh
pnpm install
(cd engine && cmake --preset debug && cmake --build --preset debug)   # engine with sanitizers
pnpm test && pnpm typecheck                                          # JS/TS packages (harness tests need dreamsy-tool)
(cd engine && ctest --preset debug)                                   # engine tests
```

The editor runs with `pnpm -C apps/site dev` (http://127.0.0.1:3100/editor). See the [README](README.md) for the rest
and [engine/README.md](engine/README.md) for engine builds and conformance checks.

## House rules

- **The game package format is the central contract.** Don't change `format/` without updating docs/format.md §3.
- **2D and 2.5D only.** No 3D. The format keeps 3D types for now, but the engine rejects 3D packages.
- **`third_party/` is vendored.** Don't edit it; update a library by replacing it and noting the version.
- **C++ stays simple** (restricted style in docs/engine.md §14.2): many contributors come from TypeScript.
- **One component at a time.** Keep a PR to one area; engine slices (§14.10) land one at a time with tests passing.
- **Every change keeps tests green** and adds tests for new behaviour. Kits need a golden example in
  `games/examples/kit-<kit>` and headless tests in `engine/tests/kits/`.
- **Docs change with behaviour.** The PR that changes behaviour updates the README, the spec and CHANGELOG.md.
- **No secrets in code or commits.** API keys live in gitignored `.env` files or in the editor's httpOnly cookies.

## AI-assisted contributions

Agents are welcome here; much of Dreamsy is built with them. The rules are the same as for any other change:

- **You own what you submit.** Understand every line, and be ready to explain and change it in review.
- **You must have run it.** Say in the PR what you ran (tests, the editor, a playtest, a device) and what you didn't.
- **Think through the edge cases** yourself; don't leave them to the reviewer.

PRs made end to end by an agent with no real testing, no verifiable check and no deliberate thought behind them are
closed without a detailed review.

## Pull requests

1. Branch from `main`, keep the change focused, and describe what and why in the PR.
2. Run `pnpm test`, `pnpm typecheck` and the engine tests you touched. CI runs the same.
3. Add a line under `Unreleased` in [CHANGELOG.md](CHANGELOG.md), written for users, or ask for the `skip-changelog`
   label.
4. Screenshots or a short clip help a lot for anything visual (editor, site, games).

## License of contributions

Dreamsy is source-available under the [PolyForm Noncommercial License 1.0.0](LICENSE). By opening a pull request you
agree that your contribution is licensed under the same terms, and that the maintainers may also offer it under other
licenses (for example, commercial licenses for companies). If you can't agree to that, please say so in the PR before it
is merged.
