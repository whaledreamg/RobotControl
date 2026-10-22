"""
消息弹窗系统
参考 xcoredatatool/form/component/message.cpp 实现：
- 顶部居中堆叠显示
- 支持 4 种类型：SUCCESS / INFO / WARNING / ERROR
- 自动堆叠排序、淡入淡出动画
- 错误类型有手动关闭按钮，其他类型自动消失
- 错误类型去重：相同内容不会重复弹
"""
from PySide6.QtWidgets import (
    QWidget, QLabel, QPushButton, QHBoxLayout, QVBoxLayout,
    QGraphicsDropShadowEffect
)
from PySide6.QtCore import (
    Qt, QTimer, QPropertyAnimation, QAbstractAnimation, QPoint, QObject, Signal, QRect, QEvent
)
from PySide6.QtGui import QFont, QColor, QPainter, QBrush, QPen


# 消息类型常量
class MessageType:
    INFO = 0
    SUCCESS = 1
    WARNING = 2
    ERROR = 3


# 消息项之间的间距
_MESSAGE_MARGIN = 12
# 默认持续时间（ms）
_DEFAULT_DURATION = 2500
# 错误类型是否可手动关闭
_ERROR_HAS_CLOSE = True


class MessageItem(QWidget):
    """单条消息"""

    # 信号占位（class body 之外用 PySide6 的方式定义，见文件末尾）
    item_ready_removed: "Signal"
    item_removed: "Signal"

    def __init__(self, parent, msg_type: int, content: str, duration: int = _DEFAULT_DURATION):
        super().__init__(parent)
        self._msg_type = msg_type
        self._content = content
        self._duration = duration if duration > 0 else _DEFAULT_DURATION
        self._life_timer = QTimer(self)
        self._life_timer.setSingleShot(True)
        self._life_timer.timeout.connect(self._on_life_timeout)

        self.setObjectName("message_item")
        # 注意：作为 QWidget 子类的弹窗提示，**不要**加 FramelessWindowHint —
        #       加了会让 widget 脱离父窗口变成独立 top-level，关闭按钮的鼠标
        #       事件会被路由到独立的 native window，导致点不到 / 关不掉。
        #       背景圆角与边框已由 paintEvent 自绘，无边框观感由样式达成。
        # 不抢焦点，鼠标事件可穿透到兄弟 widget
        self.setAttribute(Qt.WA_TransparentForMouseEvents, False)

        # 类型样式：颜色 + 左侧图标
        # 注意：背景 alpha < 255，做出半透明效果
        style_conf = {
            MessageType.SUCCESS: {
                "border": "#67C23A",
                "bg": QColor(240, 249, 235, 220),
                "color": "rgb(60,160,40)",
                "icon": "✓",
            },
            MessageType.ERROR: {
                "border": "#F56C6C",
                "bg": QColor(254, 240, 240, 225),
                "color": "rgb(200,80,80)",
                "icon": "✕",
            },
            MessageType.WARNING: {
                "border": "#E6A23C",
                "bg": QColor(253, 246, 236, 225),
                "color": "rgb(190,130,40)",
                "icon": "!",
            },
            MessageType.INFO: {
                "border": "#909399",
                "bg": QColor(237, 242, 252, 220),
                "color": "rgb(110,115,125)",
                "icon": "i",
            },
        }
        self._conf = style_conf.get(msg_type, style_conf[MessageType.INFO])
        conf = self._conf
        # QSS 只用于文字/子控件，背景由 paintEvent 自绘（这样才能用半透明 QColor）
        self.setStyleSheet(
            f"QWidget#message_item{{background:transparent;border:none;}}"
            f"QLabel{{border:none;background:transparent;"
            f"font-family:Microsoft Yahei, 'PingFang SC', sans-serif;font-size:14px;"
            f"color:{conf['color']};}}"
            f"QPushButton{{border:none;background:transparent;"
            f"color:{conf['color']};font-weight:bold;font-family:Arial;}}"
            f"QPushButton:hover{{color:rgba(0,0,0,0.55);}}"
        )

        # 加一个柔和阴影
        shadow = QGraphicsDropShadowEffect(self)
        shadow.setBlurRadius(18)
        shadow.setOffset(0, 2)
        shadow.setColor(QColor(0, 0, 0, 60))
        self.setGraphicsEffect(shadow)

        # 横向布局: [icon] [content] [close?]
        layout = QHBoxLayout(self)
        layout.setContentsMargins(20, 14, 14, 14)
        layout.setSpacing(12)

        self._icon_label = QLabel(conf["icon"], self)
        icon_font = QFont("Arial", 15, QFont.Bold)
        self._icon_label.setFont(icon_font)
        self._icon_label.setFixedWidth(22)
        self._icon_label.setAlignment(Qt.AlignCenter)
        layout.addWidget(self._icon_label)

        self._content_label = QLabel(content, self)
        self._content_label.setWordWrap(False)
        self._content_label.setMinimumWidth(120)
        self._content_label.setMaximumWidth(640)
        layout.addWidget(self._content_label, 1)

        # 错误类型有手动关闭按钮
        if msg_type == MessageType.ERROR and _ERROR_HAS_CLOSE:
            self._close_btn = QPushButton("×", self)
            self._close_btn.setFixedSize(22, 22)
            self._close_btn.setCursor(Qt.PointingHandCursor)
            self._close_btn.setFont(QFont("Arial", 16, QFont.Bold))
            self._close_btn.clicked.connect(self._on_close_clicked)
            layout.addWidget(self._close_btn)
            # 错误类型：不自动消失（duration 设为 0）
            self._duration = 0
        else:
            self._close_btn = None

        # 计算并设置尺寸（最小宽度 220，最小高度 48）
        self.adjustSize()
        hint = self.sizeHint()
        w = max(hint.width(), 220)
        h = max(hint.height(), 48)
        self.setFixedSize(w, h)
        self.hide()

    def paintEvent(self, event):
        """自绘半透明圆角矩形背景"""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        # 留 1px 给边框
        rect = self.rect().adjusted(0, 0, -1, -1)
        radius = 8
        # 填充半透明背景
        painter.setBrush(QBrush(self._conf["bg"]))
        # 边框
        border_color = QColor(self._conf["border"])
        border_color.setAlpha(220)
        painter.setPen(QPen(border_color, 1))
        painter.drawRoundedRect(rect, radius, radius)
        super().paintEvent(event)

    def is_error_message(self, content: str) -> bool:
        return self._msg_type == MessageType.ERROR and self._content == content

    def show_animated(self):
        """显示 + 淡入"""
        self.show()
        # 提升到顶层，确保不被其他 widget 遮挡
        self.raise_()
        # 启动生命周期定时器
        if self._duration > 0:
            self._life_timer.start(self._duration)
        self._appear_animation()

    def close_animated(self):
        """淡出 + 关闭"""
        self._life_timer.stop()
        self._disappear_animation()

    def _on_life_timeout(self):
        self.item_ready_removed.emit(self)

    def _on_close_clicked(self):
        self._life_timer.stop()
        self.item_ready_removed.emit(self)

    def _appear_animation(self):
        """淡入动画：通过 windowOpacity 实现，避免与 QGraphicsDropShadowEffect 冲突"""
        self.setWindowOpacity(0.0)
        anim = QPropertyAnimation(self, b"windowOpacity", self)
        anim.setDuration(180)
        anim.setStartValue(0.0)
        anim.setEndValue(1.0)
        anim.start(QAbstractAnimation.DeleteWhenStopped)

    def _disappear_animation(self):
        """淡出动画：通过 windowOpacity 实现"""
        anim = QPropertyAnimation(self, b"windowOpacity", self)
        anim.setDuration(220)
        anim.setStartValue(self.windowOpacity())
        anim.setEndValue(0.0)

        def _on_finished():
            self.item_removed.emit(self)
            self.deleteLater()

        anim.finished.connect(_on_finished)
        anim.start(QAbstractAnimation.DeleteWhenStopped)
        # 兜底：动画在某些极端场景下不会触发 finished（如 widget 被隐藏或动画被
        # 父对象提前销毁），这里 300ms 后强制走 close 流程，保证不留下「关不掉」
        # 的孤儿 widget。finished 正常触发时 deleteLater 后这条调度的 lambda 会
        # 因 QObject 已失效而自动忽略。
        QTimer.singleShot(400, self._force_close_fallback)

    def _force_close_fallback(self):
        try:
            if self.isVisible():
                # 还可见 → 动画失败/被打断 → 强制关
                self.item_removed.emit(self)
            self.deleteLater()
        except RuntimeError:
            # 已 deleteLater 的对象，忽略
            pass


