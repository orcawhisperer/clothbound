# Changelog

All notable changes to Clothbound are documented in this file.

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
