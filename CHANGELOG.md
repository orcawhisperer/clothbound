# Changelog

All notable changes to Clothbound are documented in this file.

## [0.5.1] — 2026-11-25

Universal Linux Flatpak release (`app.clothbound.Reader`) bringing Clothbound to Ubuntu, Fedora, Debian, Linux Mint, Pop!_OS, openSUSE, and SteamOS alongside Arch Linux and Omarchy, with live XDG Desktop Portal light/dark theming, enriched bug-report diagnostics, Qt 6.10 compatibility, and zero `qmllint` warnings.

### Added
- **Universal Linux Flatpak (`app.clothbound.Reader`)**: Added Flathub-ready manifest (`org.kde.Platform//6.10` + `io.qt.qtwebengine.BaseApp//6.10`), validated AppStream metainfo, CMake `-DCLOTHBOUND_APP_ID` install support, automated `clothbound-<ver>-x86_64.flatpak` release bundles with unified `SHA256SUMS`, and universal Flatpak/pacman support in `https://clothbound.app/install`.
- **Live Desktop Light & Dark Theme Across Linux**: Extended `Theme` to follow `org.freedesktop.portal.Settings` (`org.freedesktop.appearance/color-scheme`) asynchronously over D-Bus and `QStyleHints::colorSchemeChanged` so GNOME, KDE Plasma, COSMIC, and Flatpak sessions switch light/dark live without GTK3 (Omarchy palettes retain full precedence when installed).
- **Enriched Diagnostics & One-Click Report Issue**: Added zero-PII OS/distro (reading `/run/host/os-release` in Flatpak), Qt version, display platform, and Flatpak sandbox ID to **Settings › About**, plus a **Report issue** button (translated across all 50 languages) that copies diagnostics and pre-fills the GitHub bug report template.
- **Flatpak-Aware Sandbox Integration**: Honored `FLATPAK_ID` for Wayland `app_id`, window icon, MPRIS bus name/desktop entry, D-Bus StatusNotifierItem tray registration, and **Settings › Intelligence › Copy MCP config** (`flatpak run --command=clothbound-mcp app.clothbound.Reader`).
- **OS-Aware Install Experience on `clothbound.app`**: Added a zero-tracking platform selector (*Arch / Omarchy*, *Flatpak*, *Build from source*, *macOS & Windows roadmap*) and footer Report Issue link.

### Fixed
- **Qt 6.10 Build Compatibility**: Gated `NO_GENERATE_AOT_VALIDATION` in `CMakeLists.txt` on Qt ≥ 6.11 and switched `QJsonObject::cbegin()/cend()` to `constBegin()/constEnd()` in `opdsparser.cpp` and `tst_core_formats.cpp`.
- **Reader `goTo` Name Collision & Layout Warnings**: Removed the duplicate `goTo` alias colliding with `function goTo(target)` in `EpubReader.qml`, switched `WhereItRuns.qml` layout-managed status dots from `width`/`height` to `Layout.preferredWidth`/`Layout.preferredHeight`, and resolved all remaining `qmllint` diagnostics down to 0 warnings and 0 infos.

### Infrastructure
- **Parallel Cached CI**: Split Arch Linux CI into 3 parallel `ccache`-backed jobs (`arch-release`, `arch-asan`, `arch-tsan`) running concurrently alongside `flatpak`.

## [0.5.0] — 2026-11-25

Architecture, reading craft, and reliability release for Linux, Wayland, and Omarchy — featuring a standalone WebEngine-free `clothbound-mcp` binary, CMake + AOT QML compilation, symmetric int8 semantic embeddings, continuous Library audio playback, smooth Companion docking, and 100% coverage across all 50 shipped languages.

