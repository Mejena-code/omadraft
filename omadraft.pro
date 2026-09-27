QT += core gui widgets network
CONFIG += c++17 release
CONFIG -= app_bundle
TEMPLATE = app
TARGET = omadraft
VERSION = 0.4.0
version_header.input = $$PWD/src/version.h.in
version_header.output = $$OUT_PWD/omadraft_version.h
QMAKE_SUBSTITUTES += version_header
INCLUDEPATH += $$OUT_PWD
include(src/core.pri)
SOURCES += src/main.cpp
RESOURCES += assets/assets.qrc

isEmpty(PREFIX): PREFIX = /usr/local
target.path = $$PREFIX/bin
desktop.files = assets/omadraft.desktop
desktop.path = $$PREFIX/share/applications
icon.files = assets/omadraft.svg
icon.path = $$PREFIX/share/icons/hicolor/scalable/apps
license.files = LICENSE
license.path = $$PREFIX/share/licenses/omadraft
INSTALLS += target desktop icon license
