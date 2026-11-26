// WebKit (WKWebView) bridge adapter for Clothbound's EPUB reading surface.
// Translates reader.js `state.bridge.*(...)` calls into
// `window.webkit.messageHandlers.clothbound.postMessage({ method, args })`
// and forwards Promise replies when Swift uses WKScriptMessageHandlerWithReply.
// Strictly a no-op under Linux QtWebEngine (`qt.webChannelTransport`).
(() => {
    'use strict'
    const handler = typeof window !== 'undefined'
        && window.webkit
        && window.webkit.messageHandlers
        && window.webkit.messageHandlers.clothbound
    if (!handler || typeof handler.postMessage !== 'function')
        return

    const METHODS = [
        'surfaceLoaded', 'relocated', 'log', 'tapped', 'openExternal',
        'keyPressed', 'notifyPageTurn', 'selectionChanged', 'markClicked',
        'imageClicked', 'speakableReady', 'speakableReadyFor',
        'listenFrom', 'listenFromSession',
    ]

    function invoke(method, args) {
        let callback = null
        if (args.length && typeof args[args.length - 1] === 'function')
            callback = args.pop()
        const reply = handler.postMessage({ method, args })
        if (reply && typeof reply.then === 'function') {
            if (callback)
                reply.then(value => callback(value), () => callback(undefined))
            return reply
        }
        return undefined
    }

    const target = {}
    for (const method of METHODS)
        target[method] = (...args) => invoke(method, args)

    const NON_BRIDGE = new Set(['then', 'catch', 'finally', 'toJSON', 'valueOf', 'toString'])
    const bridge = typeof Proxy === 'function' ? new Proxy(target, {
        get(obj, prop) {
            if (prop in obj)
                return obj[prop]
            if (typeof prop !== 'string' || NON_BRIDGE.has(prop) || prop.startsWith('__'))
                return undefined
            return (...args) => invoke(prop, args)
        },
    }) : target

    window.clothboundBridge = bridge
    if (typeof clothbound === 'object' && clothbound && typeof clothbound.setBridge === 'function')
        clothbound.setBridge(bridge)
})()
