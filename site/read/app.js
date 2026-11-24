/**
 * Clothbound Web — Client-Side Reader Application Controller
 *
 * Orchestrates:
 *  - Zero-clutter Shelf View + IndexedDB library + 2 built-in public domain EPUB classics
 *  - Paginated CSS multi-column EPUB book spread engine (1-col & 2-col auto-adapting)
 *  - Local PDF reading surface with IndexedDB persistence
 *  - Floating text selection highlight bar + live mark re-application
 *  - Contents (T), Full-book Search (/), Marks (M), Typography & Theme (Aa), Fullscreen (F)
 *  - Full keyboard & touch swipe navigation
 */

import { parseEpub } from "./epub.js";
import {
  initStorage,
  listBooks,
  getBook,
  saveBook,
  updateBook,
  deleteBook,
  addHighlight,
  removeHighlight,
  toggleBookmark,
  removeBookmark,
  loadPrefs,
  savePrefs,
  generateId,
} from "./storage.js";
import {
  SAMPLE_WORKS,
  getSampleWork,
  buildSampleEpubBlob,
  makeGeneratedCoverDataUrl,
} from "./samples.js";

const THEMES = ["paper", "sepia", "dusk", "charcoal"];
const THEME_META = {
  paper: { label: "Paper", dot: "#FAF8F4" },
  sepia: { label: "Sepia", dot: "#F4ECD8" },
  dusk: { label: "Dusk", dot: "#1C1B19" },
  charcoal: { label: "Charcoal", dot: "#121212" },
};

/* ============================================================================
   Application State
   ============================================================================ */
const state = {
  view: "shelf", // "shelf" | "reader"
  prefs: loadPrefs(),
  library: [],

  // Active reader session
  activeRecord: null, // persisted IndexedDB record
  activeEpub: null, // parsed EPUB handle from parseEpub()
  activePdfUrl: null, // Blob URL for PDF iframe
  chapterIndex: 0,
  chapterData: null,
  chapterWords: [], // estimated word counts per chapter
  totalWords: 0,

  // Pagination geometry
  cols: 2,
  pageIndex: 0, // spread index (0-based)
  totalSpreads: 1,
  stridePx: 1000,
  viewportWidth: 1000,

  // UI overlays
  openDrawer: null, // "toc" | "search" | "marks" | null
  aaOpen: false,
  marksTab: "highlights",
  pendingSelectionText: "",
  toastTimer: null,
  saveTimer: null,
  resizeTimer: null,
  searchTimer: null,
  dragDepth: 0,
  touchStartX: 0,
  touchStartY: 0,
};

/* ============================================================================
   DOM Elements
   ============================================================================ */
const $ = (id) => document.getElementById(id);

const els = {
  body: document.body,
  html: document.documentElement,
  fileInput: $("fileInput"),
  dropOverlay: $("dropOverlay"),

  // Shelf
  shelfView: $("shelfView"),
  shelfBrandLink: $("shelfBrandLink"),
  shelfThemeBtn: $("shelfThemeBtn"),
  shelfThemeDot: $("shelfThemeDot"),
  shelfThemeLabel: $("shelfThemeLabel"),
  dropzone: $("dropzone"),
  openFileBtn: $("openFileBtn"),
  libraryCount: $("libraryCount"),
  libraryGrid: $("libraryGrid"),
  libraryEmpty: $("libraryEmpty"),
  classicsGrid: $("classicsGrid"),

  // Reader
  readerView: $("readerView"),
  readerBar: $("readerBar"),
  backToShelfBtn: $("backToShelfBtn"),
  readerBookTitle: $("readerBookTitle"),
  readerChapterTitle: $("readerChapterTitle"),
  tocBtn: $("tocBtn"),
  searchBtn: $("searchBtn"),
  bookmarkBtn: $("bookmarkBtn"),
  bookmarkFold: $("bookmarkFold"),
  marksBtn: $("marksBtn"),
  marksBadge: $("marksBadge"),
  aaBtn: $("aaBtn"),
  fullscreenBtn: $("fullscreenBtn"),

  // EPUB surface
  epubSurface: $("epubSurface"),
  epubStage: $("epubStage"),
  epubViewport: $("epubViewport"),
  epubColumns: $("epubColumns"),
  epubBookCss: $("epubBookCss"),
  prevPageBtn: $("prevPageBtn"),
  nextPageBtn: $("nextPageBtn"),
  scrubberTrack: $("scrubberTrack"),
  scrubberFill: $("scrubberFill"),
  scrubberTicks: $("scrubberTicks"),
  scrubberThumb: $("scrubberThumb"),
  folioLeft: $("folioLeft"),

  // PDF surface
  pdfSurface: $("pdfSurface"),
  pdfFrame: $("pdfFrame"),

  // Selection bar
  selectionBar: $("selectionBar"),
  selCopyBtn: $("selCopyBtn"),

  // Aa Popover
  aaPopover: $("aaPopover"),
  themePicker: $("themePicker"),
  fontPicker: $("fontPicker"),
  fontDecBtn: $("fontDecBtn"),
  fontIncBtn: $("fontIncBtn"),
  fontSizeVal: $("fontSizeVal"),
  lineDecBtn: $("lineDecBtn"),
  lineIncBtn: $("lineIncBtn"),
  lineHeightVal: $("lineHeightVal"),
  spreadPicker: $("spreadPicker"),
  dropCapToggle: $("dropCapToggle"),

  // Drawers
  drawerBackdrop: $("drawerBackdrop"),
  tocDrawer: $("tocDrawer"),
  tocBookMeta: $("tocBookMeta"),
  tocList: $("tocList"),
  searchDrawer: $("searchDrawer"),
  searchInput: $("searchInput"),
  searchStatus: $("searchStatus"),
  searchResults: $("searchResults"),
  marksDrawer: $("marksDrawer"),
  marksList: $("marksList"),

  // Toast
  toast: $("toast"),
};

/* ============================================================================
   Utility & Toast
   ============================================================================ */
function showToast(msg, duration = 2200) {
  if (!els.toast) return;
  els.toast.textContent = msg;
  els.toast.hidden = false;
  window.clearTimeout(state.toastTimer);
  state.toastTimer = window.setTimeout(() => {
    els.toast.hidden = true;
  }, duration);
}

