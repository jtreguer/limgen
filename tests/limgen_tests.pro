# Qt Test suite for limgen. The sources under test are compiled straight in,
# so the tests see the same code as the app without linking the app itself.
QT += core gui qml testlib

CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = limgen_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    ../src/backend.h \
    ../src/cli.h \
    ../src/omarchy.h \
    ../src/palette.h \
    ../src/prompt.h \
    ../src/theme.h \
    ../src/translator.h

# main.cpp is left out: the test binary has its own main(). The CLI is tested
# through the built app (see LIMGEN_BINARY in bin/test), since main() picks the
# display-less platform before Cli::run is called.
SOURCES += \
    limgen_tests.cpp \
    ../src/backend.cpp \
    ../src/cli.cpp \
    ../src/omarchy.cpp \
    ../src/palette.cpp \
    ../src/prompt.cpp \
    ../src/theme.cpp \
    ../src/translator.cpp
