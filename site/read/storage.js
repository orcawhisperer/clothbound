/**
 * Clothbound Web — Local-Only Offline Storage (IndexedDB + localStorage)
 *
 * Zero server uploads, zero accounts. Every imported book, reading position,
 * highlight, and bookmark stays strictly inside this browser's IndexedDB
 * (with graceful in-memory fallback if private browsing blocks IndexedDB).
 */

const DB_NAME = "clothbound-web-v1";
const DB_VERSION = 1;
const STORE_BOOKS = "books";
const PREFS_KEY = "clothbound-web-prefs-v1";

export const DEFAULT_PREFS = Object.freeze({
  theme: "paper", // "paper" | "sepia" | "dusk" | "charcoal"
  fontSize: 19,
  lineHeight: 1.68,
  spread: "auto", // "auto" | "single" | "two"
  dropCap: true,
  fontFamily: "literata", // "literata" | "sans"
});

const memoryBooks = new Map();
let dbPromise = null;
let storageMode = "indexeddb";

function openDb() {
  if (dbPromise) return dbPromise;
  dbPromise = new Promise((resolve) => {
    if (typeof indexedDB === "undefined") {
      storageMode = "memory";
      resolve(null);
      return;
    }
    try {
      const req = indexedDB.open(DB_NAME, DB_VERSION);
      req.onupgradeneeded = () => {
        const db = req.result;
        if (!db.objectStoreNames.contains(STORE_BOOKS)) {
          const store = db.createObjectStore(STORE_BOOKS, { keyPath: "id" });
          store.createIndex("lastOpenedAt", "lastOpenedAt", { unique: false });
          store.createIndex("addedAt", "addedAt", { unique: false });
        }
      };
      req.onsuccess = () => {
        storageMode = "indexeddb";
        resolve(req.result);
      };
      req.onerror = () => {
        storageMode = "memory";
        resolve(null);
      };
      req.onblocked = () => {
        storageMode = "memory";
        resolve(null);
      };
    } catch {
      storageMode = "memory";
      resolve(null);
    }
  });
  return dbPromise;
}

export async function initStorage() {
  await openDb();
  return storageMode;
}

export function getStorageMode() {
  return storageMode;
}

function normalizeBookRecord(rec) {
  const now = Date.now();
  return {
    id: rec.id || generateId("book"),
    title: rec.title || "Untitled",
    author: rec.author || "Unknown Author",
    format: rec.format === "pdf" ? "pdf" : "epub",
    coverDataUrl: rec.coverDataUrl || "",
    blob: rec.blob || null,
    fileSize: typeof rec.fileSize === "number" ? rec.fileSize : rec.blob?.size || 0,
    addedAt: rec.addedAt || now,
    lastOpenedAt: rec.lastOpenedAt || now,
    progress: typeof rec.progress === "number" ? Math.max(0, Math.min(1, rec.progress)) : 0,
    location: {
      chapterIndex: rec.location?.chapterIndex || 0,
      pageIndex: rec.location?.pageIndex || 0,
      scrollRatio: rec.location?.scrollRatio || 0,
      pdfPage: rec.location?.pdfPage || 1,
    },
    highlights: Array.isArray(rec.highlights) ? rec.highlights : [],
    bookmarks: Array.isArray(rec.bookmarks) ? rec.bookmarks : [],
    isSample: Boolean(rec.isSample),
    sampleId: rec.sampleId || "",
  };
}

