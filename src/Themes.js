.pragma library

// The page's colors. "Omarchy" wears the desktop theme and follows it live;
// the others are fixed, tuned for long reading.

var order = ["omarchy", "paper", "sepia", "dusk", "night"]

function tint(color, alpha) {
    var c = Qt.lighter(color, 1.0)
    return Qt.rgba(c.r, c.g, c.b, alpha)
}

function css(color) {
    var c = Qt.lighter(color, 1.0)
    return "rgba(" + Math.round(c.r * 255) + "," + Math.round(c.g * 255) + "," + Math.round(c.b * 255) + "," + c.a.toFixed(3) + ")"
}

function palette(id, theme) {
    var p
    if (id === "paper")
        p = { bg: "#fbfaf6", fg: "#23211d", link: "#8a5a12", muted: "#6f6a60", popover: "#ffffff", dark: false }
    else if (id === "sepia")
        p = { bg: "#f3e9d6", fg: "#46382a", link: "#8b4f1d", muted: "#7b6a55", popover: "#fbf4e6", dark: false }
    else if (id === "dusk")
        p = { bg: "#1e1f24", fg: "#d6d2c9", link: "#d8a75b", muted: "#97928a", popover: "#2a2b31", dark: true }
    else if (id === "night")
        p = { bg: "#000000", fg: "#b6b3ad", link: "#c99b4f", muted: "#78756f", popover: "#151515", dark: true }
    else
        p = { bg: theme.background, fg: theme.text, link: theme.accent, muted: theme.textSecondary,
              popover: theme.surface, dark: !theme.light }
    p.id = order.indexOf(id) >= 0 ? id : "omarchy"
    p.selection = css(tint(p.link, p.dark ? 0.38 : 0.26))
    p.search = css(tint(p.dark ? "#f5c542" : "#ffcc00", p.dark ? 0.32 : 0.4))
    p.found = css(tint(p.dark ? "#ff9f0a" : "#ff8c00", p.dark ? 0.7 : 0.6))
    p.speaking = css(tint(p.link, p.dark ? 0.26 : 0.18))
    p.border = css(tint(p.fg, 0.14))
    return p
}

function name(id) {
    return id === "paper" ? qsTr("Paper")
         : id === "sepia" ? qsTr("Sepia")
         : id === "dusk" ? qsTr("Dusk")
         : id === "night" ? qsTr("Night") : qsTr("Omarchy")
}