### Added
- **Standalone WebEngine-Free `clothbound-mcp`**: Dedicated lean stdio JSON-RPC 2.0 MCP server and read-only CLI (`clothbound-mcp [mcp | list | search | notes]`) with zero Qt WebEngine, Qt Quick, Qt QML, or Qt Multimedia linkage (~31 ms startup; strictly read-only SQLite via `QSQLITE_OPEN_READONLY` and `PRAGMA query_only=ON`).
- **Continuous Background Listening in the Library**: Returning to the Library (`← Library` or `Esc`) while narration is playing keeps audio running seamlessly with a floating Library mini-player (clickable cover/title returns straight to the book; `P` / `Shift+P` / `Shift+←/→` and media keys work in the Library).
- **PDF Viewport Tiling & Cursor-Focal Zoom**: Crisp tiled high-DPI PDF rendering with cursor-anchored wheel zoom, asynchronous Lightbox & Snip capture, and full text selection (`IBeamCursor` drag & double-click) across all fit modes including Fit-to-Width (`Middle-drag` or `Alt+Left-drag` pans in 2D).
- **Explicit Footer Page Buttons**: Added unobtrusive `‹` / `›` Previous & Next page buttons flanking the page count in the bottom footer alongside keyboard, wheel/trackpad, and scrubber navigation.
- **Symmetric `int8` Vector Quantization**: `PassageIndex` stores normalized `int8` quantized embeddings (~4× smaller on disk and in RAM) with SIMD dot-product scoring and automatic legacy float32 compatibility.
- **SQLite v5 Schema & Async Command Palette**: Added `ON DELETE CASCADE` integrity across `library.sqlite` (v5 migration), shared `WorksModel` store, and off-GUI-thread `CommandPalette` search so typing stays instant on large libraries.

### Changed
- **CMake 3.24+ & `qmlcachegen` AOT Build**: Migrated from qmake to CMake with an 8-module zero-cycle DAG, ahead-of-time QML compilation, hardened ELF flags (`-fstack-protector-strong`, `_FORTIFY_SOURCE=2`, full RELRO `BIND_NOW`), zero `@Qt_6_PRIVATE_API` linkage, and externalized `usr/share/clothbound/i18n/*.json` catalogs.
- **Intentional Desktop Page Turns**: Removed accidental single-click turning on the left/right 30% of EPUB and PDF pages so clicking or double-clicking words near edges never flips pages unexpectedly.
- **Smooth GPU-Lockstep Companion Docking**: Opening, closing, or drag-resizing the Companion drawer glides smoothly without mid-slide reflow jitter on both PDFs and EPUBs.
- **100% Translation Coverage Across 50 Languages**: Completed all 50 interface language catalogs (`1,524` strings, `0` missing, `0` orphan) and added cross-script validation + `--strict` CI checks to `bin/translate`.
- **Faster, More Resilient OPDS Catalogs**: Added a 5-minute 32-entry in-memory LRU feed cache for instant back-and-forth browsing, IPv4-first DNS pinning, 8 s per-IP connect/TLS failover, and transient timeout retry.

### Fixed
- **Audio Output Crash (`SIGSEGV`)**: Fixed a pipewire/Qt6 `QAudioSink` synchronous `stateChanged` use-after-free in `AudioOutputPump::resetPlayback()` and made `SpeechPlayer` zero-suppression ThreadSanitizer-clean.
- **Dark-Theme Legibility for Calibre EPUBs & Colored PDF Ink**: Overrode `<body class="calibre">` `.calibre { color: #000 }` and inline author colors on dark themes (`Omarchy`, `Dusk`, `Night`), and upgraded `PdfPages::recolor` to hue-preserving lightness inversion with a contrast floor for dark saturated ink.
- **High-Contrast Right-Click Menus**: Explicitly bound the active reading theme palette on reader context, citation, and overflow menus so Sepia/Paper popups stay readable on dark desktops.
- **Reliable Top-Edge Chrome Reveal**: Moving the pointer into the top 56 px edge now consistently reveals the top bar in every mode (including while listening and in zoomed/Fit-to-Width PDFs).
- **Marks Panel Empty-Tab Layout & Sleep Timer Alignment**: Prevented the `All | Highlights | Notes | Bookmarks` tab bar and export footer from jumping vertically on empty tabs, and centered the moon icon inside the ListenBar sleep timer button.
- **Boundary-Silent Page-Turn Sound**: Stopped pressing next/previous past the end or start of a book (including 2-page PDF spreads and scroll bounds) from playing unnecessary page-flip audio.
- **Security & Parser Hardening**: Hardened `BookScheme` CSP & XML/SVG sanitization, `UrlGuard` DNS-rebinding & SSRF checks, W3C mark anchoring, and added live headless Chromium E2E, TSan concurrency, and 6 libFuzzer parser test suites (`289+` automated tests).

