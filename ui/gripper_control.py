"""
夹爪控制模块 - 紧凑版（果冻风）
"""
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QGroupBox, QLabel,
    QPushButton, QSlider
)
from PySide6.QtCore import Qt, Signal

from ui.theme import make_button


class GripperControlWidget(QWidget):
    """夹爪控制模块（紧凑）"""

    open_requested = Signal()
    close_requested = Signal(int)
    stop_requested = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._init_ui()

    def _init_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)

        # 力度
        force_layout = QHBoxLayout()
        force_layout.setSpacing(4)
        force_layout.setContentsMargins(0, 0, 0, 0)
        lbl = QLabel("力度")
        lbl.setProperty("role", "muted")
        lbl.setMinimumWidth(30)
        force_layout.addWidget(lbl)
        self.force_slider = QSlider(Qt.Horizontal)
        self.force_slider.setMinimum(0)
        self.force_slider.setMaximum(100)
        self.force_slider.setValue(50)
        self.force_slider.setTickPosition(QSlider.TicksBelow)
        self.force_slider.setTickInterval(10)
        force_layout.addWidget(self.force_slider, 1)
        self.force_label = QLabel("50%")
        self.force_label.setAlignment(Qt.AlignCenter)
        self.force_label.setMinimumWidth(36)
        force_layout.addWidget(self.force_label)
        layout.addLayout(force_layout)

        # 按钮（占主要空间）
        btn_layout = QHBoxLayout()
        btn_layout.setSpacing(4)
        btn_layout.setContentsMargins(0, 0, 0, 0)
        self.btn_open = make_button("开", role="primary")
        self.btn_open.setMinimumHeight(32)
        self.btn_open.clicked.connect(self._on_open_clicked)
        self.btn_close = make_button("合", role="warning")
        self.btn_close.setMinimumHeight(32)
        self.btn_close.clicked.connect(self._on_close_clicked)
        self.btn_stop = make_button("停", role="danger")
        self.btn_stop.setMinimumHeight(32)
        self.btn_stop.clicked.connect(self._on_stop_clicked)
        btn_layout.addWidget(self.btn_open, 1)
        btn_layout.addWidget(self.btn_close, 1)
        btn_layout.addWidget(self.btn_stop, 1)
        layout.addLayout(btn_layout)

        # 状态
        self.status_label = QLabel("○ 待机")
        self.status_label.setAlignment(Qt.AlignCenter)
        self.status_label.setProperty("role", "muted")
        self.status_label.setMinimumHeight(22)
        layout.addWidget(self.status_label)

        self.force_slider.valueChanged.connect(self._on_force_changed)

    def _on_force_changed(self, value: int):
        self.force_label.setText(f"{value}%")

    def _on_open_clicked(self):
        self.status_label.setText("→ 打开中…")
        self.open_requested.emit()

    def _on_close_clicked(self):
        force = self.force_slider.value()
        self.status_label.setText(f"→ 闭合中 ({force}%)")
        self.close_requested.emit(force)

    def _on_stop_clicked(self):
        self.status_label.setText("■ 停止")
        self.stop_requested.emit()

    def update_status(self, status: str):
        self.status_label.setText(f"● {status}")

    def set_force(self, value: int):
        self.force_slider.setValue(value)
