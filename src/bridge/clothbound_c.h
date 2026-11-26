#ifndef CLOTHBOUND_BRIDGE_CLOTHBOUND_C_H
#define CLOTHBOUND_BRIDGE_CLOTHBOUND_C_H

/*
 * ClothboundCore Pure C11 ABI Bridge
 * ==================================
 *
 * Strictly pure C11 (zero Qt headers, zero C++ syntax) for direct Clang module
 * import into Swift 6 (Strict Concurrency) and other foreign-function hosts.
 *
 * Architecture & Threading Contract
 * ---------------------------------
 * - ClothboundCore runs Qt's QCoreApplication event loop on ONE dedicated
 *   background thread ("app.clothbound.core" / Qt Core Actor Thread).
 * - The host application (e.g. SwiftUI @main App / AppKit NSApplicationMain)
 *   retains exclusive ownership of Thread 0 with zero libqcocoa conflict and
 *   zero re-entrant QEventLoop UI hitches.
 * - All cb_engine_* and cb_session_* functions are safe to call from any host
 *   thread; they marshal work onto the Qt Core Actor Thread internally.
 * - cb_event_fn callbacks are invoked on the Qt Core Actor Thread (NEVER on
 *   Thread 0). Swift callers must hop to @MainActor / an AsyncStream before
 *   mutating UI state. Borrowed string pointers passed to cb_event_fn are valid
 *   only for the duration of the callback.
 *
 * UTF-16 Code-Unit Offset Contract (CRITICAL)
 * -------------------------------------------
 * Every text offset and span length across this ABI — highlights, bookmarks,
 * search hits, narration cues, EPUB chapter text, PDF page text, and W3C
 * TextQuoteSelector anchoring (cb_session_locate_quote_json) — is measured in
 * UTF-16 code units (matching Qt QString, DOM/JS String.length, and PDFium).
 *
 * Swift callers MUST index into `String.utf16` (NOT `String.count` grapheme
 * clusters and NOT `String.utf8` byte offsets) when converting offsets to or
 * from Swift Strings.
 *
 * String & Buffer Ownership
 * -------------------------
 * - All `const char *` input strings must be UTF-8 encoded and NUL-terminated.
 * - Every returned `char *` (and any `*err_out`) is UTF-8 encoded, heap-
 *   allocated by ClothboundCore, and MUST be released with cb_string_free().
 * - Every returned `cb_bytes_t` MUST be released with cb_bytes_free().
 * - Every populated `cb_resource_t` MUST be released with cb_resource_free().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define CB_API __attribute__((visibility("default")))
#else
#define CB_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles owned by the host until destroyed/closed. */
typedef struct cb_engine cb_engine_t;
typedef struct cb_session cb_session_t;

/* Heap-allocated byte buffer returned across the C ABI. */
typedef struct cb_bytes {
    uint8_t *data;
    size_t len;
} cb_bytes_t;

/* Single HTTP-style response header for WKURLSchemeHandler / resource replies. */
typedef struct cb_header {
    char *name;
    char *value;
} cb_header_t;

/* Resolved clothbound://local resource reply (zero-copy handoff to WebKit). */
typedef struct cb_resource {
    int status;
    char *mime_type;
    cb_bytes_t body;
    cb_header_t *headers;
    size_t header_count;
} cb_resource_t;

/*
 * Typed asynchronous event callback invoked on the Qt Core Actor Thread.
 *
 * Known event_type values:
 *   - "library.scan_started"
 *   - "library.scan_progress"     {"done": int, "total": int}
 *   - "library.scan_finished"     {"added": int, "updated": int}
 *   - "library.books_changed"     {"bookCount": int}
 *   - "library.books_added"       {"firstBookId": int, "count": int, "firstTitle": string, "targetCollectionId": int}
 *   - "library.book_changed"      {"bookId": int}
 *   - "library.book_updated"      {"bookId": int}
 *   - "library.folders_changed"   {}
 *   - "library.collections_changed" {}
 *   - "library.reading_stats_changed" {}
 *   - "marks.changed"             {"bookId": int, "sessionId": uint64}
 *   - "search.palette_ready"      {"requestId": int, "query": string, "books": [...], "passages": [...], "marks": [...]}
 *   - "narration.progress"        (reserved for Phase 1 narration bridge)
 *   - "companion.token"           (reserved for Phase 1 Companion streaming)
 *   - "language.changed"          {"language": string}
 *   - "error"                     {"message": string}
 */
