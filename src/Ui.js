.pragma library

// Small helpers shared by Dogear's QML.

// `color` (a "#rrggbb" string or color) at the given opacity.
function tint(color, alpha) {
    var c = Qt.lighter(color, 1.0)
    return Qt.rgba(c.r, c.g, c.b, alpha)
}

// A mix of two colors, `amount` of the way from `a` to `b`.
function mix(a, b, amount) {
    var x = Qt.lighter(a, 1.0), y = Qt.lighter(b, 1.0)
    return Qt.rgba(x.r + (y.r - x.r) * amount, x.g + (y.g - x.g) * amount,
                   x.b + (y.b - x.b) * amount, x.a + (y.a - x.a) * amount)
}

// A stable small number for a string, for picking colors.
function hash(text) {
    var h = 0
    for (var i = 0; i < text.length; ++i)
        h = (h * 31 + text.charCodeAt(i)) | 0
    return Math.abs(h)
}

// Black or white, whichever reads on `color` (WCAG relative luminance).
function readableOn(color) {
    var c = Qt.lighter(color, 1.0)
    function linear(v) { return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4) }
    var luminance = 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b)
    return luminance > 0.179 ? "black" : "white"
}
