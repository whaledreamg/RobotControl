"""
通信模块初始化
"""
from communication.agv_protocol import AGVProtocol, AGVCommand, AGVTaskStatus
from communication.agv_tcp_client import (
    AGVTCPClient,
    AGVPushbackClient,
    AGVControlClient,
    AGVNavigationClient,
    AGVStatusClient,
    AGVOtherClient,
    AGVConnectionManager
)
from communication.arm_websocket import ArmWebSocketClient, ArmConnectionManager
from communication.lifting_column import LiftingColumnClient
from communication.revo2_hand import (
    Revo2HandClient,
    DualHandManager,
    HandState,
    PRESETS,
    DOF_INFO,
    DOF_NAME_LIST,
    DOF_NAME_CN_LIST,
    DOF_MAX,
    DOF_COUNT,
    deg_to_raw,
    raw_to_deg,
    REG_READ_POS,
    REG_WRITE_DOF,
    REG_WRITE_ALL,
    DEFAULT_SLAVE_ADDR,
    DEFAULT_EC2RS_SLAVE,
)

__all__ = [
    "AGVProtocol",
    "AGVCommand",
    "AGVTaskStatus",
    "AGVTCPClient",
    "AGVPushbackClient",
    "AGVControlClient",
    "AGVNavigationClient",
    "AGVStatusClient",
    "AGVOtherClient",
    "AGVConnectionManager",
    "ArmWebSocketClient",
    "ArmConnectionManager",
    "LiftingColumnClient",
    "Revo2HandClient",
    "DualHandManager",
    "HandState",
    "PRESETS",
    "DOF_INFO",
    "DOF_NAME_LIST",
    "DOF_NAME_CN_LIST",
    "DOF_MAX",
    "DOF_COUNT",
    "deg_to_raw",
    "raw_to_deg",
    "REG_READ_POS",
    "REG_WRITE_DOF",
    "REG_WRITE_ALL",
    "DEFAULT_SLAVE_ADDR",
    "DEFAULT_EC2RS_SLAVE",
]
