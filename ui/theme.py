"""
全局主题：简约果冻风格（jelly / glassmorphism）

设计语言：
- 圆角 12px（卡片）/ 8px（按钮）/ 16px（大容器）
- 柔和阴影 + 微妙高光（按下时位移 + 颜色加深）
- 背景使用浅冷色 + 玻璃半透（rgba alpha ≈ 200~230）
- 强调色：薄荷绿（成功/连接）/ 蜜桃橙（注意/警告）/ 樱粉（急停/危险）
  / 海蓝（主操作）/ 淡紫（次要操作）；饱和度统一压到 35~50%
- 文字主色 #2c3e50，次 #7f8c8d，亮底色 #ffffff / 玻璃底 rgba(255,255,255,200)

调用约定：
- `apply_app_style(app)`：启动时调一次（设置全局 QSS）
- `btn(...)` / `card(...)` / `pill(...)`：单独控件工厂
- `PALETTE` 字典：业务方按需取色
"""
from __future__ import annotations

from typing import Optional

from PySide6.QtWidgets import QWidget, QPushButton, QGroupBox
from PySide6.QtGui import QColor
from PySide6.QtCore import Qt


# ============================================================
# 调色板
# ============================================================
PALETTE = {
    # 背景
    "bg":              "#f4f7fb",   # 主背景：淡冷蓝
    "bg_alt":          "#eaf0f7",   # 容器背景
    "surface":         "#ffffff",   # 卡片/控件底
    "surface_glass":   "rgba(255,255,255,200)",  # 玻璃感

    # 文字
    "text":            "#2c3e50",   # 主文字
    "text_secondary":  "#5a6a7e",   # 次文字
    "text_muted":      "#8a99ad",   # 弱化文字
    "text_invert":     "#ffffff",   # 深色按钮上的文字

    # 边框 / 分割
    "border":          "rgba(140,160,190,60)",   # 1px 淡描边
    "border_strong":   "rgba(140,160,190,120)",

    # 阴影
    "shadow":          "rgba(60,90,140,30)",      # 落影 alpha 30
    "shadow_strong":   "rgba(60,90,140,55)",

    # 强调色（低饱和）
    "primary":         "#4a90e2",   # 海蓝
    "primary_dark":    "#357abd",
    "primary_light":   "rgba(74,144,226,30)",
    "primary_glass":   "rgba(74,144,226,90)",   # 半透明按钮底
    "primary_border":  "rgba(74,144,226,180)",  # 半透明描边

    "success":         "#5ec99f",   # 薄荷
    "success_dark":    "#3fa37a",
    "success_glass":   "rgba(94,201,159,90)",
    "success_border":  "rgba(94,201,159,180)",

    "warning":         "#f4a261",   # 蜜桃
    "warning_dark":    "#d98a4a",
    "warning_glass":   "rgba(244,162,97,90)",
    "warning_border":  "rgba(244,162,97,180)",

    "danger":          "#ef6f6c",   # 樱粉
    "danger_dark":     "#c44a47",
    "danger_glass":    "rgba(239,111,108,90)",
    "danger_border":   "rgba(239,111,108,180)",

    "info":            "#8aa1b9",   # 雾蓝
    "purple":          "#a78bcf",   # 淡紫
    "purple_dark":     "#8466b3",
    "purple_glass":    "rgba(167,139,207,90)",
    "purple_border":   "rgba(167,139,207,180)",

    "cyan":            "#5fb3c1",   # 冰青
    "cyan_glass":      "rgba(95,179,193,90)",
    "cyan_border":     "rgba(95,179,193,180)",
}


# ============================================================
# 圆角 / 间距常量
# ============================================================
RADIUS = {
    "btn":   "10px",
    "card":  "14px",
    "big":   "18px",
    "pill":  "16px",
}
SPACING = {
    "tight":  6,
    "normal": 10,
    "loose":  16,
}


