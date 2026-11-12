QT += core gui quick quickcontrols2 testlib concurrent
CONFIG += c++17 testcase
TARGET = pathfinder_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/preview.h ../src/backend.h ../src/folderlist.h ../src/labels.h ../src/rclone.h ../src/theme.h ../src/translator.h
SOURCES += pathfinder_tests.cpp ../src/preview.cpp ../src/backend.cpp ../src/folderlist.cpp ../src/labels.cpp ../src/rclone.cpp ../src/theme.cpp ../src/translator.cpp