## [0.4.1] — 2026-11-25

Performance, scrolling, narration stability, and desktop reliability release for Linux, Wayland, and Omarchy.

### Performance
- **Faster Cold Start (~30% faster)**: Deferred Qt WebEngine profile & child-process creation (`LazyWebProfile`), lazy-loaded `SettingsView`, `Welcome`, `CommandPalette`, `HelpOverlay`, reader panels, and library modals on first use, and deferred `QMediaDevices` enumeration until audio actually plays.
- **Smoother Library & Covers**: Added off-thread asynchronous `CoverProvider` (`image://cover/...`), eliminated redundant SQL queries during library map enrichment (`Library::enrichBookMap`), and fixed a `CoverImage` binding loop on startup.
- **Heavy PDF Optimization**: Fused PDF page alpha compositing and theme LUT recoloring into a single pass, added a 32 MiB raw-render cache (instant theme switches on scanned JPEG2000/JBIG2 PDFs), added hi-res downscale cache reuse when docking Companion, cached per-page PDF sizes & links, and lowered background prefetch thread priority.
- **Reader & Companion Responsiveness**: Decoupled Companion docking layout until its slide animation settles, cached `fitBlocks()` whitespace scans and skipped no-op `<style>` rewrites in `reader.js`, and added `CLOTHBOUND_PROFILE=1` startup/frame-jank telemetry plus `./bin/bench` micro-benchmarks.

### Improved
- **Effortless Two-Finger Trackpad Scrolling**: Added velocity-adaptive trackpad gain and kinetic momentum coasting (with flick compounding, instant catch-on-touch, native `Qt.ScrollMomentum` passthrough, and `reducedMotion` support) to `SmoothWheel`, and attached `SmoothWheel` to PDF continuous scroll and zoomed pan views.
- **Dedicated High-Priority Audio Thread**: Moved `QAudioSink`, `QMediaDevices`, and the 15 ms feed loop onto a dedicated `QThread::HighPriority` audio output thread (`AudioOutputPump`) so UI transitions and page renders never starve narration playback.
- **Native Wayland System Tray**: Replaced `Qt.labs.platform` `SystemTrayIcon` with a native D-Bus `org.kde.StatusNotifierItem` + `com.canonical.dbusmenu` implementation (zero `QtWidgets` dependency, zero private Qt ABI).
- **Self-Healing Cache Recovery**: Deleting `~/.cache/clothbound` while Clothbound is running now recovers gracefully across audio cache, full-text SQLite index (`LibraryIndex::ensureDatabase()`), and book cover extraction.

### Fixed
- **Clean Quit While Playing**: Closing the window (`Super+W` / `Ctrl+W` / WM Close) or pressing `Ctrl+Q` / `Q` now always stops narration immediately and exits Clothbound cleanly (background listening remains explicitly available via the ListenBar tray button).
- **Panel Shortcuts**: `A` (Appearance), `T` (Contents), `M` (Marks), and `V` (Voices) now toggle open and closed symmetrically alongside `C` (Companion).
- Eliminated `setLifecycleState: Active -> Frozen` warnings on hidden WebEngine views, silenced noisy `qt.pdf.bookmarks` invalid-zoom log spam by default, and fixed cross-thread `QTimer` affinity on `AudioOutputPump`.

