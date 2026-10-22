# 展厅版 RobotControl — 架构

> 目标：把现有 PySide6 桌面控制台改造成**安卓平板/手机可装**的展厅演示 App，**通信层代码不重写**。

## 三层

1. **mobile** — Capacitor 套壳安卓 APK，本质是个 WebView + 原生权限桥。
2. **frontend** — Vue3 + Vite + Vant（移动端友好），离线走 PWA service worker。
3. **backend** — FastAPI（Python 3.11+），异步事件总线，复用现有的 `communication/` 设备客户端。

## 设备连接拓扑

```
Android APk (前端 Vue)
       │  HTTP REST + WebSocket (局域网)
       ▼
Python FastAPI (运行在展厅笔记本/工控机)
       │
       ├─ 复用 RobotControl/communication/agv_tcp_client.py
       ├─ 复用 RobotControl/communication/arm_websocket.py
       ├─ 复用 RobotControl/communication/lifting_column.py
       └─ 复用 RobotControl/communication/revo2_hand.py
       │
       ▼
    AGV / 机械臂 / 升降柱 / ReVo2 手
```

## 事件流（替代 Qt 信号槽）

设备客户端原本用 Qt Signal/Slot 推状态 → 现在改成：

```
设备驱动层 (communication/*.py)
   │
   │  原始回调（线程/同步）
   ▼
events/bus.py  (asyncio EventBus, 单例)
   │
   ├─ 推到 WebSocket `/ws/state` 给前端
   └─ 推到本地状态缓存 (core/state.py) 给 REST 查询
```

## 与原 RobotControl 的关系

- `communication/`、`core/`、`config/` **直接以 sys.path 方式 import**，不复制代码。
- 修改原驱动时只需保证它**仍然能 import**，新后端只是把它包了一层异步外壳。
- 原 PySide6 桌面工程**完全不动**，保留作为开发期调试窗口。

## 后续路线图（分期）

| 期 | 目标 | 产出 |
|---|---|---|
| M1 | 后端能 ping 通设备 | `uvicorn` 起得来，复用一两个驱动接到模拟器 |
| M2 | 前端能在浏览器控制 | 一台笔记本浏览器能控制 AGV |
| M3 | Capacitor 打包 | APK 装到平板，演示通过 |
| M4 | PWA 离线 | 断网也能用 |
| M5 | 高级：后端进手机（Termux/Kivy 路线） | 单机方案（可选） |
