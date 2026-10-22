"""
升降柱（liftingColumn）控制面板 - 紧凑版（果冻风）

按 API 说明文档第三十四章实现，封装 7 个运动相关动作（核心运动范围）：
  enable / disable / clearError / moveTo / stop / jog{Start,Heartbeat,Stop} / setZero
  + state 推送订阅
高度单位：UI 用 mm 友好显示，底层走 m（API 原生单位）。

紧凑布局（适配三列分栏，列宽 240~300px）：
  1. 设备 + 刷新一行
  2. 当前高度大字号 + 在线/使能 紧凑状态条
  3. 上电/下电/清错 三按钮一行
  4. 移动到：目标(mm) + 速度(m/s) + 移动/停止
  5. 点动：速度 + 心跳一行 + 上升/下降 紧凑大按钮
  6. 故障/急停 单独行（醒目提示）
  7. 置零 + 软限位 一行
"""
from typing import Optional

from PySide6.QtCore import Qt, Signal, Slot
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QGroupBox, QLabel, QComboBox,
    QPushButton, QFormLayout, QDoubleSpinBox, QSizePolicy, QScrollArea,
)

from ui.theme import PALETTE, set_state, make_button, soft_shadow, _set_btn_font

# UI 用 mm，API 用 m
_MM_PER_M = 1000.0


def _mm_to_m(mm: float) -> float:
    return mm / _MM_PER_M


def _m_to_mm(m: float) -> int:
    return int(round(m * _MM_PER_M))


