FORMS += \
    $$PWD/settingsdialog.ui

# handviewdialog 用 QtConcurrent::run 把 Modbus 读写放到后台线程，
# 避免阻塞 UI（与 Python hand_view_dialog.py 的 threading 一致）
QT += concurrent

# handviewdialog 依赖 ui/ 下的 Revo2 灵巧手客户端，无平台限制
HEADERS += \
    $$PWD/dialogutil.h \
    $$PWD/settingsdialog.h \
    $$PWD/handviewdialog.h \
    $$PWD/mapinfodialog.h

SOURCES += \
    $$PWD/settingsdialog.cpp \
    $$PWD/handviewdialog.cpp \
    $$PWD/mapinfodialog.cpp

# WebViewDialog：
#   桌面     -> QWebEngineView（Qt WebEngine）
#   Android  -> Qt WebView（QML WebView，经 QQuickWidget 内嵌，见 RobotControl.pro）
# iOS / WASM 暂未适配，不参与构建。
!ios:!wasm: HEADERS += \
    $$PWD/webviewdialog.h

!ios:!wasm: SOURCES += \
    $$PWD/webviewdialog.cpp