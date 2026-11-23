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

// The four highlight colours, the one place they are defined.
var swatches = { yellow: "#f2c94c", green: "#6fcf97", blue: "#56ccf2", pink: "#f78fb3" }

function palette(id, theme) {
    var p
    if (id === "paper")
        p = { bg: "#f6f1e7", fg: "#26221d", link: "#8a5a12", muted: "#6b655a", popover: "#fdfaf3", dark: false }
    else if (id === "sepia")
        p = { bg: "#eedfc4", fg: "#3a2e1f", link: "#86491a", muted: "#6e5d46", popover: "#f6ead3", dark: false }
    else if (id === "dusk")
        p = { bg: "#1e1d1b", fg: "#dcd5c8", link: "#d8a75b", muted: "#9a9384", popover: "#2a2926", dark: true }
    else if (id === "night")
        p = { bg: "#0e0e0f", fg: "#c9c9cc", link: "#c99b4f", muted: "#86868c", popover: "#19191b", dark: true }
    else
        p = { bg: theme.ground, fg: theme.ink2, link: theme.accentInk, muted: theme.ink3,
              popover: theme.raised, dark: !theme.light }
    p.id = order.indexOf(id) >= 0 ? id : "omarchy"
    p.selection = css(tint(p.link, p.dark ? 0.38 : 0.26))
    p.search = css(tint(p.dark ? "#f5c542" : "#ffcc00", p.dark ? 0.32 : 0.4))
    p.markYellow = css(tint(swatches.yellow, p.dark ? 0.34 : 0.45))
    p.markGreen = css(tint(swatches.green, p.dark ? 0.3 : 0.4))
    p.markBlue = css(tint(swatches.blue, p.dark ? 0.3 : 0.38))
    p.markPink = css(tint(swatches.pink, p.dark ? 0.32 : 0.4))
    p.found = css(tint(p.dark ? "#ff9f0a" : "#ff8c00", p.dark ? 0.7 : 0.6))
    p.speaking = css(tint(p.link, p.dark ? 0.26 : 0.18))
    p.border = css(tint(p.fg, 0.14))
    // The same edge as a QML colour: QML can't read CSS's rgba() strings.
    p.line = tint(p.fg, 0.14)
    return p
}

function name(id) {
    return id === "paper" ? qsTr("Paper")
         : id === "sepia" ? qsTr("Sepia")
         : id === "dusk" ? qsTr("Dusk")
         : id === "night" ? qsTr("Night") : qsTr("Omarchy")
}