class LiftingColumnWidget(QWidget):
    """升降柱控制面板（紧凑版）"""

    # ========== 内部信号：跨线程更新 UI ==========
    # 这些信号由后台线程 emit，Qt 自动用 QueuedConnection 派发到主线程
    # 执行连接的 @Slot 装饰的更新方法。比 QTimer.singleShot 跨线程调用
    # 更稳——Qt 保证信号源的发射与槽的接收在正确的线程上下文执行。
    apply_devices   = Signal(list)   # 设备列表更新（list 拉到时）
    apply_states    = Signal(list)   # 状态更新（push / 轮询到时）

    # ========== 外部信号：用户事件触发主窗口去执行后端动作 ==========
    device_changed            = Signal(str)
    enable_requested          = Signal(str)
    disable_requested         = Signal(str)
    clear_error_requested     = Signal(str)
    move_to_requested         = Signal(str, float, float)
    stop_requested            = Signal(str)
    set_zero_requested        = Signal(str)
    jog_start_requested       = Signal(str, int, float, int)
    jog_stop_requested        = Signal(str)
    list_requested            = Signal()

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self._devices: dict = {}         # name -> {min,max,maxV,jogV} (in m)
        self._current_device: str = ""
        self._state: dict = {}           # device -> 最近推送状态
        # 跨线程更新走信号/槽：后台线程 emit，Qt 用 QueuedConnection
        # 派发到主线程。直接调 apply_devices / apply_states 也是等价
        # 的（在同一线程下变 DirectConnection），都用同一对 @Slot 收口
        self.apply_devices.connect(self.set_devices)
        self.apply_states.connect(self.update_state)
        self._init_ui()
        self._refresh_labels()

    # ============================================================
    # UI
    # ============================================================
    def _init_ui(self):
        """结构：紧凑纵向堆叠，适合 ~320px 宽 × 占右列下半高
        1. 设备 + 刷新
        2. 当前高度（大字）+ 限位（小字）
        3. 状态点（在线 / 使能 / 运动 / 故障）
        4. 上电 / 下电 / 清错
        5. 移动到（目标 + 速度 + 移动/停止）
        6. 点动（速度 + 心跳 + 上升/下降）
        7. 故障 / 急停（醒目，状态变时显示）
        8. 置零
        """
        # 用 QScrollArea 包裹内容，确保窗口小时也能完整滚动查看
        self._scroll = QScrollArea(self)
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QScrollArea.NoFrame)
        self._scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self._scroll.setVerticalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        # 内部容器
        self._inner = QWidget()
        self._scroll.setWidget(self._inner)
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)
        outer.addWidget(self._scroll)
        # 真实布局加到 _inner 上
        layout = QVBoxLayout(self._inner)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        # —— 1. 设备 + 刷新 ——
        dev_row = QHBoxLayout()
        dev_row.setSpacing(4)
        dev_row.setContentsMargins(0, 0, 0, 0)
        lbl = QLabel("设备")
        lbl.setProperty("role", "muted")
        lbl.setMinimumWidth(30)
        dev_row.addWidget(lbl)
        self.device_combo = QComboBox()
        self.device_combo.setMinimumWidth(100)
        self.device_combo.currentTextChanged.connect(self._on_device_changed)
        dev_row.addWidget(self.device_combo, 1)
        self.btn_refresh = make_button("↻", role="purple", tooltip="刷新设备列表")
        self.btn_refresh.setFixedWidth(28)
        self.btn_refresh.clicked.connect(self.list_requested.emit)
        dev_row.addWidget(self.btn_refresh)
        layout.addLayout(dev_row)

        # —— 2. 高度大字号 + 限位 ——
        height_block = QVBoxLayout()
        height_block.setSpacing(0)
        height_block.setContentsMargins(0, 0, 0, 0)
        h_lbl = QLabel("当前高度")
        h_lbl.setProperty("role", "muted")
        h_lbl.setAlignment(Qt.AlignCenter)
        height_block.addWidget(h_lbl)
        self.lbl_height = QLabel("—")
        self.lbl_height.setAlignment(Qt.AlignCenter)
        self.lbl_height.setStyleSheet(
            f"font-size: 24px; font-weight: bold; color: {PALETTE['primary']};"
        )
        height_block.addWidget(self.lbl_height)
        self.lbl_limits = QLabel("—")
        self.lbl_limits.setAlignment(Qt.AlignCenter)
        self.lbl_limits.setProperty("role", "muted")
        self.lbl_limits.setStyleSheet("font-size: 10px;")
        height_block.addWidget(self.lbl_limits)
        layout.addLayout(height_block)

        # —— 3. 状态点（在线/使能/运动/故障）——
        s_row = QHBoxLayout()
        s_row.setSpacing(6)
        s_row.setContentsMargins(0, 0, 0, 0)
        self.lbl_online = QLabel("○—")
        self.lbl_online.setProperty("role", "muted")
        self.lbl_enabled = QLabel("○—")
        self.lbl_enabled.setProperty("role", "muted")
        self.lbl_moving = QLabel("○—")
        self.lbl_moving.setProperty("role", "muted")
        self.lbl_fault = QLabel("○—")
        self.lbl_fault.setProperty("role", "muted")
        for l in (self.lbl_online, self.lbl_enabled,
                  self.lbl_moving, self.lbl_fault):
            l.setAlignment(Qt.AlignCenter)
            s_row.addWidget(l, 1)
        layout.addLayout(s_row)

        # —— 4. 上电 / 下电 / 清错 ——
        p_row = QHBoxLayout()
        p_row.setSpacing(4)
        p_row.setContentsMargins(0, 0, 0, 0)
        self.btn_enable = make_button("⚡ 上电", role="success")
        self.btn_enable.setMinimumHeight(30)
        self.btn_enable.clicked.connect(self._emit_enable)
        self.btn_disable = make_button("⏻ 下电", role="warning")
        self.btn_disable.setMinimumHeight(30)
        self.btn_disable.clicked.connect(self._emit_disable)
        self.btn_clear = make_button("✦ 清错", role="purple")
        self.btn_clear.setMinimumHeight(30)
        self.btn_clear.clicked.connect(self._emit_clear_error)
        p_row.addWidget(self.btn_enable, 1)
        p_row.addWidget(self.btn_disable, 1)
        p_row.addWidget(self.btn_clear, 1)
        layout.addLayout(p_row)

        # —— 5. 移动到（组）——
        m_box = QGroupBox("移动到")
        m_box.setProperty("role", "muted")
        m_box_layout = QVBoxLayout(m_box)
        m_box_layout.setContentsMargins(8, 4, 8, 8)
        m_box_layout.setSpacing(4)
        in_row = QHBoxLayout()
        in_row.setSpacing(4)
        in_row.setContentsMargins(0, 0, 0, 0)
        h_lbl2 = QLabel("目标")
        h_lbl2.setProperty("role", "muted")
        h_lbl2.setMinimumWidth(30)
        in_row.addWidget(h_lbl2)
        self.height_input = QDoubleSpinBox()
        self.height_input.setRange(-2000, 2000)
        self.height_input.setDecimals(0)
        self.height_input.setSingleStep(10)
        self.height_input.setSuffix(" mm")
        self.height_input.setValue(300)
        self.height_input.setMinimumWidth(70)
        in_row.addWidget(self.height_input, 1)
        v_lbl = QLabel("速")
        v_lbl.setProperty("role", "muted")
        in_row.addWidget(v_lbl)
        self.speed_input = QDoubleSpinBox()
        # 0 用 "默认" 文本显示（Qt 特殊值），后端用 maxVelocity 缺省；
        # 当前端 min 是 0.001 时空 widget 默认会发 0.001 给控制器，1 mm/s
        # 几乎看不出运动，所以这里把下界调到 0 + special text "默认"，
        # 真正选具体速度时下界仍是 0.001
        self.speed_input.setRange(0.0, 1.0)
        self.speed_input.setDecimals(3)
        self.speed_input.setSingleStep(0.005)
        self.speed_input.setSpecialValueText("默认")
        self.speed_input.setValue(0.0)  # 默认走 controller 的 maxVelocity
        self.speed_input.setMinimumWidth(60)
        in_row.addWidget(self.speed_input, 1)
        m_box_layout.addLayout(in_row)
        btn_row = QHBoxLayout()
        btn_row.setSpacing(4)
        btn_row.setContentsMargins(0, 0, 0, 0)
        self.btn_move = make_button("▶ 移动", role="primary")
        self.btn_move.setMinimumHeight(30)
        self.btn_move.clicked.connect(self._emit_move_to)
        self.btn_stop = make_button("■ 停止", role="danger")
        self.btn_stop.setMinimumHeight(30)
        self.btn_stop.clicked.connect(self._emit_stop)
        btn_row.addWidget(self.btn_move, 2)
        btn_row.addWidget(self.btn_stop, 1)
        m_box_layout.addLayout(btn_row)
        layout.addWidget(m_box)

        # —— 6. 点动（组）——
        j_box = QGroupBox("点动（按住）")
        j_box.setProperty("role", "muted")
        j_box_layout = QVBoxLayout(j_box)
        j_box_layout.setContentsMargins(8, 4, 8, 8)
        j_box_layout.setSpacing(4)
        cfg_row = QHBoxLayout()
        cfg_row.setSpacing(4)
        cfg_row.setContentsMargins(0, 0, 0, 0)
        j1 = QLabel("速")
        j1.setProperty("role", "muted")
        j1.setMinimumWidth(30)
        cfg_row.addWidget(j1)
        self.jog_speed_input = QDoubleSpinBox()
        # 同 speed_input：默认 0 走 controller 的 jogVelocity 缺省
        self.jog_speed_input.setRange(0.0, 1.0)
        self.jog_speed_input.setDecimals(3)
        self.jog_speed_input.setSingleStep(0.005)
        self.jog_speed_input.setSpecialValueText("默认")
        self.jog_speed_input.setValue(0.0)
        self.jog_speed_input.setMinimumWidth(60)
        cfg_row.addWidget(self.jog_speed_input, 1)
        j2 = QLabel("心跳")
        j2.setProperty("role", "muted")
        cfg_row.addWidget(j2)
        self.jog_timeout_input = QDoubleSpinBox()
        self.jog_timeout_input.setRange(100, 5000)
        self.jog_timeout_input.setDecimals(0)
        self.jog_timeout_input.setSingleStep(100)
        self.jog_timeout_input.setValue(500)
        self.jog_timeout_input.setMinimumWidth(50)
        cfg_row.addWidget(self.jog_timeout_input, 1)
        j_box_layout.addLayout(cfg_row)
        btn_row2 = QHBoxLayout()
        btn_row2.setSpacing(4)
        btn_row2.setContentsMargins(0, 0, 0, 0)
        self.btn_jog_up = QPushButton("▲ 上升")
        self.btn_jog_up.setProperty("role", "primary")
        self.btn_jog_down = QPushButton("▼ 下降")
        self.btn_jog_down.setProperty("role", "primary")
        for b in (self.btn_jog_up, self.btn_jog_down):
            b.setCheckable(True)
            b.setMinimumHeight(36)
            _set_btn_font(b, 13, bold=True)
        self.btn_jog_up.pressed.connect(self._on_jog_up_pressed)
        self.btn_jog_up.released.connect(self._on_jog_released)
        self.btn_jog_down.pressed.connect(self._on_jog_down_pressed)
        self.btn_jog_down.released.connect(self._on_jog_released)
        btn_row2.addWidget(self.btn_jog_up)
        btn_row2.addWidget(self.btn_jog_down)
        j_box_layout.addLayout(btn_row2)
        layout.addWidget(j_box)

        # —— 7. 故障 / 急停（醒目，状态变时显示）——
        self._alert_label = QLabel("")
        self._alert_label.setAlignment(Qt.AlignCenter)
        self._alert_label.setMinimumHeight(22)
        self._alert_label.setStyleSheet("background: transparent;")
        self._alert_label.hide()
        layout.addWidget(self._alert_label)

        # —— 8. 置零 ——
        self.btn_zero = make_button("⊘ 把当前位置记为零点", role="purple")
        self.btn_zero.setToolTip("将当前位置写入控制器 EEPROM，重启后仍生效")
        self.btn_zero.setMinimumHeight(28)
        self.btn_zero.clicked.connect(self._emit_set_zero)
        layout.addWidget(self.btn_zero)

        layout.addStretch(1)

    # ============================================================
    # 信号发送
    # ============================================================
    def _current(self) -> str:
        return self.device_combo.currentText().strip()

    def _on_device_changed(self, name: str):
        self._current_device = name
        dev = self._devices.get(name)
        if dev:
            self.height_input.setRange(_m_to_mm(dev["min"]), _m_to_mm(dev["max"]))
            if dev.get("maxV"):
                self.speed_input.setMaximum(dev["maxV"])
                self.jog_speed_input.setMaximum(dev["maxV"])
            self.lbl_limits.setText(
                f"{_m_to_mm(dev['min'])}~{_m_to_mm(dev['max'])} mm"
            )
        else:
            self.height_input.setRange(-2000, 2000)
            self.lbl_limits.setText("—")
        self._refresh_labels()
        self.device_changed.emit(name)

    def _emit_enable(self):
        n = self._current()
        if n:
            self.enable_requested.emit(n)

    def _emit_disable(self):
        n = self._current()
        if n:
            self.disable_requested.emit(n)

    def _emit_clear_error(self):
        n = self._current()
        if n:
            self.clear_error_requested.emit(n)

    def _emit_move_to(self):
        n = self._current()
        if not n:
            return
        height_m = _mm_to_m(self.height_input.value())
        # speed_input 默认 0 = "默认" (Qt special value), 此时不传 speed 给后端,
        # 走 controller 的 maxVelocity 缺省 (35.1 moveTo 文档)
        raw_speed = self.speed_input.value()
        speed = raw_speed if raw_speed > 0 else 0.0  # 0 由 main_window 翻译为 None
        self.move_to_requested.emit(n, height_m, speed)

    def _emit_stop(self):
        n = self._current()
        if n:
            self.stop_requested.emit(n)

    def _emit_set_zero(self):
        n = self._current()
        if n:
            self.set_zero_requested.emit(n)

    def _on_jog_up_pressed(self):
        n = self._current()
        if not n:
            self.btn_jog_up.setChecked(False)
            return
        # 默认 0 = 走 controller 的 jogVelocity 缺省 (34.10)
        raw_speed = self.jog_speed_input.value()
        speed = raw_speed if raw_speed > 0 else 0.0
        self.jog_start_requested.emit(
            n, 1, speed, int(self.jog_timeout_input.value())
        )

    def _on_jog_down_pressed(self):
        n = self._current()
        if not n:
            self.btn_jog_down.setChecked(False)
            return
        raw_speed = self.jog_speed_input.value()
        speed = raw_speed if raw_speed > 0 else 0.0
        self.jog_start_requested.emit(
            n, -1, speed, int(self.jog_timeout_input.value())
        )

    def _on_jog_released(self):
        n = self._current()
        if n:
            self.jog_stop_requested.emit(n)
        if self.btn_jog_up.isChecked() and not self.btn_jog_up.isDown():
            self.btn_jog_up.setChecked(False)
        if self.btn_jog_down.isChecked() and not self.btn_jog_down.isDown():
            self.btn_jog_down.setChecked(False)

    # ============================================================
    # 外部接口
    # ============================================================
    @Slot(list)
    def set_devices(self, devices: list):
        prev = self._current()
        self.device_combo.blockSignals(True)
        self.device_combo.clear()
        self._devices.clear()
        for d in devices:
            name = d.get("name")
            if not name:
                continue
            self._devices[name] = {
                "min": float(d.get("minHeight", 0.0)),
                "max": float(d.get("maxHeight", 0.0)),
                "maxV": float(d.get("maxVelocity", 0.0)),
                "jogV": float(d.get("jogVelocity", 0.0)),
            }
            self.device_combo.addItem(name)
        self.device_combo.blockSignals(False)
        if prev and prev in self._devices:
            idx = self.device_combo.findText(prev)
            if idx >= 0:
                self.device_combo.setCurrentIndex(idx)
        elif self.device_combo.count() > 0:
            self.device_combo.setCurrentIndex(0)
        self._on_device_changed(self.device_combo.currentText())

    def set_arm_connected(self, connected: bool):
        """机械臂未连接时禁用升降柱所有动作按钮（与主窗口 state 锁定一致）。

        与 C++ mainwindow.cpp::applyConnectionState 配合：升降柱走的是
        机械臂 ws 通道，因此未连接时全部动作禁用，仅保留展示标签。
        """
        self.btn_enable.setEnabled(connected)
        self.btn_disable.setEnabled(connected)
        self.btn_clear.setEnabled(connected)
        self.btn_move.setEnabled(connected)
        self.btn_stop.setEnabled(connected)
        self.btn_jog_up.setEnabled(connected)
        self.btn_jog_down.setEnabled(connected)
        self.btn_zero.setEnabled(connected)
        # 输入框在断开时也置灰，避免空操作
        self.height_input.setEnabled(connected)
        self.speed_input.setEnabled(connected)
        self.jog_speed_input.setEnabled(connected)
        self.jog_timeout_input.setEnabled(connected)
        self.device_combo.setEnabled(connected)
        self.btn_refresh.setEnabled(connected)
        if not connected:
            # 按下状态复位，避免下次连接时按钮仍处于"按下"状态
            self.btn_jog_up.setChecked(False)
            self.btn_jog_down.setChecked(False)

    @Slot(list)
    def update_state(self, states):
        """状态推送/轮询的统一入口。

        同步做两件事：
          1. 把 db 里出现的设备加进 dropdown（min/max 未知时用合理默认）
             —— `liftingColumn/state` 推送是全量数组（文档 34.16），设备名
             与 `list` 同根；list 失败 / 慢 / 控制器此刻刚加设备时，仍能
             通过推送让 UI 下拉框可见
          2. 把 db 内容写进 self._state（保持原行为），触发 _refresh_labels
        """
        if not isinstance(states, list):
            return
        for s in states:
            if isinstance(s, dict) and s.get("name"):
                self._state[s["name"]] = s
                # 推送里出现了新设备：保证 dropdown 至少有名；min/max 等
                # 详细参数仍由 set_devices（list 结果）补齐
                if self.device_combo.findText(s["name"]) < 0:
                    # 临时占位：min/max 用「列表缺省 -0.79~0 m」容许后面
                    # moveTo / set_zero / set_devices 二次刷新
                    self._devices[s["name"]] = {
                        "min": -0.79,
                        "max": 0.0,
                        "maxV": 0.1,
                        "jogV": 0.05,
                    }
                    self.device_combo.addItem(s["name"])
        # 当前选中设备变了（list 还没回来但推送先到），刷新一下高度显示
        self._refresh_labels()

    def _refresh_labels(self):
        n = self._current()
        s = self._state.get(n) if n else None
        if s is None:
            self.lbl_height.setText("—")
            for lbl in (self.lbl_online, self.lbl_enabled, self.lbl_moving, self.lbl_fault):
                lbl.setText("—")
                lbl.setStyleSheet("")
            self._alert_label.hide()
            return

        # 高度
        h = s.get("height", 0.0)
        self.lbl_height.setText(f"{_m_to_mm(float(h))} mm")

        # 在线
        if s.get("online"):
            self.lbl_online.setText("●在线")
            self.lbl_online.setStyleSheet(
                f"color: {PALETTE['success']}; font-weight: bold;"
            )
        else:
            self.lbl_online.setText("○离线")
            self.lbl_online.setStyleSheet(f"color: {PALETTE['text_muted']};")

        # 使能
        if s.get("enabled"):
            self.lbl_enabled.setText("✓使能")
            self.lbl_enabled.setStyleSheet(
                f"color: {PALETTE['success']}; font-weight: bold;"
            )
        else:
            self.lbl_enabled.setText("○未使能")
            self.lbl_enabled.setStyleSheet(f"color: {PALETTE['text_muted']};")

        # 运动
        moving = bool(s.get("moving"))
        jogging = bool(s.get("jogging"))
        if jogging:
            self.lbl_moving.setText("▶点动中")
            self.lbl_moving.setStyleSheet(
                f"color: {PALETTE['cyan']}; font-weight: bold;"
            )
        elif moving:
            self.lbl_moving.setText("▶运动中")
            self.lbl_moving.setStyleSheet(
                f"color: {PALETTE['primary']}; font-weight: bold;"
            )
        elif s.get("enabled") and s.get("targetReached"):
            self.lbl_moving.setText("●已到位")
            self.lbl_moving.setStyleSheet(f"color: {PALETTE['success']};")
        else:
            self.lbl_moving.setText("○静止")
            self.lbl_moving.setStyleSheet(f"color: {PALETTE['text_muted']};")

        # 故障 / 急停（醒目）
        has_fault = bool(s.get("fault"))
        ec = s.get("errorCode", 0)
        has_estop = bool(s.get("emergencyStopActive"))
        if has_estop:
            self._alert_label.setText("⛔ 机器人急停生效中，本升降柱联动下电")
            self._alert_label.setStyleSheet(
                f"color: white; background: {PALETTE['danger']}; "
                f"border-radius: 4px; padding: 2px 8px; font-weight: bold;"
            )
            self._alert_label.show()
        elif has_fault:
            self._alert_label.setText(f"⚠ 驱动器故障 (码 {ec})，请清错后重新使能")
            self._alert_label.setStyleSheet(
                f"color: white; background: {PALETTE['warning']}; "
                f"border-radius: 4px; padding: 2px 8px; font-weight: bold;"
            )
            self._alert_label.show()
        else:
            self._alert_label.hide()
        self.lbl_fault.setText(
            f"⚠码 {ec}" if has_fault else "无"
        )
        self.lbl_fault.setStyleSheet(
            f"color: {PALETTE['danger']}; font-weight: bold;" if has_fault
            else f"color: {PALETTE['text_muted']};"
        )