function escapeHtml(str) {
  return String(str || "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function formatBytes(bytes) {
  if (!bytes || bytes < 1024) return `${bytes || 0} B`;
  if (bytes < 1024 * 1024) return `${Math.round(bytes / 1024)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function cleanTitleFromFilename(name) {
  return String(name || "Untitled")
    .replace(/\.(?:epub|pdf)$/i, "")
    .replace(/[_-]+/g, " ")
    .replace(/\s+/g, " ")
    .trim() || "Untitled";
}

function copyText(text) {
  if (navigator.clipboard && navigator.clipboard.writeText) {
    return navigator.clipboard.writeText(text).catch(() => fallbackCopy(text));
  }
  fallbackCopy(text);
  return Promise.resolve();
}

function fallbackCopy(text) {
  const ta = document.createElement("textarea");
  ta.value = text;
  ta.setAttribute("readonly", "");
  ta.style.position = "fixed";
  ta.style.opacity = "0";
  document.body.appendChild(ta);
  ta.select();
  try {
    document.execCommand("copy");
  } catch {}
  document.body.removeChild(ta);
}

/* ============================================================================
   Preferences & Theme Application
   ============================================================================ */
function applyPrefs(shouldRelayout = false) {
  const p = state.prefs;
  els.html.setAttribute("data-theme", p.theme);
  els.html.setAttribute("data-dropcap", p.dropCap ? "true" : "false");
  els.html.setAttribute("data-font", p.fontFamily);
  els.html.style.setProperty("--reader-font-size", `${p.fontSize}px`);
  els.html.style.setProperty("--reader-line-height", String(p.lineHeight));

  // Shelf theme button
  const meta = THEME_META[p.theme] || THEME_META.paper;
  if (els.shelfThemeLabel) els.shelfThemeLabel.textContent = meta.label;
  if (els.shelfThemeDot) els.shelfThemeDot.style.background = meta.dot;

  // Aa Popover controls sync
  els.themePicker.querySelectorAll("[data-theme-choice]").forEach((btn) => {
    btn.setAttribute(
      "aria-checked",
      btn.getAttribute("data-theme-choice") === p.theme ? "true" : "false"
    );
  });
  els.fontPicker.querySelectorAll("[data-font-choice]").forEach((btn) => {
    btn.classList.toggle(
      "active",
      btn.getAttribute("data-font-choice") === p.fontFamily
    );
  });
  els.spreadPicker.querySelectorAll("[data-spread-choice]").forEach((btn) => {
    btn.classList.toggle(
      "active",
      btn.getAttribute("data-spread-choice") === p.spread
    );
  });
  els.fontSizeVal.textContent = `${p.fontSize}px`;
  els.lineHeightVal.textContent = p.lineHeight.toFixed(2);
  els.dropCapToggle.setAttribute("aria-checked", p.dropCap ? "true" : "false");

  savePrefs(p);

  if (shouldRelayout && state.view === "reader" && state.activeEpub) {
    const ratio =
      state.totalSpreads > 1 ? state.pageIndex / (state.totalSpreads - 1) : 0;
    relayoutColumns({ preserveRatio: ratio, instant: true });
  }
}

function cycleTheme() {
  const idx = THEMES.indexOf(state.prefs.theme);
  state.prefs.theme = THEMES[(idx + 1) % THEMES.length];
  applyPrefs(false);
  showToast(`Theme: ${THEME_META[state.prefs.theme].label}`);
}

/* ============================================================================
   Shelf Rendering
   ============================================================================ */
function renderClassicsShelf() {
  els.classicsGrid.innerHTML = "";
  for (const work of SAMPLE_WORKS) {
    const card = document.createElement("button");
    card.type = "button";
    card.className = "classic-card";
    card.dataset.sampleId = work.id;
    card.innerHTML = `
      <div class="classic-cover">
        <img src="${work.coverDataUrl}" alt="${escapeHtml(work.title)} cover">
      </div>
      <div class="classic-body">
        <span class="classic-kicker">${escapeHtml(work.kicker)}</span>
        <h3 class="classic-title">${escapeHtml(work.title)}</h3>
        <div class="classic-author">${escapeHtml(work.author)} · ${escapeHtml(work.year)}</div>
        <p class="classic-blurb">${escapeHtml(work.description)}</p>
        <div class="classic-foot">
          <span>${work.chapters.length} ${work.unitLabel}</span>
          <span class="classic-read-cue">Read now →</span>
        </div>
      </div>
    `;
    card.addEventListener("click", () => openSampleBook(work.id));
    els.classicsGrid.appendChild(card);
  }
}

async function refreshLibraryShelf() {
  try {
    state.library = await listBooks();
  } catch {
    state.library = [];
  }

  const count = state.library.length;
  els.libraryCount.textContent = count === 1 ? "1 book" : `${count} books`;
  els.libraryEmpty.hidden = count > 0;
  els.libraryGrid.innerHTML = "";

  for (const book of state.library) {
    const pct = Math.round((book.progress || 0) * 100);
    const coverSrc =
      book.coverDataUrl ||
      makeGeneratedCoverDataUrl(book.title, book.author, book.format);

    const card = document.createElement("article");
    card.className = "book-card";
    card.tabIndex = 0;
    card.setAttribute("role", "button");
    card.setAttribute("aria-label", `Open ${book.title}`);
    card.dataset.bookId = book.id;

    card.innerHTML = `
      <div class="book-cover-frame">
        <img src="${coverSrc}" alt="${escapeHtml(book.title)} cover">
        <span class="book-format-badge">${escapeHtml(book.format)}</span>
        <button type="button" class="book-delete-btn" title="Remove from browser library" aria-label="Remove ${escapeHtml(book.title)}">✕</button>
      </div>
      <h3 class="book-card-title">${escapeHtml(book.title)}</h3>
      <p class="book-card-author">${escapeHtml(book.author || "Unknown Author")}</p>
      <div class="book-progress-row">
        <div class="book-progress-track">
          <div class="book-progress-fill" style="width:${pct}%"></div>
        </div>
        <div class="book-progress-meta tnum">
          <span>${book.format === "pdf" ? formatBytes(book.fileSize) : `${pct}% read`}</span>
          <span>${(book.highlights?.length || 0) + (book.bookmarks?.length || 0) > 0 ? `${(book.highlights?.length || 0) + (book.bookmarks?.length || 0)} marks` : "Local"}</span>
        </div>
      </div>
    `;

    const delBtn = card.querySelector(".book-delete-btn");
    delBtn.addEventListener("click", async (ev) => {
      ev.stopPropagation();
      await deleteBook(book.id);
      await refreshLibraryShelf();
      showToast(`Removed “${book.title}” from this browser`);
    });

    card.addEventListener("click", () => openStoredBook(book.id));
    card.addEventListener("keydown", (ev) => {
      if (ev.key === "Enter" || ev.key === " ") {
        ev.preventDefault();
        openStoredBook(book.id);
      }
    });

    els.libraryGrid.appendChild(card);
  }
}

/* ============================================================================
   File Import (EPUB & PDF) + Opening Books
   ============================================================================ */
async function importFiles(fileList) {
  const files = Array.from(fileList || []).filter(Boolean);
  if (files.length === 0) return;

  let lastOpenedId = null;
  for (const file of files) {
    const lower = (file.name || "").toLowerCase();
    const isPdf = lower.endsWith(".pdf") || file.type === "application/pdf";
    const isEpub = lower.endsWith(".epub") || file.type === "application/epub+zip";

    if (!isPdf && !isEpub) {
      showToast(`Unsupported file: ${file.name} (drop .epub or .pdf)`);
      continue;
    }

    try {
      if (isEpub) {
        const epub = await parseEpub(file);
        const title = epub.metadata.title || cleanTitleFromFilename(file.name);
        const author = epub.metadata.author || "Unknown Author";
        const coverDataUrl =
          epub.coverDataUrl || makeGeneratedCoverDataUrl(title, author, "epub");
        epub.dispose();

        const saved = await saveBook({
          id: generateId("epub"),
          title,
          author,
          format: "epub",
          coverDataUrl,
          blob: file,
          fileSize: file.size,
          addedAt: Date.now(),
          lastOpenedAt: Date.now(),
          progress: 0,
          location: { chapterIndex: 0, pageIndex: 0, scrollRatio: 0 },
          highlights: [],
          bookmarks: [],
        });
        lastOpenedId = saved.id;
      } else {
        const title = cleanTitleFromFilename(file.name);
        const coverDataUrl = makeGeneratedCoverDataUrl(title, "PDF Document", "pdf");
        const saved = await saveBook({
          id: generateId("pdf"),
          title,
          author: "Local PDF Document",
          format: "pdf",
          coverDataUrl,
          blob: file,
          fileSize: file.size,
          addedAt: Date.now(),
          lastOpenedAt: Date.now(),
          progress: 0,
          location: { pdfPage: 1 },
          highlights: [],
          bookmarks: [],
        });
        lastOpenedId = saved.id;
      }
    } catch (err) {
      console.error("Import error:", err);
      showToast(`Could not open ${file.name}: ${err?.message || "invalid file"}`, 3600);
    }
  }

  await refreshLibraryShelf();
  if (lastOpenedId) {
    await openStoredBook(lastOpenedId);
  }
}

async function openSampleBook(sampleId) {
  const work = getSampleWork(sampleId);
  if (!work) return;

  // Reuse existing IndexedDB entry for this sample so progress & highlights persist!
  const id = `sample-${work.id}`;
  let record = await getBook(id);
  if (!record || !record.blob) {
    const blob = buildSampleEpubBlob(work);
    record = await saveBook({
      id,
      title: work.title,
      author: work.author,
      format: "epub",
      coverDataUrl: work.coverDataUrl,
      blob,
      fileSize: blob.size,
      addedAt: Date.now(),
      lastOpenedAt: Date.now(),
      progress: 0,
      location: { chapterIndex: 0, pageIndex: 0, scrollRatio: 0 },
      highlights: [],
      bookmarks: [],
      isSample: true,
      sampleId: work.id,
    });
  } else {
    record = await updateBook(id, { lastOpenedAt: Date.now() });
  }

  await openBookRecord(record);
}

async function openStoredBook(bookId) {
  const record = await getBook(bookId);
  if (!record) {
    showToast("Book not found in local storage.");
    return;
  }
  const updated = (await updateBook(bookId, { lastOpenedAt: Date.now() })) || record;
  await openBookRecord(updated);
}

function cleanupActiveReader() {
  hideSelectionBar();
  closeAllOverlays();
  if (state.activeEpub) {
    state.activeEpub.dispose();
    state.activeEpub = null;
  }
  if (state.activePdfUrl) {
    try {
      URL.revokeObjectURL(state.activePdfUrl);
    } catch {}
    state.activePdfUrl = null;
  }
  els.pdfFrame.src = "about:blank";
  els.epubColumns.innerHTML = "";
  els.epubBookCss.textContent = "";
  state.activeRecord = null;
  state.chapterData = null;
  state.chapterWords = [];
  state.totalWords = 0;
}

async function openBookRecord(record) {
  cleanupActiveReader();
  state.activeRecord = record;
  state.view = "reader";
  els.body.setAttribute("data-view", "reader");
  els.shelfView.setAttribute("aria-hidden", "true");
  els.readerView.setAttribute("aria-hidden", "false");
  els.readerView.setAttribute("data-format", record.format);
  els.readerBookTitle.textContent = record.title;
  document.title = `${record.title} — Clothbound Web`;

  if (record.format === "pdf") {
    els.epubSurface.hidden = true;
    els.pdfSurface.hidden = false;
    els.readerChapterTitle.textContent = "Local PDF";
    const pdfBlob =
      record.blob instanceof Blob
        ? record.blob
        : new Blob([record.blob], { type: "application/pdf" });
    state.activePdfUrl = URL.createObjectURL(pdfBlob);
    els.pdfFrame.src = `${state.activePdfUrl}#view=FitH`;
    return;
  }

  // EPUB Reading View
  els.pdfSurface.hidden = true;
  els.epubSurface.hidden = false;

  try {
    const epub = await parseEpub(record.blob);
    state.activeEpub = epub;

    // Populate TOC drawer
    buildTocUi(epub);
    updateMarksBadge();
    renderScrubberTicks(epub.chapterCount);

    // Wait a frame so viewport dimensions settle before paginating
    await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));

    const startChapter = Math.max(
      0,
      Math.min(epub.chapterCount - 1, record.location?.chapterIndex || 0)
    );
    const startPage = Math.max(0, record.location?.pageIndex || 0);
    await renderChapter(startChapter, { targetPage: startPage, instant: true });

    // Background word count estimation for accurate reading time
    estimateBookWords(epub);
  } catch (err) {
    console.error("Failed to open EPUB:", err);
    showToast(`Could not render EPUB: ${err?.message || "parse error"}`, 3600);
    returnToShelf();
  }
}