typedef void (*cb_event_fn)(void *user_data, const char *event_type, const char *json_payload);

/* -------------------------------------------------------------------------
 * Ownership & Diagnostics
 * ------------------------------------------------------------------------- */

/* Returns the ClothboundCore semantic version string (static storage, do not free). */
CB_API const char *cb_engine_version(void);

/* Frees a UTF-8 string returned by any cb_* API (safe to call with NULL). */
CB_API void cb_string_free(char *str);

/* Frees a byte slice returned by any cb_* API (safe to call with {NULL, 0}). */
CB_API void cb_bytes_free(cb_bytes_t bytes);

/* Frees all fields inside a cb_resource_t and zeroes the struct (safe with NULL). */
CB_API void cb_resource_free(cb_resource_t *resource);

/* -------------------------------------------------------------------------
 * Core Engine Lifecycle & Library APIs
 * ------------------------------------------------------------------------- */

/*
 * Spawns the dedicated Qt Core Actor Thread running QCoreApplication::exec(),
 * opens the SQLite Library in data_dir / cache_dir, installs the catalog
 * translator from i18n_dir, and blocks until initialization finishes.
 *
 * Any of data_dir, cache_dir, i18n_dir, on_event, user_data, or err_out may
 * be NULL (empty/NULL paths fall back to platform standard locations).
 *
 * Returns a valid handle on success, or NULL on failure (with *err_out set to
 * a newly allocated UTF-8 error message if err_out != NULL).
 */
CB_API cb_engine_t *cb_engine_create(const char *data_dir,
                                     const char *cache_dir,
                                     const char *i18n_dir,
                                     cb_event_fn on_event,
                                     void *user_data,
                                     char **err_out);

/*
 * Closes any still-open sessions, shuts down Library/LibraryIndex cleanly on
 * the actor thread, posts QCoreApplication::quit(), and joins the actor thread.
 * Safe to call with NULL.
 */
CB_API void cb_engine_destroy(cb_engine_t *engine);

/* Returns true if the caller is currently executing on the Qt Core Actor Thread. */
CB_API bool cb_engine_is_on_actor_thread(const cb_engine_t *engine);

/*
 * Sets the active UI/core BCP-47 language code (e.g. "en", "fr", "pt-BR", or
 * NULL/"" to reset to default English) and reloads the JSON catalog from i18n_dir.
 */
CB_API bool cb_engine_set_language(cb_engine_t *engine, const char *bcp47_code);

/* Returns the active BCP-47 language code (caller frees with cb_string_free). */
CB_API char *cb_engine_language(cb_engine_t *engine);

/*
 * Returns a JSON snapshot of the library: books, continueReading, shelfCounts,
 * collections, folders, readingStats, and scan state.
 * Caller must free with cb_string_free().
 */
CB_API char *cb_engine_list_books_json(cb_engine_t *engine);

/*
 * Imports book files and/or adds watched library folders.
 * Book files are registered immediately; folders are persisted and scanned
 * asynchronously with progress delivered via cb_event_fn.
 */
CB_API bool cb_engine_add_paths(cb_engine_t *engine, const char *const *paths, size_t count);

/* Removes a watched folder from the library (books retain reading state). */
CB_API bool cb_engine_remove_folder(cb_engine_t *engine, const char *folder_path);

/* Triggers an asynchronous library rescan of all watched folders. */
CB_API void cb_engine_rescan(cb_engine_t *engine);

/* Cancels any in-flight library scan. */
CB_API void cb_engine_cancel_scan(cb_engine_t *engine);

/*
 * Executes a synchronous Command Palette search (books, FTS5/lexical passages,
 * and marks) without spawning nested event loops. Pass scope_book_id <= 0 for
 * library-wide search, or > 0 to scope passages and marks to one book.
 * Caller must free with cb_string_free().
 */
CB_API char *cb_engine_search_palette_json(cb_engine_t *engine, const char *query, int scope_book_id);

/*
 * Launches an asynchronous Command Palette search on Library's worker pool;
 * results are delivered via cb_event_fn ("search.palette_ready").
 * Returns the monotonic requestId, or 0 on invalid arguments.
 */
CB_API int cb_engine_search_palette_async(cb_engine_t *engine, const char *query, int scope_book_id);

/*
 * Returns JPEG-encoded cover art bytes for book_id, or {NULL, 0} if unavailable.
 * Caller must free with cb_bytes_free().
 */
CB_API cb_bytes_t cb_engine_book_cover_jpeg(cb_engine_t *engine, int book_id);

