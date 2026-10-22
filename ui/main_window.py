"""
主窗口
"""
import threading
import datetime
from PySide6.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QGridLayout,
    QGroupBox, QLabel, QPushButton, QLineEdit, QStatusBar, QCheckBox,
    QFrame, QSplitter, QSizePolicy, QSpacerItem
)
from PySide6.QtCore import Qt, Signal, QTimer, QThread, Slot
from PySide6.QtGui import QFont

from ui.agv_control import AGVControlWidget
from ui.arm_control import ArmControlWidget
from ui.hand_control import DualHandPanel
from ui.hand_view_dialog import HandViewDialog
from ui.lifting_column import LiftingColumnWidget
from ui.settings_dialog import SettingsDialog
from ui.message_widget import MessageManager, MessageType, set_global_manager
from ui.webview_dialog import WebViewDialog
from ui.theme import (
    PALETTE, set_role, set_state, soft_shadow, make_button, _set_btn_font,
)
from communication import (
    ArmConnectionManager, AGVConnectionManager
)
from communication.revo2_hand import DualHandManager
from core.settings_manager import SettingsManager


class ConnectionThread(QThread):
    """连接线程"""
    connection_finished = Signal(bool, str, object, object)  # success, message, arm_mgr, agv_mgr

    def __init__(self, arm_ip: str, agv_ip: str, settings, parent=None):
        super().__init__(parent)
        self.arm_ip = arm_ip
        self.agv_ip = agv_ip
        self.settings = settings

    def run(self):
        try:
            # 初始化机械臂连接
            arm_manager = ArmConnectionManager(self.arm_ip, self.settings.arm_websocket_port)
            arm_success = arm_manager.connect()

            # 初始化AGV连接
            agv_ports = {
                "pushback": self.settings.agv_pushback_port,
                "control": self.settings.agv_control_port,
                "nav": self.settings.agv_nav_port,
                "status": self.settings.agv_status_port,
                "other": self.settings.agv_other_port,
                "config": self.settings.agv_config_port
            }
            agv_manager = AGVConnectionManager(self.agv_ip, agv_ports)
            agv_success = agv_manager.connect_all()

            if arm_success and agv_success:
                self.connection_finished.emit(True, "连接成功", arm_manager, agv_manager)
            else:
                msg = []
                if not arm_success:
                    msg.append("机械臂连接失败")
                if not agv_success:
                    msg.append("AGV连接失败")
                self.connection_finished.emit(False, "; ".join(msg), arm_manager if arm_success else None, agv_manager if agv_success else None)

        except Exception as e:
            self.connection_finished.emit(False, f"连接失败: {e}", None, None)