async function estimateBookWords(epub) {
  const counts = new Array(epub.chapterCount).fill(450);
  let sum = 0;
  for (let i = 0; i < epub.chapterCount; i++) {
    if (state.activeEpub !== epub) return;
    try {
      const ch = await epub.loadChapter(i);
      counts[i] = Math.max(40, ch.wordCount || 40);
    } catch {
      counts[i] = 300;
    }
    sum += counts[i];
  }
  if (state.activeEpub !== epub) return;
  state.chapterWords = counts;
  state.totalWords = Math.max(1, sum);
  updateFolio();
}

async function returnToShelf() {
  if (state.activeRecord && state.activeEpub) {
    await persistCurrentLocation();
  }
  if (document.fullscreenElement) {
    try {
      await document.exitFullscreen();
    } catch {}
  }
  cleanupActiveReader();
  state.view = "shelf";
  els.body.setAttribute("data-view", "shelf");
  els.shelfView.setAttribute("aria-hidden", "false");
  els.readerView.setAttribute("aria-hidden", "true");
  document.title = "Clothbound Web — Local-only EPUB & PDF Reader";
  await refreshLibraryShelf();
}

/* ============================================================================
   EPUB Chapter Rendering & Exact Multi-Column Pagination
   ============================================================================ */
async function renderChapter(chapterIndex, opts = {}) {
  const epub = state.activeEpub;
  if (!epub) return;
  const clamped = Math.max(0, Math.min(epub.chapterCount - 1, chapterIndex));
  const ch = await epub.loadChapter(clamped);

  if (!opts.instant) {
    els.epubColumns.classList.add("chapter-fade");
    await new Promise((r) => setTimeout(r, 90));
  }

  state.chapterIndex = clamped;
  state.chapterData = ch;
  els.readerChapterTitle.textContent = ch.title;
  els.epubBookCss.textContent = ch.css || "";
  els.epubColumns.innerHTML = ch.html;

  // Intercept internal EPUB links
  els.epubColumns.querySelectorAll("a[data-epub-chapter]").forEach((a) => {
    a.addEventListener("click", (ev) => {
      ev.preventDefault();
      const targetCh = Number(a.getAttribute("data-epub-chapter"));
      const frag = a.getAttribute("data-epub-fragment") || "";
      if (!Number.isNaN(targetCh)) {
        renderChapter(targetCh, { fragment: frag, instant: false });
      }
    });
  });

  // Apply persisted highlights for this chapter
  applyChapterHighlights();

  // Re-run layout when images inside chapter finish loading
  els.epubColumns.querySelectorAll("img").forEach((img) => {
    if (!img.complete) {
      img.addEventListener(
        "load",
        () => {
          if (state.activeEpub === epub && state.chapterIndex === clamped) {
            relayoutColumns({ preservePage: state.pageIndex, instant: true });
          }
        },
        { once: true }
      );
    }
  });

  relayoutColumns({
    targetPage: opts.targetPage,
    toEnd: opts.toEnd,
    preserveRatio: opts.targetRatio,
    fragment: opts.fragment,
    highlightId: opts.highlightId,
    instant: true,
  });

  els.epubColumns.classList.remove("chapter-fade");
  syncActiveTocItem();
  syncBookmarkUi();
  scheduleSaveLocation();
}

