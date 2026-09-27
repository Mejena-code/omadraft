QT += core gui widgets network testlib
CONFIG += c++17 testcase console
TEMPLATE = app
TARGET = omadraft-tests
include(../src/core.pri)
SOURCES += test_omadraft.cpp

RESOURCES += ../assets/assets.qrc
