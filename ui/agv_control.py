"""
AGV 控制模块
包含方向控制、导航站点下拉、状态显示等
"""
import math
import time
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QGroupBox, QLabel,
    QPushButton, QSlider, QComboBox, QGridLayout, QFrame
)
from PySide6.QtCore import Qt, Signal, Slot, QThread, QCoreApplication, QEvent
from PySide6.QtGui import QFont, QKeyEvent
from ui.map_info_dialog import MapInfoDialog
from ui.theme import make_button, set_role, _set_btn_font, PALETTE


# AGV 开环控制 WASD 键位映射（按下时持续移动、释放即停）
# 设计：与现有方向按钮走完全相同的信号路径（direction_changed / stop_requested），
# 主窗口 / AGV 控制客户端不需要任何改动。
#   W → 前进    S → 后退    A → 左转    D → 右转
# 为避免抢输入焦点（速度/站点 QComboBox 等输入时按 W 应该是文本输入），
# 键位仅在 AGV widget 自身拿到焦点时响应；focusPolicy 默认 NoFocus，
# 用 focusWidget()/点击进入时设 StrongFocus。
_AGV_KEY_TO_DIR = {
    Qt.Key_W: "forward",
    Qt.Key_Up: "forward",
    Qt.Key_S: "backward",
    Qt.Key_Down: "backward",
    Qt.Key_A: "left",
    Qt.Key_Left: "left",
    Qt.Key_D: "right",
    Qt.Key_Right: "right",
}
_AGV_DIR_TO_BTN = {
    "forward":  "btn_forward",
    "backward": "btn_backward",
    "left":     "btn_left",
    "right":    "btn_right",
}


