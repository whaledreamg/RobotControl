# ASAN 验证构建：用于确认 sendAndWaitRaw 的 use-after-free 已被修复
# 用法：在 communication/ 为「修复前」代码状态下运行本工程，ASAN 会报
#       stack-use-after-scope；切换到修复后代码则干净通过。
QT       += core network websockets

CONFIG   += c++17
CONFIG   += console
CONFIG   -= app_bundle

TARGET   = test_armwebsocket_asan
TEMPLATE = app

QMAKE_CXXFLAGS += -fsanitize=address -fno-omit-frame-pointer -g -O0
QMAKE_LFLAGS   += -fsanitize=address

INCLUDEPATH += $$PWD/../../communication

SOURCES += $$PWD/main.cpp \
           $$PWD/../../communication/armwebsocket.cpp

HEADERS += $$PWD/../../communication/armwebsocket.h