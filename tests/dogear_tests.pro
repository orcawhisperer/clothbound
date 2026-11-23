QT += core gui quick quickcontrols2 testlib dbus sql concurrent network multimedia pdf webenginecore
CONFIG += c++20 testcase
TARGET = dogear_tests
TEMPLATE = app
LIBS += -lz
CONFIG += link_pkgconfig
PKGCONFIG += libsecret-1

INCLUDEPATH += ../src

HEADERS += \
    ../src/ai.h \
    ../src/aiservices.h \
    ../src/aiusage.h \
    ../src/audiocache.h \
    ../src/speech.h \
    ../src/speechplayer.h \
    ../src/sherpaonnx.h \
    ../src/localspeech.h \
    ../src/voicepacks.h \
    ../src/gemini.h \
    ../src/localsetup.h \
    ../src/audiobook.h \
    ../src/cast.h \
    ../src/director.h \
    ../src/download.h \
    ../src/narration.h \
    ../src/illustrator.h \
    ../src/pdfpages.h \
    ../src/passageindex.h \
    ../src/companion.h \
    ../src/localmodels.h \
    ../src/systemcheck.h \
    ../src/instance.h \
    ../src/keystore.h \
    ../src/sseparser.h \
    ../src/surfacewatcher.h \
    fakeserver.h \
    ../src/bookinfo.h \
    ../src/bookname.h \
    ../src/bookrecord.h \
    ../src/bookscheme.h \
    ../src/booksearch.h \
    ../src/cli.h \
    ../src/filepicker.h \
    ../src/library.h \
    ../src/librarymodel.h \
    ../src/marks.h \
    ../src/notifier.h \
    ../src/pointerdriver.h \
    ../src/portalfilepicker.h \
    ../src/reader.h \
    ../src/scanner.h \
    ../src/epubbook.h \
    ../src/htmltokenizer.h \
    ../src/pdfbook.h \
    ../src/theme.h \
    ../src/translator.h \
    ../src/voicecatalog.h \
    ../src/ziparchive.h \
    testbooks.h

SOURCES += \
    dogear_tests.cpp \
    ../src/aiservices.cpp \
    ../src/aiusage.cpp \
    ../src/audiocache.cpp \
    ../src/speech.cpp \
    ../src/speechplayer.cpp \
    ../src/localspeech.cpp \
    ../src/voicepacks.cpp \
    ../src/gemini.cpp \
    ../src/localsetup.cpp \
    ../src/audiobook.cpp \
    ../src/cast.cpp \
    ../src/director.cpp \
    ../src/download.cpp \
    ../src/narration.cpp \
    ../src/illustrator.cpp \
    ../src/pdfpages.cpp \
    ../src/passageindex.cpp \
    ../src/companion.cpp \
    ../src/localmodels.cpp \
    ../src/systemcheck.cpp \
    ../src/keystore.cpp \
    ../src/sseparser.cpp \
    ../src/surfacewatcher.cpp \
    ../src/bookinfo.cpp \
    ../src/bookname.cpp \
    ../src/bookscheme.cpp \
    ../src/booksearch.cpp \
    ../src/cli.cpp \
    ../src/library.cpp \
    ../src/librarymodel.cpp \
    ../src/marks.cpp \
    ../src/portalfilepicker.cpp \
    ../src/reader.cpp \
    ../src/scanner.cpp \
    ../src/epubbook.cpp \
    ../src/htmltokenizer.cpp \
    ../src/pdfbook.cpp \
    ../src/theme.cpp \
    ../src/translator.cpp \
    ../src/voicecatalog.cpp \
    ../src/ziparchive.cpp

RESOURCES += ../src/prompts.qrc
