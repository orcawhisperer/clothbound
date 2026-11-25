QT += core gui qml quick quickcontrols2 testlib dbus sql concurrent network multimedia pdf webenginecore webenginequick webchannel
CONFIG += c++20 testcase
DEFINES += CLOTHBOUND_VERSION=\\\"0.4.1\\\" CLOTHBOUND_UNIFIED_TESTS QT_FORCE_ASSERTS
QMAKE_CXXFLAGS += -Wall -Wextra -Werror=return-type
TARGET = clothbound_tests
TEMPLATE = app
LIBS += -lz
CONFIG += link_pkgconfig
PKGCONFIG += libsecret-1

INCLUDEPATH += $$PWD/../src

HEADERS += \
    fakeserver.h \
    testbooks.h \
    ../src/app/cli.h \
    ../src/app/filepicker.h \
    ../src/app/instance.h \
    ../src/app/migration.h \
    ../src/app/notifier.h \
    ../src/app/pointerdriver.h \
    ../src/app/portalfilepicker.h \
    ../src/app/surfacewatcher.h \
    ../src/app/systemcheck.h \
    ../src/app/theme.h \
    ../src/app/translator.h \
    ../src/app/trayicon.h \
    ../src/formats/bookinfo.h \
    ../src/formats/bookname.h \
    ../src/formats/bookrecord.h \
    ../src/formats/bookscheme.h \
    ../src/formats/coverprovider.h \
    ../src/formats/epubbook.h \
    ../src/formats/htmltokenizer.h \
    ../src/formats/pdfbook.h \
    ../src/formats/pdfpages.h \
    ../src/formats/ziparchive.h \
    ../src/library/booksearch.h \
    ../src/library/library.h \
    ../src/library/libraryfinder.h \
    ../src/library/libraryindex.h \
    ../src/library/librarymodel.h \
    ../src/library/libraryschema.h \
    ../src/library/marks.h \
    ../src/library/readingtelemetry.h \
    ../src/library/scanner.h \
    ../src/reader/reader.h \
    ../src/audio/audiobook.h \
    ../src/audio/audiocache.h \
    ../src/audio/cast.h \
    ../src/audio/director.h \
    ../src/audio/download.h \
    ../src/audio/localspeech.h \
    ../src/audio/mpris.h \
    ../src/audio/narration.h \
    ../src/audio/sherpaonnx.h \
    ../src/audio/speech.h \
    ../src/audio/speechcleaner.h \
    ../src/audio/speechplayer.h \
    ../src/audio/voicecatalog.h \
    ../src/audio/voicepacks.h \
    ../src/ai/ai.h \
    ../src/ai/aiservices.h \
    ../src/ai/aiusage.h \
    ../src/ai/companion.h \
    ../src/ai/gemini.h \
    ../src/ai/illustrator.h \
    ../src/ai/keystore.h \
    ../src/ai/localmodels.h \
    ../src/ai/localsetup.h \
    ../src/ai/mathformatter.h \
    ../src/ai/passageindex.h \
    ../src/ai/sseparser.h \
    ../src/mcp/mcpserver.h \
    ../src/online/articleextractor.h \
    ../src/online/epubwriter.h \
    ../src/online/opdsclient.h \
    ../src/online/opdsparser.h \
    ../src/online/urlguard.h \
    ../src/online/webimporter.h

SOURCES += \
    clothbound_tests.cpp \
    tst_online_ingest.cpp \
    tst_opds.cpp \
    ../src/app/cli.cpp \
    ../src/app/migration.cpp \
    ../src/app/portalfilepicker.cpp \
    ../src/app/surfacewatcher.cpp \
    ../src/app/systemcheck.cpp \
    ../src/app/theme.cpp \
    ../src/app/translator.cpp \
    ../src/app/trayicon.cpp \
    ../src/formats/bookinfo.cpp \
    ../src/formats/bookname.cpp \
    ../src/formats/bookscheme.cpp \
    ../src/formats/coverprovider.cpp \
    ../src/formats/epubbook.cpp \
    ../src/formats/htmltokenizer.cpp \
    ../src/formats/pdfbook.cpp \
    ../src/formats/pdfpages.cpp \
    ../src/formats/ziparchive.cpp \
    ../src/library/booksearch.cpp \
    ../src/library/library.cpp \
    ../src/library/libraryfinder.cpp \
    ../src/library/libraryindex.cpp \
    ../src/library/librarymodel.cpp \
    ../src/library/libraryschema.cpp \
    ../src/library/marks.cpp \
    ../src/library/readingtelemetry.cpp \
    ../src/library/scanner.cpp \
    ../src/reader/reader.cpp \
    ../src/audio/audiobook.cpp \
    ../src/audio/audiocache.cpp \
    ../src/audio/cast.cpp \
    ../src/audio/director.cpp \
    ../src/audio/download.cpp \
    ../src/audio/localspeech.cpp \
    ../src/audio/mpris.cpp \
    ../src/audio/narration.cpp \
    ../src/audio/speech.cpp \
    ../src/audio/speechcleaner.cpp \
    ../src/audio/speechplayer.cpp \
    ../src/audio/voicecatalog.cpp \
    ../src/audio/voicepacks.cpp \
    ../src/ai/aiservices.cpp \
    ../src/ai/aiusage.cpp \
    ../src/ai/companion.cpp \
    ../src/ai/gemini.cpp \
    ../src/ai/illustrator.cpp \
    ../src/ai/keystore.cpp \
    ../src/ai/localmodels.cpp \
    ../src/ai/localsetup.cpp \
    ../src/ai/mathformatter.cpp \
    ../src/ai/passageindex.cpp \
    ../src/ai/sseparser.cpp \
    ../src/mcp/mcpserver.cpp \
    ../src/online/articleextractor.cpp \
    ../src/online/epubwriter.cpp \
    ../src/online/opdsclient.cpp \
    ../src/online/opdsparser.cpp \
    ../src/online/urlguard.cpp \
    ../src/online/webimporter.cpp

RESOURCES += ../src/prompts.qrc
