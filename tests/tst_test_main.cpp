#include "clothbound_tests.h"

#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <cmath>
#include <memory>
#include <vector>

void ClothboundTests::write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(contents);
}

QByteArray ClothboundTests::png(int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor("#336699"));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QByteArray ClothboundTests::fixture(const QString &name) {
    QFile file(QFINDTESTDATA("fixtures/gemini/" + name));
    if (!file.open(QIODevice::ReadOnly))
        qFatal("missing fixture %s", qPrintable(name));
    return file.readAll();
}

QByteArray ClothboundTests::fontPayload() {
    QByteArray font(2000, Qt::Uninitialized);
    for (int i = 0; i < font.size(); ++i)
        font[i] = char(i * 7 % 251);
    return font;
}

QByteArray ClothboundTests::adobeObfuscate(QByteArray data) {
    const QByteArray key = QByteArray::fromHex("123456789abcdef0123456789abcdef0");
    for (int i = 0; i < 1024; ++i)
        data[i] = char(data[i] ^ key[i % 16]);
    return data;
}

QByteArray ClothboundTests::packsManifest(const QString &base, const QByteArray &engineSha,
                                          const QByteArray &piperSha, qint64 engineSize, qint64 piperSize) {
    const QJsonObject engine{{"version", "1.13.8"},
                             {"license", "Apache-2.0"},
                             {"x86_64", QJsonObject{{"url", base + "/engine-x64.tar.bz2"}, {"size", engineSize},
                                                    {"sha256", QString(engineSha)}, {"dir", "engine-x64"}}},
                             {"aarch64", QJsonObject{{"url", base + "/engine-arm.tar.bz2"}, {"size", engineSize},
                                                     {"sha256", QString(engineSha)}, {"dir", "engine-arm"}}}};
    const QJsonObject kokoro{
        {"id", "kokoro-v1.0"}, {"title", "Kokoro"}, {"license", "Apache-2.0"},
        {"variants", QJsonObject{{"x86_64", QJsonObject{{"url", base + "/k-int8.tar.bz2"}, {"size", 132},
                                                        {"sha256", "11"}, {"dir", "kokoro-int8"}, {"model", "model.int8.onnx"}}},
                                 {"aarch64", QJsonObject{{"url", base + "/k-fp32.tar.bz2"}, {"size", 350},
                                                         {"sha256", "22"}, {"dir", "kokoro-fp32"}, {"model", "model.onnx"}}}}},
        {"languages", QJsonObject{{"en-US", QJsonObject{{"lexicon", "lexicon-us-en.txt"}, {"lang", "en"}}},
                                  {"en-GB", QJsonObject{{"lexicon", "lexicon-gb-en.txt"}, {"lang", "en"}}}}},
        {"voices", QJsonArray{QJsonObject{{"id", "af_heart"}, {"speaker", 3}, {"name", "Heart"}, {"language", "en-US"},
                                          {"gender", "female"}, {"grade", "A"}, {"recommended", true}},
                              QJsonObject{{"id", "bm_george"}, {"speaker", 26}, {"name", "George"}, {"language", "en-GB"},
                                          {"gender", "male"}, {"grade", "C"}, {"recommended", true}},
                              QJsonObject{{"id", "bm_lewis"}, {"speaker", 27}, {"name", "Lewis"}, {"language", "en-GB"},
                                          {"gender", "male"}, {"grade", "D+"}, {"recommended", false}}}}};
    const QJsonObject piper{
        {"license", "MIT"},
        {"voices", QJsonArray{QJsonObject{{"id", "de_DE-thorsten-high"}, {"dir", "vits-piper-de_DE-thorsten-high"},
                                          {"model", "de_DE-thorsten-high.onnx"}, {"language", "de-DE"}, {"name", "Thorsten"},
                                          {"quality", "high"}, {"recommended", true}, {"url", base + "/thorsten.tar.bz2"},
                                          {"size", piperSize}, {"sha256", QString(piperSha)}},
                              QJsonObject{{"id", "de_DE-eva_k-x_low"}, {"dir", "vits-piper-de_DE-eva_k-x_low"},
                                          {"model", "de_DE-eva_k-x_low.onnx"}, {"language", "de-DE"}, {"name", "Eva K"},
                                          {"quality", "x_low"}, {"recommended", false}, {"url", base + "/eva.tar.bz2"},
                                          {"size", 5}, {"sha256", "33"}}}}};
    return QJsonDocument(QJsonObject{{"engine", engine}, {"kokoro", kokoro}, {"piper", piper}}).toJson();
}