# ============================================================
# 公共 QSS 片段（按需拼接）
# ============================================================
def _base() -> str:
    """应用级 QSS：所有 QWidget 默认外观。"""
    p = PALETTE
    return f"""
    /* 全局基调 */
    QWidget {{
        background: transparent;
        color: {p['text']};
        font-family: "Microsoft YaHei", "PingFang SC", "Segoe UI", sans-serif;
        font-size: 13px;
    }}

    /* 主窗口 */
    QMainWindow, QDialog {{
        background: {p['bg']};
    }}

    /* 容器背景（central widget 等） */
    QWidget#centralWidget {{
        background: {p['bg']};
    }}

    /* 分组框（卡片） */
    QGroupBox {{
        background: {p['surface_glass']};
        border: 1px solid {p['border']};
        border-radius: {RADIUS['card']};
        margin-top: 12px;
        padding: 14px 12px 12px 12px;
        font-weight: bold;
    }}
    QGroupBox::title {{
        subcontrol-origin: margin;
        subcontrol-position: top left;
        left: 14px;
        padding: 0 8px;
        background: transparent;
        color: {p['text_secondary']};
    }}

    /* 标签 */
    QLabel {{
        background: transparent;
        color: {p['text']};
    }}
    QLabel[role="muted"] {{
        color: {p['text_muted']};
    }}
    QLabel[role="secondary"] {{
        color: {p['text_secondary']};
    }}
    QLabel[role="primary"] {{
        color: {p['primary']};
        font-weight: bold;
    }}
    QLabel[role="purple"] {{
        color: {p['purple']};
        font-weight: bold;
    }}
    QLabel[role="success"] {{
        color: {p['success_dark']};
        font-weight: bold;
    }}

    /* 输入框 */
    QLineEdit, QDoubleSpinBox, QSpinBox, QComboBox {{
        background: {p['surface']};
        border: 1px solid {p['border_strong']};
        border-radius: {RADIUS['btn']};
        padding: 6px 10px;
        color: {p['text']};
        selection-background-color: {p['primary_light']};
    }}
    QLineEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QComboBox:focus {{
        border: 1.5px solid {p['primary']};
    }}
    QLineEdit:disabled {{
        background: rgba(220,228,238,150);
        color: {p['text_muted']};
    }}

    /* 下拉箭头 */
    QComboBox::drop-down {{
        border: none;
        width: 22px;
    }}
    QComboBox QAbstractItemView {{
        background: {p['surface']};
        border: 1px solid {p['border_strong']};
        border-radius: 8px;
        padding: 4px;
        selection-background-color: {p['primary_light']};
    }}

    /* 滑条 */
    QSlider::groove:horizontal {{
        background: {p['border']};
        height: 6px;
        border-radius: 3px;
    }}
    QSlider::handle:horizontal {{
        background: {p['primary']};
        width: 18px; height: 18px;
        margin: -7px 0;
        border-radius: 9px;
        border: 2px solid {p['surface']};
    }}
    QSlider::handle:horizontal:hover {{
        background: {p['primary_dark']};
    }}
    QSlider::sub-page:horizontal {{
        background: {p['primary']};
        height: 6px;
        border-radius: 3px;
    }}

    /* 状态栏 */
    QStatusBar {{
        background: {p['surface_glass']};
        color: {p['text_secondary']};
        border-top: 1px solid {p['border']};
        padding: 6px 12px;
        min-height: 28px;
    }}
    QStatusBar QLabel {{
        color: {p['text_secondary']};
        font-size: 13px;
        padding: 2px 6px;
    }}
    QStatusBar::item {{
        border: none;
    }}

    /* 分割器 */
    QSplitter::handle:horizontal {{
        background: transparent;
        width: 8px;
    }}
    QSplitter::handle:vertical {{
        background: transparent;
        height: 8px;
    }}
    QSplitter::handle:horizontal:hover {{
        background: {p['primary_light']};
        border-radius: 4px;
    }}

    /* 滚动条 */
    QScrollBar:vertical {{
        background: transparent;
        width: 10px;
        margin: 0;
    }}
    QScrollBar::handle:vertical {{
        background: {p['border_strong']};
        border-radius: 5px;
        min-height: 30px;
    }}
    QScrollBar::handle:vertical:hover {{
        background: {p['info']};
    }}
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{
        background: transparent; height: 0;
    }}
    QScrollBar:horizontal {{
        background: transparent;
        height: 10px;
    }}
    QScrollBar::handle:horizontal {{
        background: {p['border_strong']};
        border-radius: 5px;
        min-width: 30px;
    }}
    QScrollBar::handle:horizontal:hover {{
        background: {p['info']};
    }}
    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {{
        background: transparent; width: 0;
    }}

    /* 工具提示 */
    QToolTip {{
        background: {p['text']};
        color: {p['text_invert']};
        border: none;
        border-radius: 6px;
        padding: 4px 8px;
    }}
    """