function relayoutColumns(opts = {}) {
  const vp = els.epubViewport;
  const colsEl = els.epubColumns;
  if (!vp || !colsEl) return;

  // Determine available viewport width & column count
  const stageRect = els.epubStage.getBoundingClientRect();
  const stageStyle = window.getComputedStyle(els.epubStage);
  const padX =
    parseFloat(stageStyle.paddingLeft || "0") +
    parseFloat(stageStyle.paddingRight || "0");
  const availW = Math.max(240, Math.floor(stageRect.width - padX));

  let cols = 1;
  if (state.prefs.spread === "two") {
    cols = availW >= 680 ? 2 : 1;
  } else if (state.prefs.spread === "auto") {
    cols = availW >= 860 ? 2 : 1;
  }

  state.cols = cols;
  vp.setAttribute("data-cols", String(cols));

  const vpRect = vp.getBoundingClientRect();
  const W = Math.max(240, Math.floor(vpRect.width));
  const H = Math.max(200, Math.floor(vpRect.height));

  let gap = cols === 2 ? Math.round(Math.min(68, Math.max(44, W * 0.062))) : 40;
  if (cols === 2 && (W - gap) % 2 !== 0) {
    gap += 1;
  }

  state.viewportWidth = W;
  state.stridePx = W + gap;

  colsEl.classList.add("instant");
  colsEl.style.width = `${W}px`;
  colsEl.style.height = `${H}px`;
  colsEl.style.columnCount = String(cols);
  colsEl.style.columnWidth = "auto";
  colsEl.style.columnGap = `${gap}px`;
  colsEl.style.transform = "translate3d(0px, 0px, 0px)";

  // Force layout and measure true horizontal extent
  const scrollW = colsEl.scrollWidth;
  let contentRight = scrollW;
  try {
    const range = document.createRange();
    range.selectNodeContents(colsEl);
    const rects = range.getClientRects();
    if (rects.length > 0) {
      const originLeft = colsEl.getBoundingClientRect().left;
      let maxR = 0;
      for (let i = 0; i < rects.length; i++) {
        const r = rects[i];
        if (r.width > 0.5 && r.height > 0.5) {
          const right = r.right - originLeft;
          if (right > maxR) maxR = right;
        }
      }
      if (maxR > 0) {
        contentRight = Math.min(scrollW, maxR + 4);
      }
    }
  } catch {}

  state.totalSpreads = Math.max(1, Math.ceil((contentRight - 2) / state.stridePx));

  // Resolve target spread index
  let nextIdx = state.pageIndex;
  if (opts.highlightId) {
    const markEl = colsEl.querySelector(`[data-hl-id="${CSS.escape(opts.highlightId)}"]`);
    if (markEl) {
      nextIdx = spreadIndexForElement(markEl);
    }
  } else if (opts.fragment) {
    const fragEl =
      colsEl.querySelector(`[id="${CSS.escape(opts.fragment)}"]`) ||
      colsEl.querySelector(`[name="${CSS.escape(opts.fragment)}"]`);
    if (fragEl) {
      nextIdx = spreadIndexForElement(fragEl);
    }
  } else if (opts.toEnd) {
    nextIdx = state.totalSpreads - 1;
  } else if (typeof opts.targetPage === "number") {
    nextIdx = opts.targetPage;
  } else if (typeof opts.preserveRatio === "number") {
    nextIdx = Math.round(opts.preserveRatio * (state.totalSpreads - 1));
  } else if (typeof opts.preservePage === "number") {
    nextIdx = opts.preservePage;
  }

  state.pageIndex = Math.max(0, Math.min(state.totalSpreads - 1, nextIdx));
  applyPageTransform(true);

  if (!opts.instant) {
    requestAnimationFrame(() => colsEl.classList.remove("instant"));
  }
}

function spreadIndexForElement(el) {
  const colsRect = els.epubColumns.getBoundingClientRect();
  const elRect = el.getBoundingClientRect();
  const relLeft = elRect.left - colsRect.left;
  if (relLeft <= 0) return 0;
  return Math.max(
    0,
    Math.min(state.totalSpreads - 1, Math.floor(relLeft / state.stridePx))
  );
}

function applyPageTransform(instant = false) {
  const colsEl = els.epubColumns;
  const x = -(state.pageIndex * state.stridePx);
  if (instant) {
    colsEl.classList.add("instant");
    colsEl.style.transform = `translate3d(${x}px, 0px, 0px)`;
    // Trigger reflow before re-enabling transition
    void colsEl.offsetWidth;
    colsEl.classList.remove("instant");
  } else {
    colsEl.classList.remove("instant");
    colsEl.style.transform = `translate3d(${x}px, 0px, 0px)`;
  }

  hideSelectionBar();
  updateNavArrows();
  updateFolio();
  syncBookmarkUi();
}

function updateNavArrows() {
  const epub = state.activeEpub;
  if (!epub) return;
  const atStart = state.chapterIndex === 0 && state.pageIndex === 0;
  const atEnd =
    state.chapterIndex >= epub.chapterCount - 1 &&
    state.pageIndex >= state.totalSpreads - 1;
  els.prevPageBtn.disabled = atStart;
  els.nextPageBtn.disabled = atEnd;
}

async function turnPage(delta) {
  const epub = state.activeEpub;
  if (!epub) return;
  const target = state.pageIndex + delta;

  if (target >= 0 && target < state.totalSpreads) {
    state.pageIndex = target;
    applyPageTransform(false);
    scheduleSaveLocation();
    return;
  }

  if (delta > 0 && state.chapterIndex < epub.chapterCount - 1) {
    await renderChapter(state.chapterIndex + 1, { targetPage: 0, instant: false });
  } else if (delta < 0 && state.chapterIndex > 0) {
    await renderChapter(state.chapterIndex - 1, { toEnd: true, instant: false });
  }
}

function computeProgress() {
  const epub = state.activeEpub;
  if (!epub) return { progress: 0, minsLeft: 1 };
  const chFrac = (state.pageIndex + 1) / Math.max(1, state.totalSpreads);

  if (state.chapterWords.length === epub.chapterCount && state.totalWords > 0) {
    let wordsRead = 0;
    for (let i = 0; i < state.chapterIndex; i++) {
      wordsRead += state.chapterWords[i];
    }
    wordsRead += state.chapterWords[state.chapterIndex] * chFrac;
    const progress = Math.max(0, Math.min(1, wordsRead / state.totalWords));
    const remaining = Math.max(0, state.totalWords - wordsRead);
    const minsLeft = Math.max(1, Math.round(remaining / 235));
    return { progress, minsLeft };
  }

  const progress = Math.max(
    0,
    Math.min(1, (state.chapterIndex + chFrac) / Math.max(1, epub.chapterCount))
  );
  const approxChapterWords = state.chapterData?.wordCount || 500;
  const approxRemaining = approxChapterWords * (1 - chFrac) +
    (epub.chapterCount - 1 - state.chapterIndex) * 500;
  const minsLeft = Math.max(1, Math.round(approxRemaining / 235));
  return { progress, minsLeft };
}

