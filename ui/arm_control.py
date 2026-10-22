"""
机械臂控制模块 - 紧凑版（果冻风）

包含使能控制、速度设置、工程列表、工程控制、状态显示等。
关节角 / 末端位姿 group 改为横向单行布局，节省垂直空间。
整体放进 ScrollArea，列宽收窄时垂直可滚。
"""
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QGroupBox, QLabel,
    QPushButton, QSlider, QComboBox, QGridLayout, QScrollArea,
    QDoubleSpinBox,
)
from PySide6.QtCore import Qt, Signal, Slot, QTimer
from PySide6.QtGui import QFont

from ui.theme import make_button, set_role, _set_btn_font, PALETTE

class ArmControlWidget(QWidget):
    """机械臂控制模块（简约果冻风，紧凑）"""

    enable_left_arm = Signal(bool)
    enable_right_arm = Signal(bool)
    # 力矩拖动：UI 发臂名，MainWindow 执行 drag_arm_async / disable_arm
    drag_left_arm_requested = Signal()
    drag_right_arm_requested = Signal()
    speed_changed = Signal(float)
    project_start_requested = Signal(str)
    project_pause_requested = Signal()
    project_resume_requested = Signal()
    project_stop_requested = Signal()
    refresh_projects_requested = Signal()
    view_requested = Signal()  # 请求弹出 WebView 弹窗（由 MainWindow 打开）

    def __init__(self, parent=None):
        super().__init__(parent)
        self._left_arm_enabled = False
        self._right_arm_enabled = False
        # 力矩拖动状态（由 robot/status 推上来的 mode ∈ CSTDrag/ServoDrag 回灌）
        self._left_arm_dragging = False
        self._right_arm_dragging = False
        # 使能命令进行中标志（点完按钮到收到响应的窗口期，禁用其它使能操作）
        self._left_arm_pending = False
        self._right_arm_pending = False
        self._left_arm_status_busy = False
        self._right_arm_status_busy = False
        self._current_speed = 1.0
        # 当前工程运行状态：running / paused / idle
        # 用于断开重连后按状态恢复按钮锁定，避免“按状态文本解析”这种脆弱做法
        self._project_run_status = "idle"
        self._init_ui()

    def _init_ui(self):
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QScrollArea.NoFrame)
        scroll.setStyleSheet("background: transparent;")
        # 美化滚动条
        scroll.setStyleSheet(
            "QScrollArea { background: transparent; border: none; }"
            "QScrollBar:vertical { background: transparent; width: 6px; margin: 0; }"
            "QScrollBar::handle:vertical { background: rgba(180, 190, 220, 100);"
            "  border-radius: 3px; min-height: 20px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        )

        inner = QWidget()
        inner.setStyleSheet("background: transparent;")
        layout = QVBoxLayout(inner)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        layout.addWidget(self._create_enable_group())
        layout.addWidget(self._create_speed_group())
        layout.addWidget(self._create_project_group())
        layout.addWidget(self._create_joints_group(), 1)
        scroll.setWidget(inner)
        outer.addWidget(scroll)

    def _create_enable_group(self) -> QGroupBox:
        """双臂使能控制组"""
        group = QGroupBox("机械臂使能")
        group.setProperty("role", "sub")
        layout = QGridLayout(group)
        layout.setHorizontalSpacing(10)
        layout.setVerticalSpacing(6)
        layout.setContentsMargins(10, 4, 10, 8)

        # Left arm
        l_title = QLabel("左臂")
        l_title.setAlignment(Qt.AlignCenter)
        l_title.setStyleSheet(
            f"color: {PALETTE['primary']}; font-weight: bold; font-size: 12px;"
            f" background: transparent;"
        )
        layout.addWidget(l_title, 0, 0)
        self.btn_left_enable = QPushButton("启用")
        self.btn_left_enable.setMinimumHeight(32)
        self.btn_left_enable.setCheckable(True)
        self.btn_left_enable.setCursor(Qt.PointingHandCursor)
        self._refresh_enable_btn_style(left=True)
        self.btn_left_enable.clicked.connect(self._on_left_enable_clicked)
        layout.addWidget(self.btn_left_enable, 1, 0)
        self.label_left_status = QLabel("未使能")
        self.label_left_status.setAlignment(Qt.AlignCenter)
        self.label_left_status.setStyleSheet(
            f"color: {PALETTE['text_muted']}; font-size: 11px; background: transparent;"
        )
        layout.addWidget(self.label_left_status, 2, 0)
        self.btn_left_drag = QPushButton("拖动")
        self.btn_left_drag.setMinimumHeight(32)
        self.btn_left_drag.setCursor(Qt.PointingHandCursor)
        self.btn_left_drag.setStyleSheet(self._drag_btn_style())
        self.btn_left_drag.clicked.connect(self._on_left_drag_clicked)
        layout.addWidget(self.btn_left_drag, 3, 0)

        # Right arm（col 1）
        r_title = QLabel("右臂")
        r_title.setAlignment(Qt.AlignCenter)
        r_title.setStyleSheet(
            f"color: {PALETTE['purple']}; font-weight: bold; font-size: 12px;"
            f" background: transparent;"
        )
        layout.addWidget(r_title, 0, 1)
        self.btn_right_enable = QPushButton("启用")
        self.btn_right_enable.setMinimumHeight(32)
        self.btn_right_enable.setCheckable(True)
        self.btn_right_enable.setCursor(Qt.PointingHandCursor)
        self._refresh_enable_btn_style(left=False)
        self.btn_right_enable.clicked.connect(self._on_right_enable_clicked)
        layout.addWidget(self.btn_right_enable, 1, 1)
        self.label_right_status = QLabel("未使能")
        self.label_right_status.setAlignment(Qt.AlignCenter)
        self.label_right_status.setStyleSheet(
            f"color: {PALETTE['text_muted']}; font-size: 11px; background: transparent;"
        )
        layout.addWidget(self.label_right_status, 2, 1)
        self.btn_right_drag = QPushButton("拖动")
        self.btn_right_drag.setMinimumHeight(32)
        self.btn_right_drag.setCursor(Qt.PointingHandCursor)
        self.btn_right_drag.setStyleSheet(self._drag_btn_style())
        self.btn_right_drag.clicked.connect(self._on_right_drag_clicked)
        layout.addWidget(self.btn_right_drag, 3, 1)

        return group

    def _enable_btn_style(self, enabled: bool) -> str:
        """启用按钮统一样式（不依赖 role property / QSS polish）。"""
        if enabled:
            bg = PALETTE["success_glass"]
            bg_h = PALETTE["success"]
            fg = PALETTE["success_dark"]
        else:
            bg = PALETTE["primary_glass"]
            bg_h = PALETTE["primary"]
            fg = PALETTE["primary_dark"]
        return (
            f"QPushButton{{"
            f"background:{bg};color:{fg};"
            f"border:none;border-radius:10px;"
            f"padding:6px 16px;font-weight:bold;font-size:13px;"
            f"}}"
            f"QPushButton:hover{{background:{bg_h};color:#ffffff;}}"
            f"QPushButton:pressed{{background:{bg_h};color:#ffffff;padding-top:7px;padding-bottom:5px;}}"
            f"QPushButton:disabled{{"
            f"background:rgba(220,228,238,120);color:{PALETTE['text_muted']};"
            f"}}"
        )

    def _refresh_enable_btn_style(self, left: bool = True):
        btn = self.btn_left_enable if left else self.btn_right_enable
        enabled = (self._left_arm_enabled if left else self._right_arm_enabled)
        btn.setStyleSheet(self._enable_btn_style(enabled))

    # ========== 力矩拖动按钮（robot/drag） ==========

    _DRAG_MODES = ("CSTDrag", "ServoDrag")

    def _drag_btn_style(self, dragging: bool = False) -> str:
        """拖动按钮样式：待机=紫玻璃（提示这是个特殊模式），拖动中=黄实心（醒目警示）。"""
        p = PALETTE
        if dragging:
            bg, bg_h, fg = "rgba(250, 200, 80, 220)", p.get("warning", "#f0a020"), "#5a3d00"
        else:
            bg, bg_h, fg = p.get("purple_glass", p["purple"]), p["purple"], p.get("purple_dark", "#ffffff")
        return (
            f"QPushButton{{"
            f"background:{bg};color:{fg};"
            f"border:none;border-radius:10px;"
            f"padding:6px 16px;font-weight:bold;font-size:13px;"
            f"}}"
            f"QPushButton:hover{{background:{bg_h};color:#ffffff;}}"
            f"QPushButton:pressed{{background:{bg_h};color:#ffffff;padding-top:7px;padding-bottom:5px;}}"
            f"QPushButton:disabled{{"
            f"background:rgba(220,228,238,120);color:{p['text_muted']};"
            f"}}"
        )

    def _refresh_drag_btn(self, left: bool):
        """拖动按钮文案/样式跟 robot/status 的 mode 同步。"""
        btn = self.btn_left_drag if left else self.btn_right_drag
        dragging = self._left_arm_dragging if left else self._right_arm_dragging
        btn.setText("退出拖动" if dragging else "拖动")
        btn.setToolTip(
            "点此进入力矩拖动（robot/drag）；\n"
            "按机型自动派到 CSTDrag / ServoDrag。\n"
            "再点一次退出（= robot/disable 下电）。"
            if not dragging else
            "拖动中：手扶机械臂即可示教。\n"
            "点此退出拖动并下电。"
        )
        btn.setStyleSheet(self._drag_btn_style(dragging))

    def _on_left_drag_clicked(self):
        # 拖动中再点 = 退出（disable 走既有信号，由 MainWindow 判定）
        if self._left_arm_dragging:
            self.enable_left_arm.emit(False)
        else:
            self.drag_left_arm_requested.emit()

    def _on_right_drag_clicked(self):
        if self._right_arm_dragging:
            self.enable_right_arm.emit(False)
        else:
            self.drag_right_arm_requested.emit()

    def _create_speed_group(self) -> QGroupBox:
        """速度控制组"""
        group = QGroupBox("运动速度")
        layout = QVBoxLayout(group)
        layout.setContentsMargins(10, 4, 10, 8)
        layout.setSpacing(4)

        slider_row = QHBoxLayout()
        slider_row.setSpacing(6)
        lbl = QLabel("慢")
        lbl.setProperty("role", "muted")
        slider_row.addWidget(lbl)
        self.speed_slider = QSlider(Qt.Horizontal)
        self.speed_slider.setMinimum(0)
        self.speed_slider.setMaximum(100)
        self.speed_slider.setValue(50)
        self.speed_slider.setTickPosition(QSlider.TicksBelow)
        self.speed_slider.setTickInterval(25)
        slider_row.addWidget(self.speed_slider, 1)
        lbl2 = QLabel("快")
        lbl2.setProperty("role", "muted")
        slider_row.addWidget(lbl2)

        self.speed_label = QLabel("50%")
        self.speed_label.setAlignment(Qt.AlignCenter)
        self.speed_label.setStyleSheet(
            f"color: {PALETTE['primary']}; font-weight: bold; font-size: 13px;"
        )
        self.speed_hint = QLabel("标准 - 一般作业")
        self.speed_hint.setAlignment(Qt.AlignCenter)
        self.speed_hint.setProperty("role", "muted")
        self.speed_hint.setStyleSheet("font-size: 10px;")

        layout.addLayout(slider_row)
        layout.addWidget(self.speed_label)
        layout.addWidget(self.speed_hint)
        self.speed_slider.valueChanged.connect(self._on_speed_changed)
        return group

    def _project_btn_style(self, role: str, primary: bool = False) -> str:
        """工程作业按钮统一样式（inline 锁死，不依赖 role QSS / polish）。"""
        p = PALETTE
        bg = p.get(role + "_glass", p[role])
        bg_h = p[role]  # hover 用实色
        fg = p[role + "_dark"] if (role + "_dark") in p else "#ffffff"
        radius = "10px"
        pad_v = "8px" if primary else "6px"
        fs = "14px" if primary else "13px"
        return (
            f"QPushButton{{"
            f"background:{bg};color:{fg};"
            f"border:none;border-radius:{radius};"
            f"padding:{pad_v} 16px;font-weight:bold;font-size:{fs};"
            f"}}"
            f"QPushButton:hover{{background:{bg_h};color:#ffffff;}}"
            f"QPushButton:pressed{{background:{bg_h};color:#ffffff;padding-top:9px;padding-bottom:7px;}}"
            f"QPushButton:disabled{{"
            f"background:rgba(220,228,238,120);color:{p['text_muted']};"
            f"}}"
        )

    def _create_project_group(self) -> QGroupBox:
        """工程控制组"""
        group = QGroupBox("工程作业")
        group.setProperty("role", "sub")
        layout = QVBoxLayout(group)
        layout.setContentsMargins(10, 4, 10, 8)
        layout.setSpacing(4)

        sel_row = QHBoxLayout()
        sel_row.setSpacing(8)
        sel_row.setContentsMargins(4, 0, 4, 0)
        self.project_combo = QComboBox()
        self.project_combo.setMinimumWidth(120)
        sel_row.addWidget(self.project_combo, 1)
        self.btn_view = make_button("View", role="purple",
                                    tooltip="打开 WebView 弹窗")
        self.btn_view.clicked.connect(self.view_requested.emit)
        sel_row.addWidget(self.btn_view)
        self.btn_refresh = make_button("↻", role="purple", min_height=26)
        self.btn_refresh.clicked.connect(self.refresh_projects_requested.emit)
        sel_row.addWidget(self.btn_refresh)
        layout.addLayout(sel_row)

        # 2x2 对称布局：[开始 | 暂停] / [停止 | 继续]
        # 行1：开始(主, 成功) | 暂停(警告)
        # 行2：停止(危险)     | 继续(海蓝)
        grid = QGridLayout()
        grid.setHorizontalSpacing(6)
        grid.setVerticalSpacing(6)
        grid.setContentsMargins(0, 0, 0, 0)

        self.btn_start = QPushButton("开始")
        self.btn_start.setMinimumHeight(36)
        self.btn_start.setCursor(Qt.PointingHandCursor)
        self.btn_start.setStyleSheet(self._project_btn_style("success", primary=True))
        self.btn_start.clicked.connect(self._on_start_clicked)
        grid.addWidget(self.btn_start, 0, 0)

        self.btn_pause = QPushButton("暂停")
        self.btn_pause.setMinimumHeight(36)
        self.btn_pause.setCursor(Qt.PointingHandCursor)
        self.btn_pause.setStyleSheet(self._project_btn_style("warning", primary=True))
        self.btn_pause.clicked.connect(self._on_pause_clicked)
        self.btn_pause.setEnabled(False)
        grid.addWidget(self.btn_pause, 0, 1)

        self.btn_stop = QPushButton("停止")
        self.btn_stop.setMinimumHeight(36)
        self.btn_stop.setCursor(Qt.PointingHandCursor)
        self.btn_stop.setStyleSheet(self._project_btn_style("danger", primary=True))
        self.btn_stop.clicked.connect(self._on_stop_clicked)
        self.btn_stop.setEnabled(False)
        grid.addWidget(self.btn_stop, 1, 0)

        self.btn_resume = QPushButton("继续")
        self.btn_resume.setMinimumHeight(36)
        self.btn_resume.setCursor(Qt.PointingHandCursor)
        self.btn_resume.setStyleSheet(self._project_btn_style("primary", primary=True))
        self.btn_resume.clicked.connect(self._on_resume_clicked)
        self.btn_resume.setEnabled(False)
        grid.addWidget(self.btn_resume, 1, 1)

        grid.setColumnStretch(0, 1)
        grid.setColumnStretch(1, 1)
        grid.setRowStretch(0, 1)
        grid.setRowStretch(1, 1)
        layout.addLayout(grid)

        self.label_project_status = QLabel("当前工程: 无 — 待机")
        self.label_project_status.setAlignment(Qt.AlignCenter)
        self.label_project_status.setProperty("role", "muted")
        layout.addWidget(self.label_project_status)

        return group

    def _create_joints_group(self) -> QGroupBox:
        """关节角 + 末端位姿，横向单行（每臂 1 行 J1~J6 + 1 行 XYZ ABC）"""
        group = QGroupBox("关节角 / 末端位姿")
        v = QVBoxLayout(group)
        v.setSpacing(4)
        v.setContentsMargins(8, 4, 8, 8)

        self.left_joints = []
        self.right_joints = []
        self.left_pose_labels = []
        self.right_pose_labels = []

        for title, joints, poses, color in (
            ("LeftArm", self.left_joints, self.left_pose_labels, PALETTE['primary']),
            ("RightArm", self.right_joints, self.right_pose_labels, PALETTE['purple']),
        ):
            # J1~J6 单行
            row = QHBoxLayout()
            row.setSpacing(2)
            row.setContentsMargins(0, 0, 0, 0)
            tag = QLabel(title)
            tag.setMinimumWidth(58)
            tag.setStyleSheet(
                f"color: {color}; font-weight: bold; font-size: 12px;"
            )
            tag.setAlignment(Qt.AlignCenter)
            row.addWidget(tag)
            for i in range(6):
                lbl = QLabel(f"J{i+1} 0.0°")
                lbl.setProperty("role", "muted")
                lbl.setAlignment(Qt.AlignCenter)
                lbl.setMinimumWidth(46)
                lbl.setStyleSheet(
                    "QFrame, QLabel{background: transparent;}"
                    f"color: {PALETTE['text']};"
                )
                joints.append(lbl)
                row.addWidget(lbl)
            v.addLayout(row)

            # XYZ ABC 单行
            row2 = QHBoxLayout()
            row2.setSpacing(2)
            row2.setContentsMargins(0, 0, 0, 0)
            tag2 = QLabel("末端")
            tag2.setMinimumWidth(58)
            tag2.setProperty("role", "muted")
            tag2.setAlignment(Qt.AlignCenter)
            row2.addWidget(tag2)
            pose_names = ["X", "Y", "Z", "A", "B", "C"]
            for i, name in enumerate(pose_names):
                unit = "mm" if i < 3 else "°"
                lbl = QLabel(f"{name} 0.0{unit}")
                lbl.setProperty("role", "muted")
                lbl.setAlignment(Qt.AlignCenter)
                lbl.setMinimumWidth(46)
                poses.append(lbl)
                row2.addWidget(lbl)
            v.addLayout(row2)

        return group

    # ========== 事件处理 ==========

    def _on_left_enable_clicked(self, checked: bool):
        self._left_arm_enabled = checked
        self.btn_left_enable.blockSignals(True)
        self.btn_left_enable.setChecked(checked)
        self.btn_left_enable.blockSignals(False)
        self.btn_left_enable.setText("已使能" if checked else "启用")
        self._refresh_enable_btn_style(left=True)
        if checked:
            self.set_left_status("✓ 已使能", ok=True)
        else:
            self.set_left_status("未使能", ok=False)
        self.enable_left_arm.emit(checked)

    def _on_right_enable_clicked(self, checked: bool):
        self._right_arm_enabled = checked
        self.btn_right_enable.blockSignals(True)
        self.btn_right_enable.setChecked(checked)
        self.btn_right_enable.blockSignals(False)
        self.btn_right_enable.setText("已使能" if checked else "启用")
        self._refresh_enable_btn_style(left=False)
        if checked:
            self.set_right_status("✓ 已使能", ok=True)
        else:
            self.set_right_status("未使能", ok=False)
        self.enable_right_arm.emit(checked)

    # ========== 使能状态反馈（被 MainWindow 在收到响应后调用） ==========

    def set_left_status(self, text: str, ok: bool):
        """设置左臂状态文本。ok=True 绿色 / ok=False 红色 / ok=None 灰色提示。"""
        if ok is True:
            self.label_left_status.setText(text)
            self.label_left_status.setStyleSheet(
                f"color: {PALETTE['success_dark']}; font-weight: bold; font-size: 11px;"
                f" background: transparent;"
            )
        elif ok is False:
            self.label_left_status.setText(text)
            self.label_left_status.setStyleSheet(
                f"color: {PALETTE.get('danger', '#e74c3c')}; font-weight: bold; font-size: 11px;"
                f" background: transparent;"
            )
        else:
            self.label_left_status.setText(text)
            self.label_left_status.setStyleSheet(
                f"color: {PALETTE['text_muted']}; font-size: 11px; background: transparent;"
            )
        self._left_arm_status_busy = (ok is None)

    def set_right_status(self, text: str, ok: bool):
        """右臂版（语义见 set_left_status）"""
        if ok is True:
            self.label_right_status.setText(text)
            self.label_right_status.setStyleSheet(
                f"color: {PALETTE['success_dark']}; font-weight: bold; font-size: 11px;"
                f" background: transparent;"
            )
        elif ok is False:
            self.label_right_status.setText(text)
            self.label_right_status.setStyleSheet(
                f"color: {PALETTE.get('danger', '#e74c3c')}; font-weight: bold; font-size: 11px;"
                f" background: transparent;"
            )
        else:
            self.label_right_status.setText(text)
            self.label_right_status.setStyleSheet(
                f"color: {PALETTE['text_muted']}; font-size: 11px; background: transparent;"
            )
        self._right_arm_status_busy = (ok is None)

    def _on_speed_changed(self, value: int):
        self._current_speed = value / 100.0
        self.speed_label.setText(f"{value}%")
        self._update_speed_hint(value)
        self.speed_changed.emit(self._current_speed)

    def _update_speed_hint(self, value: int):
        if value <= 25:
            hint = "慢速 - 精细操作"
        elif value <= 50:
            hint = "标准 - 一般作业"
        elif value <= 75:
            hint = "快速 - 提高效率"
        else:
            hint = "极速 - 测试用"
        self.speed_hint.setText(hint)

    def _on_refresh_clicked(self):
        self.refresh_projects_requested.emit()

    def _on_start_clicked(self):
        project_name = self.project_combo.currentData() or self.project_combo.currentText()
        if project_name:
            self.project_start_requested.emit(project_name)
            self._project_run_status = "running"
            self._update_project_buttons("running")

    def _on_pause_clicked(self):
        self.project_pause_requested.emit()
        self._project_run_status = "paused"
        self._update_project_buttons("paused")

    def _on_resume_clicked(self):
        self.project_resume_requested.emit()
        self._project_run_status = "running"
        self._update_project_buttons("running")

    def _on_stop_clicked(self):
        self.project_stop_requested.emit()
        self._project_run_status = "idle"
        self._update_project_buttons("idle")

    def _update_project_buttons(self, status: str):
        if status == "running":
            self.btn_start.setEnabled(False)
            self.btn_pause.setEnabled(True)
            self.btn_resume.setEnabled(False)
            self.btn_stop.setEnabled(True)
        elif status == "paused":
            self.btn_start.setEnabled(False)
            self.btn_pause.setEnabled(False)
            self.btn_resume.setEnabled(True)
            self.btn_stop.setEnabled(True)
        else:
            self.btn_start.setEnabled(True)
            self.btn_pause.setEnabled(False)
            self.btn_resume.setEnabled(False)
            self.btn_stop.setEnabled(False)

    # ========== 状态更新方法 ==========

    def update_project_list(self, projects: list):
        self.project_combo.clear()
        for project in projects:
            name = project.get("name", "")
            note = project.get("note", "")
            display = f"{name} - {note}" if note else name
            self.project_combo.addItem(display, name)

    def set_arm_connected(self, connected: bool):
        """由 MainWindow 在双臂都连接成功时启用，断开时禁用。

        规则（与 C++ mainwindow.cpp::applyConnectionState 对齐）：
          - 左/右臂使能按钮、速度滑块、工程下拉、刷新按钮、View 按钮：
              connected=True 时启用，否则全部置灰
          - 工程控制按钮（开始/暂停/继续/停止）：未连接时全部禁用；
              已连接后由 _update_project_buttons 按 _project_run_status
              二次锁定（running / paused / idle）
        """
        self.btn_left_enable.setEnabled(connected and not self._left_arm_pending)
        self.btn_right_enable.setEnabled(connected and not self._right_arm_pending)
        self.btn_left_drag.setEnabled(connected)
        self.btn_right_drag.setEnabled(connected)
        self.speed_slider.setEnabled(connected)
        self.project_combo.setEnabled(connected)
        self.btn_refresh.setEnabled(connected)
        self.btn_view.setEnabled(connected)
        if connected:
            self._update_project_buttons(self._project_run_status)
        else:
            # 未连接：所有工程控制按钮统一禁用
            self.btn_start.setEnabled(False)
            self.btn_pause.setEnabled(False)
            self.btn_resume.setEnabled(False)
            self.btn_stop.setEnabled(False)

    def set_left_pending(self, pending: bool):
        """MainWindow 在发送 enable/disable 前后调用，锁定左臂使能按钮。"""
        self._left_arm_pending = pending
        # 仅当基线仍 connected=True 时禁用按钮；重连按钮又是灰色态
        self.btn_left_enable.setEnabled(not pending)

    def set_right_pending(self, pending: bool):
        """右臂版（语义同 set_left_pending）"""
        self._right_arm_pending = pending
        self.btn_right_enable.setEnabled(not pending)

    def update_left_arm_status(self, data: dict):
        enabled = data.get("enabled", False)
        joints = data.get("joints", [])
        mode = data.get("mode", "")

        # 力矩拖动状态：mode ∈ {CSTDrag, ServoDrag}
        self._left_arm_dragging = str(mode) in self._DRAG_MODES
        self._refresh_drag_btn(left=True)

        self._left_arm_enabled = enabled
        self.btn_left_enable.blockSignals(True)
        self.btn_left_enable.setChecked(enabled)
        self.btn_left_enable.blockSignals(False)
        self.btn_left_enable.setText("已使能" if enabled else "启用")
        self._refresh_enable_btn_style(left=True)
        if enabled:
            self.label_left_status.setText("✓ 已使能")
            self.label_left_status.setStyleSheet(
                f"color: {PALETTE['success_dark']}; font-weight: bold; font-size: 11px;"
                f" background: transparent;"
            )
        else:
            self.label_left_status.setText("未使能")
            self.label_left_status.setStyleSheet(
                f"color: {PALETTE['text_muted']}; font-size: 11px; background: transparent;"
            )

        for i, joint in enumerate(joints[:6]):
            if i < len(self.left_joints):
                angle_deg = joint * 180 / 3.14159
                self.left_joints[i].setText(f"J{i+1} {angle_deg:.1f}°")
                self.left_joints[i].setStyleSheet("")

        cartesian = data.get("cartesian", {})
        if isinstance(cartesian, dict) and cartesian:
            self.update_pose(cartesian, which="left")

    def update_right_arm_status(self, data: dict):
        enabled = data.get("enabled", False)
        joints = data.get("joints", [])
        mode = data.get("mode", "")

        # 力矩拖动状态：mode ∈ {CSTDrag, ServoDrag}
        self._right_arm_dragging = str(mode) in self._DRAG_MODES
        self._refresh_drag_btn(left=False)

        self._right_arm_enabled = enabled
        self.btn_right_enable.blockSignals(True)
        self.btn_right_enable.setChecked(enabled)
        self.btn_right_enable.blockSignals(False)
        self.btn_right_enable.setText("已使能" if enabled else "启用")
        self._refresh_enable_btn_style(left=False)
        if enabled:
            self.label_right_status.setText("✓ 已使能")
            self.label_right_status.setStyleSheet(
                f"color: {PALETTE['success_dark']}; font-weight: bold; font-size: 11px;"
                f" background: transparent;"
            )
        else:
            self.label_right_status.setText("未使能")
            self.label_right_status.setStyleSheet(
                f"color: {PALETTE['text_muted']}; font-size: 11px; background: transparent;"
            )

        for i, joint in enumerate(joints[:6]):
            if i < len(self.right_joints):
                angle_deg = joint * 180 / 3.14159
                self.right_joints[i].setText(f"J{i+1} {angle_deg:.1f}°")
                self.right_joints[i].setStyleSheet("")

        cartesian = data.get("cartesian", {})
        if isinstance(cartesian, dict) and cartesian:
            self.update_pose(cartesian, which="right")

    def update_project_status(self, data: dict):
        """工程状态推送回调。

        project/status 推送 db 的字段：
            state      int    0=Idle / 1=Running / 2=Paused
            isStep     bool   单步模式（state=1 时才有意义）
            name       str    当前活动工程名（无活动工程时为空串）
            errorLine  int    失败行；无错为 -1
            message    str    失败说明 / 暂停期说明 / 成功留空

        这里：
          - 顶部标签显示「当前工程: <name> <状态>」
            - state 为 1 且 isStep=true 时，状态文案补「(单步)」
          - 失败 / 暂停时把 message 直接拼到标签正文中（原先只在 tooltip，
            容易看不到），无内容则只显示工程名 + 状态
          - 始终把详细 db 写入 tooltip，便于排障
        """
        state = data.get("state", 0)
        name = data.get("name", "") or "无"
        is_step = bool(data.get("isStep", False))
        error_line = data.get("errorLine", -1)
        message = (data.get("message", "") or "").strip()

        state_names = {0: "待机", 1: "运行中", 2: "已暂停"}
        state_text = state_names.get(state, "未知")
        if state == 1 and is_step:
            state_text += "(单步)"

        # 主显示：当前工程: <name> <状态>[ · <message>]
        #   - 失败（errorLine >= 0）：把 message 用红色高亮前缀贴在状态后面
        #   - 暂停且有 message（恢复中/被拒原因）：直接附在状态后面
        #   - 其它情况：不附加 message，避免日常运行刷一长串
        if error_line is not None and error_line >= 0 and message:
            # 失败行 + 报错：用 “— ” 接在状态后，UI 上更直观
            display = (
                f"当前工程: {name} — {state_text} · "
                f"L{error_line}: {message}"
            )
        elif state == 2 and message:
            # 暂停期说明（含恢复中、被拒原因等）
            display = f"当前工程: {name} — {state_text} · {message}"
        else:
            display = f"当前工程: {name} — {state_text}"

        self.label_project_status.setText(display)
        # tooltip 仍保留完整 db，方便排障时鼠标移上去看 isStep / errorLine 等
        tooltip = (
            f"state={state} isStep={is_step} "
            f"name={name!r} errorLine={error_line} "
            f"message={message!r}"
        )
        self.label_project_status.setToolTip(tooltip)

        if state == 1:
            self._project_run_status = "running"
            self._update_project_buttons("running")
        elif state == 2:
            self._project_run_status = "paused"
            self._update_project_buttons("paused")
        else:
            self._project_run_status = "idle"
            self._update_project_buttons("idle")

    def update_pose(self, cartesian: dict, which: str = "left"):
        labels = self.left_pose_labels if which == "left" else self.right_pose_labels
        x = cartesian.get("x", 0) * 1000
        y = cartesian.get("y", 0) * 1000
        z = cartesian.get("z", 0) * 1000
        a = cartesian.get("a", 0) * 180 / 3.14159
        b = cartesian.get("b", 0) * 180 / 3.14159
        c = cartesian.get("c", 0) * 180 / 3.14159
        labels[0].setText(f"X {x:.1f}mm")
        labels[1].setText(f"Y {y:.1f}mm")
        labels[2].setText(f"Z {z:.1f}mm")
        labels[3].setText(f"A {a:.1f}°")
        labels[4].setText(f"B {b:.1f}°")
        labels[5].setText(f"C {c:.1f}°")

    def set_speed(self, value: float):
        self._current_speed = value
        self.speed_slider.setValue(int(value * 100))