export function generateId(prefix = "id") {
  if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") {
    return `${prefix}-${crypto.randomUUID().slice(0, 12)}`;
  }
  return `${prefix}-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
}

export async function listBooks() {
  const db = await openDb();
  if (!db) {
    return Array.from(memoryBooks.values())
      .map((b) => ({ ...b }))
      .sort((a, b) => (b.lastOpenedAt || b.addedAt) - (a.lastOpenedAt || a.addedAt));
  }
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_BOOKS, "readonly");
    const store = tx.objectStore(STORE_BOOKS);
    const req = store.getAll();
    req.onsuccess = () => {
      const items = (req.result || []).sort(
        (a, b) => (b.lastOpenedAt || b.addedAt) - (a.lastOpenedAt || a.addedAt)
      );
      resolve(items);
    };
    req.onerror = () => reject(req.error);
  });
}

export async function getBook(id) {
  const db = await openDb();
  if (!db) {
    return memoryBooks.get(id) || null;
  }
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_BOOKS, "readonly");
    const req = tx.objectStore(STORE_BOOKS).get(id);
    req.onsuccess = () => resolve(req.result || null);
    req.onerror = () => reject(req.error);
  });
}

export async function saveBook(record) {
  const clean = normalizeBookRecord(record);
  const db = await openDb();
  if (!db) {
    memoryBooks.set(clean.id, clean);
    return clean;
  }
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_BOOKS, "readwrite");
    tx.objectStore(STORE_BOOKS).put(clean);
    tx.oncomplete = () => resolve(clean);
    tx.onerror = () => reject(tx.error);
  });
}

export async function updateBook(id, patch) {
  const existing = await getBook(id);
  if (!existing) return null;
  const merged = normalizeBookRecord({
    ...existing,
    ...patch,
    location: patch.location
      ? { ...existing.location, ...patch.location }
      : existing.location,
  });
  return saveBook(merged);
}

export async function deleteBook(id) {
  const db = await openDb();
  if (!db) {
    memoryBooks.delete(id);
    return;
  }
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_BOOKS, "readwrite");
    tx.objectStore(STORE_BOOKS).delete(id);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

export async function addHighlight(bookId, { chapterIndex, quote, note = "", color = "amber" }) {
  const book = await getBook(bookId);
  if (!book) return null;
  const item = {
    id: generateId("hl"),
    chapterIndex: chapterIndex || 0,
    quote: String(quote || "").trim().slice(0, 600),
    note: String(note || "").trim().slice(0, 600),
    color,
    createdAt: Date.now(),
  };
  const highlights = [item, ...(book.highlights || [])];
  await saveBook({ ...book, highlights });
  return item;
}

export async function removeHighlight(bookId, highlightId) {
  const book = await getBook(bookId);
  if (!book) return null;
  const highlights = (book.highlights || []).filter((h) => h.id !== highlightId);
  return saveBook({ ...book, highlights });
}

export async function toggleBookmark(bookId, { chapterIndex, label, pageIndex = 0 }) {
  const book = await getBook(bookId);
  if (!book) return { book: null, added: false };
  const list = book.bookmarks || [];
  const existingIdx = list.findIndex(
    (b) => b.chapterIndex === chapterIndex && (b.pageIndex || 0) === pageIndex
  );
  if (existingIdx >= 0) {
    const next = list.filter((_, i) => i !== existingIdx);
    const saved = await saveBook({ ...book, bookmarks: next });
    return { book: saved, added: false };
  }
  const item = {
    id: generateId("bm"),
    chapterIndex: chapterIndex || 0,
    pageIndex: pageIndex || 0,
    label: String(label || `Section ${(chapterIndex || 0) + 1}`).trim(),
    createdAt: Date.now(),
  };
  const saved = await saveBook({ ...book, bookmarks: [item, ...list] });
  return { book: saved, added: true, bookmark: item };
}

export async function removeBookmark(bookId, bookmarkId) {
  const book = await getBook(bookId);
  if (!book) return null;
  const bookmarks = (book.bookmarks || []).filter((b) => b.id !== bookmarkId);
  return saveBook({ ...book, bookmarks });
}

/* ============================================================================
   Reading Preferences (localStorage)
   ============================================================================ */

const VALID_THEMES = new Set(["paper", "sepia", "dusk", "charcoal"]);
const VALID_SPREADS = new Set(["auto", "single", "two"]);
const VALID_FONTS = new Set(["literata", "sans"]);

export function loadPrefs() {
  const prefs = { ...DEFAULT_PREFS };
  try {
    const raw = window.localStorage.getItem(PREFS_KEY);
    if (raw) {
      const parsed = JSON.parse(raw);
      if (VALID_THEMES.has(parsed.theme)) prefs.theme = parsed.theme;
      if (typeof parsed.fontSize === "number") {
        prefs.fontSize = Math.max(14, Math.min(28, Math.round(parsed.fontSize)));
      }
      if (typeof parsed.lineHeight === "number") {
        prefs.lineHeight = Math.max(1.35, Math.min(2.0, Number(parsed.lineHeight.toFixed(2))));
      }
      if (VALID_SPREADS.has(parsed.spread)) prefs.spread = parsed.spread;
      if (typeof parsed.dropCap === "boolean") prefs.dropCap = parsed.dropCap;
      if (VALID_FONTS.has(parsed.fontFamily)) prefs.fontFamily = parsed.fontFamily;
    } else {
      // Honor landing page Paper/Dusk choice on first visit to /read
      const siteTheme = window.localStorage.getItem("clothbound-site-theme");
      if (siteTheme === "dusk" || siteTheme === "paper") {
        prefs.theme = siteTheme;
      }
    }
  } catch {
    // Ignore storage errors in private browsing
  }

  // Allow explicit ?theme= override for deep linking & verification
  try {
    const qTheme = new URLSearchParams(window.location.search).get("theme");
    if (qTheme && VALID_THEMES.has(qTheme)) {
      prefs.theme = qTheme;
    }
  } catch {}

  return prefs;
}

export function savePrefs(prefs) {
  try {
    window.localStorage.setItem(PREFS_KEY, JSON.stringify(prefs));
    if (prefs.theme === "paper" || prefs.theme === "dusk") {
      window.localStorage.setItem("clothbound-site-theme", prefs.theme);
    }
  } catch {
    // Ignore quota/private-mode errors
  }
}
