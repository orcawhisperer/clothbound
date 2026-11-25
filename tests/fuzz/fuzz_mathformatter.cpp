#include "ai/mathformatter.h"

#include <QByteArray>
#include <QGuiApplication>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

constexpr size_t kMaxInputBytes = 64 * 1024;

void ensureOffscreenGuiApp() {
    static std::unique_ptr<QGuiApplication> app = [] {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QT_QPA_PLATFORMTHEME", "generic");
        static char arg0[] = "fuzz_mathformatter";
        static char *argv[] = {arg0, nullptr};
        static int argc = 1;
        return std::make_unique<QGuiApplication>(argc, argv);
    }();
    (void)app;
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    ensureOffscreenGuiApp();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size > kMaxInputBytes)
        return 0;

    ensureOffscreenGuiApp();

    const QString text = QString::fromUtf8(reinterpret_cast<const char *>(data), qsizetype(size));
    const QString tint = (size >= 12) ? text.left(24) : QStringLiteral("rgba(255,255,255,0.06)");

    (void)MathFormatter::hasMath(text);
    (void)MathFormatter::cleanMarkdown(text);
    (void)MathFormatter::formatLatexInMarkdown(text);
    (void)MathFormatter::formatDisplayHtml(text, tint);
    return 0;
}
