.pragma library

// Reading times and sizes in words, in the user's language.

// "45 min", "2 h 10 min", "12 h".
function duration(minutes) {
    minutes = Math.max(1, Math.round(minutes))
    if (minutes < 60)
        return qsTr("%n min", "", minutes)
    var hours = Math.floor(minutes / 60), rest = minutes % 60
    if (hours >= 10)
        return qsTr("%n h", "", rest >= 30 ? hours + 1 : hours)
    if (rest === 0)
        return qsTr("%n h", "", hours)
    return qsTr("%1 h %2 min").arg(hours).arg(rest)
}

// "1.4 MB", in the given locale's digits and separators.
function size(bytes, locale) {
    if (bytes < 1024 * 1024)
        return qsTr("%1 KB").arg(Math.max(1, Math.round(bytes / 1024)).toLocaleString(locale, "f", 0))
    return qsTr("%1 MB").arg((bytes / (1024 * 1024)).toLocaleString(locale, "f", 1))
}