class AGVControlWidget(QWidget):
    """AGV 控制模块（简约果冻风）"""

    speed_changed = Signal(float)
    direction_changed = Signal(str)
    nav_start_requested = Signal(str)
    nav_pause_requested = Signal()
    nav_continue_requested = Signal()
    nav_cancel_requested = Signal()
    emergency_stop_requested = Signal()
    stop_requested = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._current_direction = None
        self._nav_status = "idle"
        self._last_move_ts = 0.0
        self._move_interval = 0.1
        self._maps = []
        self._current_map = ""
        self._map_data = {}
        self._map_dialog = None
        self._last_agv_pose = None
        self._last_lasers = None
        # 键盘控制状态：当前按下的方向键列表（按按下顺序）。
        # 多个键同时按（如 W+D = 右前）按方向分别报，主窗口的方向处理
        # 仍是单方向的 start_continuous_move，多键场景以「最后按下的」键
        # 为准（与主流游戏手柄语义一致）；松键时只要还有其它方向键按着
        # 就在着不报 stop，方向切换到剩余键中最后按下的。
        self._held_keys: list = []
        # 接受焦点才能收 key 事件：用户可点击方向按钮 / 速度滑块
        # 等进入 widget；输入框里的 W 仍按文本输入（焦点在子控件时
        # key 事件被吞、不会到 widget 这层）。
        self.setFocusPolicy(Qt.StrongFocus)
        self._init_ui()

    def _init_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        layout.addWidget(self._create_speed_group())
        layout.addWidget(self._create_direction_group())
        layout.addWidget(self._create_navigation_group())
        layout.addWidget(self._create_status_group())
        layout.addStretch()

    def _create_speed_group(self) -> QGroupBox:
        group = QGroupBox("速度控制")
        layout = QVBoxLayout(group)
        layout.setSpacing(4)

        self.speed_slider = QSlider(Qt.Horizontal)
        self.speed_slider.setMinimum(0)
        self.speed_slider.setMaximum(100)
        self.speed_slider.setValue(50)
        self.speed_slider.setTickPosition(QSlider.TicksBelow)
        self.speed_slider.setTickInterval(10)
        self.speed_slider.valueChanged.connect(self._on_speed_changed)
        layout.addWidget(self.speed_slider)

        self.speed_label = QLabel("速度: 0.50 m/s")
        self.speed_label.setAlignment(Qt.AlignCenter)
        self.speed_label.setFont(QFont("", 10, QFont.Bold))
        self.speed_label.setStyleSheet(f"color: {PALETTE['primary']};")
        layout.addWidget(self.speed_label)
        return group

    def _create_direction_group(self) -> QGroupBox:
        group = QGroupBox("方向控制（按住移动）")
        layout = QVBoxLayout(group)
        layout.setSpacing(4)

        grid = QGridLayout()
        grid.setSpacing(8)
        size = 64

        self.btn_forward  = self._mk_dir_btn("↑", size)
        self.btn_left     = self._mk_dir_btn("←", size)
        self.btn_right    = self._mk_dir_btn("→", size)
        self.btn_backward = self._mk_dir_btn("↓", size)
        for btn, d in (
            (self.btn_forward,  "forward"),
            (self.btn_left,     "left"),
            (self.btn_right,    "right"),
            (self.btn_backward, "backward"),
        ):
            btn.pressed.connect(lambda d=d: self._on_direction_pressed(d))
            btn.released.connect(self._on_direction_released)
        self.btn_stop = make_button("停止", role="danger")
        self.btn_stop.setFixedSize(size, size)
        _set_btn_font(self.btn_stop, 14, bold=True)
        self.btn_stop.clicked.connect(self._on_stop_clicked)

        grid.addWidget(self.btn_forward,  0, 1)
        grid.addWidget(self.btn_left,     1, 0)
        grid.addWidget(self.btn_stop,     1, 1)
        grid.addWidget(self.btn_right,    1, 2)
        grid.addWidget(self.btn_backward, 2, 1)

        # 让网格居中
        wrap = QHBoxLayout()
        wrap.addStretch()
        wrap.addLayout(grid)
        wrap.addStretch()
        layout.addLayout(wrap)
        return group

    def _mk_dir_btn(self, text: str, size: int) -> QPushButton:
        b = QPushButton(text)
        b.setProperty("role", "primary")
        b.setFixedSize(size, size)
        # 设为可勾选：键盘 WASD 按住时通过 setChecked(True) 高亮按钮
        b.setCheckable(True)
        p = PALETTE
        b.setStyleSheet(
            f"QPushButton{{"
            f"  background:{p['primary_glass']};color:{p['primary_dark']};"
            f"  border:none;border-radius:12px;font-size:18px;font-weight:bold;"
            f"}}"
            f"QPushButton:pressed, QPushButton:checked{{"
            f"  background:{p['primary']};color:{p['text_invert']};"
            f"}}"
            f"QPushButton:disabled{{"
            f"  background:rgba(220,228,238,120);color:{p['text_muted']};"
            f"}}"
        )
        return b

    def _create_navigation_group(self) -> QGroupBox:
        group = QGroupBox("导航控制")
        layout = QVBoxLayout(group)
        layout.setSpacing(6)

        hbox = QHBoxLayout()
        hbox.setSpacing(6)
        lbl = QLabel("目标站点")
        lbl.setProperty("role", "muted")
        lbl.setMinimumWidth(48)
        hbox.addWidget(lbl)

        self.nav_station_combo = QComboBox()
        self.nav_station_combo.setMinimumWidth(120)
        self.nav_station_combo.currentTextChanged.connect(self._on_station_changed)
        hbox.addWidget(self.nav_station_combo, 1)

        self.btn_view_map = make_button("View", role="purple",
                                        tooltip="查看地图与导航站点")
        self.btn_view_map.setFixedWidth(64)
        self.btn_view_map.clicked.connect(self._on_view_map_clicked)
        hbox.addWidget(self.btn_view_map)
        layout.addLayout(hbox)

        btn_layout = QHBoxLayout()
        btn_layout.setSpacing(6)
        self.btn_nav_start = make_button("开始", role="success")
        self.btn_nav_start.clicked.connect(self._on_nav_start)
        self.btn_nav_pause = make_button("暂停", role="warning")
        self.btn_nav_pause.clicked.connect(self._on_nav_pause)
        self.btn_nav_pause.setEnabled(False)
        self.btn_nav_cancel = make_button("取消", role="purple")
        self.btn_nav_cancel.clicked.connect(self._on_nav_cancel)
        self.btn_nav_cancel.setEnabled(False)
        self.btn_emergency = make_button("⛔ 急停", role="danger")
        self.btn_emergency.clicked.connect(self._on_emergency)
        btn_layout.addWidget(self.btn_nav_start)
        btn_layout.addWidget(self.btn_nav_pause)
        btn_layout.addWidget(self.btn_nav_cancel)
        btn_layout.addWidget(self.btn_emergency)
        layout.addLayout(btn_layout)
        return group

    def _create_status_group(self) -> QGroupBox:
        group = QGroupBox("位置 / 状态")
        layout = QGridLayout(group)
        layout.setHorizontalSpacing(10)
        layout.setVerticalSpacing(4)

        self.label_x = QLabel("X: 0.00 m")
        self.label_y = QLabel("Y: 0.00 m")
        self.label_angle = QLabel("θ: 0.0°")
        self.label_battery = QLabel("电池: 100%")
        self.label_status = QLabel("● 待机")
        self.label_station = QLabel("站点: -")

        for lbl in (self.label_x, self.label_y, self.label_angle,
                    self.label_battery, self.label_status, self.label_station):
            lbl.setProperty("role", "secondary")

        layout.addWidget(self.label_x, 0, 0)
        layout.addWidget(self.label_y, 0, 1)
        layout.addWidget(self.label_angle, 1, 0)
        layout.addWidget(self.label_battery, 1, 1)
        layout.addWidget(self.label_station, 2, 0)
        layout.addWidget(self.label_status, 2, 1)
        return group

    # ========== 事件 ==========

    def _on_speed_changed(self, value: int):
        speed = value / 100.0
        self.speed_label.setText(f"速度: {speed:.2f} m/s")
        self.speed_changed.emit(speed)

    def _on_direction_pressed(self, direction: str):
        now = time.time()
        if now - self._last_move_ts < self._move_interval:
            return
        self._last_move_ts = now
        self._current_direction = direction
        self.direction_changed.emit(direction)

    def _on_direction_released(self):
        self._current_direction = None
        # 鼠标释放时还原所有方向按钮的勾选态（checkable 按钮的 released
        # 不会自动清除 checked，所以这里手动复位）
        for attr in ("btn_forward", "btn_backward", "btn_left", "btn_right"):
            b = getattr(self, attr, None)
            if b is not None and b.isChecked():
                # 用 blockSignals 避免 toggle 信号又把 released 触发一遍
                b.blockSignals(True)
                b.setChecked(False)
                b.blockSignals(False)
        self.stop_requested.emit()

    def _on_stop_clicked(self):
        self.stop_requested.emit()

    # ========== 键盘事件（WASD / 方向键）==========
    # 按下方向键 → 复用 _on_direction_pressed 逻辑，发 direction_changed
    # 释放方向键 → 只要没有别的方向键还按着，就发 stop_requested
    # 多个键同时按（斜向）：以「最后按下的」为准；松键时只要还有任一方向键
    # 按着就持续移动，符合手柄/常见 WASD 遥控语义。
    def keyPressEvent(self, event: QKeyEvent):
        if event.isAutoRepeat():
            # 长按重复触发的 key 事件，忽略
            return
        key = event.key()
        direction = _AGV_KEY_TO_DIR.get(key)
        if direction is None:
            super().keyPressEvent(event)
            return
        # 焦点检查：避免在子控件（如速度/站点输入）输入时被截走
        if not self._agv_widget_has_focus():
            super().keyPressEvent(event)
            return
        event.accept()
        if key in self._held_keys:
            return
        self._held_keys.append(key)
        # 视觉反馈：把对应方向按钮高亮
        btn = self._dir_btn(direction)
        if btn is not None:
            btn.setChecked(True)
        # 键盘是单次触发（无连点节流），直接发 direction_changed
        self._current_direction = direction
        self._last_move_ts = time.time()  # 避免与按钮的 _move_interval 互相拦截
        self.direction_changed.emit(direction)

    def keyReleaseEvent(self, event: QKeyEvent):
        if event.isAutoRepeat():
            return
        key = event.key()
        if key not in _AGV_KEY_TO_DIR:
            super().keyReleaseEvent(event)
            return
        if key not in self._held_keys:
            # 焦点丢失导致的 stray release（用户切到别的输入框）
            return
        self._held_keys.remove(key)
        # 还原按钮高亮
        direction = _AGV_KEY_TO_DIR[key]
        btn = self._dir_btn(direction)
        if btn is not None:
            btn.setChecked(False)
        # 只要还有别的方向键按着就持续运动（取「最后按下」的方向）
        if not self._held_keys:
            self._on_direction_released()
        else:
            # 取 list 里最后加入的键作为新方向，直接发不节流
            last_key = self._held_keys[-1]
            new_dir = _AGV_KEY_TO_DIR.get(last_key)
            if new_dir and new_dir != self._current_direction:
                self._current_direction = new_dir
                self._last_move_ts = time.time()
                self.direction_changed.emit(new_dir)

    def focusInEvent(self, event):
        super().focusInEvent(event)
        # 进入 widget 时不清空 _held_keys（保留外部切回时状态）

    def focusOutEvent(self, event):
        super().focusOutEvent(event)
        # 焦点离开时若还有方向键按着，按「全释放」处理：发 stop 并清空按键集合
        if self._held_keys:
            for k in list(self._held_keys):
                direction = _AGV_KEY_TO_DIR.get(k)
                btn = self._dir_btn(direction) if direction else None
                if btn is not None:
                    btn.setChecked(False)
            self._held_keys.clear()
            self._on_direction_released()

    def _agv_widget_has_focus(self) -> bool:
        """只有当焦点在 widget 自身（不在子输入控件）时键盘才生效。"""
        fw = self.focusWidget()
        # focusWidget() = None 表示焦点在 widget 自身
        if fw is None or fw is self:
            return True
        # 子控件里有焦点（如 QComboBox / QSlider）→ 不响应 WASD
        return False

    def _dir_btn(self, direction: str):
        attr = _AGV_DIR_TO_BTN.get(direction)
        if not attr:
            return None
        return getattr(self, attr, None)

    def _on_station_changed(self, text: str):
        pass

    def _on_nav_start(self):
        station = self.nav_station_combo.currentData()
        if station:
            self.nav_start_requested.emit(station)
            self._update_nav_buttons("running")

    def _on_nav_pause(self):
        self.nav_pause_requested.emit()
        self._update_nav_buttons("paused")

    def _on_nav_cancel(self):
        self.nav_cancel_requested.emit()
        self._update_nav_buttons("idle")

    def _on_emergency(self):
        self.emergency_stop_requested.emit()
        self._update_nav_buttons("idle")

    def _update_nav_buttons(self, status: str):
        self._nav_status = status
        if status == "running":
            self.btn_nav_start.setEnabled(False)
            self.btn_nav_pause.setEnabled(True)
            self.btn_nav_cancel.setEnabled(True)
        elif status == "paused":
            self.btn_nav_start.setEnabled(False)
            self.btn_nav_pause.setEnabled(False)
            self.btn_nav_cancel.setEnabled(True)
        else:
            self.btn_nav_start.setEnabled(True)
            self.btn_nav_pause.setEnabled(False)
            self.btn_nav_cancel.setEnabled(False)

    # ========== 状态更新 ==========

    def update_agv_state(self, data: dict):
        x = data.get("x", None)
        y = data.get("y", None)
        angle = data.get("angle", None)
        if x is None:
            return

        self.label_x.setText(f"X: {x:.2f} m")
        self.label_y.setText(f"Y: {y:.2f} m")
        self.label_angle.setText(f"θ: {math.degrees(angle):.1f}°")
        self._last_agv_pose = (x, y, math.degrees(angle))
        if self._map_dialog is not None and self._map_dialog.isVisible():
            self._map_dialog.update_agv_pose(x, y, math.degrees(angle))

        battery = data.get("battery_level", 0)
        self.label_battery.setText(f"电池: {int(battery * 100)}%")
        current_station = data.get("current_station", "-")
        self.label_station.setText(f"站点: {current_station}")

        if data.get("emergency"):
            self.label_status.setText("⛔ 急停")
            self.label_status.setStyleSheet(
                f"color: {PALETTE['danger']}; font-weight: bold;"
            )
        elif data.get("blocked"):
            self.label_status.setText("⚠ 被阻挡")
            self.label_status.setStyleSheet(
                f"color: {PALETTE['warning']}; font-weight: bold;"
            )
        else:
            self.label_status.setText("● 待机")
            self.label_status.setProperty("role", "secondary")
            self.label_status.setStyleSheet("")

    def update_lasers(self, lasers: list):
        self._last_lasers = list(lasers or [])
        if self._map_dialog is not None and self._map_dialog.isVisible():
            self._map_dialog.update_lasers(self._last_lasers)

    def update_navigation_status(self, status: int):
        status_names = {
            0: "无任务", 1: "等待中", 2: "运行中",
            3: "已暂停", 4: "已完成", 5: "失败", 6: "已取消",
        }
        self.label_status.setText(f"● {status_names.get(status, '未知')}")
        self.label_status.setProperty("role", "secondary")
        self.label_status.setStyleSheet("")

        if status == 2:
            self._update_nav_buttons("running")
        elif status == 3:
            self._update_nav_buttons("paused")
        else:
            self._update_nav_buttons("idle")

    def update_station_list(self, stations: list):
        self.nav_station_combo.clear()
        for station in stations:
            name = station.get("name", station.get("id", ""))
            station_id = station.get("id", name)
            self.nav_station_combo.addItem(name, station_id)

    def set_map_info(self, maps: list, current_map: str, map_data: dict):
        self._maps = list(maps or [])
        self._current_map = current_map or ""
        self._map_data = map_data or {}

    def _on_view_map_clicked(self):
        dlg = MapInfoDialog(self)
        dlg.set_data(self._maps, self._current_map, self._map_data)
        if self._last_agv_pose is not None:
            x, y, ang = self._last_agv_pose
            dlg.update_agv_pose(x, y, ang)
        if self._last_lasers is not None:
            dlg.update_lasers(self._last_lasers)
        dlg._on_fit()
        self._map_dialog = dlg
        try:
            dlg.exec()
        finally:
            self._map_dialog = None

    def set_agv_connected(self, connected: bool):
        """由 MainWindow 在 AGV 连接状态变化时调用，统一启用/禁用所有 AGV 控件。

        规则（与 C++ mainwindow.cpp::applyConnectionState 对齐）：
          - 未连接：所有 AGV 控件置灰禁用（包括速度、方向四按钮、停止、急停、
                   站点下拉、View、开始/暂停/取消）
          - 已连接：恢复各按钮的“导航状态锁定”逻辑
        """
        self.speed_slider.setEnabled(connected)
        for b in (self.btn_forward, self.btn_backward,
                  self.btn_left, self.btn_right, self.btn_stop):
            b.setEnabled(connected)
        self.btn_emergency.setEnabled(connected)
        self.nav_station_combo.setEnabled(connected)
        self.btn_view_map.setEnabled(connected)
        # 导航按钮由 _update_nav_buttons 按运行状态二次锁定
        if connected:
            self._update_nav_buttons(self._nav_status)
        else:
            # 断开连接：所有导航按钮一起禁用
            self.btn_nav_start.setEnabled(False)
            self.btn_nav_pause.setEnabled(False)
            self.btn_nav_cancel.setEnabled(False)
            self._nav_status = "idle"

    # 兼容旧调用，等价于 set_agv_connected(True)
    def set_enabled(self, enabled: bool):
        self.set_agv_connected(enabled)
