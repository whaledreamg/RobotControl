"""
设置对话框
"""
from PySide6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QTabWidget, QWidget,
    QFormLayout, QLabel, QLineEdit, QSpinBox, QPushButton,
    QGroupBox, QDialogButtonBox, QMessageBox
)
from PySide6.QtCore import Qt

from core.settings_manager import SettingsManager


class SettingsDialog(QDialog):
    """设置对话框"""
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self.settings = SettingsManager()
        self.setWindowTitle("系统设置")
        self.setMinimumWidth(500)
        self._init_ui()
        self._load_settings()
    
    def _init_ui(self):
        layout = QVBoxLayout(self)
        
        # 创建标签页
        tabs = QTabWidget()
        
        # 连接设置页
        tabs.addTab(self._create_connection_tab(), "连接配置")
        
        # 机械臂设置页
        tabs.addTab(self._create_arm_tab(), "机械臂配置")
        
        # AGV设置页
        tabs.addTab(self._create_agv_tab(), "AGV配置")
        
        # 数据管理页
        tabs.addTab(self._create_data_tab(), "数据管理")
        
        layout.addWidget(tabs)
        
        # 按钮
        buttons = QDialogButtonBox(
            QDialogButtonBox.Ok | QDialogButtonBox.Cancel | QDialogButtonBox.RestoreDefaults
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        buttons.button(QDialogButtonBox.RestoreDefaults).clicked.connect(self._on_restore)
        
        layout.addWidget(buttons)
    
    def _create_connection_tab(self) -> QWidget:
        """连接配置页"""
        widget = QWidget()
        layout = QVBoxLayout(widget)
        
        group = QGroupBox("网络连接")
        form = QFormLayout(group)
        
        # 机械臂 IP地址
        self.ip_edit = QLineEdit()
        self.ip_edit.setPlaceholderText("例如: 192.168.100.16")
        form.addRow("机械臂 IP:", self.ip_edit)
        
        # AGV IP地址
        self.agv_ip_edit = QLineEdit()
        self.agv_ip_edit.setPlaceholderText("例如: 192.168.100.101")
        form.addRow("AGV IP:", self.agv_ip_edit)
        
        layout.addWidget(group)
        layout.addStretch()
        
        return widget
    
    def _create_arm_tab(self) -> QWidget:
        """机械臂配置页"""
        widget = QWidget()
        layout = QVBoxLayout(widget)
        
        group = QGroupBox("机械臂配置 (WebSocket)")
        form = QFormLayout(group)
        
        # WebSocket端口
        self.arm_port_spin = QSpinBox()
        self.arm_port_spin.setRange(1, 65535)
        self.arm_port_spin.setSuffix(" port")
        form.addRow("WebSocket端口:", self.arm_port_spin)
        
        # 左臂名称
        self.left_arm_edit = QLineEdit()
        self.left_arm_edit.setPlaceholderText("例如: LeftArm")
        form.addRow("左臂名称:", self.left_arm_edit)
        
        # 右臂名称
        self.right_arm_edit = QLineEdit()
        self.right_arm_edit.setPlaceholderText("例如: RightArm")
        form.addRow("右臂名称:", self.right_arm_edit)
        
        # 推送间隔
        self.arm_interval_spin = QSpinBox()
        self.arm_interval_spin.setRange(50, 1000)
        self.arm_interval_spin.setSuffix(" ms (10Hz = 100ms)")
        self.arm_interval_spin.setSingleStep(50)
        form.addRow("推送间隔:", self.arm_interval_spin)
        
        layout.addWidget(group)
        layout.addStretch()
        
        return widget
    
    def _create_agv_tab(self) -> QWidget:
        """AGV配置页"""
        widget = QWidget()
        layout = QVBoxLayout(widget)
        
        group = QGroupBox("AGV配置 (TCP)")
        form = QFormLayout(group)
        
        # 推送端口
        self.agv_pushback_spin = QSpinBox()
        self.agv_pushback_spin.setRange(1, 65535)
        self.agv_pushback_spin.setSuffix(" port")
        form.addRow("推送端口:", self.agv_pushback_spin)
        
        # 控制端口
        self.agv_control_spin = QSpinBox()
        self.agv_control_spin.setRange(1, 65535)
        self.agv_control_spin.setSuffix(" port")
        form.addRow("控制端口:", self.agv_control_spin)
        
        # 导航端口
        self.agv_nav_spin = QSpinBox()
        self.agv_nav_spin.setRange(1, 65535)
        self.agv_nav_spin.setSuffix(" port")
        form.addRow("导航端口:", self.agv_nav_spin)
        
        # 状态端口
        self.agv_status_spin = QSpinBox()
        self.agv_status_spin.setRange(1, 65535)
        self.agv_status_spin.setSuffix(" port")
        form.addRow("状态端口:", self.agv_status_spin)
        
        # 其他端口
        self.agv_other_spin = QSpinBox()
        self.agv_other_spin.setRange(1, 65535)
        self.agv_other_spin.setSuffix(" port")
        form.addRow("其他端口:", self.agv_other_spin)
        
        layout.addWidget(group)
        layout.addStretch()
        
        return widget
    
    def _create_data_tab(self) -> QWidget:
        """数据管理页"""
        widget = QWidget()
        layout = QVBoxLayout(widget)
        
        group = QGroupBox("配置管理")
        form = QFormLayout(group)
        
        self.reset_btn = QPushButton("恢复默认设置")
        self.reset_btn.clicked.connect(self._on_restore)
        form.addRow("", self.reset_btn)
        
        info_label = QLabel(
            "点击\"恢复默认设置\"将把所有配置恢复为默认值。\n"
            "此操作不可撤销。"
        )
        info_label.setStyleSheet("color: #7f8c8d;")
        form.addRow("", info_label)
        
        layout.addWidget(group)
        layout.addStretch()
        
        return widget
    
    def _load_settings(self):
        """加载配置"""
        # 连接
        self.ip_edit.setText(self.settings.ip)
        self.agv_ip_edit.setText(self.settings.agv_ip)
        
        # 机械臂
        self.arm_port_spin.setValue(self.settings.arm_websocket_port)
        self.left_arm_edit.setText(self.settings.left_arm_name)
        self.right_arm_edit.setText(self.settings.right_arm_name)
        self.arm_interval_spin.setValue(self.settings.arm_push_interval)
        
        # AGV
        self.agv_pushback_spin.setValue(self.settings.agv_pushback_port)
        self.agv_control_spin.setValue(self.settings.agv_control_port)
        self.agv_nav_spin.setValue(self.settings.agv_nav_port)
        self.agv_status_spin.setValue(self.settings.agv_status_port)
        self.agv_other_spin.setValue(self.settings.agv_other_port)
    
    def _on_accept(self):
        """保存并关闭"""
        # 保存连接
        self.settings.ip = self.ip_edit.text()
        self.settings.agv_ip = self.agv_ip_edit.text()
        
        # 保存机械臂
        self.settings.arm_websocket_port = self.arm_port_spin.value()
        self.settings.left_arm_name = self.left_arm_edit.text()
        self.settings.right_arm_name = self.right_arm_edit.text()
        self.settings.arm_push_interval = self.arm_interval_spin.value()
        
        # 保存AGV
        self.settings.agv_pushback_port = self.agv_pushback_spin.value()
        self.settings.agv_control_port = self.agv_control_spin.value()
        self.settings.agv_nav_port = self.agv_nav_spin.value()
        self.settings.agv_status_port = self.agv_status_spin.value()
        self.settings.agv_other_port = self.agv_other_spin.value()
        
        self.settings.sync()
        
        self.accept()
    
    def _on_restore(self):
        """恢复默认"""
        reply = QMessageBox.question(
            self,
            "恢复默认设置",
            "确定要恢复所有设置为默认值吗？",
            QMessageBox.Yes | QMessageBox.No
        )
        
        if reply == QMessageBox.Yes:
            self.settings.reset_to_default()
            self._load_settings()
            QMessageBox.information(self, "成功", "已恢复默认设置")
