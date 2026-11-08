QT += core gui qml quick quickcontrols2 dbus network concurrent

CONFIG += c++17 release
TARGET = omadrive
TEMPLATE = app

HEADERS += src/recent.h \
    src/account.h \
    src/backend.h \
    src/folderlist.h \
    src/graph.h \
    src/mirror.h \
    src/onedrivelist.h \
    src/preview.h \
    src/theme.h \
    src/transfer.h \
    src/translator.h

SOURCES += src/recent.cpp \
    src/main.cpp \
    src/account.cpp \
    src/backend.cpp \
    src/folderlist.cpp \
    src/graph.cpp \
    src/mirror.cpp \
    src/onedrivelist.cpp \
    src/preview.cpp \
    src/transfer.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
