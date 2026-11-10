QT += core gui quick quickcontrols2 testlib dbus network concurrent
CONFIG += c++17 testcase
TARGET = omadrive_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/account.h ../src/graph.h ../src/mirror.h ../src/preview.h ../src/sync.h ../src/backend.h ../src/folderlist.h ../src/rclone.h ../src/theme.h ../src/translator.h ../src/transfer.h
SOURCES += omadrive_tests.cpp ../src/account.cpp ../src/graph.cpp ../src/mirror.cpp ../src/preview.cpp ../src/sync.cpp ../src/backend.cpp ../src/folderlist.cpp ../src/rclone.cpp ../src/theme.cpp ../src/translator.cpp ../src/transfer.cpp
