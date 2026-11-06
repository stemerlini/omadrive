QT += core gui quick quickcontrols2 testlib dbus
CONFIG += c++17 testcase
TARGET = omadrive_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/folderlist.h ../src/theme.h ../src/translator.h
SOURCES += omadrive_tests.cpp ../src/backend.cpp ../src/folderlist.cpp ../src/theme.cpp ../src/translator.cpp
