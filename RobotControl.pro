QT       += core gui network websockets widgets

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

# Qt WebEngine / QtWebEngineWidgets 仅桌面平台（Windows / macOS / Linux）支持，
# Qt 官方明确不提供 Android / iOS 实现。本项目通过
# Qt 6 的 android|ios|wasm 平台检测条件引入：
# - 在桌面平台仍完整使用 QWebEngineView（WebViewDialog）
# - 在 Android 等平台编译通过，但 WebViewDialog 类本身被排除，
#   mainwindow.cpp 中相关代码用宏守护，避免链接/编译错误。
qtHaveModule(webenginewidgets) {
    !android:!ios:!wasm: \
        QT += webenginewidgets
}

# Android 上使用 Qt WebView（Qt 6 中仅有 QML 类型，无 C++ 控件），
# 通过 QQuickWidget 嵌入 widgets 界面。
android {
    QT += quickwidgets webview
}

CONFIG   += c++17
CONFIG   += thread

TARGET   = RobotControl
TEMPLATE = app

DEFINES += QT_DEPRECATED_WARNINGS

INCLUDEPATH += $$PWD

# 包含子模块配置
include(common/common.pri)
include(communication/communication.pri)
include(core/core.pri)
include(dialogs/dialogs.pri)
include(ui/ui.pri)

# 主窗口和入口
SOURCES += main.cpp \
           mainwindow.cpp

HEADERS += mainwindow.h

FORMS   += mainwindow.ui

# 资源文件
RESOURCES += resources/resources.qrc

# 翻译
TRANSLATIONS += translations/translations_zh_CN.ts

# 部署配置
target.path = $$[QT_INSTALL_BIN]
INSTALLS += target
