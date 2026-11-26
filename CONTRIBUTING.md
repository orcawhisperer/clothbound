# Contributing to Clothbound

Contributions — bug fixes, packaging improvements, translations, and thoughtful feature PRs — are warmly welcome.

## License

Clothbound's shared core and Linux app are licensed under the **GNU General Public License v3.0 or later (`GPL-3.0-or-later`)** — see [LICENSE](LICENSE). Bundled typefaces (**Literata** and **Inter**) remain under the **SIL Open Font License 1.1 (`OFL-1.1`)** ([`src/fonts/OFL.txt`](src/fonts/OFL.txt), [`src/fonts/Inter-OFL.txt`](src/fonts/Inter-OFL.txt)), and small Linux desktop integration pieces originally adapted from Omacut and Hype retain their original MIT License notices.

## Contributor License Agreement (CLA)

Clothbound's shared core and Linux app will remain free and open source under **GPL-3.0-or-later** forever. To sustain independent development, Clothbound's author also intends to ship commercial / App Store native Apple editions built on the same core.

Because `GPL-3.0-or-later` is incompatible with Apple App Store distribution terms, external code contributions are accepted under this lightweight Contributor License Agreement:

- By submitting a pull request, patch, or other contribution to Clothbound, you confirm that your contribution is your original work (or that you have the legal authority to submit it) and grant Clothbound's author (**Vasantha Kumar**) a perpetual, worldwide, non-exclusive, no-charge, royalty-free, irrevocable copyright and patent license to use, modify, redistribute, and relicense your contribution — including in commercial and App Store native Apple editions.
- You keep full ownership and copyright of your own work, and your contribution remains available to the community in this repository under `GPL-3.0-or-later`.
- No separate paperwork is required: opening a pull request indicates your agreement to these terms. If you are contributing as part of your employment, please confirm you have your employer's permission to submit under this CLA.

## Trademark & Brand Notice

The name **"Clothbound"** and the Clothbound app icon (`pkgbuild/clothbound.svg`, `site/favicon.svg`) identify official releases from this project. Forks and modified builds are always welcome under `GPL-3.0-or-later`, provided they use a distinct name and app icon so users are not confused about which release is official.

## Design principles

Before proposing large changes, keep Clothbound's core invariants in mind:

- **Local-first & non-destructive**: User book files are read where they live and never modified.
- **Zero external CDNs**: Everything in the app and `clothbound.app` works offline without third-party font/script hosts.
- **Public Qt 6 API only**: Never link against `@Qt_6_PRIVATE_API` symbols so binaries stay stable across Qt6 updates.
- **Strict headless layering**: Keep `clothbound_headless` and `clothbound_mcp` (`src/mcp/`) completely free of `QtWebEngine`, `QtQuick`, `QtQml`, `QtMultimedia`, and `src/app/` includes so `clothbound-mcp` stays lean, fast, and strictly read-only.
- **Calm, keyboard-first UI**: Keep chrome unobtrusive, respect `theme.reducedMotion` and `theme.radius` (`Ui.radius(..., theme.radius)`), and guard single-key shortcuts when text fields have focus.

## Development setup

Clothbound builds with **CMake 3.24+** (C++20, Ninja or Unix Makefiles, Qt 6). On Arch Linux / Omarchy:

```sh
./bin/build                    # CMake Release build -> build/clothbound and build/clothbound-mcp
./bin/test                     # builds & runs offscreen QtTest suites (clothbound_tests, tst_concurrency, tst_webengine_e2e)
./bin/test --asan              # runs the test suite with AddressSanitizer + UBSan
./bin/test --tsan              # runs tst_concurrency with ThreadSanitizer
./bin/bench                    # runs C++ core micro-benchmarks and headless startup/memory benchmarks
./bin/translate check --strict # validates all 50 translation catalogs (0 missing, 0 orphan)
```

### Standalone `clothbound-mcp` binary

`./bin/build` produces both `build/clothbound` (full GUI + CLI) and `build/clothbound-mcp` (WebEngine-free read-only stdio MCP server and headless CLI):

```sh
./build/clothbound-mcp [mcp]                 # read-only stdio JSON-RPC 2.0 MCP server (default)
./build/clothbound-mcp list [--json]         # list books (read-only)
./build/clothbound-mcp search <query> [--json] # read-only FTS5 + lexical search
./build/clothbound-mcp notes [book] [--json] # export marks as Markdown or JSON
```

If you edit `site/` or `docs/clothbound-prototype.html`:

```sh
node site/build.mjs
```

## Pull requests

- Keep PRs focused and include regression tests in `tests/` for bug fixes or new behavior.
- For UI changes, attach a screenshot (and test both dark/light or rounded/sharp modes when relevant).
- Check that `./bin/build`, `./bin/test`, `./bin/translate check --strict`, and `qmllint` (0 warnings on edited QML files) pass cleanly, and that CI (`build-and-test` and `Validate clothbound.app`) is green.
