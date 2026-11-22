---
name: dogear
description: |
  Read, inspect, and work with the user's ebook library through Dogear, the
  Omarchy ebook reader. Use for any request about their EPUB or PDF books:
  what a book is, its contents, opening a book, or the reading library.
---

# Dogear

Dogear is the user's ebook reader. Its commands need no display; add
`--json` for machine-readable output. Errors go to stderr and the exit code
is 1.

- `dogear info <book> [--json]`: title, author, edition (editor, publisher,
  year), language, description, chapters, pages, and the table of contents.
- `dogear open <book>`: open the book in the reader, where the user left off.

Dogear never changes book files.
