QT       += core network websockets

CONFIG   += c++17
CONFIG   += console
CONFIG   -= app_bundle

TARGET   = test_armwebsocket_pending
TEMPLATE = app

INCLUDEPATH += $$PWD/../../communication

SOURCES += $$PWD/main.cpp \
           $$PWD/../../communication/armwebsocket.cpp

HEADERS += $$PWD/../../communication/armwebsocket.h
