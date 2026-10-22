"""设备管理器 —— 把现有通信层 client 包一层 asyncio 友好外壳。

真实设备：
    communication.agv_tcp_client.AGVTcpClient   (Qt 版本)
    communication.arm_websocket.ArmWebSocket   (Qt 版本)

替换思路：
    不改原通信层。新建 adapters/ 把 Qt 的回调转成 asyncio 事件，
    推给 event_bus。前端从 /ws/state 订阅。

本期先留 mock 实现，把接口定下来。
"""
from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any


@dataclass
class DeviceState:
    device_id: str
    connected: bool = False
    last_payload: dict[str, Any] = field(default_factory=dict)
    last_seen: float = 0.0


class DeviceManager:
    """所有设备客户端的统一门面。"""

    def __init__(self) -> None:
        self._states: dict[str, DeviceState] = {}
        self._lock = asyncio.Lock()

    async def upsert_state(self, device_id: str, **kw: Any) -> None:
        async with self._lock:
            st = self._states.setdefault(device_id, DeviceState(device_id))
            for k, v in kw.items():
                setattr(st, k, v)
            st.last_seen = asyncio.get_event_loop().time()

    async def snapshot(self) -> list[dict[str, Any]]:
        async with self._lock:
            return [
                {
                    "device_id": s.device_id,
                    "connected": s.connected,
                    "last_payload": s.last_payload,
                    "last_seen": s.last_seen,
                }
                for s in self._states.values()
            ]

    # ----- 真实驱动接口（后续 M1 阶段实现）-----
    async def connect_agv(self, host: str, port: int) -> None: ...
    async def connect_arm(self, url: str) -> None: ...
    async def connect_lifting(self, host: str, port: int) -> None: ...
    async def connect_revo2(self, mac: str) -> None: ...

    # ----- 控制指令接口 -----
    async def agv_move(self, vx: float, vy: float, w: float) -> None: ...
    async def arm_send(self, payload: dict[str, Any]) -> None: ...


device_manager = DeviceManager()
