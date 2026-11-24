## Summary

<!-- Briefly explain what this PR changes and why. -->

Closes #

## Verification

- [ ] `./bin/build` compiles cleanly with `-Wall -Wextra` (0 warnings)
- [ ] `./bin/test` passes (`ClothboundTests`, `TestOnlineIngest`, `OpdsTests`)
- [ ] `./bin/translate check` reports all 50 catalogs valid
- [ ] `qmllint` passes on modified QML files (if touching `src/ui/`)
- [ ] Zero `@Qt_6_PRIVATE_API` symbols in `build/clothbound`
- [ ] `node site/build.mjs` succeeds (if touching `site/` or `docs/clothbound-prototype.html`)

## Screenshots / Notes

<!-- Attach before/after screenshots for UI changes. -->
