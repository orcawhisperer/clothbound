// Liang's hyphenation, the way TeX does it, over the hyph-utf8 patterns
// served beside this file. The browser engine here ships no hyphenation
// dictionaries (CSS `hyphens: auto` does nothing), so justified text gets
// soft hyphens put in by this instead.
'use strict'

const Hyphenation = (() => {
    const SHY = '­'
    const leftMin = 2, rightMin = 3, shortest = 6
    const MAX_CACHE = 20000
    let trie = null
    let loading = null
    const exceptions = new Map()
    const cache = new Map()

    function parse(pattern) {
        const letters = []
        const points = []
        let pending = 0
        for (const ch of pattern) {
            if (ch >= '0' && ch <= '9') {
                pending = Number(ch)
            } else {
                points.push(pending)
                letters.push(ch)
                pending = 0
            }
        }
        points.push(pending)
        return [letters.join(''), points]
    }

    function build(text) {
        const root = new Map()
        for (const line of text.split('\n')) {
            if (line.startsWith('#'))
                continue
            for (const pattern of line.trim().split(/\s+/)) {
                if (!pattern)
                    continue
                const [letters, points] = parse(pattern)
                let node = root
                for (const ch of letters) {
                    if (!node.has(ch))
                        node.set(ch, new Map())
                    node = node.get(ch)
                }
                node.points = points
            }
        }
        return root
    }

    function readExceptions(text) {
        for (const line of text.split('\n')) {
            if (line.startsWith('#'))
                continue
            for (const entry of line.trim().split(/\s+/))
                if (entry)
                    exceptions.set(entry.replace(/-/g, ''), entry.split('-'))
        }
    }

    // Patterns exist for English only, for now; other languages are set
    // without hyphenation rather than with wrong breaks.
    function load(language) {
        if (!/^en\b|^en-/i.test(language || 'en'))
            return Promise.resolve(false)
        if (trie)
            return Promise.resolve(true)
        if (!loading)
            loading = Promise.all([
                fetch('hyph-en-us.pat.txt').then(r => r.text()),
                fetch('hyph-en-us.hyp.txt').then(r => r.text()),
            ]).then(([patterns, hyp]) => {
                trie = build(patterns)
                readExceptions(hyp)
                return true
            }).catch(() => false)
        return loading
    }

    // The word's pieces between permitted break points.
    function pieces(word) {
        const lower = word.toLowerCase()
        if (exceptions.has(lower)) {
            const parts = exceptions.get(lower)
            let at = 0
            return parts.map(part => {
                const piece = word.slice(at, at + part.length)
                at += part.length
                return piece
            })
        }
        const w = '.' + lower + '.'
        const points = new Array(w.length + 1).fill(0)
        for (let i = 0; i < w.length; ++i) {
            let node = trie
            for (let j = i; j < w.length; ++j) {
                node = node.get(w[j])
                if (!node)
                    break
                if (node.points)
                    node.points.forEach((value, k) => {
                        if (value > points[i + k])
                            points[i + k] = value
                    })
            }
        }
        const result = []
        let start = 0
        for (let i = leftMin; i <= word.length - rightMin; ++i)
            if (points[i + 1] % 2 === 1) {
                result.push(word.slice(start, i))
                start = i
            }
        result.push(word.slice(start))
        return result
    }

    function word(text) {
        if (!trie || text.length < shortest || /['\u2019]/.test(text))
            return text
        let hyphenated = cache.get(text)
        if (hyphenated === undefined) {
            hyphenated = pieces(text).join(SHY)
            if (cache.size >= MAX_CACHE)
                cache.delete(cache.keys().next().value)
            cache.set(text, hyphenated)
        }
        return hyphenated
    }

    // Soft hyphens into every long word of a text node, in place.
    // Match apostrophe-joined tokens whole so contractions like "couldn't"
    // aren't split before the apostrophe.
    const words = /[A-Za-zÀ-ɏ]+(?:['\u2019][A-Za-zÀ-ɏ]+)*/g
    function node(textNode) {
        const text = textNode.data
        if (text.length < shortest || text.includes(SHY))
            return
        const replaced = text.replace(words, word)
        if (replaced !== text)
            textNode.data = replaced
    }

    return { load, node, word, pieces }
})()