function updateFolio() {
  const { progress, minsLeft } = computeProgress();
  const pct = Math.round(progress * 100);

  let pageLabel;
  if (state.cols === 2) {
    const totalPages = state.totalSpreads * 2;
    const p1 = state.pageIndex * 2 + 1;
    const p2 = Math.min(totalPages, p1 + 1);
    pageLabel = `Page ${p1}–${p2} of ${totalPages}`;
  } else {
    pageLabel = `Page ${state.pageIndex + 1} of ${state.totalSpreads}`;
  }

  els.folioLeft.textContent = `${pageLabel} · ${pct}% · ~${minsLeft} min left`;
  els.scrubberFill.style.width = `${pct}%`;
  els.scrubberThumb.style.left = `${pct}%`;
  els.scrubberTrack.setAttribute("aria-valuenow", String(pct));
}

function renderScrubberTicks(chapterCount) {
  els.scrubberTicks.innerHTML = "";
  if (chapterCount <= 1 || chapterCount > 40) return;
  for (let i = 1; i < chapterCount; i++) {
    const tick = document.createElement("span");
    tick.className = "scrubber-tick";
    tick.style.left = `${(i / chapterCount) * 100}%`;
    els.scrubberTicks.appendChild(tick);
  }
}

function scrubToRatio(ratio) {
  const epub = state.activeEpub;
  if (!epub) return;
  const r = Math.max(0, Math.min(0.9999, ratio));
  const exactChapter = r * epub.chapterCount;
  const targetCh = Math.min(epub.chapterCount - 1, Math.floor(exactChapter));
  const withinCh = exactChapter - targetCh;

  if (targetCh === state.chapterIndex) {
    state.pageIndex = Math.min(
      state.totalSpreads - 1,
      Math.floor(withinCh * state.totalSpreads)
    );
    applyPageTransform(true);
    scheduleSaveLocation();
  } else {
    renderChapter(targetCh, { targetRatio: withinCh, instant: true });
  }
}

/* ============================================================================
   Location Persistence
   ============================================================================ */
function scheduleSaveLocation() {
  window.clearTimeout(state.saveTimer);
  state.saveTimer = window.setTimeout(persistCurrentLocation, 350);
}

async function persistCurrentLocation() {
  if (!state.activeRecord || !state.activeEpub) return;
  const { progress } = computeProgress();
  const scrollRatio =
    state.totalSpreads > 1 ? state.pageIndex / (state.totalSpreads - 1) : 0;
  const updated = await updateBook(state.activeRecord.id, {
    lastOpenedAt: Date.now(),
    progress,
    location: {
      chapterIndex: state.chapterIndex,
      pageIndex: state.pageIndex,
      scrollRatio,
    },
  });
  if (updated) state.activeRecord = updated;
}

/* ============================================================================
   Highlights & Text Selection Mini-Bar
   ============================================================================ */
function applyChapterHighlights() {
  const record = state.activeRecord;
  if (!record || !record.highlights || record.highlights.length === 0) return;
  const chapterHighlights = record.highlights.filter(
    (h) => h.chapterIndex === state.chapterIndex && h.quote
  );
  if (chapterHighlights.length === 0) return;

  for (const hl of chapterHighlights) {
    wrapQuoteInContainer(els.epubColumns, hl.quote, hl.color || "amber", hl.id);
  }
}

function wrapQuoteInContainer(root, quote, color, hlId) {
  const needle = quote.replace(/\s+/g, " ").trim();
  if (needle.length < 2) return false;

  // Build flat text index across text nodes inside root
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  const segments = [];
  let flat = "";
  let node;
  while ((node = walker.nextNode())) {
    if (!node.nodeValue || node.parentElement?.closest("mark.cb-hl")) continue;
    const start = flat.length;
    flat += node.nodeValue;
    segments.push({ node, start, end: flat.length });
  }

  let matchIdx = flat.indexOf(needle);
  let matchLen = needle.length;
  if (matchIdx === -1) {
    // Fallback whitespace-normalized search
    const normQuote = needle.toLowerCase();
    const normFlat = flat.toLowerCase();
    matchIdx = normFlat.indexOf(normQuote);
  }
  if (matchIdx === -1) return false;

  const matchEnd = matchIdx + matchLen;
  // Find affected text nodes (reverse order so splitting doesn't shift indices)
  const touched = segments.filter((s) => s.end > matchIdx && s.start < matchEnd);
  for (let i = touched.length - 1; i >= 0; i--) {
    const seg = touched[i];
    const localStart = Math.max(0, matchIdx - seg.start);
    const localEnd = Math.min(seg.node.nodeValue.length, matchEnd - seg.start);
    if (localEnd <= localStart) continue;
    try {
      const range = document.createRange();
      range.setStart(seg.node, localStart);
      range.setEnd(seg.node, localEnd);
      const mark = document.createElement("mark");
      mark.className = `cb-hl cb-hl-${color}`;
      mark.dataset.hlId = hlId;
      range.surroundContents(mark);
    } catch {
      // Ignore partial element boundary edge cases
    }
  }
  return true;
}

function updateSelectionBar() {
  if (state.view !== "reader" || !state.activeEpub) {
    hideSelectionBar();
    return;
  }
  const sel = window.getSelection();
  if (!sel || sel.isCollapsed || sel.rangeCount === 0) {
    hideSelectionBar();
    return;
  }
  const text = sel.toString().replace(/\s+/g, " ").trim();
  if (text.length < 3) {
    hideSelectionBar();
    return;
  }
  const range = sel.getRangeAt(0);
  if (!els.epubColumns.contains(range.commonAncestorContainer)) {
    hideSelectionBar();
    return;
  }

  const rect = range.getBoundingClientRect();
  if (!rect || (rect.width === 0 && rect.height === 0)) {
    hideSelectionBar();
    return;
  }

  state.pendingSelectionText = text;
  els.selectionBar.hidden = false;
  const barRect = els.selectionBar.getBoundingClientRect();
  const top =
    rect.top > 72
      ? rect.top - barRect.height - 10
      : Math.min(window.innerHeight - barRect.height - 16, rect.bottom + 10);
  const left = Math.max(
    12,
    Math.min(
      window.innerWidth - barRect.width - 12,
      rect.left + rect.width / 2 - barRect.width / 2
    )
  );
  els.selectionBar.style.top = `${Math.round(top)}px`;
  els.selectionBar.style.left = `${Math.round(left)}px`;
}

function hideSelectionBar() {
  state.pendingSelectionText = "";
  if (els.selectionBar) els.selectionBar.hidden = true;
}

async function createHighlightFromSelection(color) {
  const quote = state.pendingSelectionText;
  if (!quote || !state.activeRecord) return;

  const item = await addHighlight(state.activeRecord.id, {
    chapterIndex: state.chapterIndex,
    quote,
    color,
  });
  if (!item) return;

  state.activeRecord = (await getBook(state.activeRecord.id)) || state.activeRecord;
  wrapQuoteInContainer(els.epubColumns, item.quote, item.color, item.id);
  window.getSelection()?.removeAllRanges();
  hideSelectionBar();
  updateMarksBadge();
  if (state.openDrawer === "marks") renderMarksDrawer();
  showToast("Highlight saved locally");
}

