# Dogear

An ebook reader for [Omarchy](https://omarchy.org): your EPUBs and PDFs in one
library, a calm page to read them on, and voices to read them to you. It
follows your Omarchy theme and language, is driven from the keyboard (and
works just as well with a mouse or touchpad), and never changes your book
files.

![Dogear reading The Time Machine](screenshot.png)

## What it does

- **A library** of your EPUB and PDF books, found in the watched folders you
  manage, with covers, editions grouped together, custom **collections/shelves**,
  status & format filter chips (Reading, Unread, Finished, EPUB, PDF, Series),
  series ordering, a compact **list view** alongside the cover grid, and
  **reading stats** (total time, daily streak, 14-day activity, and pace).
- **Command Palette (`Ctrl+K`)**: instant fuzzy search across every book in
  your library, the current book's chapters, cross-book highlights & notes,
  page/percentage jumps, and every app or reader action.
- **A page to read on**: EPUB in columns or scrolling, with your typeface,
  size, spacing and theme; PDF pages recolored to the reading theme with
  their blank margins cut, one at a time, two side by side, or scrolling.
- **Highlights, notes and bookmarks**, with W3C text-quote anchoring, undo,
  cross-book search, Obsidian/Readwise Markdown file export, and `dogear notes`
  to take them elsewhere. **Search** by words, or **by meaning** ("the scene
  where they first meet").
- **Listening**: the book read aloud with the sentence shown as it's read,
  from any paragraph, at any speed, with a sleep timer and the media keys.
  - *Gemini voices*: the most natural, with styles (plain, natural,
    dramatic), a narrator designed for the book, and a **full cast** giving
    characters voices of their own. Uses your own Gemini key.
  - *Built-in voices* (Kokoro, Pocket TTS and Piper, 42 languages): free, private and
    offline, made on this computer.
  - *Studio voices*: from a voice model you run yourself (Qwen3-TTS,
    Kokoro-FastAPI, speaches).
  - Download a book's narration to listen offline, and save it as an **M4B
    audiobook** with chapters. Close the window and it keeps reading; open
    Dogear again to come back.
- **The X-Ray companion** (C): *Define*, *Explain* or *Translate* words you
  select, *Ask* about the book, read *Previously…* when you come back to it,
  browse *Who's who*, and *Illustrate* a moment in the manner of the book's own
  time. It only ever sees the book up to your page, so it can't spoil anything.

## Intelligence: where it runs

Everything above that needs a model can run with Gemini (your own key, with
what it costs shown before it's spent, and a monthly budget), on a model
server on this computer, or both. Choose in Intelligence (✦ in the library):

| | Gemini | Both | This computer |
|---|---|---|---|
| Narration | Gemini voices | Gemini voices; built-in ones offline | Built-in or Studio voices |
| Styles, Define, Translate, Who's who, casting | Gemini | This computer | This computer |
| Explain, Previously…, Ask | Gemini | Gemini | This computer (Ask from the passages that matter) |
| Search by meaning | Gemini | This computer | This computer |
| Illustrate | Gemini | Gemini | Not available |

Dogear doesn't bundle a language model. Intelligence sets one up for you: it
installs Ollama in Omarchy's terminal (you type your password there), then
downloads the model that suits your computer, showing its size first. Or it
finds the one you already run (llama.cpp's `llama-server`, LM Studio, MLX,
vLLM, or any OpenAI-compatible address). **Test this computer** in Intelligence says, feature by feature,
what will work well here, what will be slow, and what needs Gemini.

## What it needs

| To | Memory | Processor and graphics | Disk |
|---|---|---|---|
| Read, search, mark, listen to downloaded narration | 4 GB | Any x86_64 or aarch64 | — |
| Everything, with Gemini | 8 GB | Any | Up to 4 GB of audio cache |
| Built-in voices, free and offline | 8 GB | 4 cores or more (AVX2, or ARM dot product) | 0.2–0.6 GB |
| + a small local model (2–4B): styles, Define, Translate, Who's who, search by meaning | 16 GB | Any graphics card helps | ~5 GB |
| + a larger one (9–12B or MoE): Explain, Previously…, Ask | 32 GB, or 16 GB with an 8–12 GB graphics card, or Apple Silicon with 24 GB | | ~12 GB |
| + the largest (27–31B), Studio voices live | 32–64 GB | A 16–24 GB graphics card | ~40 GB |

## Keys

| In the library | |
|---|---|
| ← → ↑ ↓ / h j k l | Move between books |
| ↵ | Open the book, or choose an edition |
| Space | Continue the book you were reading |
| / or Ctrl F | Search the library |
| Ctrl K | Command palette: jump to any book, note, or action |
| I · S · G · V · A | Details · order (incl. series) · group editions · grid/list view · add a folder |

| Reading | |
|---|---|
| Space → l j / ⇧Space ← h k | Next / previous page |
| [ ] | Previous / next chapter |
| Ctrl K | Command palette: chapters, notes, books, and reader actions |
| T · G | Contents · go to a page or percentage |
| / or Ctrl F, then n N | Search in the book, next / previous result |
| B · M | Bookmark this page · highlights, notes and bookmarks |
| C | The X-Ray companion: ask, previously, who's who, pictures |
| A · D · + − 0 | Typography and theme · next theme · text size (zoom, for PDFs) |
| Ctrl Z · Ctrl ⇧ Z | Undo · redo |
| Esc | Close a panel, or back to the library |

| Listening | |
|---|---|
| P · ⇧P | Listen from this page, pause, go on · stop |
| , . | Previous / next paragraph |
| < > | Slower / faster |
| V | Narrator, style, full cast, and M4B audiobook export |

`Ctrl+K` opens the command palette anywhere, `Ctrl+,` opens Intelligence settings, `?` shows every key in the app, `F` or `F11` is full screen, and `Q` quits.

## Commands

Commands need no display, and take `--json`:

```sh
dogear [open] [book]                # open the library, or a book where you left off
dogear add <folder-or-book>...      # add books to the library
dogear list                         # the books, with reading progress
dogear info <book>                  # metadata and table of contents
dogear notes <book>                 # highlights, notes and bookmarks, as Markdown
dogear voices [list|install|remove|bench] # built-in voices and their speed here
dogear ai [status|key|forget|local] # Gemini key and costs, and models on this computer
dogear skill [install]              # the skill for coding agents
```

## Build

```sh
bin/build      # build/dogear
bin/test       # unit tests, offscreen
bin/install    # builds and installs the package, so Dogear is in the launcher
bin/translate  # translate what's new into Omarchy's languages (needs GEMINI_API_KEY)
```

## Credits

Dogear is MIT licensed. It reads with Qt (LGPL) and Chromium's engine, and
sets books in Literata (SIL Open Font License). Built-in voices are made by
[sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) (Apache-2.0) with
[Kokoro](https://huggingface.co/hexgrad/Kokoro-82M) (Apache-2.0),
[Pocket TTS](https://github.com/kyutai-labs/pocket-tts) by Kyutai (CC BY 4.0) and
[Piper](https://github.com/rhasspy/piper) voices (each with its own license,
listed in Intelligence), downloaded only when you choose one; sherpa-onnx's
espeak-ng is GPL-3.0.
