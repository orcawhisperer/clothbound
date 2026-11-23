---
name: dogear
description: |
  Read, inspect, and work with the user's ebook library through Dogear, the
  Omarchy ebook reader. Use for any request about their EPUB or PDF books:
  what a book is, its contents, opening a book, or the reading library.
---

# Dogear

Dogear is the user's EPUB & PDF ebook reader and audiobook player on Omarchy.
Its commands start fast without a display; add `--json` for machine-readable
output. Errors go to stderr and the exit code is 1.

## CLI commands

- `dogear add <folder-or-book>... [--json]`: add watched folders or book files
  to the library (files are never changed).
- `dogear info <book> [--json]`: title, author, edition (editor, publisher,
  year), series, language, description, chapters, pages, and table of contents.
- `dogear list [--json]`: the books in the library, with reading progress.
- `dogear notes <book> [--json]`: the user's highlights, notes and bookmarks
  in a book (named by its file or a few words of its title), as Markdown.
- `dogear open <book>`: open the book in the reader, where the user left off
  (or bring back the Dogear already running, with that book).
- `dogear voices [list|install|remove|bench] [--json]`: the built-in voices
  (free, offline: Kokoro, Pocket TTS, Piper), which are installed, and how fast
  this computer makes them.
- `dogear ai [status|local] [--json]`: whether Gemini is set up, what it has
  cost this month, where text work runs, and the model servers found on this
  computer (Ollama, llama.cpp, LM Studio, MLX, vLLM).

## Reader capabilities (v0.3.0+)

- **Command Palette (`Ctrl+K`)**: fuzzy search across all books, current book
  chapters, cross-book highlights & notes, page/percentage jumps, and actions.
- **Library power tools**: custom collections/shelves, status & format filters
  (Reading, Unread, Finished, EPUB, PDF, Series), series ordering, grid/list
  views (`V`), watched-folder management, and reading stats/streaks.
- **EPUB & PDF reader**: column or scroll layout, theme-aware PDF rendering
  with margin cropping, resilient annotations with Obsidian/Readwise Markdown
  export, and word or meaning search.
- **X-Ray Companion (`C`)**: spoiler-free Ask, Define, Explain, Translate,
  Previously…, Who's who, and Illustrations.
- **Narration & Audiobooks (`P` / `V`)**: Gemini voices, offline built-in
  voices, Studio voices, Full Cast multi-voice narration, offline download, and
  M4B audiobook export with chapters.

Dogear never changes book files.