## [0.4.0] — 2026-11-24

Clothbound's v0.4.0 public launch release: native Qt6 desktop craft for Linux, Wayland, and Omarchy, bringing online-to-offline acquisition, Study Companion, academic PDF research tools, and local MCP integration together in one calm reader.

### Added
- **Online-to-Offline Ingestion (`Ctrl+Shift+U`)**:
  - Save web articles directly as clean offline EPUBs onto a dedicated **Articles** shelf (`ArticleExtractor`, `EpubWriter`, `WebImporter`, SSRF-hardened `UrlGuard`).
  - Direct URL downloads for EPUB, PDF, arXiv, alphaXiv, Hugging Face Papers, ar5iv, and pages exposing `citation_pdf_url`.
- **OPDS 1.2 & 2.0 Catalogs (`Ctrl+Shift+O`)**:
  - Built-in public-domain catalogs (Standard Ebooks, Project Gutenberg, Open Library, arXiv, Feedbooks) plus custom OPDS servers (Calibre-Web, Kavita, Komga) with keyring-stored credentials.
  - Full OPDS 2.0 standalone publication manifests, edition ranking (`open-access` first), miniature cover previews, and reliable HTTP/1.1 downloads.
- **PDF Preset View Modes (`0` Fit page · `W` Fit width · `1` Actual)**:
  - Live auto-refit on window resize and docked Companion toggle, smooth custom zoom handoff (`50%–400%`), global & per-book persistence, and single-press spread navigation on `←`/`→`.
- **Academic PDF Craft**:
  - **Visual Snip & Explain (`X`)** drag-region overlay (`Explain`, `Copy LaTeX`, `Extract Table`, `Ask…`) with Gemini and local Ollama vision models.
  - **Zero-Jump Quick-Look Peek Card** for clickable internal figures & citations (`Dismiss` / `Jump to page N →`).
  - Automatic **arXiv / DOI** detection pill in the reader header and Book Details with one-click **BibTeX / APA** citation copy.
- **Adaptive Story & Study Companion (`C`)**:
  - `Story | Study` switcher (`Ask / Previously / Who's who / Pictures` vs `Ask / Brief / Glossary`), Smart Scope (`Up to here` · `Whole book` · `This shelf`), offline LaTeX math rendering, response read-aloud toggle, and left-edge drag resize (double-click snaps to 384 px).
- **Local MCP Server (`clothbound mcp`)**:
  - Read-only JSON-RPC 2.0 stdio server exposing 6 library tools (`list_books`, `search_library`, `search_passages`, `get_book_outline`, `read_passage`, `list_marks`) plus one-click `Copy MCP config` in **Settings › Intelligence**.
- **Desktop Craft & Polish**:
  - **Corners (`Match desktop | Rounded | Sharp`)** override in **Settings › General** alongside live Hyprland rounding detection and **Light / Dark** modes.
  - **Settings** footer button in the library sidebar (`Icons.gear` + live intelligence status), overflow menu, and `Ctrl+K` Command Palette.
  - Adaptive 44 px / 86 px floating **ListenBar**, simplified 2-tab **Narrator** sheet, subtle hardcover **Page-turn sound** (auto-mutes while listening), live miniature reading preview with aligned drop cap in **Settings › Reading**, and 50 shipped interface languages.

### Fixed
- Kept the top-right bookmark dog-ear unobstructed with a clean 52 px right tool inset and let `C` toggle Companion closed without getting trapped in the text composer.
- Prevented transparent reader chrome and bottom progress bar from clashing with zoomed text via an adaptive paper-wash backdrop.
- Disabled HTTP/2 on OPDS and remote cover requests to eliminate Open Library / Internet Archive `Data on closed stream` errors.
- Clamped remaining hardcoded pill/shadow radii and rounded the Welcome illustration pane cleanly when Corners is set to Rounded or Sharp.
