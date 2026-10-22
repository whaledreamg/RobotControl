"""
Revo2 灵巧手主面板（基于 HTML revoHands 的简化版）

主界面只放"开/合"两个快捷按钮（左/右各 2 个） + View 按钮。
6 个自由度的精确微调放在 View 弹窗里。

布局：
┌────────────────────────────────────────┐
│  左手 Left  ●在线  [View]              │
│  [✊ 开 抓取]              [🖐 合 释放]  │
├────────────────────────────────────────┤
│  右手 Right ●离线 [View]              │
│  [✊ 开 抓取]              [🖐 合 释放]  │
├────────────────────────────────────────┤
│  [连接双手]   [⏹ 急停]                │
└────────────────────────────────────────┘
"""
from __future__ import annotations

from typing import Dict, Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QFrame, QSizePolicy,
)

from communication.revo2_hand import (
    DualHandManager, Revo2HandClient, HandState,
    DOF_NAME_LIST, DOF_NAME_CN_LIST, DOF_MAX, DOF_COUNT,
    raw_to_deg, PRESETS,
)
from ui.theme import make_button, PALETTE, set_state


class HandPanel(QFrame):
    """单只手的简化控制面板（主界面）

    ┌────────────────────────────┐
    │ 🤚 左手 Left  ● 离线  [View]│
    │ [✊ 开 抓取]   [🖐 合 释放]  │
    │ 拇指: 0°/0° 食指: 0° ...   │
    └────────────────────────────┘
    """
    grab_requested = Signal(str)
    # 跨线程安全的状态推送（替代 QTimer.singleShot）
    state_updated = Signal(object)  # HandState
    release_requested = Signal(str)
    view_requested = Signal(str)

    def __init__(self, side: str, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.side = side
        self.client: Optional[Revo2HandClient] = None
        self.setObjectName("handPanel")
        self.setStyleSheet(f"""
            QFrame#handPanel {{
                background: rgba(255,255,255,180);
                border: 1px solid {PALETTE['border']};
                border-radius: 12px;
                padding: 8px;
            }}
        """)
        self._build()
        self.set_enabled(False)

    def _build(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(8)
        # 标题行
        title_row = QHBoxLayout()
        title_row.setSpacing(8)
        title_row.setContentsMargins(0, 0, 0, 0)
        side_cn = "左手" if self.side == "left" else "右手"
        self.title_label = QLabel(f"🤚  {side_cn}  {self.side.capitalize()}")
        self.title_label.setStyleSheet("font-weight: bold; font-size: 14px;")
        title_row.addWidget(self.title_label)
        title_row.addStretch(1)
        # 状态点
        self.status_dot = QLabel("●")
        self.status_dot.setProperty("role", "statusDot")
        set_state(self.status_dot, "off")
        title_row.addWidget(self.status_dot)
        self.status_label = QLabel("未初始化")
        self.status_label.setProperty("role", "muted")
        self.status_label.setStyleSheet("font-size: 11px;")
        title_row.addWidget(self.status_label)
        # View 按钮
        self.btn_view = make_button("View", role="purple",
                                    tooltip="6 自由度微调 + Modbus 调试")
        self.btn_view.setFixedHeight(24)
        self.btn_view.setMinimumWidth(50)
        self.btn_view.clicked.connect(lambda: self.view_requested.emit(self.side))
        title_row.addWidget(self.btn_view)
        layout.addLayout(title_row)
        # 开/合按钮
        btn_row = QHBoxLayout()
        btn_row.setSpacing(8)
        btn_row.setContentsMargins(0, 0, 0, 0)
        self.btn_grab = make_button("✊  开  抓取", role="success")
        self.btn_grab.setMinimumHeight(36)
        self.btn_grab.setStyleSheet(
            self.btn_grab.styleSheet() + "font-weight: bold; font-size: 13px;")
        self.btn_grab.clicked.connect(lambda: self.grab_requested.emit(self.side))
        btn_row.addWidget(self.btn_grab, 1)
        self.btn_release = make_button("🖐  合  释放", role="warning")
        self.btn_release.setMinimumHeight(36)
        self.btn_release.setStyleSheet(
            self.btn_release.styleSheet() + "font-weight: bold; font-size: 13px;")
        self.btn_release.clicked.connect(lambda: self.release_requested.emit(self.side))
        btn_row.addWidget(self.btn_release, 1)
        layout.addLayout(btn_row)
        # 错误
        self.err_label = QLabel("")
        self.err_label.setProperty("role", "muted")
        self.err_label.setStyleSheet("font-size: 10px; color: #c44a47;")
        self.err_label.setWordWrap(True)
        layout.addWidget(self.err_label)

    def _on_state_update(self, state: HandState):
        self.state_updated.emit(state)

    def _apply_state(self, state: HandState):
        # 状态点
        if state.connected and state.inited:
            set_state(self.status_dot, "on")
            self.status_label.setText("在线")
        elif state.connected:
            set_state(self.status_dot, "warn")
            self.status_label.setText("已连接（通道未 init）")
        else:
            set_state(self.status_dot, "off")
            self.status_label.setText("未初始化")
        # 错误
        if state.last_error:
            self.err_label.setText(f"⚠ {state.last_error}")
        else:
            self.err_label.setText("")

    def bind_client(self, client: Revo2HandClient):
        self.client = client
        client.register_state_callback(self._on_state_update)
        self.state_updated.connect(self._apply_state)

    def set_enabled(self, enabled: bool):
        self.btn_grab.setEnabled(enabled)
        self.btn_release.setEnabled(enabled)
        self.btn_view.setEnabled(True)
        if not enabled:
            set_state(self.status_dot, "off")
            self.status_label.setText("未初始化")
            self.err_label.setText("")


class DualHandPanel(QWidget):
    """双手控制总面板"""
    grab_requested = Signal(str)
    release_requested = Signal(str)
    view_requested = Signal(str)
    init_requested = Signal()
    stop_all_requested = Signal()

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.manager: Optional[DualHandManager] = None
        self._build()

    def _build(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)
        # hand_widget 自身设 Maximum 高度策略，按内容收缩（不与升降柱抢空间）
        self.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Maximum)
        self.left_panel = HandPanel("left")
        self.right_panel = HandPanel("right")
        for p in (self.left_panel, self.right_panel):
            p.grab_requested.connect(self.grab_requested.emit)
            p.release_requested.connect(self.release_requested.emit)
            p.view_requested.connect(self.view_requested.emit)
            # HandPanel 按内容（不强制限高，让它按实际子组件展开）
            p.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Preferred)
            layout.addWidget(p)
        # 底部按钮
        bottom = QHBoxLayout()
        bottom.setSpacing(8)
        bottom.setContentsMargins(0, 0, 0, 0)
        self.btn_init = make_button("连接双手", role="success",
                                    tooltip="setType(Generic485) + ec2rs485/init")
        self.btn_init.setMinimumHeight(30)
        self.btn_init.clicked.connect(self.init_requested.emit)
        bottom.addWidget(self.btn_init, 1)
        self.btn_stop_all = make_button("⏹  急停", role="danger")
        self.btn_stop_all.setMinimumHeight(30)
        self.btn_stop_all.setEnabled(False)
        self.btn_stop_all.clicked.connect(self.stop_all_requested.emit)
        bottom.addWidget(self.btn_stop_all, 1)
        layout.addLayout(bottom)

    def bind_manager(self, manager: DualHandManager):
        self.manager = manager
        self.left_panel.bind_client(manager.left)
        self.right_panel.bind_client(manager.right)

    def set_arm_connected(self, connected: bool):
        if not connected:
            self.left_panel.set_enabled(False)
            self.right_panel.set_enabled(False)
            self.btn_stop_all.setEnabled(False)
            self.btn_init.setEnabled(False)
        else:
            self.btn_init.setEnabled(True)

    def on_init_done(self, left_ok: bool, right_ok: bool):
        self.left_panel.set_enabled(left_ok)
        self.right_panel.set_enabled(right_ok)
        self.btn_stop_all.setEnabled(left_ok or right_ok)
        self.btn_init.setText("重新连接")
