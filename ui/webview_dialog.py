"""
通用 WebView 弹窗
=================
基于 PySide6.QtWebEngineWidgets.QWebEngineView，加载一个 URL，
提供工具栏：后退 / 前进 / 刷新 / 跳转 / 关闭。

注意：
    PySide6（Qt6）已移除旧的 QWebView，必须用 QWebEngineView。
    需要安装 PySide6-Addons（或 PyQt6-WebEngine 等价包），
    `PySide6>=6.5` 的 `pip install PySide6` 不会自动包含。
"""
from PySide6.QtCore import Qt, QUrl, Signal
from PySide6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QLineEdit, QPushButton,
    QLabel, QSizePolicy, QToolBar, QStyle,
)
from PySide6.QtGui import QIcon, QAction

try:
    from PySide6.QtWebEngineWidgets import QWebEngineView
    _HAS_WEB_ENGINE = True
except Exception:  # pragma: no cover
    # 环境里没装 PySide6-Addons 时给出友好提示
    QWebEngineView = None
    _HAS_WEB_ENGINE = False


class WebViewDialog(QDialog):
    """带工具栏的简单 WebView 弹窗

    用法：
        dlg = WebViewDialog("http://192.168.100.16:9198/",
                            title="机械臂 Web", parent=self)
        dlg.resize(1100, 750)
        dlg.show()    # 非模态；用 exec() 也行

    Signals:
        url_changed(str)  - 当前 URL 变化（用户导航时）
    """

    url_changed = Signal(str)

    def __init__(self, url: str = "", title: str = "WebView",
                 parent=None, modal: bool = False):
        super().__init__(parent)
        self.setWindowTitle(title)
        self.setSizeGripEnabled(True)
        # 非模态，避免阻塞机械臂控制
        self.setModal(modal)
        self.resize(1100, 750)

        self._init_ui()
        if url:
            self.load_url(url)

    # -----------------------------------------------------------
    # UI
    # -----------------------------------------------------------
    def _init_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(6, 6, 6, 6)
        layout.setSpacing(6)

        # --- 顶部工具栏：后退 / 前进 / 刷新 / 地址栏 / 跳转 / 关闭 ---
        bar = QHBoxLayout()
        bar.setSpacing(6)

        if _HAS_WEB_ENGINE:
            self.btn_back = QPushButton("◀")
            self.btn_back.setFixedWidth(32)
            self.btn_back.setToolTip("后退")
            self.btn_back.clicked.connect(self._on_back)
            bar.addWidget(self.btn_back)

            self.btn_forward = QPushButton("▶")
            self.btn_forward.setFixedWidth(32)
            self.btn_forward.setToolTip("前进")
            self.btn_forward.clicked.connect(self._on_forward)
            bar.addWidget(self.btn_forward)

            self.btn_reload = QPushButton("↻")
            self.btn_reload.setFixedWidth(32)
            self.btn_reload.setToolTip("刷新")
            self.btn_reload.clicked.connect(self._on_reload)
            bar.addWidget(self.btn_reload)

        self.url_edit = QLineEdit()
        self.url_edit.setPlaceholderText("输入 URL，例如 http://192.168.100.16:9198/")
        self.url_edit.returnPressed.connect(self._on_url_entered)
        bar.addWidget(self.url_edit, 1)

        self.btn_go = QPushButton("跳转")
        self.btn_go.clicked.connect(self._on_url_entered)
        bar.addWidget(self.btn_go)

        self.btn_close = QPushButton("关闭")
        self.btn_close.clicked.connect(self.accept)
        bar.addWidget(self.btn_close)

        layout.addLayout(bar)

        # --- 内容区 ---
        if _HAS_WEB_ENGINE:
            self.view = QWebEngineView()
            self.view.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
            self.view.urlChanged.connect(self._on_view_url_changed)
            self.view.titleChanged.connect(self._on_view_title_changed)
            layout.addWidget(self.view, 1)
        else:
            # 退化处理：环境缺包时给出明确提示
            placeholder = QLabel(
                "未找到 QtWebEngineWidgets。\n\n"
                "请安装：\n"
                "  pip install PySide6-Addons\n"
                "或：\n"
                "  pip install PyQt6-WebEngine\n\n"
                "安装后重启程序即可。"
            )
            placeholder.setAlignment(Qt.AlignCenter)
            placeholder.setStyleSheet("color: #c0392b; font-size: 13px; padding: 24px;")
            placeholder.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
            layout.addWidget(placeholder, 1)
            self.view = None

    # -----------------------------------------------------------
    # Public API
    # -----------------------------------------------------------
    def load_url(self, url: str):
        """加载 URL（接受裸域或带 scheme 的完整 URL）"""
        if not self.view:
            return
        url = (url or "").strip()
        if not url:
            return
        if "://" not in url:
            url = "http://" + url
        qurl = QUrl(url)
        self.url_edit.setText(url)
        self.view.setUrl(qurl)

    # -----------------------------------------------------------
    # Slots
    # -----------------------------------------------------------
    def _on_url_entered(self):
        text = self.url_edit.text().strip()
        if text:
            self.load_url(text)

    def _on_reload(self):
        if self.view:
            self.view.reload()

    def _on_back(self):
        if self.view:
            self.view.back()

    def _on_forward(self):
        if self.view:
            self.view.forward()

    def _on_view_url_changed(self, qurl: QUrl):
        url = qurl.toString()
        self.url_edit.setText(url)
        self.url_changed.emit(url)

    def _on_view_title_changed(self, title: str):
        # 站点标题为空时不覆盖弹窗标题
        if title and title.strip():
            self.setWindowTitle(f"{title} — WebView")


# 便利函数，一行打开
def open_webview_window(url: str, title: str = "WebView", parent=None):
    """非模态打开一个 WebView 弹窗；返回 dialog 引用，调用方可持有以便关闭。"""
    dlg = WebViewDialog(url=url, title=title, parent=parent, modal=False)
    dlg.show()
    return dlg