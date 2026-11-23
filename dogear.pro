QT += core gui qml quick quickcontrols2 dbus sql concurrent network multimedia pdf webenginecore webenginequick webchannel

CONFIG += c++20 release
TARGET = dogear
TEMPLATE = app
DEFINES += DOGEAR_VERSION=\\\"0.1.0\\\"

# EPUBs are ZIP archives; zlib inflates them (Qt itself links it). API keys
# live in the desktop keyring through libsecret.
LIBS += -lz
CONFIG += link_pkgconfig
PKGCONFIG += libsecret-1

HEADERS += \
    src/ai.h \
    src/aiservices.h \
    src/aiusage.h \
    src/audiocache.h \
    src/bookinfo.h \
    src/bookname.h \
    src/bookrecord.h \
    src/bookscheme.h \
    src/cli.h \
    src/epubbook.h \
    src/filepicker.h \
    src/gemini.h \
    src/htmltokenizer.h \
    src/keystore.h \
    src/library.h \
    src/librarymodel.h \
    src/narration.h \
    src/pdfbook.h \
    src/portalfilepicker.h \
    src/reader.h \
    src/scanner.h \
    src/speech.h \
    src/speechplayer.h \
    src/sseparser.h \
    src/theme.h \
    src/translator.h \
    src/voicecatalog.h \
    src/ziparchive.h

SOURCES += \
    src/aiservices.cpp \
    src/aiusage.cpp \
    src/audiocache.cpp \
    src/bookinfo.cpp \
    src/bookname.cpp \
    src/bookscheme.cpp \
    src/cli.cpp \
    src/epubbook.cpp \
    src/gemini.cpp \
    src/htmltokenizer.cpp \
    src/keystore.cpp \
    src/library.cpp \
    src/librarymodel.cpp \
    src/narration.cpp \
    src/main.cpp \
    src/pdfbook.cpp \
    src/portalfilepicker.cpp \
    src/reader.cpp \
    src/scanner.cpp \
    src/speech.cpp \
    src/speechplayer.cpp \
    src/sseparser.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/voicecatalog.cpp \
    src/ziparchive.cpp

RESOURCES += src/resources.qrc src/prompts.qrc
