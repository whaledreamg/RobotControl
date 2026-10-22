"""
Revo2 View 弹窗（基于 HTML revoHands 实现）

精确还原 business-test-revo2.html 的 revoHands 行为：
- 6 自由度（DOF）卡片：每个 DOF 都有「发送/当前」双行滑块
- 角度按各 DOF 的最大行程显示（拇指弯曲 0-59°，拇指侧摆 0-90°，其他 0-81°）
- 写时间 1-2000ms 可调
- 实时读取模式（200ms 一次）
- "生成当前关节位置帧"：只生成多指位置+时间帧（功能码 10，寄存器 1010），不发送

TABS:
1. 6 自由度微调（主功能，复刻 HTML revoHands 行为）
2. 预设手势（一把抓/释放/半抓/1010/两指捏/点赞/指点）
3. Modbus 调试（自由 0x04/0x10 报文 + 收发记录）
4. 参数配置（从站地址/ec2rs485通道/写时间/寄存器基址）
5. 关节序列（多步位置时间编辑 + 捕获当前位置 + 播放）
"""
from __future__ import annotations

import threading
import time
from typing import Callable, Dict, List, Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QFormLayout, QGridLayout,
    QLabel, QLineEdit, QComboBox, QPushButton, QTextEdit,
    QGroupBox, QTabWidget, QWidget, QSpinBox, QSlider,
    QTableWidget, QTableWidgetItem, QHeaderView, QFrame,
    QCheckBox, QDoubleSpinBox,
)

from communication.revo2_hand import (
    Revo2HandClient, HandState,
    DOF_INFO, DOF_COUNT, DOF_NAME_LIST, DOF_NAME_CN_LIST, DOF_MAX,
    deg_to_raw, raw_to_deg,
    PRESETS,
    REG_READ_POS, REG_WRITE_DOF, REG_WRITE_ALL,
    WRITE_TIME_MS_DEFAULT, WRITE_TIME_MS_MIN, WRITE_TIME_MS_MAX,
    read_positions_frame, write_one_dof_frame, write_all_dofs_frame,
    crc16, verify_crc,
    DEFAULT_SLAVE_ADDR, DEFAULT_EC2RS_SLAVE,
)
from ui.theme import make_button, PALETTE, set_state, soft_shadow


# ============================================================
# 公共：u16be hex 工具
# ============================================================
def fmt_hex(b: bytes) -> str:
    return " ".join(f"{x:02X}" for x in b)


def bytes_to_list(data) -> List[int]:
    if isinstance(data, list):
        return data
    if isinstance(data, (bytes, bytearray)):
        return list(data)
    return []


