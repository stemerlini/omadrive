QT += core gui qml quick quickcontrols2 dbus network concurrent

CONFIG += c++17 release link_pkgconfig
PKGCONFIG += fuse3
TARGET = omadrive
TEMPLATE = app

HEADERS += src/agent.h \
    src/recent.h \
    src/rclone.h \
    src/account.h \
    src/backend.h \
    src/folderlist.h \
    src/graph.h \
    src/mirror.h \
    src/mount.h \
    src/onedrivelist.h \
    src/preview.h \
    src/sync.h \
    src/theme.h \
    src/transfer.h \
    src/translator.h

SOURCES += src/agent.cpp \
    src/recent.cpp \
    src/rclone.cpp \
    src/main.cpp \
    src/account.cpp \
    src/backend.cpp \
    src/folderlist.cpp \
    src/graph.cpp \
    src/mirror.cpp \
    src/mount.cpp \
    src/onedrivelist.cpp \
    src/preview.cpp \
    src/sync.cpp \
    src/transfer.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
