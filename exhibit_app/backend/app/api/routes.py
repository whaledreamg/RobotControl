"""REST + WebSocket API。

REST 用于"动作"（连接、断开、发指令）。
WebSocket 用于"状态广播"（所有设备实时数据）。
"""
from __future__ import annotations

import asyncio
import json

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from app.devices.manager import device_manager
from app.events.bus import event_bus

router = APIRouter()


# ---------- REST ----------

@router.get("/api/system/devices")
async def list_devices() -> dict:
    """前端启动时拉取一次设备清单，渲染侧边栏。"""
    return {"devices": await device_manager.snapshot()}


@router.post("/api/device/agv/connect")
async def agv_connect(host: str, port: int) -> dict:
    await device_manager.connect_agv(host, port)
    return {"ok": True, "device_id": "agv"}


@router.post("/api/device/agv/move")
async def agv_move(vx: float, vy: float, w: float) -> dict:
    await device_manager.agv_move(vx, vy, w)
    return {"ok": True}


# arm / lifting / revo2 同模式，本期省略

# ---------- WebSocket 广播 ----------

@router.websocket("/ws/state")
async def ws_state(ws: WebSocket) -> None:
    await ws.accept()

    @event_bus.subscribe("#")  # 简化：先全量收，后面接主题过滤
    async def _push(_topic: str, payload) -> None:
        try:
            await ws.send_text(json.dumps({"topic": _topic, "payload": payload}, default=str))
        except Exception:
            pass

    try:
        while True:
            # 服务端不期望客户端发数据；保活用 ping
            msg = await ws.receive_text()
            if msg == "ping":
                await ws.send_text("pong")
    except WebSocketDisconnect:
        pass
