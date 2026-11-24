QT += core gui qml quick dbus sql concurrent pdf
CONFIG += c++20 release
DEFINES += CLOTHBOUND_VERSION=\\\"0.4.1\\\"
TARGET = clothbound_bench
TEMPLATE = app
LIBS += -lz

INCLUDEPATH += $$PWD/../src $$PWD

HEADERS += \
    testbooks.h \
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