class MainWindow(QMainWindow):
    """主窗口"""

    # 信号：从子线程安全传递 AGV 状态到 UI（避免 QTimer 被事件循环阻塞）
    _sig_agv_state = Signal(dict)
    _sig_agv_nav = Signal(int)
    _sig_agv_lasers = Signal(list)  # 激光数据：lasers 列表（dict 列表）
    # 加载 AGV 站点：子线程拉完地图后 emit，主线程 slot 接管 UI
    _sig_agv_stations_loaded = Signal(list, list, str, dict)
    # 机械臂信号
    _sig_arm_left = Signal(dict)
    _sig_arm_right = Signal(dict)
    _sig_arm_project = Signal(dict)
    # 力矩拖动结果: (arm_name, ok, err, left)
    _drag_result = Signal(str, bool, str, bool)
    # 「清除异常」流水线: 子线程 / QTimer 派发到主线程的跨线程派发信号
    # 用法: 子线程 emit (label, ok, is_agv) / (label, ok, False)，
    # 主线程 @Slot 接收集成 (label, ok) 累加到 state, 并按需要推进流水线
    _sig_clear_err_step = Signal(str, bool, bool)  # (label, ok, is_agv)
    _sig_clear_err_advance = Signal(str)            # ("arms_done" / "lifting_done")
    # Revo2 灵巧手初始化结果: (left_ok, left_err, right_ok, right_err, all_ok, joined_err)
    _sig_hand_init_done = Signal(bool, str, bool, str, bool, str)

    def __init__(self):
        super().__init__()
        self.settings = SettingsManager()

        # 通信管理器
        self.arm_manager = None
        self.agv_manager = None

        # 连接状态
        self._arm_connected = False
        self._agv_connected = False
        # 自动断线重联
        #   - _user_disconnect: 用户主动点「断开」时为 True，重联监视器据此放行
        #   - _reconnect_attempts: 当前重联失败计数（用于指数退避，3s→6s→12s）
        self._user_disconnect = False
        self._reconnect_attempts = 0
        self._reconnect_armed = False  # 用户连上后才置 True，断开真异常才触发

        self._init_ui()
        self._init_connections()
        # 自动断线重联监视器（2s 检测一次）
        self._reconnect_timer = QTimer(self)
        self._reconnect_timer.setInterval(2000)
        self._reconnect_timer.timeout.connect(self._check_auto_reconnect)
        self._reconnect_timer.start()

        # 连接子线程信号（用于 AGV 状态更新，与 Qt 事件循环解耦）
        self._sig_agv_state.connect(self._on_sig_agv_state)
        self._sig_agv_nav.connect(self._on_sig_agv_nav)
        self._sig_agv_lasers.connect(self._on_sig_agv_lasers)
        self._sig_agv_stations_loaded.connect(self._apply_agv_stations_to_ui)
        # 机械臂信号
        self._sig_arm_left.connect(self._on_sig_arm_left)
        self._sig_arm_right.connect(self._on_sig_arm_right)
        self._sig_arm_project.connect(self._on_sig_arm_project)
        self._drag_result.connect(self._on_drag_result)
        # 清除异常流水线信号: 子线程 emit, Qt QueuedConnection 派发到主线程
        self._sig_clear_err_step.connect(self._on_clear_err_step)
        self._sig_clear_err_advance.connect(self._on_clear_err_advance)
        self._sig_hand_init_done.connect(self._on_sig_hand_init_done)

        # 状态更新定时器
        self._status_timer = QTimer()
        self._status_timer.timeout.connect(self._update_status_bar)
        self._status_timer.start(1000)

        # 初始按钮使能状态：未连接，全部 AGV/机械臂/升降柱动作按钮置灰
        # 与 C++ mainwindow.cpp::applyConnectionState 一致
        self._apply_connection_state()

    # ============================================================
    # 布局
    # ============================================================
    def _init_ui(self):
        """初始化UI"""
        self.setWindowTitle("机器人综合控制系统")
        self.resize(1440, 900)
        self.setMinimumSize(1200, 760)

        # 中央部件
        central = QWidget()
        central.setObjectName("centralWidget")
        self.setCentralWidget(central)

        # 消息弹窗系统（装在主窗口上，确保浮在所有子控件之上）
        self._message_manager = MessageManager(self)
        set_global_manager(self._message_manager)

        main_layout = QVBoxLayout(central)
        main_layout.setContentsMargins(14, 14, 14, 8)
        main_layout.setSpacing(12)

        # 顶部连接卡
        top_bar = self._create_top_bar()
        main_layout.addWidget(top_bar)

        # 主体区域：左右分栏
        splitter = QSplitter(Qt.Horizontal)
        splitter.setHandleWidth(10)
        splitter.setChildrenCollapsible(False)

        # 左：AGV
        left_panel = self._create_left_panel()
        splitter.addWidget(left_panel)

        # 右：双臂 + 夹爪 + 升降柱
        right_panel = self._create_right_panel()
        splitter.addWidget(right_panel)

        splitter.setStretchFactor(0, 1)
        splitter.setStretchFactor(1, 2)
        splitter.setSizes([460, 980])

        main_layout.addWidget(splitter, 1)

        # 状态栏
        self.status_bar = QStatusBar()
        self.setStatusBar(self.status_bar)
        self._init_status_bar()

    def _create_left_panel(self) -> QWidget:
        """左面板：AGV 底盘控制"""
        panel = QWidget()
        panel.setObjectName("panel")
        soft_shadow(panel, blur=22, offset=3, alpha=45)
        v = QVBoxLayout(panel)
        v.setContentsMargins(14, 14, 14, 14)
        v.setSpacing(10)

        # 不再加外层标题（AGV widget 内部已有"速度控制/方向控制/导航控制/位置状态"分块）
        self.agv_widget = AGVControlWidget()
        v.addWidget(self.agv_widget, 1)
        return panel

    def _create_right_panel(self) -> QWidget:
        """右面板：左列 = 双臂（占整列高）／ 右列 = 夹爪 + 升降柱（上下分布）"""
        panel = QWidget()
        panel.setObjectName("panel")
        soft_shadow(panel, blur=22, offset=3, alpha=45)
        h = QHBoxLayout(panel)
        h.setContentsMargins(14, 14, 14, 14)
        h.setSpacing(10)

        # —— 左列：双臂（占整列高，stretch 1）——
        self.arm_widget = ArmControlWidget()
        h.addWidget(self.arm_widget, 1)

        # —— 右列：夹爪 + 升降柱（上下分布，stretch 0 固定列宽）——
        right_col = QVBoxLayout()
        right_col.setSpacing(8)
        right_col.setContentsMargins(0, 0, 0, 0)

        # 上：灵巧手（双手） - 替代旧的 GripperControlWidget
        # 不设 stretch → 按内容收缩；6 DOF 角度行已合并为单行紧凑显示
        hand_box = QGroupBox("🤖  Revo2 灵巧手")
        hand_box.setProperty("role", "sub")
        hl = QVBoxLayout(hand_box)
        hl.setContentsMargins(10, 4, 10, 8)
        hl.setSpacing(6)
        self.hand_widget = DualHandPanel()
        # 关键：设 Maximum 高度策略 + 强制限高，让 hand_box 按内容收缩
        # 每个手 panel 98px + 间距 6 + 底栏 30 = 232；groupbox 标题 24 + margins 12 = 268
        from PySide6.QtWidgets import QSizePolicy
        self.hand_widget.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Maximum)
        hand_box.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Maximum)
        # 每手: title12 + 状态行12 + view24 + 按钮36 + 错误+间距+margins ≈ 110
        # 两手 2*110 + 间距 6 + 底栏 30 = 256, + groupbox title+margins = 280
        hand_box.setMaximumHeight(280)
        hl.addWidget(self.hand_widget)
        right_col.addWidget(hand_box, 0)  # 不撑高

        # 下：升降柱（占右列主要空间）
        lifting_box = QGroupBox("📏  升降柱")
        lifting_box.setProperty("role", "sub")
        ll = QVBoxLayout(lifting_box)
        ll.setContentsMargins(10, 4, 10, 8)
        ll.setSpacing(6)
        self.lifting_widget = LiftingColumnWidget()
        ll.addWidget(self.lifting_widget, 1)
        right_col.addWidget(lifting_box, 1)  # 撑满剩余空间

        # 右列固定宽度（双手紧凑 + 升降柱；380 → 350 让双手更紧凑）
        right_container = QWidget()
        right_container.setFixedWidth(350)
        right_container.setLayout(right_col)
        h.addWidget(right_container, 0)

        return panel

    # ============================================================
    # 顶部连接卡
    # ============================================================
    def _create_top_bar(self) -> QFrame:
        """顶部品牌 + 连接卡（果冻风格）"""
        frame = QFrame()
        frame.setObjectName("topBar")
        soft_shadow(frame, blur=20, offset=2, alpha=40)

        layout = QHBoxLayout(frame)
        layout.setContentsMargins(20, 12, 20, 12)
        layout.setSpacing(18)

        # —— 左：品牌 ——
        brand = QHBoxLayout()
        brand.setSpacing(10)
        emoji = QLabel("🤖")
        emoji.setFont(QFont("", 20))
        emoji.setStyleSheet("background: transparent;")
        brand.addWidget(emoji)

        title_box = QVBoxLayout()
        title_box.setSpacing(0)
        title = QLabel("机器人综合控制系统")
        title.setFont(QFont("", 15, QFont.Bold))
        title.setStyleSheet(
            f"background: transparent; color: {PALETTE['text']};"
        )
        subtitle = QLabel("Robot Control Center")
        subtitle.setFont(QFont("", 9))
        subtitle.setProperty("role", "muted")
        title_box.addWidget(title)
        title_box.addWidget(subtitle)
        brand.addLayout(title_box)
        # 关键：brand 套一个 QWidget 让它保持自然宽度，不被后面挤压
        brand_w = QWidget()
        brand_w.setLayout(brand)
        brand_w.setMinimumWidth(260)
        brand_w.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Preferred)
        layout.addWidget(brand_w)

        # 分隔
        layout.addSpacing(8)
        sep1 = self._vsep()
        layout.addWidget(sep1)
        layout.addSpacing(8)

        # —— 中：连接配置 ——
        conn = QHBoxLayout()
        conn.setSpacing(8)

        lbl_arm = QLabel("机械臂")
        lbl_arm.setProperty("role", "muted")
        conn.addWidget(lbl_arm)
        self.arm_ip_input = QLineEdit()
        self.arm_ip_input.setText(self.settings.ip)
        self.arm_ip_input.setPlaceholderText("192.168.100.16")
        self.arm_ip_input.setMaximumWidth(150)
        self.arm_ip_input.setMinimumWidth(120)
        conn.addWidget(self.arm_ip_input)

        lbl_agv = QLabel("AGV")
        lbl_agv.setProperty("role", "muted")
        conn.addWidget(lbl_agv)
        self.agv_ip_input = QLineEdit()
        self.agv_ip_input.setText(self.settings.agv_ip)
        self.agv_ip_input.setPlaceholderText("192.168.100.101")
        self.agv_ip_input.setMaximumWidth(150)
        self.agv_ip_input.setMinimumWidth(120)
        conn.addWidget(self.agv_ip_input)

        self.btn_connect = make_button("连接", role="primary")
        self.btn_connect.setMinimumWidth(112)
        self.btn_connect.setMinimumHeight(36)
        self.btn_connect.clicked.connect(self._on_connect_clicked)
        conn.addWidget(self.btn_connect)

        # 自动断线重联：勾选后，连接异常断开时自动重新连接；用户主动断开不重联
        self.chk_auto_reconnect = QCheckBox("自动断线重联")
        self.chk_auto_reconnect.setCursor(Qt.PointingHandCursor)
        self.chk_auto_reconnect.setToolTip(
            "勾选后，若已连接的机械臂或 AGV 出现异常断开，\n"
            "系统将按 3s / 6s / 12s（上限）自动重新连接。\n"
            "用户主动点「断开」不会触发重联。"
        )
        # 跟随主题文本色（继承 role，不写死颜色）
        conn.addWidget(self.chk_auto_reconnect)
        conn.addStretch(1)

        self.btn_estop = make_button("急停", role="danger")
        self.btn_estop.setMinimumWidth(112)
        self.btn_estop.setMinimumHeight(36)
        self.btn_estop.setToolTip("触发 AGV 软急停并取消导航")
        self.btn_estop.clicked.connect(self._on_agv_emergency_stop)
        conn.addWidget(self.btn_estop)

        layout.addLayout(conn)

        # 分隔
        layout.addSpacing(8)
        sep2 = self._vsep()
        layout.addWidget(sep2)
        layout.addSpacing(8)

        # —— 右：连接状态点 + 功能按钮 ——
        right = QVBoxLayout()
        right.setSpacing(4)

        status_row = QHBoxLayout()
        status_row.setSpacing(14)
        self.arm_status_dot = QLabel("●")
        self.arm_status_dot.setProperty("role", "statusDot")
        set_state(self.arm_status_dot, "off")
        self.arm_status_label = QLabel("机械臂: 未连接")
        self.arm_status_label.setProperty("role", "secondary")
        status_row.addWidget(self.arm_status_dot)
        status_row.addWidget(self.arm_status_label)

        self.agv_status_dot = QLabel("●")
        self.agv_status_dot.setProperty("role", "statusDot")
        set_state(self.agv_status_dot, "off")
        self.agv_status_label = QLabel("AGV: 未连接")
        self.agv_status_label.setProperty("role", "secondary")
        status_row.addWidget(self.agv_status_dot)
        status_row.addWidget(self.agv_status_label)
        right.addLayout(status_row)

        btn_row = QHBoxLayout()
        btn_row.setSpacing(8)
        self.btn_clear_error = make_button("清除异常", role="warning")
        self.btn_clear_error.clicked.connect(self._on_clear_error)
        btn_row.addWidget(self.btn_clear_error)
        self.btn_settings = make_button("⚙ 设置", role="purple")
        self.btn_settings.clicked.connect(self._on_settings_clicked)
        btn_row.addWidget(self.btn_settings)
        right.addLayout(btn_row)

        layout.addLayout(right)
        return frame

    def _vsep(self) -> QFrame:
        """细竖直分隔线"""
        f = QFrame()
        f.setFrameShape(QFrame.VLine)
        f.setStyleSheet(
            f"color: {PALETTE['border']}; background: {PALETTE['border']};"
        )
        f.setFixedWidth(1)
        return f

    # ============================================================
    # 状态栏
    # ============================================================
    def _init_status_bar(self):
        self.status_label = QLabel("系统就绪")
        self.status_label.setProperty("role", "muted")
        self.status_bar.addWidget(self.status_label, 1)

        self.time_label = QLabel()
        self.time_label.setProperty("role", "muted")
        self.status_bar.addPermanentWidget(self.time_label)

    # ============================================================
    # 信号连接
    # ============================================================
    def _init_connections(self):
        # AGV控制信号
        self.agv_widget.speed_changed.connect(self._on_agv_speed_changed)
        self.agv_widget.direction_changed.connect(self._on_agv_direction_changed)
        self.agv_widget.nav_start_requested.connect(self._on_agv_nav_start)
        self.agv_widget.nav_pause_requested.connect(self._on_agv_nav_pause)
        self.agv_widget.nav_cancel_requested.connect(self._on_agv_nav_cancel)
        self.agv_widget.emergency_stop_requested.connect(self._on_agv_emergency_stop)
        self.agv_widget.stop_requested.connect(self._on_agv_stop)

        # 机械臂控制信号
        self.arm_widget.enable_left_arm.connect(self._on_arm_enable_left)
        self.arm_widget.enable_right_arm.connect(self._on_arm_enable_right)
        self.arm_widget.drag_left_arm_requested.connect(self._on_arm_drag_left)
        self.arm_widget.drag_right_arm_requested.connect(self._on_arm_drag_right)
        self.arm_widget.speed_changed.connect(self._on_arm_speed_changed)
        self.arm_widget.project_start_requested.connect(self._on_arm_project_start)
        self.arm_widget.project_pause_requested.connect(self._on_arm_project_pause)
        self.arm_widget.project_resume_requested.connect(self._on_arm_project_resume)
        self.arm_widget.project_stop_requested.connect(self._on_arm_project_stop)
        self.arm_widget.refresh_projects_requested.connect(self._on_arm_refresh_projects)
        # 机械臂 View 按钮：打开 WebView 弹窗
        self.arm_widget.view_requested.connect(self._on_arm_view_requested)

        # 升降柱控制信号
        self.lifting_widget.list_requested.connect(self._refresh_lifting_devices)
        self.lifting_widget.enable_requested.connect(self._on_lifting_enable)
        self.lifting_widget.disable_requested.connect(self._on_lifting_disable)
        self.lifting_widget.clear_error_requested.connect(self._on_lifting_clear_error)
        self.lifting_widget.move_to_requested.connect(self._on_lifting_move_to)
        self.lifting_widget.stop_requested.connect(self._on_lifting_stop)
        self.lifting_widget.set_zero_requested.connect(self._on_lifting_set_zero)
        self.lifting_widget.jog_start_requested.connect(self._on_lifting_jog_start)
        self.lifting_widget.jog_stop_requested.connect(self._on_lifting_jog_stop)

        # Revo2 灵巧手信号
        self.hand_widget.init_requested.connect(self._on_hand_init)
        self.hand_widget.view_requested.connect(self._on_hand_view_requested)
        self.hand_widget.grab_requested.connect(self._on_hand_grab)
        self.hand_widget.release_requested.connect(self._on_hand_release)
        self.hand_widget.stop_all_requested.connect(self._on_hand_stop_all)

    # ========== 连接相关 ==========

    def _on_connect_clicked(self):
        """连接按钮点击"""
        if self._arm_connected or self._agv_connected:
            self._disconnect_all()
        else:
            # 任何主动再点连接都视为用户行为，抑制重联监视器误判
            self._user_disconnect = False
            self._reconnect_armed = False
            self._reconnect_attempts = 0
            arm_ip = self.arm_ip_input.text().strip()
            agv_ip = self.agv_ip_input.text().strip()

            if not arm_ip:
                self._message_manager.warning("请输入机械臂IP地址")
                return
            if not agv_ip:
                self._message_manager.warning("请输入AGV IP地址")
                return

            self._connect_all(arm_ip, agv_ip)

    def _connect_all(self, arm_ip: str, agv_ip: str):
        """连接所有设备"""
        self.btn_connect.setEnabled(False)
        self.btn_connect.setText("连接中…")
        self.status_label.setText(f"正在连接…  机械臂:{arm_ip}  AGV:{agv_ip}")

        # 保存IP
        self.settings.ip = arm_ip
        self.settings.agv_ip = agv_ip
        self.settings.sync()

        # 在线程中连接
        self._connection_thread = ConnectionThread(arm_ip, agv_ip, self.settings, self)
        self._connection_thread.connection_finished.connect(self._on_connection_finished)
        self._connection_thread.start()

    def _on_connection_finished(self, success: bool, message: str, arm_mgr, agv_mgr):
        """连接完成"""
        self.btn_connect.setEnabled(True)

        # 保存管理器引用
        if arm_mgr:
            self.arm_manager = arm_mgr
            self._arm_connected = True
            # 注册机械臂状态回调
            self.arm_manager.set_state_callback(f"robot/{self.settings.left_arm_name}/status", self._on_left_arm_status)
            self.arm_manager.set_state_callback(f"robot/{self.settings.right_arm_name}/status", self._on_right_arm_status)
            self.arm_manager.set_state_callback("project/status", self._on_project_status)
            # 升降柱：订阅状态推送 + 拉取设备列表
            self._init_lifting_column()
            # Revo2 灵巧手：复用 arm_manager.client（ArmWebSocketClient） 通道
            try:
                ws = self.arm_manager.client
                if ws is not None:
                    from communication.revo2_hand import (
                        DEFAULT_EC2RS_SLAVE, DEFAULT_SLAVE_ADDR
                    )
                    self.hand_manager = DualHandManager(
                        ws,
                        left_slave=DEFAULT_EC2RS_SLAVE["left"],
                        right_slave=DEFAULT_EC2RS_SLAVE["right"],
                        left_addr=DEFAULT_SLAVE_ADDR["left"],
                        right_addr=DEFAULT_SLAVE_ADDR["right"],
                    )
                    self.hand_widget.bind_manager(self.hand_manager)
            except Exception as e:
                print(f"[Main] 灵巧手管理器初始化异常: {e}")
        if agv_mgr:
            self.agv_manager = agv_mgr
            self._agv_connected = True
            # 注册 AGV 状态回调（推送端口如果有数据，会触发）
            self.agv_manager.set_state_callback(self._on_agv_pushback_status)
            # 诊断：打印各端口连接状态
            print(f"[Main][诊断] AGV 各端口连接状态:")
            for name, client in [("pushback", self.agv_manager.pushback),
                                  ("control", self.agv_manager.control),
                                  ("nav", self.agv_manager.navigation),
                                  ("status", self.agv_manager.status),
                                  ("other", self.agv_manager.others),
                                  ("config", self.agv_manager.config)]:
                if client is None:
                    print(f"  - {name}: None")
                else:
                    print(f"  - {name}: connected={client.connected}, "
                          f"port={client.port}, sock={client.socket is not None}")
            # 主动拉取一次（防止推送端口被防火墙挡了）
            QTimer.singleShot(500, self._refresh_agv_navigation_status)
            QTimer.singleShot(800, self._load_agv_stations)
            # 启动轮询定时器：每 1 秒拉取一次 AGV 位置/导航状态
            self._agv_poll_timer = QTimer(self)
            self._agv_poll_timer.timeout.connect(self._poll_agv_state)
            self._agv_poll_timer.start(1000)
            print(f"[Main] AGV 轮询定时器已启动")

            # 激光轮询：独立子线程 200ms 一次
            self._laser_poll_running = True
            threading.Thread(target=self._laser_poll_loop, daemon=True).start()
            print(f"[Main] AGV 激光轮询线程已启动")

        if success:
            self.btn_connect.setText("断开")
            set_role(self.btn_connect, "connected")
            self._update_connection_status()
            self._apply_connection_state()
            self.status_label.setText("连接成功 ✓")
            # 双侧都连上 → arm 重联监视器；否则不 arm（防部分连接时误判）
            if self._arm_connected and self._agv_connected:
                self._reconnect_armed = True
                self._reconnect_attempts = 0
        else:
            self.btn_connect.setText("重连")
            set_role(self.btn_connect, "danger")
            self.status_label.setText(message)
            self._message_manager.error(message)
            # 部分连接成功也需要刷新按钮使能状态
            self._update_connection_status()
            self._apply_connection_state()

    def _disconnect_all(self):
        """断开所有连接"""
        # 标记：此次断开是用户主动的，监视器不应触发重联
        self._user_disconnect = True
        self._reconnect_armed = False

        # 停激光轮询
        self._laser_poll_running = False
        if hasattr(self, '_agv_poll_timer'):
            self._agv_poll_timer.stop()

        if self.arm_manager:
            try:
                lc = self.arm_manager.get_lifting_column()
                if lc.is_jogging:
                    lc.stop_jog()
                lc.unsubscribe_state()
            except Exception as e:
                print(f"[Main] 断开时清理升降柱异常: {e}")
            self.arm_manager.disconnect()
            self.arm_manager = None

        if self.agv_manager:
            self.agv_manager.disconnect_all()
            self.agv_manager = None

        self._arm_connected = False
        self._agv_connected = False

        self.btn_connect.setText("连接")
        set_role(self.btn_connect, "primary")

        self._update_connection_status()
        self._apply_connection_state()
        self.status_label.setText("已断开连接")
        if hasattr(self, '_agv_poll_timer'):
            self._agv_poll_timer.stop()

    def _update_connection_status(self):
        """更新连接状态显示"""
        if self._arm_connected:
            self.arm_status_label.setText("机械臂: 已连接")
            set_state(self.arm_status_dot, "on")
        else:
            self.arm_status_label.setText("机械臂: 未连接")
            set_state(self.arm_status_dot, "off")

        if self._agv_connected:
            self.agv_status_label.setText("AGV: 已连接")
            set_state(self.agv_status_dot, "on")
        else:
            self.agv_status_label.setText("AGV: 未连接")
            set_state(self.agv_status_dot, "off")

    # ========== 自动断线重联 ==========

    def _check_auto_reconnect(self):
        """每 2s 检测一次：若勾选了「自动断线重联」且发生被动断线 → 触发重联。

        关键判别：
          - 主动断开（_user_disconnect=True）→ 不动
          - 未 arm（_reconnect_armed=False）→ 不动（防止部分连接状态误判）
          - 监视器只看 manager.connected 的回弹信号，与手动断 / 手动连完全解耦
        """
        if not self.chk_auto_reconnect.isChecked():
            return
        if not self._reconnect_armed:
            return
        if self._user_disconnect:
            return
        # 只要一侧真断就重联（保留 IP 一致性：可能只一侧挂死）
        arm_alive = self.arm_manager is not None and self.arm_manager.connected
        agv_alive = self.agv_manager is not None and self.agv_manager.connected
        if arm_alive and agv_alive:
            # 全部活的——清失败计数
            self._reconnect_attempts = 0
            return
        # 至少一侧断
        if self._connection_thread is not None and self._connection_thread.isRunning():
            return  # 已有重联在跑，不并发
        # 指数退避：第 1 次 3s，第 2 次 6s，第 3 次起 12s 上限
        self._reconnect_attempts += 1
        delay_ms = {1: 3000, 2: 6000}.get(self._reconnect_attempts, 12000)
        self.statusBar().showMessage(
            f"⚠ 检测到连接断开：arm={arm_alive}, agv={agv_alive}；"
            f"{delay_ms/1000:.0f}s 后自动重连（第 {self._reconnect_attempts} 次）",
            delay_ms,
        )
        QTimer.singleShot(delay_ms, self._do_auto_reconnect)

    def _do_auto_reconnect(self):
        """自动重连实际动作：复用 _on_connect_clicked 的「再点连接」分支逻辑。"""
        # 在等待期内用户可能已点断开
        if not self.chk_auto_reconnect.isChecked() or self._user_disconnect:
            return
        # 用户改了 IP 也照重启用（IP 跟随当前输入框）
        arm_ip = self.arm_ip_input.text().strip()
        agv_ip = self.agv_ip_input.text().strip()
        if not arm_ip or not agv_ip:
            return

        # 清 manager 引用并清连接状态（不走 _disconnect_all，那里会标主动断开）
        if self.arm_manager:
            try:
                self.arm_manager.disconnect()
            except Exception:
                pass
            self.arm_manager = None
        if self.agv_manager:
            try:
                self.agv_manager.disconnect_all()
            except Exception:
                pass
            self.agv_manager = None
        if hasattr(self, '_agv_poll_timer'):
            try:
                self._agv_poll_timer.stop()
            except Exception:
                pass
        self._laser_poll_running = False
        self._arm_connected = False
        self._agv_connected = False
        self._reconnect_armed = False

        # 复用既有连接流程（与用户手动点连接的路径完全一致）
        self._connect_all(arm_ip, agv_ip)
        # 状态栏进度提示
        self.statusBar().showMessage("🔄 自动重连中…", 3000)

    def _apply_connection_state(self):
        """根据 AGV/机械臂连接状态批量启用/禁用子组件按钮。

        与 C++ mainwindow.cpp::applyConnectionState 对齐：
          - AGV 控件（速度滑块、四方向按钮、停止、急停、站点下拉、View、
                      开始/暂停/取消导航）：仅当 AGV 连接时启用
          - 机械臂控件（左/右臂使能、速度滑块、工程下拉、刷新、View，
                         开始/暂停/继续/停止工程）：仅当双臂都连接时启用
          - 升降柱控件（上电/下电/清错/移动/停止/点动上升下降/置零/刷新/
                       设备下拉/目标/速度输入）：仅当机械臂连接时启用
                       （升降柱走机械臂 ws 通道）
          - 各子组件内部已有的“运行状态锁定”（导航 4 态、工程 3 态）
            会在 set_agv_connected / set_arm_connected 内自动叠加
        """
        try:
            self.agv_widget.set_agv_connected(self._agv_connected)
        except Exception as e:
            print(f"[Main] set_agv_connected 异常: {e}")
        try:
            self.arm_widget.set_arm_connected(self._arm_connected)
        except Exception as e:
            print(f"[Main] set_arm_connected 异常: {e}")
        try:
            # 升降柱依赖机械臂 ws 通道，使用 _arm_connected 判断
            self.lifting_widget.set_arm_connected(self._arm_connected)
        except Exception as e:
            print(f"[Main] lifting.set_arm_connected 异常: {e}")
        try:
            # Revo2 灵巧手同样依赖机械臂 ws 通道
            self.hand_widget.set_arm_connected(self._arm_connected)
        except Exception as e:
            print(f"[Main] hand.set_arm_connected 异常: {e}")

    # ========== AGV 状态回调 ==========

    def _on_agv_pushback_status(self, data: dict):
        """AGV 状态推送回调（在 recv 线程中调用）"""
        self._sig_agv_state.emit(data)

    def _on_sig_agv_state(self, location: dict):
        """信号槽：处理子线程传来的 AGV 位置数据"""
        self.agv_widget.update_agv_state(location)

    def _on_sig_agv_nav(self, task_status: int):
        """信号槽：处理子线程传来的导航状态"""
        self.agv_widget.update_navigation_status(task_status)

    def _on_sig_agv_lasers(self, lasers: list):
        """信号槽：处理子线程传来的激光数据"""
        self.agv_widget.update_lasers(lasers)

    def _update_agv_state_safe(self, data: dict):
        """更新AGV状态（主线程）"""
        try:
            self.agv_widget.update_agv_state(data)
        except Exception as e:
            print(f"更新AGV状态失败: {e}")

    def _load_agv_stations(self):
        """加载AGV导航站点 - 异步执行，不阻塞 UI"""
        if not self.agv_manager or not self.agv_manager.status:
            return
        if not self.agv_manager.status.connected:
            return

        def _do_load():
            try:
                maps_result = self.agv_manager.status.get_remote_maps()
                if not maps_result:
                    return
                if "maps" not in maps_result or not maps_result["maps"]:
                    return
                current_map = maps_result.get("current_map") or maps_result["maps"][0]
                map_data = self.agv_manager.config.get_map(current_map)
                if not map_data:
                    return
                point_list = (map_data.get("advancedPointList")
                              or map_data.get("advanced_point_list")
                              or map_data.get("points")
                              or [])
                if not point_list and isinstance(map_data.get("db"), dict):
                    db = map_data["db"]
                    point_list = (db.get("advancedPointList")
                                  or db.get("advanced_point_list")
                                  or db.get("points")
                                  or [])
                stations = []
                for p in point_list:
                    if not isinstance(p, dict):
                        continue
                    name = (p.get("instanceName") or p.get("instance_name")
                            or p.get("InstanceName") or p.get("name")
                            or p.get("className") or p.get("class_name"))
                    if not name:
                        continue
                    stations.append({"id": name, "name": name})
                maps_list = list(maps_result.get("maps", []))
                self._sig_agv_stations_loaded.emit(
                    stations, maps_list, current_map, map_data
                )
            except Exception as e:
                print(f"加载AGV站点失败: {e}")

        threading.Thread(target=_do_load, daemon=True).start()

    @Slot(list, list, str, dict)
    def _apply_agv_stations_to_ui(self, stations: list, maps_list: list,
                                   current_map: str, map_data: dict):
        """主线程：更新 UI"""
        try:
            self.agv_widget.update_station_list(stations)
            self.agv_widget.set_map_info(maps_list, current_map, map_data)
        except Exception as e:
            print(f"[apply_agv_stations_to_ui] 异常: {e}")

    def _refresh_agv_navigation_status(self):
        """异步刷新 AGV 导航状态"""
        if not self.agv_manager or not self.agv_manager.status:
            return

        def _do_query():
            try:
                result = self.agv_manager.status.get_navigation_status(simple=True)
                if result and "task_status" in result:
                    QTimer.singleShot(0, lambda: self.agv_widget.update_navigation_status(result["task_status"]))
            except Exception as e:
                print(f"查询导航状态失败: {e}")

        threading.Thread(target=_do_query, daemon=True).start()

    def _poll_agv_state(self):
        """每 1 秒轮询一次 AGV 位置和导航状态"""
        if not self._agv_connected or not self.agv_manager or not self.agv_manager.status:
            return
        if not self.agv_manager.status.connected:
            return

        def _do_poll():
            try:
                location = self.agv_manager.status.get_agv_location()
                if location and isinstance(location, dict):
                    self._sig_agv_state.emit(location)
                nav = self.agv_manager.status.get_navigation_status(simple=True)
                if nav and isinstance(nav, dict) and "task_status" in nav:
                    self._sig_agv_nav.emit(nav["task_status"])
            except Exception:
                pass

    def _laser_poll_loop(self):
        """激光数据轮询循环：200ms 一次"""
        import time
        while getattr(self, '_laser_poll_running', False):
            if not self._agv_connected or not self.agv_manager or not self.agv_manager.status:
                time.sleep(0.5)
                continue
            if not self.agv_manager.status.connected:
                time.sleep(0.5)
                continue
            try:
                resp = self.agv_manager.status.get_laser_data()
                if resp and isinstance(resp, dict):
                    if resp.get("ret_code", 0) != 0:
                        time.sleep(0.2)
                        continue
                    lasers = resp.get("lasers")
                    if lasers is None and "data" in resp:
                        lasers = resp["data"].get("lasers") if isinstance(resp["data"], dict) else None
                    if lasers is None and isinstance(resp.get("data"), list):
                        lasers = resp["data"]
                    if lasers and isinstance(lasers, list):
                        self._sig_agv_lasers.emit(lasers)
            except Exception:
                pass
            time.sleep(0.2)

        threading.Thread(target=_do_poll, daemon=True).start()

    # ========== AGV 控制 ==========

    def _on_agv_speed_changed(self, speed: float):
        """AGV速度变化 - 异步发送"""
        self.settings.agv_speed = speed
        if self.agv_manager and self.agv_manager.control:
            if self.agv_widget._current_direction:
                direction = self.agv_widget._current_direction
                self._on_agv_direction_changed(direction)

    def _on_agv_direction_changed(self, direction: str):
        """AGV方向变化"""
        if not self.agv_manager or not self.agv_manager.control:
            return

        speed = self.settings.agv_speed
        print(f"[AGV] 方向={direction}, 速度={speed} m/s, control.connected={self.agv_manager.control.connected}")

        if direction == "forward":
            self.agv_manager.control.start_continuous_move(vx=speed, vy=0, w=0)
        elif direction == "backward":
            self.agv_manager.control.start_continuous_move(vx=-speed, vy=0, w=0)
        elif direction == "left":
            self.agv_manager.control.start_continuous_move(vx=0, vy=0, w=speed)
        elif direction == "right":
            self.agv_manager.control.start_continuous_move(vx=0, vy=0, w=-speed)

    def _on_agv_stop(self):
        """AGV停止"""
        if self.agv_manager and self.agv_manager.control:
            self.agv_manager.control.stop_continuous_move()

    def _on_agv_nav_start(self, station_id: str):
        if self.agv_manager and self.agv_manager.navigation:
            self.agv_manager.navigation.go_target_point(station_id)

    def _on_agv_nav_pause(self):
        if self.agv_manager and self.agv_manager.navigation:
            self.agv_manager.navigation.pause_navigation()

    def _on_agv_nav_cancel(self):
        if self.agv_manager and self.agv_manager.navigation:
            self.agv_manager.navigation.cancel_navigation()

    def _on_agv_emergency_stop(self):
        """AGV急停"""
        if self.agv_manager:
            if self.agv_manager.control:
                self.agv_manager.control.set_soft_emergency(True)
            if self.agv_manager.navigation:
                self.agv_manager.navigation.cancel_navigation()
        self.status_label.setText("⛔ 急停已触发")

    # ========== 机械臂状态回调 ==========

    def _on_left_arm_status(self, data: dict):
        import os as _os
        if _os.environ.get("ARM_DEBUG_PUSH") == "1":
            print(f"[MainWindow] _on_left_arm_status 收到, keys={list(data.keys()) if isinstance(data, dict) else type(data).__name__}")
        self._sig_arm_left.emit(data)

    def _on_right_arm_status(self, data: dict):
        import os as _os
        if _os.environ.get("ARM_DEBUG_PUSH") == "1":
            print(f"[MainWindow] _on_right_arm_status 收到, keys={list(data.keys()) if isinstance(data, dict) else type(data).__name__}")
        self._sig_arm_right.emit(data)

    def _on_project_status(self, data: dict):
        import os as _os
        if _os.environ.get("ARM_DEBUG_PUSH") == "1":
            print(f"[MainWindow] _on_project_status 收到, data={data}")
        self._sig_arm_project.emit(data)

    def _on_sig_arm_left(self, data: dict):
        self.arm_widget.update_left_arm_status(data)

    def _on_sig_arm_right(self, data: dict):
        self.arm_widget.update_right_arm_status(data)

    def _on_sig_arm_project(self, data: dict):
        self.arm_widget.update_project_status(data)

    # ========== 机械臂控制 ==========

    def _on_arm_enable_left(self, enable: bool):
        if self.arm_manager:
            arm_name = self.settings.left_arm_name
            if enable:
                self.arm_manager.enable_arm(arm_name)
            else:
                self.arm_manager.disable_arm(arm_name)

    def _on_arm_enable_right(self, enable: bool):
        if self.arm_manager:
            arm_name = self.settings.right_arm_name
            if enable:
                self.arm_manager.enable_arm(arm_name)
            else:
                self.arm_manager.disable_arm(arm_name)

    # ========== 力矩拖动（robot/drag） ==========

    def _on_arm_drag_left(self):
        """左臂进入力矩拖动模式（异步，不阻塞 UI）。

        拖动模式只能退到 Disabled，所以「退出拖动」= disable_arm，
        由 ArmControlWidget 检测到 dragging 状态后复用 enable 信号发出。
        """
        self._start_drag(self.settings.left_arm_name, left=True)

    def _on_arm_drag_right(self):
        self._start_drag(self.settings.right_arm_name, left=False)

    def _start_drag(self, arm_name: str, left: bool):
        """发起 robot/drag，结果通过 robot/status 推送自动回灌 UI 状态。"""
        if not self.arm_manager:
            return
        if not arm_name:
            return
        btn = self.arm_widget.btn_left_drag if left else self.arm_widget.btn_right_drag
        btn.setEnabled(False)
        btn.setText("进入中…")

        def _done(ok: bool, err: str):
            # 回调发生在后台线程，通过 Qt 信号切回主线程更新 UI
            self._drag_result.emit(arm_name, ok, err, left)

        self.arm_manager.drag_arm_async(arm_name, _done)

    def _on_drag_result(self, arm_name: str, ok: bool, err: str, left: bool):
        """拖动请求的最终结果（主线程）。

        成功时不改按钮状态——等 robot/status 推上来 mode=CSTDrag/ServoDrag
        后由 update_*_arm_status 回灌，这样 UI 与真机状态始终一致。
        """
        if left:
            btn = self.arm_widget.btn_left_drag
        else:
            btn = self.arm_widget.btn_right_drag
        connected = self.arm_manager.connected if self.arm_manager else False
        btn.setEnabled(connected)
        if ok:
            btn.setText("拖动")  # 等 status 推送改成「退出拖动」
            self.statusBar().showMessage(f"✓ {arm_name} 进入力矩拖动模式", 3000)
        else:
            btn.setText("拖动")
            btn.setToolTip(f"上次拖动失败: {err}")
            self.statusBar().showMessage(f"⚠ {arm_name} 进入拖动失败: {err}", 5000)

    def _on_arm_speed_changed(self, ratio: float):
        self.settings.arm_speed = ratio
        if self.arm_manager:
            self.arm_manager.set_speed_override(ratio)

    def _on_arm_project_start(self, project_name: str):
        if self.arm_manager:
            self.arm_manager.run_project(project_name)

    def _on_arm_project_pause(self):
        if self.arm_manager:
            self.arm_manager.pause_project()

    def _on_arm_project_resume(self):
        if self.arm_manager:
            self.arm_manager.resume_project()

    def _on_arm_project_stop(self):
        if self.arm_manager:
            self.arm_manager.stop_project()

    def _on_arm_refresh_projects(self):
        if self.arm_manager:
            projects = self.arm_manager.list_projects()
            self.arm_widget.update_project_list(projects)

    def _on_arm_view_requested(self):
        """机械臂 View 按钮：弹出 WebView 加载指定 URL（默认机械臂网页）"""
        url = "http://192.168.100.16:9198/"
        dlg = WebViewDialog(url, title="机械臂 WebView", parent=self)
        # 非模态打开：弹窗不阻塞主窗口，可同时操作 AGV/机械臂控制
        dlg.setAttribute(Qt.WA_DeleteOnClose)
        dlg.show()

    # ========== 升降柱（liftingColumn） ==========

    def _init_lifting_column(self):
        """机械臂 ws 通道建好后，初始化升降柱。

        流程与「项目状态」「机械臂状态」保持一致：
          1. 拉设备清单（list）→ 注入「已知设备」让 LiftingColumnClient
             的首帧与兜底轮询知道要给哪些设备发 getState
          2. 订阅 liftingColumn/state 推送（报文字段在顶层）
          3. 订阅内部会自动 fetch_initial_state（用已知设备）+ 启动
             兜底轮询

        首帧 list 可能因为 ws 刚握手 / 控制器主站尚未起来 返回空数组——
        这是控制器侧 34.1「未接升降柱：控制器启动时等待升降柱所属主站 6 s」
        的窗口。这种情况下 UI dropdown 会空，但推送 / 轮询只要拿到名字
        仍能把设备加进 dropdown（lifting_column.py::update_state 兜底）。
        """
        if not self.arm_manager:
            return
        # 1) 先拉设备清单；结果经 self._apply_lifting_devices 注入
        #    LiftingColumnClient._known_devices_set，再启动订阅
        self._refresh_lifting_devices()
        # 2) 订阅推送；UI 走原有回调路径不变
        try:
            lc = self.arm_manager.get_lifting_column()
            ok = lc.subscribe_state(self._on_lifting_state_push, tc_ms=100)
            print(f"[Main] liftingColumn 状态订阅 ok={ok}")
        except Exception as e:
            print(f"[Main] 订阅 liftingColumn 状态异常: {e}")
        # 3) 兜底：3 s / 8 s 再拉两次 list；前几次 ws 刚握手时控制器
        #    「升降柱所属主站」可能未上线导致空数组
        QTimer.singleShot(3000, self._refresh_lifting_devices)
        QTimer.singleShot(8000, self._refresh_lifting_devices)

    def _refresh_lifting_devices(self):
        """拉升降柱设备清单。

        UI 更新走 `LiftingColumnWidget.set_devices`（用 @Slot 装饰，
        Qt 收到 emit 会自动用 QueuedConnection 派发到主线程）。
        """
        if not self.arm_manager:
            return

        def _do():
            try:
                lc = self.arm_manager.get_lifting_column()
                devices = lc.list_devices() or []
                print(f"[Main] liftingColumn list -> {len(devices)} 台: "
                      f"{[d.get('name') for d in devices]}")
                # 把设备清单喂给客户端：让 fetch_initial_state 与
                # 兜底轮询知道该给哪些设备发 getState
                names = [d.get("name") for d in devices if d.get("name")]
                lc.set_known_devices(names)
                # list 拉到东西了，立刻再 fetch_initial_state 一次（subscribe
                # 时已知设备还是空，set_known_devices 也会触发一次；这里
                # 再保险补一刀，覆盖已知设备已被替换 / 重置的情况）
                if devices:
                    lc.fetch_initial_state(devices=names)
                # 跨线程 UI 更新：Signal.emit() 在 PySide6 中跨线程会
                # 自动用 QueuedConnection 派发到目标 widget 所在线程
                # （主线程），由 Qt 保证原子性与线程安全
                self.lifting_widget.apply_devices.emit(devices)
            except Exception as e:
                print(f"[Main] 拉升降柱列表异常: {e}")

        threading.Thread(target=_do, daemon=True).start()

    def _on_lifting_state_push(self, states):
        # 跨线程：直接走 widget 的 apply_states 信号，
        # Qt 会用 QueuedConnection 派发到主线程
        self.lifting_widget.apply_states.emit(states)

    def _on_lifting_enable(self, device: str):
        if not self.arm_manager:
            return
        def _do():
            ok = self.arm_manager.get_lifting_column().enable(device)
            print(f"[Main] liftingColumn enable({device}) -> {ok}")
        threading.Thread(target=_do, daemon=True).start()

    def _on_lifting_disable(self, device: str):
        if not self.arm_manager:
            return
        def _do():
            ok = self.arm_manager.get_lifting_column().disable(device)
            print(f"[Main] liftingColumn disable({device}) -> {ok}")
        threading.Thread(target=_do, daemon=True).start()

    def _on_lifting_clear_error(self, device: str):
        if not self.arm_manager:
            return
        def _do():
            ok = self.arm_manager.get_lifting_column().clear_error(device)
            print(f"[Main] liftingColumn clearError({device}) -> {ok}")
        threading.Thread(target=_do, daemon=True).start()

    def _on_lifting_move_to(self, device: str, height_m: float, speed_mps: float):
        if not self.arm_manager:
            return
        def _do():
            ok = self.arm_manager.get_lifting_column().move_to(
                device, height_m, speed_mps if speed_mps > 0 else None
            )
            print(f"[Main] liftingColumn moveTo({device}, {height_m}m, "
                  f"{speed_mps}m/s) -> {ok}")
        threading.Thread(target=_do, daemon=True).start()

    def _on_lifting_stop(self, device: str):
        if not self.arm_manager:
            return
        def _do():
            ok = self.arm_manager.get_lifting_column().stop(device)
            print(f"[Main] liftingColumn stop({device}) -> {ok}")
        threading.Thread(target=_do, daemon=True).start()

    def _on_lifting_set_zero(self, device: str):
        if not self.arm_manager:
            return
        def _do():
            ok = self.arm_manager.get_lifting_column().set_zero(device)
            print(f"[Main] liftingColumn setZero({device}) -> {ok}")
            if ok:
                QTimer.singleShot(200, self._refresh_lifting_devices)
        threading.Thread(target=_do, daemon=True).start()

    def _on_lifting_jog_start(self, device: str, direction: int,
                              speed_mps: float, timeout_ms: int):
        if not self.arm_manager:
            return
        def _do():
            ok = self.arm_manager.get_lifting_column().start_jog(
                device, direction,
                speed_mps if speed_mps > 0 else None,
                timeout_ms,
            )
            print(f"[Main] liftingColumn jogStart({device}, dir={direction}, "
                  f"v={speed_mps}, t={timeout_ms}ms) -> {ok}")
        threading.Thread(target=_do, daemon=True).start()

    def _on_lifting_jog_stop(self, device: str):
        if not self.arm_manager:
            return
        def _do():
            ok = self.arm_manager.get_lifting_column().stop_jog(device)
            print(f"[Main] liftingColumn jogStop({device}) -> {ok}")
        threading.Thread(target=_do, daemon=True).start()

    # ========== Revo2 灵巧手 ==========

    def _on_hand_init(self):
        """初始化双手（setType + ec2rs485/init）"""
        if not getattr(self, "hand_manager", None):
            self._message_manager.warning("请先连接机械臂")
            return
        self.hand_widget.btn_init.setEnabled(False)
        self.hand_widget.btn_init.setText("初始化中…")
        self.status_label.setText("正在初始化 Revo2 灵巧手…")

        def _do():
            lo, le, ro, re_ = self.hand_manager.init()
            ok = lo and ro
            err = "; ".join(filter(None, [le, re_]))
            # 用信号切回主线程更新 UI
            self._sig_hand_init_done.emit(lo, le, ro, re_, ok, err)

        threading.Thread(target=_do, daemon=True).start()

    def _on_hand_view_requested(self, side: str):
        """打开 View 弹窗（Modbus 调试 + 参数配置 + 序列）"""
        if not getattr(self, "hand_manager", None):
            self._message_manager.warning("请先连接机械臂并初始化灵巧手")
            return
        client = self.hand_manager.left if side == "left" else self.hand_manager.right
        dlg = HandViewDialog(side, client, parent=self)
        dlg.setAttribute(Qt.WA_DeleteOnClose)
        dlg.show()

    def _on_sig_hand_init_done(self, lo: bool, le: str, ro: bool, re_: str,
                                ok: bool, err: str):
        """主线程：处理灵巧手初始化结果"""
        self.hand_widget.btn_init.setEnabled(True)
        self.hand_widget.btn_init.setText("重新连接")
        self.hand_widget.on_init_done(lo, ro)
        if ok:
            self.status_label.setText("Revo2 灵巧手已连接 ✓")
            self._message_manager.info("灵巧手初始化成功")
        else:
            self.status_label.setText(f"Revo2 灵巧手初始化失败: {err}")
            self._message_manager.error(f"灵巧手初始化失败: {err}")

    def _on_hand_grab(self, side: str):
        """主面板「开/抓取」按钮：执行一把抓预设"""
        mgr = getattr(self, "hand_manager", None)
        if not mgr:
            return
        client = mgr.left if side == "left" else mgr.right
        if not client.is_ready():
            self._message_manager.warning(f"{side}手未初始化，请先点击「连接双手」")
            return
        side_cn = "左手" if side == "left" else "右手"
        self.status_label.setText(f"🤚 {side_cn} 一把抓…")

        def _do():
            # 灵巧手可能正在执行上一个动作（write_time_ms 内不会响应新写命令）
            # 先轮询读一次确认手"空闲"，再发写命令
            import time as _t
            for _ in range(5):
                rerr = client.read_positions()
                if not rerr:
                    break
                _t.sleep(0.1)
            # 发写命令（写多指）；遇通信错误重试 2 次
            for attempt in range(3):
                err = client.execute_preset("一把抓")
                if not err:
                    return
                print(f"[Main] {side} 一把抓失败(尝试 {attempt+1}/3): {err}")
                _t.sleep(0.2)
            print(f"[Main] {side} 一把抓最终失败")

        threading.Thread(target=_do, daemon=True).start()

    def _on_hand_release(self, side: str):
        """主面板「合/释放」按钮：执行释放预设"""
        mgr = getattr(self, "hand_manager", None)
        if not mgr:
            return
        client = mgr.left if side == "left" else mgr.right
        if not client.is_ready():
            self._message_manager.warning(f"{side}手未初始化，请先点击「连接双手」")
            return
        side_cn = "左手" if side == "left" else "右手"
        self.status_label.setText(f"🤚 {side_cn} 释放…")

        def _do():
            # 灵巧手可能正在执行上一个动作（write_time_ms 内不会响应新写命令）
            # 先轮询读一次确认手"空闲"，再发写命令
            import time as _t
            for _ in range(5):
                rerr = client.read_positions()
                if not rerr:
                    break
                _t.sleep(0.1)
            # 发写命令（释放）；遇通信错误重试 2 次
            for attempt in range(3):
                err = client.execute_preset("释放")
                if not err:
                    return
                print(f"[Main] {side} 释放失败(尝试 {attempt+1}/3): {err}")
                _t.sleep(0.2)
            print(f"[Main] {side} 释放最终失败")

        threading.Thread(target=_do, daemon=True).start()

    def _on_hand_stop_all(self):
        """急停：双手同时发送停止命令

        Revo2 没有专门的停止命令；我们用一个 0 时间 + 0 位置的快速写命令
        让控制器立即进入静止状态。
        """
        mgr = getattr(self, "hand_manager", None)
        if not mgr:
            return
        def _do():
            # 写 0 位置 + 0 时间（最快停下）
            mgr.left.set_write_time_ms(1)
            mgr.left.write_all([0] * 6)
            mgr.right.set_write_time_ms(1)
            mgr.right.write_all([0] * 6)
        threading.Thread(target=_do, daemon=True).start()
        self.status_label.setText("⏹ 灵巧手急停")
        self._message_manager.warning("灵巧手已发送急停")

    # ========== 其他 ==========

    def _on_clear_error(self):
        """「清除异常」按钮：一次清掉机械臂两台 + 升降柱全部设备 + AGV motor1。

        设计要点：
          - 双臂 / 升降柱 / AGV 的异常互不影响，必须各发各的清错动作
            才能全部解除。机械臂按文档 3.23 要对每台 arm 各发一次
            `robot/clearServoError`（互碰时两臂各清各的）；升降柱每台
            独立 `liftingColumn/clearError`；AGV motor1 保留旧行为
          - 跨线程更新 UI 走 Signal/Slot 模式（与 `_sig_arm_left` 等
            内部信号一致）：子线程 emit，Qt 用 QueuedConnection 派发
            到主线程。比 `QTimer.singleShot(0, callable)` 更稳——
            后者在 PySide6 中跨线程调用始终 DirectConnection 派发到
            当前线程，并不会自动跨线程
          - 流水线：arms(子线程) → lifting(主线程起子线程) → agv(主线程) → 收尾
            收尾按 state["pending_lifting_n"] 等齐所有升降柱结果后再统一报告
        """
        # state 在 self 上保持 (主线程持有, 跨方法共享)
        self._clear_error_state = {"results": [], "pending_lifting_n": 0}
        # 1) 机械臂: 子线程跑双臂清错, 完成后通过信号回主线程推进
        threading.Thread(target=self._clear_error_arms_thread, daemon=True).start()

    def _clear_error_arms_thread(self):
        """子线程: 对左、右两台臂各发一次 clearServoError。

        每条臂清错结果通过 self._sig_clear_err_step 派发到主线程的
        @Slot 收口。完成后 emit 'arms_done' 推进到 lifting 步骤。
        """
        if not self.arm_manager or not self.arm_manager.connected:
            self._sig_clear_err_step.emit("机械臂(未连接)", False, False)
            self._sig_clear_err_advance.emit("arms_done")
            return
        try:
            arm_results = self.arm_manager.clear_all_servo_errors()
        except Exception as e:
            print(f"[Main] clear_all_servo_errors 异常: {e}")
            self._sig_clear_err_step.emit("机械臂(调用异常)", False, False)
            self._sig_clear_err_advance.emit("arms_done")
            return
        for name, ok in arm_results.items():
            self._sig_clear_err_step.emit(f"机械臂 {name}", bool(ok), False)
        self._sig_clear_err_advance.emit("arms_done")

    @Slot(str, bool, bool)
    def _on_clear_err_advance(self, event: str):
        """主线程: 流水线推进钩子。"""
        if event == "arms_done":
            self._clear_error_lifting()

    def _clear_error_lifting(self):
        """主线程: 对升降柱每台设备各发一次 clearError。

        每台起独立子线程, 完成后通过 self._sig_clear_err_step 把
        结果派发回主线程累加。pending_lifting_n 在 main 线程设置,
        AGV 步骤完成后开始按"升降柱 X"条数收尾。
        """
        if not self.arm_manager:
            self._sig_clear_err_step.emit("升降柱(无可清)", True, False)
            self._clear_error_agv()
            return
        try:
            lc = self.arm_manager.get_lifting_column()
        except Exception as e:
            print(f"[Main] 取得 liftingColumn 失败: {e}")
            self._sig_clear_err_step.emit("升降柱(取得失败)", False, False)
            self._clear_error_agv()
            return
        # 优先用 list 取设备名; 失败/空时用客户端缓存的 _known_devices 兜底
        devices: list = []
        try:
            devices = lc.list_devices() or []
        except Exception as e:
            print(f"[Main] 拉升降柱列表用于清错异常: {e}")
        if not devices:
            try:
                names = lc._known_devices()  # noqa: SLF001
            except Exception:
                names = []
            devices = [{"name": n} for n in names]
        if not devices:
            self._sig_clear_err_step.emit("升降柱(无可清)", True, False)
            self._clear_error_agv()
            return

        def _clear_one(name):
            try:
                ok = bool(lc.clear_error(name))
            except Exception as e:
                print(f"[Main] liftingColumn.clearError({name}) 异常: {e}")
                ok = False
            self._sig_clear_err_step.emit(f"升降柱 {name}", ok, False)

        pending_n = 0
        for d in devices:
            name = d.get("name")
            if not name:
                continue
            threading.Thread(target=_clear_one, args=(name,), daemon=True).start()
            pending_n += 1
        self._clear_error_state["pending_lifting_n"] = pending_n
        # 跑 AGV 步骤; 收尾在 _on_clear_err_step 里按"升降柱 X"条数判断
        self._clear_error_agv()

    def _clear_error_agv(self):
        """主线程: AGV motor1 复位, 并通过 signal 把最后一条派发到收口。"""
        agv_ok = True
        if self.agv_manager and self.agv_manager.others:
            try:
                self.agv_manager.others.set_motor_enable("motor1", True)
            except Exception as e:
                print(f"[Main] AGV motor1 复位异常: {e}")
                agv_ok = False
        else:
            agv_ok = False
        self._sig_clear_err_step.emit("AGV motor1", agv_ok, True)

    @Slot(str, bool, bool)
    def _on_clear_err_step(self, label: str, ok: bool, is_agv: bool):
        """主线程: 累加每步结果, AGV 步骤到达时启动收尾轮询。"""
        state = self._clear_error_state
        state["results"].append((label, ok))
        if is_agv:
            self._clear_error_finish_when_done(deadline_ms=5000)

    def _clear_error_finish_when_done(self, deadline_ms: int):
        """主线程: 等升降柱全到 / 超时, 再统一收尾。"""
        state = self._clear_error_state
        pending_n = state.get("pending_lifting_n", 0)
        got_n = sum(1 for label, _ in state["results"]
                    if label.startswith("升降柱 ")
                    and not label.startswith("升降柱(无")
                    and not label.startswith("升降柱(取"))
        if pending_n <= 0 or got_n >= pending_n:
            state["pending_lifting_n"] = 0
            self._finish_clear_error(state["results"])
            return
        if deadline_ms <= 0:
            missing = pending_n - got_n
            for _ in range(missing):
                state["results"].append(("升降柱(超时)", False))
            state["pending_lifting_n"] = 0
            self._finish_clear_error(state["results"])
            return
        QTimer.singleShot(100, lambda: self._clear_error_finish_when_done(
            deadline_ms - 100
        ))

    def _finish_clear_error(self, results: list):
        """收口: 状态栏 + 消息管理器报告清除异常的结果。"""
        if not results:
            self.status_label.setText("已清除异常")
            return
        ok = all(ok for _, ok in results)
        # 状态栏简报
        if ok:
            self.status_label.setText(
                "已清除异常: " + "、".join(label for label, _ in results)
            )
        else:
            failed = [label for label, _ok in results if not _ok]
            self.status_label.setText(
                f"已尝试清除异常；失败: {', '.join(failed)}"
            )
        # 消息管理器详细报告
        try:
            if ok:
                self._message_manager.success(
                    "清除异常: " + "、".join(label for label, _ in results)
                )
            else:
                failed = [label for label, _ok in results if not _ok]
                self._message_manager.warning(
                    "清除异常 部分失败: " + ", ".join(failed)
                )
        except Exception as e:
            print(f"[Main] 报告清除异常结果异常: {e}")

    def _on_settings_clicked(self):
        dialog = SettingsDialog(self)
        if dialog.exec():
            self.arm_ip_input.setText(self.settings.ip)
            self.agv_ip_input.setText(self.settings.agv_ip)
            self._message_manager.success("设置已保存，重新连接后生效")

    def _update_status_bar(self):
        self.time_label.setText(datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))

    def closeEvent(self, event):
        self._disconnect_all()
        event.accept()
