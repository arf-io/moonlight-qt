QT += core gui network testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = test-desktopclipboard
SOURCES += test_desktopclipboard.cpp ../../app/streaming/desktopclipboard.cpp
HEADERS += ../../app/streaming/desktopclipboard.h
