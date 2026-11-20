# Contributing to Dreamsy ✦

Thanks for wanting to help make tiny games easier to make. Bug reports, kits, example games, docs and engine fixes are
all welcome.

## Before you start

- Read [docs/README.md](docs/README.md): the spec index and the map of the spec in `docs/`, which is the source of truth.
- For anything bigger than a small fix, open an issue first so we can agree on the approach.
- Be kind: this project follows the [Code of Conduct](CODE_OF_CONDUCT.md).

## Setup

```sh
pnpm install
(cd engine && cmake --preset debug && cmake --build --preset debug)   # engine with sanitizers
pnpm test && pnpm typecheck                                          # JS/TS packages
(cd engine && ctest --preset debug)                                   # engine tests
```

The editor runs with `pnpm -C apps/site dev` (http://127.0.0.1:3100/editor). See the [README](README.md) for the rest.

## House rules

- **The game package format is the central contract.** Don't change `format/` without updating docs/format.md §3.
- **2D and 2.5D only.** The engine rejects 3D packages.
- **`third_party/` is vendored.** Don't edit it; update a library by replacing it and noting the version.
- **C++ stays simple** (restricted style in docs/engine.md §14): many contributors come from TypeScript.
- **Every change keeps tests green** and adds tests for new behaviour. Kits need a golden example in
  `games/examples/kit-<kit>` and headless tests in `engine/tests/kits/`.
- **No secrets in code or commits.** API keys live in gitignored `.env` files or in the editor's httpOnly cookies.

## Pull requests

1. Branch from `main`, keep the change focused, and describe what and why in the PR.
2. Run `pnpm test`, `pnpm typecheck` and the engine tests you touched.
3. Screenshots or a short clip help a lot for anything visual (editor, site, games).

## License of contributions

Dreamsy is under the [PolyForm Noncommercial License 1.0.0](LICENSE). By opening a pull request you agree that your
contribution is licensed under the same terms, and that the maintainers may also offer it under other licenses (for
example, commercial licenses for companies). If you can't agree to that, please say so in the PR before it is merged.
