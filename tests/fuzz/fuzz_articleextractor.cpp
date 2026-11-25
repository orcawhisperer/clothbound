#include "online/articleextractor.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QUrl>
#include <cstddef>
#include <cstdint>

namespace {
constexpr size_t kMaxInputBytes = 256 * 1024;
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size > kMaxInputBytes)
        return 0;

    static const QUrl pageUrl(QStringLiteral("https://example.org/essays/northern-lantern"));
    const QByteArray raw(reinterpret_cast<const char *>(data), qsizetype(size));

    const ExtractedArticle article = ArticleExtractor::extract(raw, pageUrl);
    if (!article.bodyXhtml.isEmpty()) {
        QHash<QString, QString> allDownloaded;
        QHash<QString, QString> halfDownloaded;
        for (qsizetype i = 0; i < article.images.size(); ++i) {
            const QString &path = article.images[i].localPath;
            allDownloaded.insert(path, path);
            if ((i & 1) == 0)
                halfDownloaded.insert(path, path);
        }
        (void)ArticleExtractor::finalizeImages(article.bodyXhtml, allDownloaded);
        (void)ArticleExtractor::finalizeImages(article.bodyXhtml, halfDownloaded);
    }

    // Exercise QString overload directly on smaller slices to keep execs/s high.
    if (size <= 4096) {
        const ExtractedArticle fromStr = ArticleExtractor::extract(QString::fromUtf8(raw), pageUrl);
        (void)fromStr.wordCount;
        (void)fromStr.readingMinutes;
    }

    return 0;
}
