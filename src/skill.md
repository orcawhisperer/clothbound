---
name: clothbound
description: |
  Read, inspect, search, and cite the user's private ebook library through
  Clothbound, the Omarchy ebook reader. Use for any request about their EPUB
  or PDF books: listing books, searching passages, reading chapters/pages,
  inspecting outlines, reviewing highlights/notes, or opening a book.
---

# Clothbound

Clothbound is the user's EPUB & PDF ebook reader and audiobook player on Omarchy.
Its commands start fast without a display; add `--json` for machine-readable
output. Errors go to stderr and the exit code is 1.

## Local MCP server (`clothbound-mcp`)

Clothbound ships a standalone, WebEngine-free, strictly read-only local stdio
Model Context Protocol (MCP) server (`clothbound-mcp`, ~31 ms startup; also
available via `clothbound mcp`) so Antigravity, Claude Code, Cursor, Codex, and
other MCP clients can query the user's private library locally — nothing leaves
the computer.

Configure your MCP client with:

```json
{
  "mcpServers": {
    "clothbound": {
      "command": "clothbound-mcp",
      "args": []
    }
  }
}
```

(Or copy the exact config from **Settings → Intelligence → Local agents (MCP)**;
toggle **Enable local MCP server** there at any time.)

### MCP tools (strictly read-only)

1. **`list_books`**: optional `query`, `format` (`"epub"` | `"pdf"`),
   `collection`, `limit` (default `50`, max `200`) → returns books with `id`,
   `title`, `author`, `format`, `progress`, `pages`, `chapters`, `collections`,
   and `filePath`.
2. **`search_library`**: `query` (required), optional `book_id`, `limit`
   (default `15`, max `50`) → fast SQLite FTS5 full-text keyword search
   returning book title, chapter/page, and verbatim snippets.
3. **`search_passages`**: `query` (required), optional `book_id`, `collection`,
   `limit` (default `8`, max `25`) → hybrid/semantic passage search via
   `PassageIndex` (with automatic FTS5 + BM25 lexical fallback when embeddings
   are not built) returning verbatim cited passages (`book_id`, `title`,
   `chapter`, `page`, `text`, `score`).
4. **`get_book_outline`**: `book_id` (required) → book metadata plus full table
   of contents, chapters/spine, and print/PDF page map.
5. **`read_passage`**: `book_id` (required), `chapter` (0-based int) or `page`
   (1-based int), plus optional `page_end`, `offset`, and `max_chars` (default
   `8000`, hard cap `24000`) → exact verbatim EPUB or PDF text with a citation
   header.
6. **`list_marks`**: optional `book_id`, `query`, `limit` (default `50`, max
   `200`) → highlights, bookmarks, and notes with quoted text, chapter/page,
   color, and note text.

### MCP resources

- `clothbound://library/books`: all visible books in the library as JSON.
- `clothbound://book/<id>/outline`: metadata, TOC, and page map for a book.
- `clothbound://book/<id>/marks`: highlights, bookmarks, and notes for a book.

## CLI commands

- `clothbound-mcp [mcp | list | search | notes] [--json]`: standalone
  WebEngine-free read-only stdio MCP server (default) and fast headless query
  CLI (`list`, `search <query>`, `notes [book]`).
- `clothbound mcp`: run the local stdio MCP server via the main binary
  (JSON-RPC 2.0 on stdin/stdout; all diagnostics go to stderr).
- `clothbound search <query> [--book <id>] [--limit <n>] [--json]`: search
  across all books (or one book) via the local FTS5 full-text index.
- `clothbound passage <book_id> [--chapter <n> | --page <n>] [--page-end <n>] [--max-chars <n>] [--json]`:
  print exact verbatim text from an EPUB chapter or PDF page with a citation
  header.
- `clothbound add <folder|book|url>... [--json]`: add watched folders, local
  EPUB/PDF files, or remote URLs (web articles packaged offline as EPUBs,
  `arxiv:` / arXiv papers with metadata, and direct EPUB/PDF links). Local files
  are never changed.
- `clothbound info <book> [--json]`: title, author, edition (editor, publisher,
  year), series, language, description, chapters, pages, and table of contents.
- `clothbound list [--json]`: the books in the library, with reading progress.
- `clothbound notes <book> [--json]`: the user's highlights, notes and bookmarks
  in a book (named by its file or a few words of its title), as Markdown.
- `clothbound open <book>`: open the book in the reader, where the user left off
  (or bring back the Clothbound already running, with that book).
- `clothbound voices [list|install|remove|bench] [--json]`: the built-in voices
  (free, offline: Kokoro, Pocket TTS, Piper), which are installed, and how fast
  this computer makes them.
- `clothbound ai [status|local] [--json]`: whether Gemini is set up, what it has
  cost this month, where text work runs, and the model servers found on this
  computer (Ollama, llama.cpp, LM Studio, MLX, vLLM).

## Reader capabilities

- **Online-to-offline ingest (`Ctrl+Shift+U`) & OPDS catalogs (`Ctrl+Shift+O`)**:
  save web articles offline as EPUBs (with dedicated **Articles** shelf), fetch
  arXiv papers with Atom metadata, download direct EPUB/PDF links, and browse
  built-in public or self-hosted OPDS 1.2 & 2.0 catalogs (Calibre-Web, Kavita,
  Komga, etc.).

- **Command Palette (`Ctrl+K`)**: fuzzy search across all books, current book
  chapters, cross-book highlights & notes, page/percentage jumps, and actions.
- **Library power tools**: custom collections/shelves, status & format filters
  (Reading, Unread, Finished, EPUB, PDF, Series), series ordering, grid/list
  views (`V`), watched-folder management, and reading stats/streaks.
- **EPUB & PDF reader**: column or scroll layout, theme-aware PDF rendering
  with margin cropping, equation/chart/table snipping (`X` in PDFs), resilient
  annotations with Obsidian/Readwise Markdown export, and word or meaning search.
- **X-Ray Companion (`C`)**: spoiler-free Ask, Define, Explain, Translate,
  Previously…, Who's who, and Illustrations.
- **Narration & Audiobooks (`P` / `V`)**: Gemini voices, offline built-in
  voices, Studio voices, Full Cast multi-voice narration, offline download, and
  M4B audiobook export with chapters.

Clothbound never changes book files.
