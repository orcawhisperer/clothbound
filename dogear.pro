QT += core gui qml quick quickcontrols2 dbus sql concurrent network pdf webenginecore webenginequick webchannel

CONFIG += c++20 release
TARGET = dogear
TEMPLATE = app
DEFINES += DOGEAR_VERSION=\\\"0.1.0\\\"

# EPUBs are ZIP archives; zlib inflates them (Qt itself links it).
LIBS += -lz

HEADERS += \
    src/bookinfo.h \
    src/bookname.h \
    src/bookrecord.h \
    src/bookscheme.h \
    src/cli.h \
    src/epubbook.h \
    src/filepicker.h \
    src/htmltokenizer.h \
    src/library.h \
    src/librarymodel.h \
    src/pdfbook.h \
    src/portalfilepicker.h \
    src/reader.h \
    src/scanner.h \
    src/theme.h \
    src/translator.h \
    src/ziparchive.h

SOURCES += \
    src/bookinfo.cpp \
    src/bookname.cpp \
    src/bookscheme.cpp \
    src/cli.cpp \
    src/epubbook.cpp \
    src/htmltokenizer.cpp \
    src/library.cpp \
    src/librarymodel.cpp \
    src/main.cpp \
    src/pdfbook.cpp \
    src/portalfilepicker.cpp \
    src/reader.cpp \
    src/scanner.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/ziparchive.cpp

RESOURCES += src/resources.qrc
