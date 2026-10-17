QT += core network
CONFIG += console c++17 testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = relay_core_tests
INCLUDEPATH += ..
SOURCES += relay_core_tests.cpp \
           ../relay_core.cpp \
           ../relay_runtime.cpp
HEADERS += ../relay_core.h \
           ../relay_runtime.h
