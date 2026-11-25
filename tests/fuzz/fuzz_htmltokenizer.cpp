#include "formats/htmltokenizer.h"

#include <QByteArray>
#include <QString>
#include <cstddef>
#include <cstdint>

namespace {
constexpr size_t kMaxInputBytes = 256 * 1024;
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size > kMaxInputBytes)
        return 0;

    const QByteArray raw(reinterpret_cast<const char *>(data), qsizetype(size));

    // 1. Encoding detection + entity decoding.
    const QString decoded = HtmlTokenizer::decodeDocument(raw);
    (void)HtmlTokenizer::decodeEntities(decoded);

    // 2. Streaming tokenizer pass.
    HtmlTokenizer tokenizer(decoded);
    int tokenBudget = 8192;
    while (tokenBudget-- > 0) {
        const HtmlToken tok = tokenizer.next();
        if (tok.type == HtmlToken::End)
            break;
        if (tok.type == HtmlToken::StartTag) {
            (void)tok.localName();
            (void)Html::isVoidElement(tok.localName());
            (void)tok.attribute(QStringLiteral("href"));
            (void)tok.attribute(QStringLiteral("id"));
        }
    }

    // 3. Tree builder in both XML and HTML modes + iterative destruction.
    for (bool htmlMode : {false, true}) {
        auto tree = Html::parse(decoded, htmlMode);
        if (tree) {
            (void)tree->textContent();
            (void)tree->simplifiedText();
            (void)tree->elements(QStringLiteral("p"));
            if (const HtmlNode *link = tree->first(QStringLiteral("a"))) {
                (void)link->attribute(QStringLiteral("href"));
                (void)link->hasAttribute(QStringLiteral("rel"));
            }
            (void)tree->childElements();
        }
    }

    // 4. Raw-byte parse overload.
    auto fromRaw = Html::parse(raw, (size & 1u) != 0);
    if (fromRaw)
        (void)fromRaw->first(QStringLiteral("title"));

    return 0;
}
