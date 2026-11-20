QT += core gui qml quick quickcontrols2

CONFIG += c++17 release
TARGET = limgen
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/cli.h \
    src/omarchy.h \
    src/palette.h \
    src/prompt.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/omarchy.cpp \
    src/palette.cpp \
    src/prompt.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
