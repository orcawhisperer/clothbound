# Contributing to Clothbound

Contributions — bug fixes, packaging improvements, translations, and thoughtful feature PRs — are welcome.

## Design principles

Before proposing large changes, keep Clothbound's core invariants in mind:

- **Local-first & non-destructive**: User book files are read where they live and never modified.
- **Zero external CDNs**: Everything in the app and `clothbound.app` works offline without third-party font/script hosts.
- **Public Qt 6 API only**: Never link against `@Qt_6_PRIVATE_API` symbols so binaries stay stable across Qt6 updates.
- **Calm, keyboard-first UI**: Keep chrome unobtrusive, respect `theme.reducedMotion` and `theme.radius` (`Ui.radius(..., theme.radius)`), and guard single-key shortcuts when text fields have focus.

## Development setup

On Arch Linux / Omarchy:

```sh
./bin/build             # compiles build/clothbound
./bin/test              # runs the offscreen QtTest suites
./bin/translate check   # validates all 50 translation catalogs
```

If you edit `site/` or `docs/clothbound-prototype.html`:

```sh
cd site && node build.mjs
```

## Pull requests

- Keep PRs focused and include regression tests in `tests/` for bug fixes or new behavior.
- For UI changes, attach a screenshot (and test both dark/light or rounded/sharp modes when relevant).
- Check that CI (`build-and-test` and `Validate clothbound.app`) passes cleanly.