/* ============================================================================
   Bookmarks & Marks Drawer (M)
   ============================================================================ */
function isCurrentPageBookmarked() {
  const bms = state.activeRecord?.bookmarks || [];
  return bms.some(
    (b) =>
      b.chapterIndex === state.chapterIndex &&
      (b.pageIndex || 0) === state.pageIndex
  );
}

function syncBookmarkUi() {
  const on = isCurrentPageBookmarked();
  els.bookmarkBtn.setAttribute("aria-pressed", on ? "true" : "false");
  els.bookmarkFold.setAttribute("aria-pressed", on ? "true" : "false");
  els.bookmarkFold.classList.toggle("bookmarked", on);
}

async function handleToggleBookmark() {
  if (!state.activeRecord || !state.activeEpub) return;
  const label = `${state.chapterData?.title || `Section ${state.chapterIndex + 1}`} · Page ${
    state.cols === 2 ? state.pageIndex * 2 + 1 : state.pageIndex + 1
  }`;
  const res = await toggleBookmark(state.activeRecord.id, {
    chapterIndex: state.chapterIndex,
    pageIndex: state.pageIndex,
    label,
  });
  if (res.book) state.activeRecord = res.book;
  syncBookmarkUi();
  updateMarksBadge();
  if (state.openDrawer === "marks") renderMarksDrawer();
  showToast(res.added ? "Page bookmarked" : "Bookmark removed");
}

function updateMarksBadge() {
  const count =
    (state.activeRecord?.highlights?.length || 0) +
    (state.activeRecord?.bookmarks?.length || 0);
  els.marksBadge.textContent = String(count);
  els.marksBadge.hidden = count === 0;
}

function renderMarksDrawer() {
  const record = state.activeRecord;
  els.marksList.innerHTML = "";
  if (!record) return;

  if (state.marksTab === "highlights") {
    const items = record.highlights || [];
    if (items.length === 0) {
      els.marksList.innerHTML = `<div class="marks-empty">Select any passage on a page to highlight it in Amber, Sage, Cobalt, or Rose. Highlights stay saved locally in this browser.</div>`;
      return;
    }
    for (const hl of items) {
      const chTitle =
        state.activeEpub?.toc.find((t) => t.chapterIndex === hl.chapterIndex)?.title ||
        `Section ${hl.chapterIndex + 1}`;
      const card = document.createElement("div");
      card.className = "mark-card";
      card.innerHTML = `
        <div class="mark-card-top">
          <button type="button" class="mark-ch-btn">${escapeHtml(chTitle)}</button>
          <button type="button" class="mark-del-btn" aria-label="Delete highlight">Delete</button>
        </div>
        <p class="mark-quote ${escapeHtml(hl.color || "amber")}">“${escapeHtml(hl.quote)}”</p>
      `;
      const jump = () => {
        closeAllOverlays();
        renderChapter(hl.chapterIndex, { highlightId: hl.id, instant: true });
      };
      card.querySelector(".mark-ch-btn").addEventListener("click", jump);
      card.querySelector(".mark-quote").addEventListener("click", jump);
      card.querySelector(".mark-del-btn").addEventListener("click", async () => {
        const updated = await removeHighlight(record.id, hl.id);
        if (updated) state.activeRecord = updated;
        // Unwrap mark in current DOM if present
        els.epubColumns
          .querySelectorAll(`mark[data-hl-id="${CSS.escape(hl.id)}"]`)
          .forEach((m) => {
            const parent = m.parentNode;
            while (m.firstChild) parent.insertBefore(m.firstChild, m);
            parent.removeChild(m);
          });
        updateMarksBadge();
        renderMarksDrawer();
      });
      els.marksList.appendChild(card);
    }
  } else {
    const items = record.bookmarks || [];
    if (items.length === 0) {
      els.marksList.innerHTML = `<div class="marks-empty">Press <kbd>B</kbd> or tap the page corner to bookmark your place.</div>`;
      return;
    }
    for (const bm of items) {
      const card = document.createElement("div");
      card.className = "mark-card";
      card.innerHTML = `
        <div class="mark-card-top">
          <button type="button" class="mark-ch-btn">${escapeHtml(bm.label)}</button>
          <button type="button" class="mark-del-btn" aria-label="Delete bookmark">Delete</button>
        </div>
      `;
      card.querySelector(".mark-ch-btn").addEventListener("click", () => {
        closeAllOverlays();
        renderChapter(bm.chapterIndex, { targetPage: bm.pageIndex || 0, instant: true });
      });
      card.querySelector(".mark-del-btn").addEventListener("click", async () => {
        const updated = await removeBookmark(record.id, bm.id);
        if (updated) state.activeRecord = updated;
        syncBookmarkUi();
        updateMarksBadge();
        renderMarksDrawer();
      });
      els.marksList.appendChild(card);
    }
  }
}

/* ============================================================================
   TOC & Search Drawers
   ============================================================================ */
function buildTocUi(epub) {
  els.tocBookMeta.textContent = `${epub.metadata.author} · ${epub.chapterCount} sections`;
  els.tocList.innerHTML = "";
  for (const entry of epub.toc) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "toc-item";
    btn.dataset.chapterIndex = String(entry.chapterIndex);
    btn.style.paddingLeft = `${11 + Math.min(3, entry.depth || 0) * 16}px`;
    btn.textContent = entry.title;
    btn.addEventListener("click", () => {
      closeAllOverlays();
      renderChapter(entry.chapterIndex, {
        fragment: entry.fragment || "",
        targetPage: 0,
        instant: true,
      });
    });
    els.tocList.appendChild(btn);
  }
}

function syncActiveTocItem() {
  els.tocList.querySelectorAll(".toc-item").forEach((btn) => {
    btn.classList.toggle(
      "active",
      Number(btn.dataset.chapterIndex) === state.chapterIndex
    );
  });
}

async function runSearch(query) {
  const epub = state.activeEpub;
  if (!epub) return;
  const trimmed = String(query || "").trim();
  if (trimmed.length < 2) {
    els.searchStatus.textContent = "Type at least 2 characters";
    els.searchResults.innerHTML = "";
    return;
  }

  els.searchStatus.textContent = "Searching…";
  const hits = await epub.searchBook(trimmed, 50);
  if (els.searchInput.value.trim() !== trimmed) return;

  els.searchStatus.textContent =
    hits.length === 0
      ? `No matches for “${trimmed}”`
      : `${hits.length}${hits.length >= 50 ? "+" : ""} matches`;

  els.searchResults.innerHTML = "";
  const safeRe = new RegExp(
    trimmed.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"),
    "gi"
  );

  for (const hit of hits) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "search-hit";
    const highlighted = escapeHtml(hit.snippet).replace(
      safeRe,
      (m) => `<mark>${m}</mark>`
    );
    btn.innerHTML = `
      <span class="search-hit-ch">${escapeHtml(hit.chapterTitle)}</span>
      <span class="search-hit-snippet">${highlighted}</span>
    `;
    btn.addEventListener("click", () => {
      closeAllOverlays();
      renderChapter(hit.chapterIndex, {
        targetRatio: hit.ratio,
        instant: true,
      });
    });
    els.searchResults.appendChild(btn);
  }
}