def _buttons() -> str:
    p = PALETTE
    r = RADIUS
    return f"""
    /* 通用按钮：默认（淡色"果冻"质感，无描边） */
    QPushButton {{
        background: {p['surface']};
        color: {p['text']};
        border: none;
        border-radius: {r['btn']};
        padding: 7px 16px;
        font-weight: 500;
    }}
    QPushButton:hover {{
        background: {p['bg_alt']};
    }}
    QPushButton:pressed {{
        background: {p['primary_light']};
        color: {p['primary_dark']};
    }}
    QPushButton:disabled {{
        background: rgba(220,228,238,120);
        color: {p['text_muted']};
    }}
    QPushButton:checkable:checked {{
        background: {p['primary_light']};
        color: {p['primary_dark']};
        border: none;
    }}

    /* 主操作（海蓝） */
    QPushButton[role="primary"] {{
        background: {p['primary_glass']};
        color: {p['primary_dark']};
        border: none;
    }}
    QPushButton[role="primary"]:hover {{
        background: {p['primary']};
        color: {p['text_invert']};
    }}
    QPushButton[role="primary"]:pressed {{
        background: {p['primary_dark']};
        color: {p['text_invert']};
        padding-top: 8px; padding-bottom: 6px;
    }}
    QPushButton[role="primary"]:disabled {{
        background: rgba(220,228,238,120);
        color: {p['text_muted']};
    }}

    /* 成功（薄荷） */
    QPushButton[role="success"] {{
        background: {p['success_glass']};
        color: {p['success_dark']};
        border: none;
    }}
    QPushButton[role="success"]:hover {{
        background: {p['success']};
        color: {p['text_invert']};
    }}
    QPushButton[role="success"]:disabled {{
        background: rgba(220,228,238,120);
        color: {p['text_muted']};
    }}

    /* 警告（蜜桃） */
    QPushButton[role="warning"] {{
        background: {p['warning_glass']};
        color: {p['warning_dark']};
        border: none;
    }}
    QPushButton[role="warning"]:hover {{
        background: {p['warning']};
        color: {p['text_invert']};
    }}
    QPushButton[role="warning"]:disabled {{
        background: rgba(220,228,238,120);
        color: {p['text_muted']};
    }}

    /* 危险（樱粉） */
    QPushButton[role="danger"] {{
        background: {p['danger_glass']};
        color: {p['danger_dark']};
        border: none;
    }}
    QPushButton[role="danger"]:hover {{
        background: {p['danger']};
        color: {p['text_invert']};
    }}
    QPushButton[role="danger"]:disabled {{
        background: rgba(220,228,238,120);
        color: {p['text_muted']};
    }}

    /* 紫色（次要操作） */
    QPushButton[role="purple"] {{
        background: {p['purple_glass']};
        color: {p['purple_dark']};
        border: none;
    }}
    QPushButton[role="purple"]:hover {{
        background: {p['purple']};
        color: {p['text_invert']};
    }}
    QPushButton[role="purple"]:pressed {{
        background: {p['purple_dark']};
        color: {p['text_invert']};
    }}
    QPushButton[role="purple"]:disabled {{
        background: rgba(220,228,238,120);
        color: {p['text_muted']};
    }}

    /* 连接断开态（动态切换） */
    QPushButton[role="connected"] {{
        background: {p['success_glass']};
        color: {p['success_dark']};
        border: none;
    }}
    QPushButton[role="connected"]:hover {{
        background: {p['success']};
        color: {p['text_invert']};
    }}
    QPushButton[role="connected"]:disabled {{
        background: rgba(220,228,238,120);
        color: {p['text_muted']};
    }}
    """


