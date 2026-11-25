QT += core gui qml quick dbus sql concurrent network pdf
CONFIG += c++20 release link_pkgconfig
PKGCONFIG += libsecret-1
QMAKE_CXXFLAGS += -Wall -Wextra -Werror=return-type
DEFINES += CLOTHBOUND_VERSION=\\\"0.4.1\\\" CLOTHBOUND_SOURCE_DIR=\\\"$$PWD/..\\\"
TARGET = clothbound_bench
TEMPLATE = app
LIBS += -lz

INCLUDEPATH += $$PWD/../src $$PWD

HEADERS += \
    testbooks.h \
    ../src/ai/ai.h \
    ../src/ai/aiservices.h \
    ../src/ai/aiusage.h \
    ../src/ai/gemini.h \
    ../src/ai/keystore.h \
    ../src/ai/localmodels.h \
    ../src/ai/passageindex.h \
    ../src/ai/sseparser.h \
    ../src/audio/sherpaonnx.h \
    ../src/audio/speech.h \
    ../src/audio/voicepacks.h \
    ../src/formats/bookinfo.h \
    ../src/formats/bookname.h \
    ../src/formats/bookrecord.h \
    ../src/formats/epubbook.h \
    ../src/formats/htmltokenizer.h \
    ../src/formats/pdfbook.h \
    ../src/formats/pdfpages.h \
    ../src/formats/ziparchive.h \
    ../src/library/library.h \
    ../src/library/libraryindex.h \
    ../src/library/librarymodel.h \
    ../src/library/marks.h \
    ../src/library/scanner.h

SOURCES += \
    bench.cpp \
    ../src/ai/aiservices.cpp \
    ../src/ai/aiusage.cpp \
    ../src/ai/gemini.cpp \
    ../src/ai/keystore.cpp \
    ../src/ai/localmodels.cpp \
    ../src/ai/passageindex.cpp \
    ../src/ai/sseparser.cpp \
    ../src/audio/speech.cpp \
    ../src/audio/voicepacks.cpp \
    ../src/formats/bookinfo.cpp \
    ../src/formats/bookname.cpp \
    ../src/formats/epubbook.cpp \
    ../src/formats/htmltokenizer.cpp \
    ../src/formats/pdfbook.cpp \
    ../src/formats/pdfpages.cpp \
    ../src/formats/ziparchive.cpp \
    ../src/library/library.cpp \
    ../src/library/libraryindex.cpp \
    ../src/library/librarymodel.cpp \
    ../src/library/marks.cpp \
    ../src/library/scanner.cpp