/* ============================================================================
   Drawers & Popovers Management
   ============================================================================ */
function closeAllOverlays() {
  state.openDrawer = null;
  state.aaOpen = false;
  els.tocDrawer.hidden = true;
  els.searchDrawer.hidden = true;
  els.marksDrawer.hidden = true;
  els.drawerBackdrop.hidden = true;
  els.aaPopover.hidden = true;
  els.tocBtn.setAttribute("aria-expanded", "false");
  els.searchBtn.setAttribute("aria-expanded", "false");
  els.marksBtn.setAttribute("aria-expanded", "false");
  els.aaBtn.setAttribute("aria-expanded", "false");
  els.readerView.classList.remove("chrome-active");
}

function toggleDrawer(name) {
  const wasOpen = state.openDrawer === name;
  closeAllOverlays();
  if (wasOpen) return;

  state.openDrawer = name;
  els.drawerBackdrop.hidden = false;
  els.readerView.classList.add("chrome-active");

  if (name === "toc") {
    els.tocDrawer.hidden = false;
    els.tocBtn.setAttribute("aria-expanded", "true");
    syncActiveTocItem();
  } else if (name === "search") {
    els.searchDrawer.hidden = false;
    els.searchBtn.setAttribute("aria-expanded", "true");
    setTimeout(() => els.searchInput.focus(), 30);
  } else if (name === "marks") {
    els.marksDrawer.hidden = false;
    els.marksBtn.setAttribute("aria-expanded", "true");
    renderMarksDrawer();
  }
}

function toggleAaPopover() {
  const wasOpen = state.aaOpen;
  closeAllOverlays();
  if (wasOpen) return;
  state.aaOpen = true;
  els.aaPopover.hidden = false;
  els.aaBtn.setAttribute("aria-expanded", "true");
  els.readerView.classList.add("chrome-active");
}

async function toggleFullscreen() {
  try {
    if (!document.fullscreenElement) {
      await document.documentElement.requestFullscreen();
    } else {
      await document.exitFullscreen();
    }
  } catch {
    showToast("Fullscreen unavailable in this window");
  }
}

/* ============================================================================
   Event Listeners & Keyboard Shortcuts
   ============================================================================ */