def _frames() -> str:
    p = PALETTE
    return f"""
    /* 顶部品牌/连接卡 */
    QFrame#topBar {{
        background: {p['surface_glass']};
        border: 1px solid {p['border']};
        border-radius: {RADIUS['card']};
    }}

    /* 状态点（连接状态用） */
    QLabel[role="statusDot"][state="on"] {{
        color: {p['success']};
        font-weight: bold;
    }}
    QLabel[role="statusDot"][state="off"] {{
        color: {p['text_muted']};
        font-weight: bold;
    }}
    QLabel[role="statusDot"][state="warn"] {{
        color: {p['warning']};
        font-weight: bold;
    }}
    QLabel[role="statusDot"][state="error"] {{
        color: {p['danger']};
        font-weight: bold;
    }}
    """


def _panels() -> str:
    p = PALETTE
    return f"""
    /* 主面板容器（左右分栏的内层） */
    QWidget#panel {{
        background: {p['surface_glass']};
        border: 1px solid {p['border']};
        border-radius: {RADIUS['card']};
    }}

    /* 子组（半透明卡片） */
    QGroupBox[role="sub"] {{
        background: rgba(255,255,255,140);
        border: 1px solid {p['border']};
        border-radius: 12px;
        margin-top: 14px;
        padding: 12px 10px 10px 10px;
    }}
    QGroupBox[role="sub"]::title {{
        subcontrol-origin: margin;
        subcontrol-position: top left;
        left: 12px;
        padding: 0 6px;
        background: transparent;
        color: {p['text_secondary']};
    }}
    """


# ============================================================
# 公开 API
# ============================================================
GLOBAL_QSS = _base() + _buttons() + _frames() + _panels()


def apply_app_style(app) -> None:
    """应用一次全局 QSS（在 QApplication 创建后立即调用）。"""
    app.setStyleSheet(GLOBAL_QSS)


def set_role(widget: QWidget, role: str) -> None:
    """运行时给某个控件打 role（用于状态切换，如按钮从默认变 connected）。"""
    widget.setProperty("role", role)
    # 强制刷新样式
    widget.style().unpolish(widget)
    widget.style().polish(widget)


def set_state(widget: QWidget, state: str) -> None:
    """运行时给状态点控件打 state（on / off / warn / error）。"""
    widget.setProperty("state", state)
    widget.style().unpolish(widget)
    widget.style().polish(widget)


def set_muted(widget: QWidget, muted: bool = True) -> None:
    """标签 muted / secondary 切换。"""
    widget.setProperty("role", "muted" if muted else "secondary")
    widget.style().unpolish(widget)
    widget.style().polish(widget)


def soft_shadow(widget: QWidget, blur: int = 18, offset: int = 2,
                alpha: int = 60) -> None:
    """给任一控件加柔和阴影（用于卡片/弹窗）。"""
    from PySide6.QtWidgets import QGraphicsDropShadowEffect
    eff = QGraphicsDropShadowEffect(widget)
    eff.setBlurRadius(blur)
    eff.setOffset(0, offset)
    eff.setColor(QColor(60, 90, 140, alpha))
    widget.setGraphicsEffect(eff)


# ============================================================
# 预设好的工厂按钮（按业务色）
# ============================================================
def _set_btn_font(btn: QPushButton, size: int, bold: bool = False):
    """直接设 QFont，不碰 QSS — 避免破坏 role QSS 样式。"""
    f = btn.font()
    f.setPointSize(size)
    if bold:
        f.setBold(True)
    btn.setFont(f)


def make_button(text: str, role: str = "",
                tooltip: str = "",
                checkable: bool = False,
                min_height: int = 0) -> QPushButton:
    """构造一个带预设 role 的按钮，简化调用。"""
    b = QPushButton(text)
    if role:
        b.setProperty("role", role)
        # 强制 QSS 立刻重算 role 选择器，否则部分环境下首次显示前不刷新
        b.style().unpolish(b)
        b.style().polish(b)
    if tooltip:
        b.setToolTip(tooltip)
    b.setCheckable(checkable)
    if min_height:
        b.setMinimumHeight(min_height)
    return b
