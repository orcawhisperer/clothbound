# Dogear

An ebook reader for [Omarchy](https://omarchy.org): your EPUBs and PDFs in one
library, and a calm page to read them on. It follows your Omarchy theme and
language, is driven from the keyboard, and never changes your book files.

## Build

```sh
bin/build      # build/dogear
bin/test       # unit tests, offscreen
bin/install    # builds and installs the package, so Dogear is in the launcher
```

## Commands

```sh
dogear [book]                 # open the library, or a book where you left off
dogear info <book> [--json]   # metadata and table of contents
dogear skill [install]        # the skill for coding agents
```
