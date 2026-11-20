# Qt Test suite for limgen. The sources under test are compiled straight in,
# so the tests see the same code as the app without linking the app itself.
QT += core gui qml testlib

CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = limgen_tests
TEMPLATE = app

INCLUDEPATH += ../src

# The repository root, for bin/fake-sd-cli and tests/data.
DEFINES += SOURCE_ROOT=\\\"$$PWD/..\\\"

# Compiled in for the built-in model catalogue (:/models.json).
RESOURCES += ../src/resources.qrc

HEADERS += \
    ../src/backend.h \
    ../src/catalogue.h \
    ../src/cli.h \
    ../src/engine.h \
    ../src/hardware.h \
    ../src/library.h \
    ../src/omarchy.h \
    ../src/palette.h \
    ../src/progress.h \
    ../src/prompt.h \
    ../src/sdcppengine.h \
    ../src/theme.h \
    ../src/translator.h

# main.cpp is left out: the test binary has its own main(). The CLI is tested
# through the built app (see LIMGEN_BINARY in bin/test), since main() picks the
# display-less platform before Cli::run is called.
SOURCES += \
    limgen_tests.cpp \
    ../src/backend.cpp \
    ../src/catalogue.cpp \
    ../src/cli.cpp \
    ../src/hardware.cpp \
    ../src/library.cpp \
    ../src/omarchy.cpp \
    ../src/palette.cpp \
    ../src/progress.cpp \
    ../src/prompt.cpp \
    ../src/sdcppengine.cpp \
    ../src/theme.cpp \
    ../src/translator.cpp
