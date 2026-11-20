QT += core gui qml quick quickcontrols2

CONFIG += c++17 release
TARGET = limgen
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/catalogue.h \
    src/cli.h \
    src/engine.h \
    src/hardware.h \
    src/library.h \
    src/omarchy.h \
    src/palette.h \
    src/progress.h \
    src/prompt.h \
    src/sdcppengine.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/catalogue.cpp \
    src/cli.cpp \
    src/hardware.cpp \
    src/library.cpp \
    src/omarchy.cpp \
    src/palette.cpp \
    src/progress.cpp \
    src/prompt.cpp \
    src/sdcppengine.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
