// Which paragraph of a chapter opens with a drop cap: the first real
// paragraph of running text near the top, long enough to carry a three-line
// initial, starting with a letter from a cased script (not an epigraph,
// a quotation, a list, or a line of verse). Loaded by reader.html and
// tested from node.

const DropCap = {
    // How far into a chapter to look, and how long the paragraph must be.
    lookahead: 8,
    minimumLength: 120,

    // `paragraphs`: [{ text, excluded }] in reading order. The index of the
    // paragraph to open with a drop cap, or -1 for none.
    choose(paragraphs) {
        const limit = Math.min(paragraphs.length, this.lookahead)
        for (let i = 0; i < limit; ++i) {
            const p = paragraphs[i]
            if (p.excluded)
                continue
            const text = (p.text || '').trim()
            if (text.length < this.minimumLength)
                continue
            return this.startsWithLetter(text) ? i : -1
        }
        return -1
    },

    // The first character past any opening quotes or brackets is a letter
    // with upper and lower case (Latin, Greek, Cyrillic, Armenian...).
    startsWithLetter(text) {
        const match = text.match(/^[\s"'“‘«‹„‚(\[—–-]*(.)/u)
        if (!match)
            return false
        const c = match[1]
        return c.toLowerCase() !== c.toUpperCase()
    },
}

if (typeof module !== 'undefined')
    module.exports = DropCap
