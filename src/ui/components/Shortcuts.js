.pragma library

// Every keyboard shortcut, by where it works: shown by ? (HelpOverlay) and in
// Settings. Call with translator.language so lists follow a language switch.

function library() {
    return [
        { keys: "← → ↑ ↓  h j k l", action: qsTr("Move between books") },
        { keys: "↵", action: qsTr("Open the book, or choose an edition") },
        { keys: "Space", action: qsTr("Continue the book you were reading") },
        { keys: "/  Ctrl F", action: qsTr("Search the library") },
        { keys: "I", action: qsTr("Details about the book") },
        { keys: "S", action: qsTr("Change the order") },
        { keys: "G", action: qsTr("Group or separate editions") },
        { keys: "A", action: qsTr("Add a folder of books") },
        { keys: "Ctrl ⇧ U", action: qsTr("Add from URL") },
        { keys: "Ctrl ⇧ O", action: qsTr("Browse catalogs") },
        { keys: "V", action: qsTr("Covers or a list") },
        { keys: "Ctrl B", action: qsTr("Show or hide the sidebar") },
        { keys: "Ctrl Z", action: qsTr("Undo") }
    ]
}
function reading() {
    return [
        { keys: "Space  →  PgDn  l  j", action: qsTr("Next page") },
        { keys: "⇧ Space  ←  PgUp  h  k", action: qsTr("Previous page") },
        { keys: "↑  ↓", action: qsTr("Scroll tall or zoomed page (PDFs)") },
        { keys: "[  ]", action: qsTr("Previous / next chapter") },
        { keys: "T", action: qsTr("Contents") },
        { keys: "/  Ctrl F", action: qsTr("Search in the book") },
        { keys: "n  N", action: qsTr("Next / previous result") },
        { keys: "B", action: qsTr("Bookmark this page") },
        { keys: "M", action: qsTr("Highlights, notes, and bookmarks") },
        { keys: "C", action: qsTr("Companion: ask, previously, who's who") },
        { keys: "X", action: qsTr("Snip equation, chart, or table (PDFs)") },
        { keys: "A", action: qsTr("Typography and theme") },
        { keys: "+  −  0", action: qsTr("Larger, smaller, default text or zoom") },
        { keys: "W  1", action: qsTr("Fit width or actual size (PDFs)") },
        { keys: "D", action: qsTr("Next reading theme") },
        { keys: "G", action: qsTr("Go to a page or percentage") },
        { keys: "Ctrl C", action: qsTr("Copy selected text") },
        { keys: "Tab", action: qsTr("Show or hide the controls") },
        { keys: "Alt ←  Alt →", action: qsTr("Back / forward in reading history") },
        { keys: "Ctrl Z  Ctrl ⇧ Z", action: qsTr("Undo / redo") },
        { keys: "Esc", action: qsTr("Close a panel, or back to the library") }
    ]
}
function listening() {
    return [
        { keys: "P", action: qsTr("Listen from this page, pause, go on") },
        { keys: "⇧ P", action: qsTr("Stop listening") },
        { keys: ",  .", action: qsTr("Previous / next paragraph") },
        { keys: "⇧ ←  ⇧ →", action: qsTr("Skip back / forward 15 seconds") },
        { keys: "<  >", action: qsTr("Slower / faster") },
        { keys: "V", action: qsTr("Narrator and style") }
    ]
}
function everywhere() {
    return [
        { keys: "Ctrl K", action: qsTr("Command palette") },
        { keys: "Ctrl ,", action: qsTr("Settings") },
        { keys: "F  F11", action: qsTr("Full screen") },
        { keys: "?", action: qsTr("Show these shortcuts") },
        { keys: "Q  Ctrl Q", action: qsTr("Quit") }
    ]
}