# 在类外补上信号声明（PySide6 写法）
MessageItem.item_ready_removed = Signal(object)
MessageItem.item_removed = Signal(object)


class MessageManager(QObject):
    """消息管理器：堆叠管理多条消息

    用法：
        mm = MessageManager(parent_widget)
        mm.push(MessageType.ERROR, "连接失败")
        mm.push(MessageType.SUCCESS, "保存成功")
    """

    def __init__(self, parent_widget: QWidget):
        super().__init__(parent_widget)
        self._parent = parent_widget
        self._items = []  # list[MessageItem]
        # 监听父窗口尺寸变化，重排
        if hasattr(parent_widget, 'installEventFilter'):
            parent_widget.installEventFilter(self)
        self._margin_top = 20  # 距离顶部

    def push(self, msg_type: int, content: str, duration: int = _DEFAULT_DURATION):
        """推入一条消息"""
        # 错误类型去重
        if msg_type == MessageType.ERROR:
            for it in self._items:
                if it.is_error_message(content):
                    return

        item = MessageItem(self._parent, msg_type, content, duration)
        item.item_ready_removed.connect(self._on_item_ready_removed)
        item.item_removed.connect(self._on_item_removed)

        # 计算垂直位置：堆叠在已有消息下方
        height = self._margin_top
        for existing in self._items:
            height += _MESSAGE_MARGIN + existing.height()

        # 水平居中
        x = (self._parent.width() - item.width()) // 2
        item.move(max(0, x), height)
        self._items.append(item)
        item.show_animated()

    # 便捷方法
    def info(self, content: str, duration: int = _DEFAULT_DURATION):
        self.push(MessageType.INFO, content, duration)

    def success(self, content: str, duration: int = _DEFAULT_DURATION):
        self.push(MessageType.SUCCESS, content, duration)

    def warning(self, content: str, duration: int = _DEFAULT_DURATION):
        self.push(MessageType.WARNING, content, duration)

    def error(self, content: str):
        # 错误类型无自动消失
        self.push(MessageType.ERROR, content, 0)

    def _on_item_ready_removed(self, item):
        """生命周期到 / 用户点关闭"""
        item.close_animated()

    def _on_item_removed(self, item):
        """淡出动画完成，移除"""
        if item in self._items:
            self._items.remove(item)
        self._relayout()

    def _relayout(self):
        """重新堆叠布局"""
        height = self._margin_top
        for it in self._items:
            x = (self._parent.width() - it.width()) // 2
            target = QRect(max(0, x), height, it.width(), it.height())
            anim = QPropertyAnimation(it, "geometry", self)
            anim.setDuration(200)
            anim.setStartValue(it.geometry())
            anim.setEndValue(target)
            anim.start(QAbstractAnimation.DeleteWhenStopped)
            height += _MESSAGE_MARGIN + it.height()

    def eventFilter(self, obj, event):
        """父窗口 resize 时重排"""
        if obj is self._parent and event.type() == QEvent.Resize:
            # debounce: 用 singleShot 避免 resize 风暴
            QTimer.singleShot(50, self._relayout)
        return super().eventFilter(obj, event)


# 全局单例（可选）
_global_manager: "MessageManager | None" = None


def set_global_manager(manager: "MessageManager"):
    global _global_manager
    _global_manager = manager


def get_global_manager() -> "MessageManager | None":
    return _global_manager


# 便捷全局函数
def info(content: str):
    if _global_manager:
        _global_manager.info(content)


def success(content: str):
    if _global_manager:
        _global_manager.success(content)


def warning(content: str):
    if _global_manager:
        _global_manager.warning(content)


def error(content: str):
    if _global_manager:
        _global_manager.error(content)
