QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++17 release
TARGET = omadrive
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/folderlist.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/folderlist.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
