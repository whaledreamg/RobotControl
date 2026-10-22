FORMS += \
    $$PWD/settingsdialog.ui

# handviewdialog 用 QtConcurrent::run 把 Modbus 读写放到后台线程，
# 避免阻塞 UI（与 Python hand_view_dialog.py 的 threading 一致）
QT += concurrent

# handviewdialog 依赖 ui/ 下的 Revo2 灵巧手客户端，无平台限制
HEADERS += \
    $$PWD/settingsdialog.h \
    $$PWD/handviewdialog.h \
    $$PWD/mapinfodialog.h

SOURCES += \
    $$PWD/settingsdialog.cpp \
    $$PWD/handviewdialog.cpp \
    $$PWD/mapinfodialog.cpp

# webenginewidgets 不支持 Android / iOS / WASM（仅桌面平台可用），因此
# webviewdialog 系列源文件仅在桌面平台加入构建。Android 上 WebViewDialog
# 类整体不参与编译，mainwindow.cpp 中相关代码已用 Q_OS_xxx 宏守护。
!android:!ios:!wasm: HEADERS += \
    $$PWD/webviewdialog.h

!android:!ios:!wasm: SOURCES += \
    $$PWD/webviewdialog.cpp