void ClothboundTests::fakeInstall(const QString &root, const QString &dir) {
    QDir().mkpath(root + "/" + dir);
    QFile marker(root + "/" + dir + "/.clothbound-installed");
    QVERIFY(marker.open(QIODevice::WriteOnly));
}

SpeechBlock ClothboundTests::block(qint64 start, const QString &text, const QString &kind) {
    SpeechBlock b;
    b.start = start;
    b.end = start + text.size();
    b.text = text;
    b.kind = kind;
    return b;
}

QVector<float> ClothboundTests::ToneEngine::speak(const QString &text, int, const std::atomic_bool &stop) {
    spoken->append(text);
    if (text.contains("slowly"))
        QThread::msleep(400);
    if (stop)
        return {};
    QVector<float> samples(8000);
    for (int n = 0; n < samples.size(); ++n)
        samples[n] = 0.5f * std::sin(2 * M_PI * 300 * n / 16000.0);
    return samples;
}

#ifdef CLOTHBOUND_UNIFIED_TESTS
std::unique_ptr<QObject> createOnlineIngestTests();
std::unique_ptr<QObject> createOpdsTests();
#endif

int main(int argc, char *argv[]) {
    // Always avoid GTK3 platform theme (crashes headless without a display).
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    // Hermetic XDG isolation: tests never touch ~/.config/clothbound/settings.ini
    // or collide with a live Clothbound D-Bus instance.
    QTemporaryDir testXdg;
    if (testXdg.isValid()) {
        QDir(testXdg.path()).mkpath("config");
        QDir(testXdg.path()).mkpath("data");
        QDir(testXdg.path()).mkpath("cache");
        qputenv("XDG_CONFIG_HOME", testXdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", testXdg.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", testXdg.filePath("cache").toUtf8());
    }
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("clothbound"));
    app.setAttribute(Qt::AA_Use96Dpi, true);
    QQuickStyle::setStyle(QStringLiteral("Clothbound"));
    QTEST_SET_MAIN_SOURCE_PATH

    std::vector<std::unique_ptr<QObject>> suites;
    suites.push_back(std::make_unique<ClothboundTests>());
#ifdef CLOTHBOUND_UNIFIED_TESTS
    suites.push_back(createOnlineIngestTests());
    suites.push_back(createOpdsTests());
#endif

    // Collect non-flag positional test function filters (e.g. `./bin/test myTest`).
    QStringList filterNames;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith(QLatin1Char('-'))) {
            // Skip value of two-token QtTest flags like `-o file,txt`.
            if (arg == QLatin1String("-o") || arg == QLatin1String("-eventdelay") || arg == QLatin1String("-keydelay") ||
                arg == QLatin1String("-mousedelay") || arg == QLatin1String("-maxwarnings") ||
                arg == QLatin1String("-iterations") || arg == QLatin1String("-minimumvalue") ||
                arg == QLatin1String("-minimumtotal"))
                ++i;
            continue;
        }
        filterNames.append(arg.section(QLatin1Char(':'), 0, 0));
    }

    auto hasAnySlot = [](QObject *obj, const QStringList &names) {
        const QMetaObject *mo = obj->metaObject();
        for (const QString &name : names) {
            const QByteArray sig = (name + QStringLiteral("()")).toLatin1();
            if (mo->indexOfSlot(sig.constData()) >= 0)
                return true;
        }
        return false;
    };

    int rc = 0;
    bool ranAny = false;
    for (auto &suite : suites) {
        if (!filterNames.isEmpty() && !hasAnySlot(suite.get(), filterNames))
            continue;
        ranAny = true;
        rc |= QTest::qExec(suite.get(), argc, argv);
    }
    if (!ranAny && !suites.empty())
        rc = QTest::qExec(suites.front().get(), argc, argv);
    return rc;
}
