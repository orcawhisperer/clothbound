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
