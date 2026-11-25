# Contributing to Clothbound

Contributions — bug fixes, packaging improvements, translations, and thoughtful feature PRs — are welcome.

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
./bin/build             # CMake Release build -> build/clothbound and build/clothbound-mcp
./bin/test              # builds & runs offscreen QtTest suites (clothbound_tests, tst_concurrency, tst_webengine_e2e)
./bin/test --asan       # runs the test suite with AddressSanitizer + UBSan
./bin/test --tsan       # runs tst_concurrency with ThreadSanitizer
./bin/bench             # runs C++ core micro-benchmarks and headless startup/memory benchmarks
./bin/translate check   # validates all 50 translation catalogs
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
cd site && node build.mjs
```

## Pull requests

- Keep PRs focused and include regression tests in `tests/` for bug fixes or new behavior.
- For UI changes, attach a screenshot (and test both dark/light or rounded/sharp modes when relevant).
- Check that CI (`build-and-test` and `Validate clothbound.app`) passes cleanly and `qmllint` reports zero warnings on edited QML files.
