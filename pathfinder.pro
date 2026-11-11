QT += core gui qml quick quickcontrols2 network concurrent

CONFIG += c++17 release
TARGET = pathfinder
TEMPLATE = app

HEADERS += src/agent.h \
    src/rclone.h \
    src/backend.h \
    src/cloud.h \
    src/folderlist.h \
    src/preview.h \
    src/theme.h \
    src/translator.h

SOURCES += src/agent.cpp \
    src/rclone.cpp \
    src/main.cpp \
    src/backend.cpp \
    src/cloud.cpp \
    src/folderlist.cpp \
    src/preview.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