# ============================================================
# Tab 1: 6 自由度微调（复刻 HTML revoHands）
# ============================================================
class MicroCard(QFrame):
    """单个 DOF 微调卡（HTML revo-finger 风格）

    关键设计：拖动中只更新本地（slider ↔ spin 同步），不发送 Modbus 命令。
    发送的触发点：
      - send_slider.sliderReleased
      - send_val.editingFinished（回车或失焦）
    这样避免拖动过程中堆积几十个 write 命令阻塞 Qt 事件循环。
    """
    # (dof_idx, deg) — 在拖动结束时（sliderReleased / spin editingFinished）发出
    send_requested = Signal(int, float)

    def __init__(self, dof_idx: int, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.dof_idx = dof_idx
        self.info = DOF_INFO[dof_idx]
        self._touched = False
        self._suppress_signal = False  # 内部同步时用
        self.setObjectName("microCard")
        self.setStyleSheet(f"""
            QFrame#microCard {{
                background: rgba(255,255,255,140);
                border: 1px solid {PALETTE['border']};
                border-radius: 8px;
                padding: 4px;
            }}
        """)
        self._build()

    def _build(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(2)
        # 名称
        name = QLabel(f"{self.info['name_cn']}\n0~{self.info['max']}°")
        name.setStyleSheet("font-size: 11px; font-weight: bold;")
        name.setAlignment(Qt.AlignCenter)
        layout.addWidget(name)
        # 发送行：slider + spin
        send_row = QHBoxLayout()
        send_row.setSpacing(2)
        send_row.setContentsMargins(0, 0, 0, 0)
        tag = QLabel("发送")
        tag.setProperty("role", "muted")
        tag.setStyleSheet("font-size: 9px;")
        tag.setFixedWidth(28)
        send_row.addWidget(tag)
        self.send_slider = QSlider(Qt.Horizontal)
        self.send_slider.setMinimum(0)
        self.send_slider.setMaximum(int(self.info['max'] * 10))  # 0.1° 步长
        self.send_slider.setValue(0)
        # 默认启用：write_dof 会按需自动 init（用户拖动会真正执行；未连接时弹错误）
        self.send_slider.setEnabled(True)
        # 拖动中：仅本地同步，不发信号
        self.send_slider.valueChanged.connect(self._on_send_slider)
        # 释放时：触发发送
        self.send_slider.sliderReleased.connect(self._on_send_released)
        send_row.addWidget(self.send_slider, 1)
        self.send_val = QDoubleSpinBox()
        self.send_val.setDecimals(1)
        self.send_val.setMinimum(0)
        self.send_val.setMaximum(self.info['max'])
        self.send_val.setSingleStep(0.1)
        self.send_val.setValue(0)
        self.send_val.setEnabled(True)
        self.send_val.setFixedWidth(58)
        # spin 值变 → 同步 slider
        self.send_val.valueChanged.connect(self._on_send_spin)
        # spin 编辑完成 → 触发发送
        self.send_val.editingFinished.connect(self._on_send_released)
        send_row.addWidget(self.send_val)
        layout.addLayout(send_row)
        # 当前行：只读
        cur_row = QHBoxLayout()
        cur_row.setSpacing(2)
        cur_row.setContentsMargins(0, 0, 0, 0)
        tag2 = QLabel("当前")
        tag2.setProperty("role", "muted")
        tag2.setStyleSheet("font-size: 9px;")
        tag2.setFixedWidth(28)
        cur_row.addWidget(tag2)
        self.cur_slider = QSlider(Qt.Horizontal)
        self.cur_slider.setMinimum(0)
        self.cur_slider.setMaximum(int(self.info['max'] * 10))
        self.cur_slider.setValue(0)
        # 不 setEnabled(False)：disabled 状态下 setValue() 不会重绘，
        # 导致 "当前" 滑块永远卡在初始位置。用 WA_TransparentForMouseEvents
        # 拦截鼠标事件来实现只读，同时保持 setValue 正常刷新显示。
        self.cur_slider.setAttribute(Qt.WA_TransparentForMouseEvents, True)
        self.cur_slider.setFocusPolicy(Qt.NoFocus)
        cur_row.addWidget(self.cur_slider, 1)
        self.cur_label = QLabel("—")
        self.cur_label.setProperty("role", "muted")
        self.cur_label.setStyleSheet("font-size: 11px; font-family: Consolas, monospace;")
        self.cur_label.setFixedWidth(58)
        self.cur_label.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        cur_row.addWidget(self.cur_label)
        layout.addLayout(cur_row)

    def _on_send_slider(self, val_tenths: int):
        """拖动中：只同步 spin，不触发发送"""
        if self._suppress_signal:
            return
        deg = val_tenths / 10.0
        self._suppress_signal = True
        try:
            self.send_val.setValue(deg)
        finally:
            self._suppress_signal = False
        self._touched = True

    def _on_send_spin(self, deg: float):
        """spin 值变：只同步 slider，不触发发送"""
        if self._suppress_signal:
            return
        val_tenths = int(round(deg * 10))
        self._suppress_signal = True
        try:
            self.send_slider.setValue(val_tenths)
        finally:
            self._suppress_signal = False
        self._touched = True

    def _on_send_released(self):
        """滑块释放 / spin 编辑完成：触发一次发送"""
        deg = self.send_val.value()
        self.send_requested.emit(self.dof_idx, deg)

    def set_enabled(self, enabled: bool):
        self.send_slider.setEnabled(enabled)
        self.send_val.setEnabled(enabled)

    def set_cur_deg(self, deg: float):
        """设置当前角度（从回读）"""
        val_tenths = int(round(deg * 10))
        # 当前滑块（只读）
        self._suppress_signal = True
        try:
            self.cur_slider.setValue(val_tenths)
            self.cur_label.setText(f"{deg:.1f}°")
            # 如果用户没手动调整过，让发送对齐当前
            if not self._touched:
                self.send_slider.setValue(val_tenths)
                self.send_val.setValue(deg)
        finally:
            self._suppress_signal = False

    def get_deg(self) -> float:
        return self.send_val.value()


class FingerMicroTab(QWidget):
    """6 自由度微调 Tab（复刻 HTML revoHands 行为）

    设计：
    - 滑块只在"释放"时触发 send_requested；不在拖动中堆积 Modbus 命令
    - 写命令在后台线程中执行（绝不阻塞 Qt 事件循环）
    - 状态刷新有两条路径：
        (a) client 的后台 200ms 轮询线程 → 推 state → UI 更新 6 DOF 当前角度
        (b) live checkbox 开启时：本 tab 单独 200ms 读一次（同时停掉 client 轮询以避免重复读）
    """
    # 跨线程安全的状态推送（替代 QTimer.singleShot）
    state_updated = Signal(object)  # HandState

    def __init__(self, client: Revo2HandClient, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.client = client
        self._build()
        client.register_state_callback(self._on_state_update)
        self.state_updated.connect(self._apply_init_state)
        # live 模式单独定时器
        self._live_timer = None
        # 写命令后台线程
        self._write_thread: Optional[threading.Thread] = None
        self._write_lock = threading.Lock()  # 保证一次只有一个写命令
        # View 弹窗轮询防重入标志
        self._poll_inflight = False
        # 初始化
        self._apply_init_state(self.client.get_state())
        # ===== View 弹窗自带轮询定时器（与 DualHandPanel 的后台轮询解耦）=====
        # 即使主面板未启动 polling，View 弹窗打开后也能实时刷新。
        from PySide6.QtCore import QTimer
        self._view_poll_timer = QTimer(self)
        self._view_poll_timer.setInterval(200)
        self._view_poll_timer.timeout.connect(self._on_view_poll_tick)
        self._view_poll_timer.start()

    def _on_view_poll_tick(self):
        """View 弹窗自己的 200ms 轮询。
        关键：read_positions 内部会同步等 Modbus 响应（100-400ms），
              如果在主线程调用，UI 会被卡住 → 滑动块变卡。
        修复：丢到后台线程，并用 _poll_inflight 防止上一次还没回就再次触发。
        读成功后会通过 client._notify() 回调 _on_state_update → _apply_init_state
        """
        if self._poll_inflight:
            # 上一次 read 还没回来（timeout 内），本次跳过
            return
        # 不论 inited 与否都调 read_positions（内部会自动 init 后再读）
        self._poll_inflight = True
        threading.Thread(
            target=self._thread_poll_read,
            daemon=True,
            name="revo2-view-poll",
        ).start()

    def _thread_poll_read(self):
        try:
            self.client.read_positions()
        except Exception as e:
            print(f"[Revo2-view] 轮询异常: {e}")
        finally:
            self._poll_inflight = False

    def _build(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(6)
        # 顶部 meta
        meta = QHBoxLayout()
        meta.setSpacing(10)
        meta.setContentsMargins(0, 0, 0, 0)
        lbl = QLabel("通道:")
        lbl.setProperty("role", "muted")
        meta.addWidget(lbl)
        self.cmb_bus = QComboBox()
        self.cmb_bus.setEditable(True)
        self.cmb_bus.addItems(["EtherCAT485L", "EtherCAT485R"])
        idx = self.cmb_bus.findText(self.client.ec2rs_slave)
        if idx >= 0:
            self.cmb_bus.setCurrentIndex(idx)
        meta.addWidget(self.cmb_bus)
        lbl2 = QLabel("从站:")
        lbl2.setProperty("role", "muted")
        meta.addWidget(lbl2)
        self.spin_sid = QSpinBox()
        self.spin_sid.setRange(1, 254)
        self.spin_sid.setValue(self.client.modbus_addr)
        meta.addWidget(self.spin_sid)
        lbl3 = QLabel("时间 ms:")
        lbl3.setProperty("role", "muted")
        meta.addWidget(lbl3)
        self.spin_time = QSpinBox()
        self.spin_time.setRange(WRITE_TIME_MS_MIN, WRITE_TIME_MS_MAX)
        self.spin_time.setValue(WRITE_TIME_MS_DEFAULT)
        meta.addWidget(self.spin_time)
        meta.addStretch(1)
        self.btn_read = make_button("读取当前位置", role="primary")
        self.btn_read.clicked.connect(self._on_read_clicked)
        meta.addWidget(self.btn_read)
        self.chk_live = QCheckBox("实时读取 (200ms)")
        self.chk_live.stateChanged.connect(self._on_live_toggle)
        meta.addWidget(self.chk_live)
        layout.addLayout(meta)
        # 6 DOF 卡片网格
        grid = QGridLayout()
        grid.setSpacing(6)
        self.cards: List[MicroCard] = []
        for i in range(DOF_COUNT):
            card = MicroCard(i)
            card.send_requested.connect(self._on_card_send_requested)
            self.cards.append(card)
            r, c = divmod(i, 3)
            grid.addWidget(card, r, c)
        layout.addLayout(grid)
        # 状态行
        info_row = QHBoxLayout()
        self.lbl_stat = QLabel("先读取当前位置，发送条对齐后再拖动")
        self.lbl_stat.setProperty("role", "muted")
        self.lbl_stat.setStyleSheet("font-size: 11px;")
        info_row.addWidget(self.lbl_stat, 1)
        self.lbl_counter = QLabel("TX:0 RX:0 ERR:0")
        self.lbl_counter.setProperty("role", "muted")
        self.lbl_counter.setStyleSheet("font-size: 11px; font-family: Consolas, monospace;")
        info_row.addWidget(self.lbl_counter)
        layout.addLayout(info_row)

    # ---------- 状态更新（client 后台轮询会推 state）----------

    def _on_state_update(self, state: HandState):
        # Signal 跨线程安全，会自动切到主线程触发 connected slot
        self.state_updated.emit(state)

    def _apply_init_state(self, state: HandState):
        # 滑块始终可用：write_dof 内部会按需自动 init（即使未 init 也能拖动写命令）
        # 不依赖 connected 状态，避免刚打开 View 看着所有滑块都是灰色以为坏了
        for card in self.cards:
            card.set_enabled(True)
        # 更新 6 DOF 当前角度
        for i, raw in enumerate(state.positions):
            if i < len(self.cards):
                deg = raw_to_deg(raw, i)
                self.cards[i].set_cur_deg(deg)
        # 计数
        self.lbl_counter.setText(
            f"TX:{self.client.get_tx_count()} "
            f"RX:{self.client.get_rx_count()} "
            f"ERR:{self.client.get_err_count()}")
        # 状态文本
        if state.last_error:
            self.lbl_stat.setText(f"⚠ {state.last_error}")
        elif state.inited:
            if self.chk_live.isChecked():
                self.lbl_stat.setText("● 通道已 init · 实时模式（200ms）")
            else:
                self.lbl_stat.setText("● 通道已 init · 后台轮询（200ms）")
        else:
            self.lbl_stat.setText("未 init · 拖动将自动 init 并写入（请先到主页确保连接）")

    # ---------- 滑块释放 → 后台线程写 ----------

    def _on_card_send_requested(self, idx: int, deg: float):
        """滑块释放/spin 提交时触发：派发到后台线程执行 write_dof"""
        # 取当前 user 配置
        user_time = self.spin_time.value()
        # 后台线程写
        threading.Thread(
            target=self._thread_write_dof,
            args=(idx, deg, user_time),
            daemon=True,
            name=f"revo2-write-{idx}"
        ).start()

    def _thread_write_dof(self, idx: int, deg: float, time_ms: int):
        # 用锁保证一次只有一个写命令在跑（避免堆线程）
        with self._write_lock:
            # 同步 write_time_ms
            self.client.set_write_time_ms(time_ms)
            # 角度 → 0-1000
            raw = deg_to_raw(deg, idx)
            err = self.client.write_dof(idx, raw)
            # 切回主线程更新状态文字
            from PySide6.QtCore import QTimer
            QTimer.singleShot(0, lambda: self._after_write(idx, raw, err))

    def _after_write(self, idx: int, raw: int, err):
        if err:
            self.lbl_stat.setText(f"⚠ DOF{idx} ({DOF_NAME_CN_LIST[idx]}) 失败: {err}")
        else:
            self.lbl_stat.setText(
                f"✓ DOF{idx} ({DOF_NAME_CN_LIST[idx]}) = {raw_to_deg(raw, idx):.1f}°")
        self.lbl_counter.setText(
            f"TX:{self.client.get_tx_count()} "
            f"RX:{self.client.get_rx_count()} "
            f"ERR:{self.client.get_err_count()}")

    # ---------- 手动读 ----------

    def _on_read_clicked(self):
        """单次读：后台线程跑，不阻塞 UI"""
        threading.Thread(
            target=self._thread_read,
            daemon=True,
            name="revo2-read-once"
        ).start()

    def _thread_read(self):
        err = self.client.read_positions()
        from PySide6.QtCore import QTimer
        QTimer.singleShot(0, lambda: self._after_read(err))

    def _after_read(self, err):
        if err:
            self.lbl_stat.setText(f"⚠ 读失败: {err}")
        else:
            self.lbl_stat.setText("✓ 当前位置已更新")
        self.lbl_counter.setText(
            f"TX:{self.client.get_tx_count()} "
            f"RX:{self.client.get_rx_count()} "
            f"ERR:{self.client.get_err_count()}")

    # ---------- live 模式（调整 client 后台轮询频率）----------

    def _on_live_toggle(self, state: int):
        if state == Qt.Checked:
            # 加速后台轮询：200ms → 50ms
            try:
                if self.client._poll_thread and self.client._poll_thread.is_alive():
                    self.client.stop_polling()
                    self.client.start_polling(0.05)
            except Exception:
                pass
            self.lbl_stat.setText("● 实时模式（50ms 轮询）")
        else:
            # 恢复 200ms
            try:
                if self.client._poll_thread and self.client._poll_thread.is_alive():
                    self.client.stop_polling()
                    self.client.start_polling(0.2)
            except Exception:
                pass
            state = self.client.get_state()
            if state.inited:
                self.lbl_stat.setText("● 通道已 init · 后台轮询（200ms）")
            else:
                self.lbl_stat.setText("未 init")

    # 保留旧方法以防外部调用
    def _start_live(self):
        self.chk_live.setChecked(True)

    def _stop_live(self):
        self.chk_live.setChecked(False)


# ============================================================
# Tab 2: 预设手势
# ============================================================
class PresetTab(QWidget):
    def __init__(self, client: Revo2HandClient, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.client = client
        self._build()

    def _build(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(6)
        title = QLabel("点击按钮执行预设手势：")
        title.setProperty("role", "secondary")
        layout.addWidget(title)
        for name in PRESETS.keys():
            row = QHBoxLayout()
            row.setSpacing(8)
            btn = make_button(f"✋  {name}", role="primary")
            btn.setMinimumHeight(36)
            btn.setStyleSheet(btn.styleSheet() + "font-weight: bold; font-size: 13px;")
            btn.clicked.connect(lambda _, n=name: self._on_preset(n))
            row.addWidget(btn, 1)
            desc = QLabel(self._desc(name))
            desc.setProperty("role", "muted")
            desc.setStyleSheet("font-size: 11px;")
            row.addWidget(desc, 2)
            layout.addLayout(row)
        layout.addStretch(1)

    def _desc(self, name: str) -> str:
        return {
            "一把抓":   "所有 DOF = 1000（满握）",
            "释放":     "所有 DOF = 0（张开）",
            "半抓":     "所有 DOF = 500",
            "1010":     "食指+无名指伸开，其他握紧",
            "两指捏":   "拇指弯曲+侧摆，食指半弯",
            "点赞":     "拇指全伸，其他全握",
            "指点":     "仅食指伸开",
        }.get(name, "")

    def _on_preset(self, name: str):
        from PySide6.QtCore import QTimer
        QTimer.singleShot(0, lambda: self.client.execute_preset(name))


# ============================================================
# Tab 3: Modbus 调试（与 HTML rs485 行为一致）
# ============================================================
class ModbusDebugTab(QWidget):
    def __init__(self, client: Revo2HandClient, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.client = client
        self._build()

    def _build(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(6)
        # 状态
        status_row = QHBoxLayout()
        self.lbl_status = QLabel("通道状态: —")
        self.lbl_status.setProperty("role", "muted")
        status_row.addWidget(self.lbl_status)
        status_row.addStretch(1)
        self.btn_refresh_status = make_button("↻ 刷新状态", role="purple")
        self.btn_refresh_status.clicked.connect(self._refresh_status)
        status_row.addWidget(self.btn_refresh_status)
        layout.addLayout(status_row)
        # 发送
        send_box = QGroupBox("发送（Modbus RTU）")
        sf = QFormLayout(send_box)
        self.cmb_fc = QComboBox()
        self.cmb_fc.addItems([
            "0x04 读输入寄存器 (Read Input)",
            "0x10 写多寄存器 (Write Multiple)",
        ])
        sf.addRow("功能码:", self.cmb_fc)
        self.spin_reg = QSpinBox()
        self.spin_reg.setRange(0, 0xFFFF)
        self.spin_reg.setValue(REG_READ_POS)
        self.spin_reg.setDisplayIntegerBase(16)
        sf.addRow("起始寄存器:", self.spin_reg)
        self.spin_qty = QSpinBox()
        self.spin_qty.setRange(1, 32)
        self.spin_qty.setValue(DOF_COUNT)
        sf.addRow("数量:", self.spin_qty)
        self.txt_data = QLineEdit()
        self.txt_data.setPlaceholderText("HEX 字节，空格分隔，例如：00 00 03 E8 00 64")
        sf.addRow("数据(HEX):", self.txt_data)
        # 便捷按钮
        quick = QHBoxLayout()
        self.btn_send = make_button("发送", role="primary")
        self.btn_send.clicked.connect(self._on_send)
        quick.addWidget(self.btn_send)
        self.btn_gen_read = make_button("生成读位置帧", role="purple")
        self.btn_gen_read.clicked.connect(self._gen_read_frame)
        quick.addWidget(self.btn_gen_read)
        self.btn_gen_all = make_button("生成多指帧 (1010)", role="purple")
        self.btn_gen_all.clicked.connect(self._gen_all_frame)
        quick.addWidget(self.btn_gen_all)
        self.btn_clear = make_button("清空记录", role="warning")
        self.btn_clear.clicked.connect(lambda: self.txt_log.clear())
        quick.addWidget(self.btn_clear)
        sf.addRow(quick)
        layout.addWidget(send_box)
        # 收发
        log_box = QGroupBox("收发记录")
        ll = QVBoxLayout(log_box)
        self.txt_log = QTextEdit()
        self.txt_log.setReadOnly(True)
        self.txt_log.setFontFamily("Consolas, monospace")
        self.txt_log.setMinimumHeight(180)
        ll.addWidget(self.txt_log)
        layout.addWidget(log_box, 1)

    def _refresh_status(self):
        resp = self.client._ws.send_and_wait("ec2rs485/getStatus",
                                             {"slave": self.client.ec2rs_slave},
                                             timeout=3.0)
        if not resp:
            self.lbl_status.setText("通道状态: 无响应")
            return
        if resp.get("err"):
            self.lbl_status.setText(f"通道状态: 错误 {resp['err']}")
            return
        db = resp.get("db", {})
        self.lbl_status.setText(
            f"ready={db.get('ready')}  configured={db.get('configured')}  "
            f"busy={db.get('busy')}  occupiedBy='{db.get('occupiedBy','')}'  "
            f"rx={db.get('receivedLength',0)}B"
        )

    def _parse_hex(self, text: str) -> Optional[List[int]]:
        text = text.strip()
        if not text:
            return []
        try:
            return [int(x, 16) for x in text.split()]
        except ValueError:
            return None

    def _gen_read_frame(self):
        # 读 6 DOF 位置
        frame = read_positions_frame(self.client.modbus_addr)
        self.txt_data.setText(fmt_hex(bytes(frame)).replace(" ", " "))
        self.spin_reg.setValue(REG_READ_POS)
        self.spin_qty.setValue(DOF_COUNT)
        self.cmb_fc.setCurrentIndex(0)

    def _gen_all_frame(self):
        # 多指帧 1010（用当前位置填充）
        state = self.client.get_state()
        positions = state.positions if len(state.positions) == DOF_COUNT else [0] * DOF_COUNT
        time_ms = state.write_time_ms
        frame = write_all_dofs_frame(self.client.modbus_addr, positions, time_ms)
        self.txt_data.setText(fmt_hex(bytes(frame)))
        self.spin_reg.setValue(REG_WRITE_ALL)
        self.spin_qty.setValue(DOF_COUNT * 2)
        self.cmb_fc.setCurrentIndex(1)
        self._log(f"[生成] 多指帧 1010（{DOF_COUNT}×(pos,time)={DOF_COUNT*2} 寄存器）")

    def _on_send(self):
        fc_idx = self.cmb_fc.currentIndex()
        reg = self.spin_reg.value()
        qty = self.spin_qty.value()
        data_hex = self._parse_hex(self.txt_data.text())
        if data_hex is None:
            self._log("✗ 数据格式错误（HEX）")
            return
        slave = self.client.modbus_addr
        if fc_idx == 0:
            # 0x04
            payload = [(reg >> 8) & 0xFF, reg & 0xFF,
                       (qty >> 8) & 0xFF, qty & 0xFF]
            body = bytes([slave, 0x04] + payload)
            from communication.revo2_hand import crc_bytes
            frame = list(body) + crc_bytes(body)
            read_len = 5 + 2 * qty
        else:
            # 0x10：把 HEX 字节直接当作完整帧发送（带 CRC）
            # 兼容模式：用户输入 data 区(不含 sid+fc+reg+qty+bytecount)，自动补 frame 头
            if data_hex and len(data_hex) >= 6 and data_hex[0] == 0 and False:
                pass
            # 简化：用户输入 data 区字节
            # 构造 0x10 帧：sid, 0x10, reg_hi, reg_lo, qty_hi, qty_lo, byte_count, [data], crc
            byte_count = len(data_hex)
            payload = [(reg >> 8) & 0xFF, reg & 0xFF,
                       (qty >> 8) & 0xFF, qty & 0xFF,
                       byte_count] + data_hex
            body = bytes([slave, 0x10] + payload)
            from communication.revo2_hand import crc_bytes
            frame = list(body) + crc_bytes(body)
            read_len = 8
        self._log(f"[{self._ts()}] TX → {fmt_hex(bytes(frame))}")
        rx, err = self.client._ecrs_write_read(
            frame, read_length=read_len, timeout=400)
        if err:
            self._log(f"[{self._ts()}] ✗ {err}")
            return
        if not rx:
            self._log(f"[{self._ts()}] ✗ 无应答")
            return
        crc_ok = verify_crc(rx)
        hex_str = fmt_hex(bytes(rx))
        self._log(f"[{self._ts()}] RX ← {hex_str}  CRC={'OK' if crc_ok else 'FAIL'}")

    def _ts(self) -> str:
        return time.strftime("%H:%M:%S", time.localtime()) + \
               f".{int(time.time() * 1000) % 1000:03d}"

    def _log(self, line: str):
        self.txt_log.append(line)


# ============================================================
# Tab 4: 参数配置
# ============================================================
class ParamConfigTab(QWidget):
    def __init__(self, client: Revo2HandClient, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.client = client
        self._build()

    def _build(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(6)
        # 串口
        uart_box = QGroupBox("串口参数")
        uf = QFormLayout(uart_box)
        self.spin_slave_addr = QSpinBox()
        self.spin_slave_addr.setRange(1, 254)
        self.spin_slave_addr.setValue(self.client.modbus_addr)
        uf.addRow("从站地址 (1~254):", self.spin_slave_addr)
        self.cmb_ec_slave = QComboBox()
        self.cmb_ec_slave.setEditable(True)
        self.cmb_ec_slave.addItems(["EtherCAT485L", "EtherCAT485R"])
        idx = self.cmb_ec_slave.findText(self.client.ec2rs_slave)
        if idx >= 0:
            self.cmb_ec_slave.setCurrentIndex(idx)
        uf.addRow("ec2rs485 通道:", self.cmb_ec_slave)
        self.cmb_baud = QComboBox()
        self.cmb_baud.addItems([
            "115200", "57600", "38400", "19200", "9600", "4800"
        ])
        self.cmb_baud.setCurrentText("115200")
        uf.addRow("波特率:", self.cmb_baud)
        self.cmb_parity = QComboBox()
        self.cmb_parity.addItems(["无校验(0)", "奇校验(1)", "偶校验(2)"])
        uf.addRow("校验位:", self.cmb_parity)
        self.cmb_stop = QComboBox()
        self.cmb_stop.addItems(["1", "2"])
        uf.addRow("停止位:", self.cmb_stop)
        self.spin_time_ms = QSpinBox()
        self.spin_time_ms.setRange(WRITE_TIME_MS_MIN, WRITE_TIME_MS_MAX)
        with self.client._state_lock:
            self.spin_time_ms.setValue(self.client._state.write_time_ms)
        uf.addRow("写时间 ms (1~2000):", self.spin_time_ms)
        layout.addWidget(uart_box)
        # 寄存器
        reg_box = QGroupBox("寄存器（Revo2 协议固定）")
        rf = QFormLayout(reg_box)
        for label, val in [
            ("读当前位置 (0x07D0)", REG_READ_POS),
            ("写单个 DOF (0x041C)", REG_WRITE_DOF),
            ("写所有 DOF (0x03F2)", REG_WRITE_ALL),
        ]:
            sp = QSpinBox()
            sp.setRange(0, 0xFFFF)
            sp.setValue(val)
            sp.setDisplayIntegerBase(16)
            sp.setEnabled(False)  # 固定
            rf.addRow(label + ":", sp)
        layout.addWidget(reg_box)
        layout.addStretch(1)
        # 保存
        btn_row = QHBoxLayout()
        btn_row.addStretch(1)
        self.btn_save = make_button("保存（立即生效）", role="primary")
        self.btn_save.clicked.connect(self._on_save)
        btn_row.addWidget(self.btn_save)
        layout.addLayout(btn_row)

    def _on_save(self):
        self.client.modbus_addr = self.spin_slave_addr.value()
        self.client.ec2rs_slave = self.cmb_ec_slave.currentText().strip() or "EtherCAT485L"
        self.client.set_write_time_ms(self.spin_time_ms.value())
        # 通道变了，下次写/读要重新 init
        self.client._state.inited = False
        QMessageBox_ok(self, "已保存", "参数已生效。下次写/读时若通道变化会重新 init。")


def QMessageBox_ok(parent, title: str, text: str):
    from PySide6.QtWidgets import QMessageBox
    QMessageBox.information(parent, title, text)


# ============================================================
# Tab 5: 关节序列
# ============================================================
class SequenceEditTab(QWidget):
    def __init__(self, client: Revo2HandClient, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.client = client
        self._build()

    def _build(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(6)
        self.table = QTableWidget()
        cols = ["#"] + [f"{name}.{i+1}" for i, name in enumerate(DOF_NAME_LIST)] + \
               ["时间(ms)"]
        self.table.setColumnCount(len(cols))
        self.table.setHorizontalHeaderLabels(cols)
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
        self.table.horizontalHeader().setStretchLastSection(True)
        layout.addWidget(self.table)
        btn_row = QHBoxLayout()
        self.btn_add = make_button("+ 添加步骤", role="primary")
        self.btn_add.clicked.connect(self._add_row)
        btn_row.addWidget(self.btn_add)
        self.btn_del = make_button("- 删除选中", role="warning")
        self.btn_del.clicked.connect(self._del_row)
        btn_row.addWidget(self.btn_del)
        self.btn_clear = make_button("清空", role="danger")
        self.btn_clear.clicked.connect(lambda: self.table.setRowCount(0))
        btn_row.addWidget(self.btn_clear)
        btn_row.addStretch(1)
        self.btn_capture = make_button("捕获当前位置", role="purple")
        self.btn_capture.clicked.connect(self._capture_current)
        btn_row.addWidget(self.btn_capture)
        self.btn_play = make_button("▶ 播放", role="success")
        self.btn_play.clicked.connect(self._on_play)
        btn_row.addWidget(self.btn_play)
        layout.addLayout(btn_row)
        self._add_row()

    def _add_row(self):
        r = self.table.rowCount()
        self.table.insertRow(r)
        item = QTableWidgetItem(str(r + 1))
        item.setFlags(item.flags() & ~Qt.ItemIsEditable)
        self.table.setItem(r, 0, item)
        for c in range(1, DOF_COUNT + 1):
            self.table.setItem(r, c, QTableWidgetItem("0"))
        self.table.setItem(r, DOF_COUNT + 1, QTableWidgetItem("100"))

    def _del_row(self):
        r = self.table.currentRow()
        if r >= 0:
            self.table.removeRow(r)

    def _capture_current(self):
        state = self.client.get_state()
        r = self.table.rowCount() - 1
        if r < 0:
            self._add_row()
            r = 0
        for c in range(DOF_COUNT):
            self.table.item(r, c + 1).setText(str(state.positions[c]))

    def _on_play(self):
        from PySide6.QtCore import QTimer
        for r in range(self.table.rowCount()):
            positions = []
            for c in range(DOF_COUNT):
                txt = self.table.item(r, c + 1).text() if self.table.item(r, c + 1) else "0"
                try:
                    v = max(0, min(1000, int(txt)))
                except ValueError:
                    v = 0
                positions.append(v)
            dur_txt = self.table.item(r, DOF_COUNT + 1).text() if self.table.item(r, DOF_COUNT + 1) else "100"
            try:
                duration_ms = max(10, int(dur_txt))
            except ValueError:
                duration_ms = 100
            QTimer.singleShot(0, lambda pos=positions: self.client.write_all(pos))
            time.sleep(duration_ms / 1000.0)


# ============================================================
# 顶层 View 弹窗
# ============================================================
class HandViewDialog(QDialog):
    """Revo2 View 弹窗（复刻 HTML revoHands 行为 + Modbus 调试）"""
    def __init__(self, side: str, client: Revo2HandClient,
                 parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.side = side
        self.client = client
        self.setWindowTitle(f"Revo2 View — {side.capitalize()}手  6 自由度微调")
        self.resize(900, 620)
        self._build()

    def closeEvent(self, event):
        # 停止 View 弹窗的轮询定时器，避免关闭后后台还在读
        try:
            if hasattr(self, "tab_micro") and self.tab_micro._view_poll_timer:
                self.tab_micro._view_poll_timer.stop()
        except Exception:
            pass
        super().closeEvent(event)

    def _build(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(8)
        side_cn = "左手" if self.side == "left" else "右手"
        title = QLabel(
            f"🤖 Revo2 灵巧手 — {side_cn} "
            f"· 从站 {self.client.modbus_addr} · {self.client.ec2rs_slave}")
        title.setStyleSheet("font-weight: bold; font-size: 14px;")
        layout.addWidget(title)
        self.tabs = QTabWidget()
        self.tab_micro = FingerMicroTab(self.client)
        self.tab_preset = PresetTab(self.client)
        self.tab_debug = ModbusDebugTab(self.client)
        self.tab_param = ParamConfigTab(self.client)
        self.tab_seq = SequenceEditTab(self.client)
        self.tabs.addTab(self.tab_micro, "✋ 6 自由度微调")
        self.tabs.addTab(self.tab_preset, "⭐ 预设手势")
        self.tabs.addTab(self.tab_debug, "🔧 Modbus 调试")
        self.tabs.addTab(self.tab_param, "⚙ 参数配置")
        self.tabs.addTab(self.tab_seq, "▶ 关节序列")
        layout.addWidget(self.tabs, 1)
        btn_row = QHBoxLayout()
        btn_row.addStretch(1)
        self.btn_close = make_button("关闭", role="primary")
        self.btn_close.clicked.connect(self.close)
        btn_row.addWidget(self.btn_close)
        layout.addLayout(btn_row)
        soft_shadow(self, blur=22, offset=3, alpha=70)