function bindEvents() {
  // Shelf navigation & theme toggle
  els.shelfBrandLink.addEventListener("click", (ev) => {
    ev.preventDefault();
    if (state.view === "reader") returnToShelf();
  });
  els.shelfThemeBtn.addEventListener("click", cycleTheme);

  // File picker & dropzone
  els.openFileBtn.addEventListener("click", (ev) => {
    ev.stopPropagation();
    els.fileInput.click();
  });
  els.dropzone.addEventListener("click", () => els.fileInput.click());
  els.dropzone.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter" || ev.key === " ") {
      ev.preventDefault();
      els.fileInput.click();
    }
  });
  els.fileInput.addEventListener("change", () => {
    if (els.fileInput.files?.length) {
      importFiles(els.fileInput.files);
      els.fileInput.value = "";
    }
  });

  // Full-window Drag & Drop
  window.addEventListener("dragenter", (ev) => {
    if (!ev.dataTransfer?.types?.includes("Files")) return;
    ev.preventDefault();
    state.dragDepth++;
    els.dropOverlay.classList.add("active");
    els.dropOverlay.setAttribute("aria-hidden", "false");
  });
  window.addEventListener("dragover", (ev) => {
    if (!ev.dataTransfer?.types?.includes("Files")) return;
    ev.preventDefault();
  });
  window.addEventListener("dragleave", () => {
    state.dragDepth = Math.max(0, state.dragDepth - 1);
    if (state.dragDepth === 0) {
      els.dropOverlay.classList.remove("active");
      els.dropOverlay.setAttribute("aria-hidden", "true");
    }
  });
  window.addEventListener("drop", (ev) => {
    ev.preventDefault();
    state.dragDepth = 0;
    els.dropOverlay.classList.remove("active");
    els.dropOverlay.setAttribute("aria-hidden", "true");
    if (ev.dataTransfer?.files?.length) {
      importFiles(ev.dataTransfer.files);
    }
  });

  // Reader Top Bar Actions
  els.backToShelfBtn.addEventListener("click", returnToShelf);
  els.tocBtn.addEventListener("click", () => toggleDrawer("toc"));
  els.searchBtn.addEventListener("click", () => toggleDrawer("search"));
  els.marksBtn.addEventListener("click", () => toggleDrawer("marks"));
  els.bookmarkBtn.addEventListener("click", handleToggleBookmark);
  els.bookmarkFold.addEventListener("click", handleToggleBookmark);
  els.aaBtn.addEventListener("click", (ev) => {
    ev.stopPropagation();
    toggleAaPopover();
  });
  els.fullscreenBtn.addEventListener("click", toggleFullscreen);

  document.addEventListener("fullscreenchange", () => {
    const isFs = Boolean(document.fullscreenElement);
    els.fullscreenBtn.setAttribute("aria-pressed", isFs ? "true" : "false");
    if (state.view === "reader" && state.activeEpub) {
      setTimeout(() => relayoutColumns({ preservePage: state.pageIndex, instant: true }), 120);
    }
  });

  // Close drawers & popovers
  els.drawerBackdrop.addEventListener("click", closeAllOverlays);
  document.querySelectorAll("[data-close-drawer]").forEach((btn) => {
    btn.addEventListener("click", closeAllOverlays);
  });
  document.addEventListener("click", (ev) => {
    if (
      state.aaOpen &&
      !els.aaPopover.contains(ev.target) &&
      !els.aaBtn.contains(ev.target)
    ) {
      closeAllOverlays();
    }
  });

  // Page turn arrows & click edges
  els.prevPageBtn.addEventListener("click", () => turnPage(-1));
  els.nextPageBtn.addEventListener("click", () => turnPage(1));

  // Touch swipe pagination
  els.epubStage.addEventListener(
    "touchstart",
    (ev) => {
      if (ev.touches.length !== 1) return;
      state.touchStartX = ev.touches[0].clientX;
      state.touchStartY = ev.touches[0].clientY;
    },
    { passive: true }
  );
  els.epubStage.addEventListener(
    "touchend",
    (ev) => {
      if (!ev.changedTouches?.length) return;
      const dx = ev.changedTouches[0].clientX - state.touchStartX;
      const dy = ev.changedTouches[0].clientY - state.touchStartY;
      if (Math.abs(dx) > 48 && Math.abs(dx) > Math.abs(dy) * 1.4) {
        turnPage(dx < 0 ? 1 : -1);
      }
    },
    { passive: true }
  );

  // Scrubber drag & click
  let scrubbing = false;
  const handleScrub = (clientX) => {
    const rect = els.scrubberTrack.getBoundingClientRect();
    const ratio = (clientX - rect.left) / Math.max(1, rect.width);
    scrubToRatio(ratio);
  };
  els.scrubberTrack.addEventListener("pointerdown", (ev) => {
    scrubbing = true;
    els.scrubberTrack.setPointerCapture(ev.pointerId);
    handleScrub(ev.clientX);
  });
  els.scrubberTrack.addEventListener("pointermove", (ev) => {
    if (scrubbing) handleScrub(ev.clientX);
  });
  els.scrubberTrack.addEventListener("pointerup", () => {
    scrubbing = false;
  });

  // Text selection floating bar
  document.addEventListener("mouseup", () => {
    setTimeout(updateSelectionBar, 10);
  });
  document.addEventListener("selectionchange", () => {
    const sel = window.getSelection();
    if (!sel || sel.isCollapsed) hideSelectionBar();
  });
  els.selectionBar.addEventListener("mousedown", (ev) => {
    ev.preventDefault(); // preserve text selection while clicking highlight swatch
  });
  els.selectionBar.querySelectorAll("[data-hl-color]").forEach((btn) => {
    btn.addEventListener("click", () => {
      createHighlightFromSelection(btn.getAttribute("data-hl-color"));
    });
  });
  els.selCopyBtn.addEventListener("click", async () => {
    if (!state.pendingSelectionText) return;
    await copyText(state.pendingSelectionText);
    window.getSelection()?.removeAllRanges();
    hideSelectionBar();
    showToast("Copied to clipboard");
  });

  // Aa Popover Controls
  els.themePicker.addEventListener("click", (ev) => {
    const btn = ev.target.closest("[data-theme-choice]");
    if (!btn) return;
    state.prefs.theme = btn.getAttribute("data-theme-choice");
    applyPrefs(false);
  });
  els.fontPicker.addEventListener("click", (ev) => {
    const btn = ev.target.closest("[data-font-choice]");
    if (!btn) return;
    state.prefs.fontFamily = btn.getAttribute("data-font-choice");
    applyPrefs(true);
  });
  els.spreadPicker.addEventListener("click", (ev) => {
    const btn = ev.target.closest("[data-spread-choice]");
    if (!btn) return;
    state.prefs.spread = btn.getAttribute("data-spread-choice");
    applyPrefs(true);
  });
  els.fontDecBtn.addEventListener("click", () => {
    state.prefs.fontSize = Math.max(14, state.prefs.fontSize - 1);
    applyPrefs(true);
  });
  els.fontIncBtn.addEventListener("click", () => {
    state.prefs.fontSize = Math.min(28, state.prefs.fontSize + 1);
    applyPrefs(true);
  });
  els.lineDecBtn.addEventListener("click", () => {
    state.prefs.lineHeight = Math.max(
      1.35,
      Number((state.prefs.lineHeight - 0.08).toFixed(2))
    );
    applyPrefs(true);
  });
  els.lineIncBtn.addEventListener("click", () => {
    state.prefs.lineHeight = Math.min(
      2.0,
      Number((state.prefs.lineHeight + 0.08).toFixed(2))
    );
    applyPrefs(true);
  });
  els.dropCapToggle.addEventListener("click", () => {
    state.prefs.dropCap = !state.prefs.dropCap;
    applyPrefs(true);
  });

  // Search Input
  els.searchInput.addEventListener("input", () => {
    window.clearTimeout(state.searchTimer);
    state.searchTimer = window.setTimeout(() => {
      runSearch(els.searchInput.value);
    }, 180);
  });

  // Marks Tabs
  document.querySelectorAll("[data-marks-tab]").forEach((tab) => {
    tab.addEventListener("click", () => {
      state.marksTab = tab.getAttribute("data-marks-tab");
      document.querySelectorAll("[data-marks-tab]").forEach((t) => {
        const active = t === tab;
        t.classList.toggle("active", active);
        t.setAttribute("aria-selected", active ? "true" : "false");
      });
      renderMarksDrawer();
    });
  });

  // Responsive Relayout on Resize
  window.addEventListener("resize", () => {
    if (state.view !== "reader" || !state.activeEpub) return;
    window.clearTimeout(state.resizeTimer);
    state.resizeTimer = window.setTimeout(() => {
      const ratio =
        state.totalSpreads > 1 ? state.pageIndex / (state.totalSpreads - 1) : 0;
      relayoutColumns({ preserveRatio: ratio, instant: true });
    }, 100);
  });

  // Keyboard Shortcuts
  document.addEventListener("keydown", (ev) => {
    const tag = (ev.target?.tagName || "").toLowerCase();
    const typing = tag === "input" || tag === "textarea" || ev.target?.isContentEditable;

    if (ev.key === "Escape") {
      if (state.openDrawer || state.aaOpen) {
        ev.preventDefault();
        closeAllOverlays();
        return;
      }
      if (state.view === "reader") {
        ev.preventDefault();
        returnToShelf();
      }
      return;
    }

    if (typing || ev.metaKey || ev.ctrlKey || ev.altKey) return;

    if (state.view !== "reader") {
      if (ev.key.toLowerCase() === "d") {
        ev.preventDefault();
        cycleTheme();
      }
      return;
    }

    const key = ev.key;
    const lower = key.toLowerCase();

    if (lower === "d") {
      ev.preventDefault();
      cycleTheme();
    } else if (lower === "a") {
      ev.preventDefault();
      toggleAaPopover();
    } else if (lower === "f") {
      ev.preventDefault();
      toggleFullscreen();
    } else if (state.activeEpub) {
      if (key === "ArrowRight" || lower === "j" || (key === " " && !ev.shiftKey)) {
        ev.preventDefault();
        turnPage(1);
      } else if (key === "ArrowLeft" || lower === "k" || (key === " " && ev.shiftKey)) {
        ev.preventDefault();
        turnPage(-1);
      } else if (key === "]") {
        ev.preventDefault();
        if (state.chapterIndex < state.activeEpub.chapterCount - 1) {
          renderChapter(state.chapterIndex + 1, { targetPage: 0, instant: false });
        }
      } else if (key === "[") {
        ev.preventDefault();
        if (state.chapterIndex > 0) {
          renderChapter(state.chapterIndex - 1, { targetPage: 0, instant: false });
        }
      } else if (lower === "t") {
        ev.preventDefault();
        toggleDrawer("toc");
      } else if (key === "/") {
        ev.preventDefault();
        toggleDrawer("search");
      } else if (lower === "b") {
        ev.preventDefault();
        handleToggleBookmark();
      } else if (lower === "m") {
        ev.preventDefault();
        toggleDrawer("marks");
      }
    }
  });
}

/* ============================================================================
   Bootstrap & Test Hook
   ============================================================================ */
async function init() {
  await initStorage();
  applyPrefs(false);
  renderClassicsShelf();
  await refreshLibraryShelf();
  bindEvents();

  // Deep link support: /read?sample=yellow-wallpaper
  try {
    const sampleParam = new URLSearchParams(window.location.search).get("sample");
    if (sampleParam && getSampleWork(sampleParam)) {
      await openSampleBook(sampleParam);
    }
  } catch {}
}

// Expose clean local inspection hook for automated verification
window.__clothboundReader = {
  importFiles,
  openSampleBook,
  returnToShelf,
  getState: () => ({
    view: state.view,
    theme: state.prefs.theme,
    cols: state.cols,
    chapterIndex: state.chapterIndex,
    pageIndex: state.pageIndex,
    totalSpreads: state.totalSpreads,
    libraryCount: state.library.length,
    activeBookId: state.activeRecord?.id || null,
    activeBookTitle: state.activeRecord?.title || null,
  }),
};

init();
