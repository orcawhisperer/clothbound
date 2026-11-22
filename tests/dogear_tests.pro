QT += core gui quick quickcontrols2 testlib dbus sql concurrent pdf
CONFIG += c++20 testcase
TARGET = dogear_tests
TEMPLATE = app
LIBS += -lz

INCLUDEPATH += ../src

HEADERS += \
    ../src/bookinfo.h \
    ../src/bookname.h \
    ../src/epubbook.h \
    ../src/htmltokenizer.h \
    ../src/pdfbook.h \
    ../src/theme.h \
    ../src/translator.h \
    ../src/ziparchive.h \
    testbooks.h

SOURCES += \
    dogear_tests.cpp \
    ../src/bookinfo.cpp \
    ../src/bookname.cpp \
    ../src/epubbook.cpp \
    ../src/htmltokenizer.cpp \
    ../src/pdfbook.cpp \
    ../src/theme.cpp \
    ../src/translator.cpp \
    ../src/ziparchive.cpp