/*
 * Resolves a clothbound://local/... resource URL for macOS WKURLSchemeHandler
 * (both /app/... reader surface assets and /book/<token>/... EPUB entries).
 *
 * When referer is NULL/empty (WebKit strips Referer on custom scheme
 * subresources), root-relative paths like "/images/cover.jpg" safely fall back
 * to fallback_token.
 *
 * Always initializes *out; returns true on HTTP 2xx, false on 4xx/error.
 * Caller must release *out with cb_resource_free().
 */
CB_API bool cb_engine_resolve_resource(cb_engine_t *engine,
                                       const char *url,
                                       const char *referer,
                                       const char *fallback_token,
                                       cb_resource_t *out);

/* -------------------------------------------------------------------------
 * Per-Window Book Session APIs (Multi-Window / Multi-Tab Safe)
 * ------------------------------------------------------------------------- */

/*
 * Opens a book by library book_id in a new independent session (multiple
 * sessions may be open concurrently, for distinct books or the same book).
 * Returns NULL on failure (with *err_out populated if non-NULL).
 */
CB_API cb_session_t *cb_session_open(cb_engine_t *engine, int book_id, char **err_out);

/* Closes a book session and releases its resources (safe to call with NULL). */
CB_API void cb_session_close(cb_session_t *session);

/* Returns the library book_id associated with session (0 if invalid). */
CB_API int cb_session_book_id(const cb_session_t *session);

/*
 * Returns a JSON object containing book metadata, TOC/outline, spine/pages,
 * EPUB resourceToken + baseUrl, and saved reading position.
 * Caller must free with cb_string_free().
 */
CB_API char *cb_session_metadata_json(cb_session_t *session);

/*
 * Persists reading progress (0.0..1.0), chapter/page index, and optional JSON
 * locator payload for the session's book.
 */
CB_API bool cb_session_save_position(cb_session_t *session,
                                     int chapter,
                                     double progress,
                                     const char *locator_json);

/*
 * Returns a JSON snapshot of all bookmarks, highlights, and notes for the
 * session's book. All start/end offsets are UTF-16 code units.
 * Caller must free with cb_string_free().
 */
CB_API char *cb_session_marks_json(cb_session_t *session);

/*
 * Adds a highlight or bookmark described by mark_json:
 *   {"kind":"highlight"|"bookmark", "chapter":int, "start":int64, "end":int64,
 *    "text":string, "color":string, "note":string, "label":string,
 *    "prefix":string, "suffix":string}
 * Offsets must be UTF-16 code units.
 * Returns JSON of the created mark (caller frees with cb_string_free()), or NULL.
 */
CB_API char *cb_session_add_mark_json(cb_session_t *session, const char *mark_json);

/* Deletes a mark by mark_id from the session's book. */
CB_API bool cb_session_delete_mark(cb_session_t *session, int mark_id);

/*
 * Resolves a W3C TextQuoteSelector (quote + prefix + suffix + optional UTF-16
 * hint_start, or -1 if unknown) against the chapter (EPUB spine index) or page
 * (PDF 0-based page index) text.
 *
 * Returns JSON:
 *   {"found":bool, "chapter":int, "start":int64, "end":int64, "drifted":bool,
 *    "chapterLengthUtf16":int64, "matchedText":string}
 * All start/end/length values are UTF-16 code units.
 * Caller must free with cb_string_free().
 */
CB_API char *cb_session_locate_quote_json(cb_session_t *session,
                                          int chapter,
                                          const char *quote,
                                          const char *prefix,
                                          const char *suffix,
                                          int64_t hint_start);

/*
 * Renders a 0-based PDF page to PNG bytes at the requested pixel dimensions.
 * If only one of width_px / height_px is > 0, the other is derived from the
 * page's aspect ratio. Returns {NULL, 0} for non-PDF sessions or invalid pages.
 * Caller must free with cb_bytes_free().
 */
CB_API cb_bytes_t cb_session_pdf_page_png(cb_session_t *session, int page, int width_px, int height_px);

/*
 * Returns JSON containing a PDF page's UTF-16-indexed text, dimensions in
 * points, printed label, speakable paragraph spans, and normalized links.
 * Caller must free with cb_string_free().
 */
CB_API char *cb_session_pdf_text_json(cb_session_t *session, int page);

#ifdef __cplusplus
}
#endif

#endif /* CLOTHBOUND_BRIDGE_CLOTHBOUND_C_H */
