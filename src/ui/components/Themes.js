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
var swatches = { yellow: "#E9C860", green: "#86C38A", blue: "#7FA8E8", pink: "#E39AB6" }

function palette(id, theme) {
    var p
    if (id === "paper")
        p = { bg: "#F6F1E7", fg: "#26221D", inkSoft: "#4D463D", link: "#3E5F99", accentInk: "#3E5F99",
              muted: "#6E665A", fold: "#E2D9C6", track: "#E3DCCD", tick: "#C9BFAC", highlight: "#F7DE8A",
              popover: "#FDFAF3", panel: "#FDFAF3", dark: false }
    else if (id === "sepia")
        p = { bg: "#EEDFC4", fg: "#3A2E1F", inkSoft: "#56472F", link: "#8A4B1F", accentInk: "#8A4B1F",
              muted: "#6F5D44", fold: "#D8C6A4", track: "#DCCAA8", tick: "#BFAA86", highlight: "#EFC96B",
              popover: "#F6EAD3", panel: "#F6EAD3", dark: false }
    else if (id === "dusk")
        p = { bg: "#1E1D1B", fg: "#DCD5C8", inkSoft: "#B9B1A3", link: "#9DB8F2", accentInk: "#9DB8F2",
              muted: "#958D80", fold: "#2E2C29", track: "#33312D", tick: "#4A4741", highlight: "#5E4E22",
              popover: "#2A2926", panel: "#252421", dark: true }
    else if (id === "night")
        p = { bg: "#0E0E0F", fg: "#C9C9CC", inkSoft: "#A5A5AB", link: "#9DB8F2", accentInk: "#9DB8F2",
              muted: "#85858C", fold: "#1E1E21", track: "#232326", tick: "#3A3A3F", highlight: "#4D4220",
              popover: "#19191B", panel: "#171719", dark: true }
    else
        p = { bg: theme.ground, fg: theme.ink2, inkSoft: theme.ink2, link: theme.accentInk, accentInk: theme.accentInk,
              muted: theme.ink3, fold: theme.raised, track: theme.separator, tick: theme.ink4,
              highlight: theme.light ? "#F7DE8A" : "#5E4E22",
              popover: theme.raised, panel: theme.sidebar, dark: !theme.light }
    var accent = theme ? theme.accent : p.link
    p.id = order.indexOf(id) >= 0 ? id : "omarchy"
    p.selection = css(tint(accent, p.dark ? 0.34 : 0.28))
    p.search = css(tint(p.link, p.dark ? 0.26 : 0.20))
    p.searchUnderline = p.link
    p.markYellow = p.highlight
    p.markGreen = css(tint(swatches.green, p.dark ? 0.30 : 0.42))
    p.markBlue = css(tint(swatches.blue, p.dark ? 0.30 : 0.38))
    p.markPink = css(tint(swatches.pink, p.dark ? 0.32 : 0.40))
    p.found = css(tint(p.dark ? "#ff9f0a" : "#ff8c00", p.dark ? 0.65 : 0.55))
    p.speaking = css(tint(accent, p.dark ? 0.30 : 0.28))
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
