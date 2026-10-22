"""
QSettings 封装，用于持久化存储配置
"""
from PySide6.QtCore import QSettings


class SettingsManager:
    """配置管理器"""
    
    _instance = None
    
    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._initialized = False
        return cls._instance
    
    def __init__(self):
        if self._initialized:
            return
        self._initialized = True
        self._settings = QSettings("RobotControl", "RobotControlApp")
    
    def get(self, key: str, default=None):
        """获取配置值"""
        return self._settings.value(key, default)
    
    def set(self, key: str, value):
        """设置配置值"""
        self._settings.setValue(key, value)
    
    # 连接配置
    @property
    def ip(self) -> str:
        return self.get("connection/ip", "192.168.100.16")
    
    @ip.setter
    def ip(self, value: str):
        self.set("connection/ip", value)
    
    @property
    def agv_ip(self) -> str:
        return self.get("connection/agv_ip", "192.168.100.101")
    
    @agv_ip.setter
    def agv_ip(self, value: str):
        self.set("connection/agv_ip", value)
    
    # 机械臂配置
    @property
    def arm_websocket_port(self) -> int:
        return int(self.get("arm/websocket_port", 9000))
    
    @arm_websocket_port.setter
    def arm_websocket_port(self, value: int):
        self.set("arm/websocket_port", value)
    
    @property
    def left_arm_name(self) -> str:
        return self.get("arm/left_name", "LeftArm")
    
    @left_arm_name.setter
    def left_arm_name(self, value: str):
        self.set("arm/left_name", value)
    
    @property
    def right_arm_name(self) -> str:
        return self.get("arm/right_name", "RightArm")
    
    @right_arm_name.setter
    def right_arm_name(self, value: str):
        self.set("arm/right_name", value)
    
    @property
    def arm_push_interval(self) -> int:
        return int(self.get("arm/push_interval", 100))
    
    @arm_push_interval.setter
    def arm_push_interval(self, value: int):
        self.set("arm/push_interval", value)
    
    @property
    def arm_speed(self) -> float:
        return float(self.get("arm/speed", 1.0))
    
    @arm_speed.setter
    def arm_speed(self, value: float):
        self.set("arm/speed", value)
    
    # AGV配置
    @property
    def agv_pushback_port(self) -> int:
        return int(self.get("agv/pushback_port", 19301))
    
    @agv_pushback_port.setter
    def agv_pushback_port(self, value: int):
        self.set("agv/pushback_port", value)
    
    @property
    def agv_control_port(self) -> int:
        return int(self.get("agv/control_port", 19205))
    
    @agv_control_port.setter
    def agv_control_port(self, value: int):
        self.set("agv/control_port", value)
    
    @property
    def agv_nav_port(self) -> int:
        return int(self.get("agv/nav_port", 19206))
    
    @agv_nav_port.setter
    def agv_nav_port(self, value: int):
        self.set("agv/nav_port", value)
    
    @property
    def agv_other_port(self) -> int:
        return int(self.get("agv/other_port", 19210))
    
    @agv_other_port.setter
    def agv_other_port(self, value: int):
        self.set("agv/other_port", value)
    
    @property
    def agv_status_port(self) -> int:
        return int(self.get("agv/status_port", 19204))

    @agv_status_port.setter
    def agv_status_port(self, value: int):
        self.set("agv/status_port", value)

    @property
    def agv_config_port(self) -> int:
        return int(self.get("agv/config_port", 19207))

    @agv_config_port.setter
    def agv_config_port(self, value: int):
        self.set("agv/config_port", value)
    
    @property
    def agv_speed(self) -> float:
        return float(self.get("agv/speed", 0.5))
    
    @agv_speed.setter
    def agv_speed(self, value: float):
        self.set("agv/speed", value)
    
    # 升降柱（liftingColumn）配置
    @property
    def lifting_device(self) -> str:
        """默认升降柱设备名（出厂 "Column"），与 liftingColumn/list 的 name 字段一致。"""
        return str(self.get("liftingColumn/device", "Column"))

    @lifting_device.setter
    def lifting_device(self, value: str):
        self.set("liftingColumn/device", str(value))

    @property
    def lifting_target_height_mm(self) -> int:
        """UI 上次输入的目标高度，mm；与控制器 m 单位间由界面换算。"""
        return int(self.get("liftingColumn/target_height_mm", 300))

    @lifting_target_height_mm.setter
    def lifting_target_height_mm(self, value: int):
        self.set("liftingColumn/target_height_mm", int(value))

    @property
    def lifting_speed_mps(self) -> float:
        """运动缺省速度，m/s；<=0 表示交给控制器用 maxVelocity。"""
        return float(self.get("liftingColumn/speed_mps", 0.05))

    @lifting_speed_mps.setter
    def lifting_speed_mps(self, value: float):
        self.set("liftingColumn/speed_mps", float(value))

    @property
    def lifting_jog_speed_mps(self) -> float:
        """点动缺省速度，m/s；<=0 表示交给控制器用 jogVelocity。"""
        return float(self.get("liftingColumn/jog_speed_mps", 0.03))

    @lifting_jog_speed_mps.setter
    def lifting_jog_speed_mps(self, value: float):
        self.set("liftingColumn/jog_speed_mps", float(value))

    @property
    def lifting_jog_timeout_ms(self) -> int:
        """点动心跳超时，ms；范围 100~5000。"""
        return int(self.get("liftingColumn/jog_timeout_ms", 500))

    @lifting_jog_timeout_ms.setter
    def lifting_jog_timeout_ms(self, value: int):
        v = max(100, min(5000, int(value)))
        self.set("liftingColumn/jog_timeout_ms", v)

    # 夹爪配置
    @property
    def gripper_force(self) -> int:
        return int(self.get("gripper/force", 50))
    
    @gripper_force.setter
    def gripper_force(self, value: int):
        self.set("gripper/force", value)
    
    def sync(self):
        """同步保存到磁盘"""
        self._settings.sync()
    
    def reset_to_default(self):
        """恢复默认配置"""
        keys = [
            "connection/ip",
            "connection/agv_ip",
            "arm/websocket_port", "arm/left_name", "arm/right_name", "arm/push_interval", "arm/speed",
            "agv/pushback_port", "agv/control_port", "agv/nav_port", "agv/other_port", "agv/status_port", "agv/speed",
            # 旧 bollard 键：保留覆盖以免历史值残留
            "bollard/height", "bollard/upper_limit", "bollard/lower_limit",
            "liftingColumn/device", "liftingColumn/target_height_mm",
            "liftingColumn/speed_mps", "liftingColumn/jog_speed_mps", "liftingColumn/jog_timeout_ms",
            "gripper/force"
        ]
        defaults = {
            "connection/ip": "192.168.100.16",
            "connection/agv_ip": "192.168.100.101",
            "arm/websocket_port": 9000,
            "arm/left_name": "LeftArm",
            "arm/right_name": "RightArm",
            "arm/push_interval": 100,
            "arm/speed": 1.0,
            "agv/pushback_port": 19301,
            "agv/control_port": 19205,
            "agv/nav_port": 19206,
            "agv/other_port": 19210,
            "agv/status_port": 19204,
            "agv/speed": 0.5,
            "bollard/height": 500,        # 旧键保留以兼容历史配置；新代码用 liftingColumn/*
            "bollard/upper_limit": 1000,
            "bollard/lower_limit": 0,
            "liftingColumn/device": "Column",
            "liftingColumn/target_height_mm": 300,
            "liftingColumn/speed_mps": 0.05,
            "liftingColumn/jog_speed_mps": 0.03,
            "liftingColumn/jog_timeout_ms": 500,
            "gripper/force": 50,
        }
        for key in keys:
            self.set(key, defaults[key])
        self.sync()
