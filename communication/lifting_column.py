"""
升降柱（liftingColumn）通信客户端

与 C++ communication/liftingcolumn.h/cpp 对齐

依据 API 文档第三十四章实现，11 个动作 + 状态推送订阅。
复用已有的 ArmWebSocketClient：它本身就是一个通用 JSON 信封客户端，
能处理任意 `liftingColumn/*` 报文，本模块只在其上加动作封装与点动心跳线程。

高度单位：API 用 m；UI 层负责 mm ↔ m 转换。

设计要点：
- 点动心跳：独立线程在按下按钮期间每 200ms 持续发 jogHeartbeat，
  松开 / 异常 / 连接断开时自动停止
- 状态订阅：liftingColumnState/sub（字段在顶层）+ 首帧主动拉取 + 1s 兜底轮询
- 错误码映射：API 文档三十章硬错误码
"""
import threading
import time
from typing import Callable, Optional, Dict, List, Any

from communication.arm_websocket import ArmWebSocketClient


# 升降柱业务错误码（与 C++ 保持一致）
ERR_UNKNOWN_DEVICE   = 23260
ERR_OFFLINE         = 23261
ERR_DRIVER_FAULT    = 23262
ERR_NOT_ENABLED     = 23263
ERR_HEIGHT_OUT      = 23264
ERR_SPEED_INVALID   = 23265
ERR_EMERGENCY_STOP  = 23266
ERR_ENABLE_TIMEOUT  = 23267
ERR_MOVING          = 23268
ERR_SETZERO_SAVE    = 23269
ERR_DRIVER_ACK      = 23270
ERR_JOG_DIR_INVALID = 23271
ERR_CLEAR_TIMEOUT   = 23272
ERR_AT_LIMIT        = 23273

# 错误码映射（与 C++ LiftingColumnClient::errorText() 对齐）
ERROR_TEXT = {
    ERR_UNKNOWN_DEVICE:   "未知设备",
    ERR_OFFLINE:          "设备离线",
    ERR_DRIVER_FAULT:     "驱动器故障",
    ERR_NOT_ENABLED:      "设备未使能",
    ERR_HEIGHT_OUT:       "高度超出范围",
    ERR_SPEED_INVALID:    "速度无效",
    ERR_EMERGENCY_STOP:   "急停触发",
    ERR_ENABLE_TIMEOUT:   "使能超时",
    ERR_MOVING:           "设备正在运动",
    ERR_SETZERO_SAVE:     "零点保存失败",
    ERR_DRIVER_ACK:       "驱动器无应答",
    ERR_JOG_DIR_INVALID:  "点动方向非法",
    ERR_CLEAR_TIMEOUT:    "清除超时",
    ERR_AT_LIMIT:         "已到达限位",
}


def error_text(code: int) -> str:
    """获取错误码对应的文本（与 C++ LiftingColumnClient::errorText() 对齐）"""
    return ERROR_TEXT.get(code, f"未知错误({code})")


def error_text_from_obj(err_obj: Dict) -> str:
    """从错误对象获取错误文本"""
    if not err_obj:
        return ""
    code = err_obj.get("code", 0)
    msg = err_obj.get("message", "")
    if code != 0:
        text = error_text(code)
        return text + (f": {msg}" if msg else "")
    return msg or "未知错误"


# 心跳默认周期：API 文档说发送间隔须小于 timeoutMs，建议不超过其一半。
# 缺省 timeoutMs = 500 ms，故心跳默认 200 ms 一次。
DEFAULT_JOG_TIMEOUT_MS = 500
DEFAULT_JOG_HEARTBEAT_INTERVAL_S = 0.2


class LiftingColumnClient:
    """
    升降柱客户端：包在 ArmWebSocketClient 之上，封装 11 个动作 + state 推送。
    
    与 C++ LiftingColumnClient 对齐
    
    设计要点：
    - 不重起 TCP/WS 连接，直接复用 arm_manager 的连接；
    - `send_and_wait` 走 ws 同步收发，与机械臂动作同源；
    - 点动心跳由独立子线程在按下按钮期间持续发送 `jogHeartbeat`，
      松开按钮 / 异常 / 连接断开时自动停止。
    """

    def __init__(self, ws: ArmWebSocketClient):
        self._ws = ws
        # 心跳线程控制
        self._jog_lock = threading.Lock()
        self._jog_thread: Optional[threading.Thread] = None
        self._jog_device: Optional[str] = None
        self._jog_stop = threading.Event()
        # 状态推送回调
        self._state_callbacks: Dict[str, Callable] = {}
        # 状态缓存
        self._state_cache: Dict[str, dict] = {}
        self._state_lock = threading.Lock()
        # 已知设备清单
        self._known_devices_set: set = set()
        self._known_lock = threading.Lock()
        # 兜底轮询
        self._poll_lock = threading.Lock()
        self._poll_thread: Optional[threading.Thread] = None
        self._poll_stop: Optional[threading.Event] = None
        # 订阅状态
        self._subscribed = False

    # ===================== 设备列表 =====================

    def list_devices(self) -> List[dict]:
        """
        列出全部升降柱及其软限位 / 速度上限
        
        与 C++ LiftingColumnClient::listDevices() 对齐
        """
        resp = self._ws.send_and_wait("liftingColumn/list", {}, timeout=5.0)
        if not resp:
            print("[liftingColumn] list 无响应")
            return []
        if resp.get("err"):
            print(f"[liftingColumn] list 失败: {error_text_from_obj(resp.get('err', {}))}")
            return []
        db = resp.get("db")
        return db if isinstance(db, list) else []

    # ===================== 状态查询 / 推送 =====================

    def get_state(self, device: str) -> Optional[dict]:
        """
        读一台升降柱的完整状态
        
        与 C++ LiftingColumnClient::getState() 对齐
        """
        resp = self._ws.send_and_wait(
            "liftingColumn/getState", {"device": device}, timeout=5.0
        )
        if not resp:
            print(f"[liftingColumn] getState({device}) 无响应")
            return None
        if resp.get("err"):
            print(f"[liftingColumn] getState({device}) 失败: {error_text_from_obj(resp.get('err', {}))}")
            return None
        return resp.get("db") if isinstance(resp.get("db"), dict) else None

    def subscribe_state(self, callback: Callable[[List[dict]], None],
                       tc_ms: int = 100) -> bool:
        """
        订阅 liftingColumn/state 推送（全量周期），并启动兜底轮询
        
        与 C++ LiftingColumnClient::subscribeState() 对齐
        
        与工程状态（`project/status`）保持一致的「订阅 + 首帧 + 兜底」模式：
          - 发送 `liftingColumnState/sub` 订阅（报文字段在顶层）
          - 启动兜底轮询线程：每秒拉一次 `getState` 给每台已知设备
          - 主动拉一次首帧并触发回调
        """
        # 文档 34.15 规定：订阅字段在报文顶层
        msg_id = self._ws._send_top_level(
            "liftingColumnState/sub",
            topics="liftingColumn/state",
            tc=int(tc_ms),
        )
        if msg_id < 0:
            return False
        
        self._state_callbacks["liftingColumn/state"] = callback
        self._ws.register_callback("liftingColumn/state", self._on_state_push)
        self._subscribed = True
        
        # 主动拉一次首帧
        self.fetch_initial_state(devices=self._known_devices())
        # 兜底轮询
        self._start_polling()
        return True

    def unsubscribe_state(self) -> bool:
        """
        退订状态推送并停止兜底轮询
        
        与 C++ LiftingColumnClient::unsubscribeState() 对齐
        """
        self._stop_polling()
        msg_id = self._ws._send_top_level(
            "liftingColumnState/sub",
            topics="liftingColumn/state",
            off=1,
        )
        self._state_callbacks.pop("liftingColumn/state", None)
        self._ws.unregister_callback("liftingColumn/state")
        
        # 清空缓存与设备清单
        with self._state_lock:
            self._state_cache.clear()
        with self._known_lock:
            self._known_devices_set.clear()
        self._subscribed = False
        return msg_id >= 0

    def fetch_initial_state(self, devices: Optional[List[str]] = None) -> List[dict]:
        """
        主动拉一次首帧
        
        与 C++ LiftingColumnClient::fetchInitialState() 对齐
        """
        names = list(devices) if devices else list(self._known_devices())
        if not names:
            return []
        
        with self._known_lock:
            self._known_devices_set.update(names)
        
        states: List[dict] = []
        for name in names:
            try:
                s = self.get_state(name)
            except Exception as e:
                print(f"[liftingColumn] getState({name}) 异常: {e}")
                s = None
            if isinstance(s, dict) and s.get("name") == name:
                with self._state_lock:
                    self._state_cache[name] = s
                states.append(s)
        
        if states:
            cb = self._state_callbacks.get("liftingColumn/state")
            if cb:
                try:
                    cb(states)
                except Exception as e:
                    print(f"[liftingColumn] 首帧回调异常: {e}")
        return states

    def set_known_devices(self, devices: List[str]) -> None:
        """
        记录当前已知的设备名清单
        
        与 C++ LiftingColumnClient::setKnownDevices() 对齐
        """
        cleaned = [d for d in devices if d]
        with self._known_lock:
            was_empty = not self._known_devices_set
            self._known_devices_set.clear()
            self._known_devices_set.update(cleaned)
        
        if was_empty and cleaned and self._state_callbacks.get("liftingColumn/state"):
            try:
                self.fetch_initial_state(devices=cleaned)
            except Exception as e:
                print(f"[liftingColumn] set_known_devices 首帧异常: {e}")

    def _known_devices(self) -> List[str]:
        with self._known_lock:
            return list(self._known_devices_set)

    def _start_polling(self) -> None:
        """
        启动兜底轮询线程
        
        与 C++ LiftingColumnClient::startPolling() 对齐
        """
        with self._poll_lock:
            if self._poll_thread and self._poll_thread.is_alive():
                return
            self._poll_stop = threading.Event()
        t = threading.Thread(target=self._poll_loop, daemon=True)
        with self._poll_lock:
            self._poll_thread = t
        t.start()

    def _stop_polling(self) -> None:
        """停止兜底轮询"""
        with self._poll_lock:
            stop = self._poll_stop
            t = self._poll_thread
        if stop is not None:
            stop.set()
        if t is not None and t.is_alive():
            t.join(timeout=1.0)
        with self._poll_lock:
            self._poll_thread = None
            self._poll_stop = None

    def _poll_loop(self) -> None:
        """
        兜底轮询循环
        
        与 C++ LiftingColumnClient::onPollTick() 对齐
        """
        with self._poll_lock:
            stop = self._poll_stop
        if stop is None:
            return
        
        # 让订阅帧先到达；1s 后再开始轮询
        if stop.wait(1.0):
            return
        
        while not stop.is_set():
            try:
                names = self._known_devices()
                if not names:
                    if stop.wait(2.0):
                        return
                    continue
                
                states: List[dict] = []
                for name in names:
                    s = self.get_state(name)
                    if isinstance(s, dict) and s.get("name") == name:
                        with self._state_lock:
                            self._state_cache[name] = s
                        states.append(s)
                
                if states:
                    cb = self._state_callbacks.get("liftingColumn/state")
                    if cb:
                        try:
                            cb(states)
                        except Exception as e:
                            print(f"[liftingColumn] 轮询回调异常: {e}")
            except Exception as e:
                print(f"[liftingColumn] 轮询异常: {e}")
            
            if stop.wait(1.0):
                return

    def _on_state_push(self, db: Any):
        """
        状态推送处理
        
        与 C++ LiftingColumnClient::onStatePush() 对齐
        """
        if not isinstance(db, list):
            return
        
        with self._state_lock:
            for s in db:
                if isinstance(s, dict) and "name" in s:
                    self._state_cache[s["name"]] = s
        
        # 记录已知设备
        with self._known_lock:
            for s in db:
                if isinstance(s, dict) and s.get("name"):
                    self._known_devices_set.add(s["name"])
        
        cb = self._state_callbacks.get("liftingColumn/state")
        if cb:
            try:
                cb(db)
            except Exception as e:
                print(f"[liftingColumn] 状态回调异常: {e}")

    def cached_state(self, device: str) -> Optional[dict]:
        """读最近一次缓存的状态"""
        with self._state_lock:
            return self._state_cache.get(device)

    # ===================== 上电控制 =====================

    def enable(self, device: str) -> bool:
        """
        使能
        
        与 C++ LiftingColumnClient::enable() 对齐
        """
        return self._simple_call("liftingColumn/enable", device)

    def disable(self, device: str) -> bool:
        """
        下电
        
        与 C++ LiftingColumnClient::disable() 对齐
        """
        return self._simple_call("liftingColumn/disable", device)

    def clear_error(self, device: str) -> bool:
        """
        清除驱动器故障
        
        与 C++ LiftingColumnClient::clearError() 对齐
        """
        return self._simple_call("liftingColumn/clearError", device)

    # ===================== 运动 =====================

    def move_to(self, device: str, height_m: float,
                speed_mps: Optional[float] = None) -> bool:
        """
        定高运动
        
        与 C++ LiftingColumnClient::moveTo() 对齐
        """
        db: Dict[str, Any] = {"device": device, "height": float(height_m)}
        if speed_mps is not None:
            db["speed"] = float(speed_mps)
        
        resp = self._ws.send_and_wait("liftingColumn/moveTo", db, timeout=3.0)
        if not resp:
            print(f"[liftingColumn] moveTo({device}) 无响应")
            return False
        if resp.get("err"):
            print(f"[liftingColumn] moveTo({device}) 失败: {error_text_from_obj(resp.get('err', {}))}")
            return False
        return resp.get("db") is True

    def stop(self, device: str) -> bool:
        """
        停止
        
        与 C++ LiftingColumnClient::stop() 对齐
        """
        return self._simple_call("liftingColumn/stop", device)

    def set_zero(self, device: str) -> bool:
        """
        把当前位置记为高度 0 并保存
        
        与 C++ LiftingColumnClient::setZero() 对齐
        """
        return self._simple_call("liftingColumn/setZero", device, timeout=4.0)

    # ===================== 点动（按住 + 心跳）=====================

    def start_jog(self, device: str, direction: int,
                  speed_mps: Optional[float] = None,
                  timeout_ms: int = DEFAULT_JOG_TIMEOUT_MS) -> bool:
        """
        开始点动
        
        与 C++ LiftingColumnClient::startJog() 对齐
        
        direction=1 上升，-1 下降
        成功后会自动起一个心跳线程
        """
        if direction not in (1, -1):
            print(f"[liftingColumn] start_jog 方向非法: {direction}")
            return False
        
        with self._jog_lock:
            if self._jog_thread and self._jog_thread.is_alive():
                self._stop_jog_locked()
            
            db: Dict[str, Any] = {
                "device": device,
                "direction": int(direction),
                "timeoutMs": int(timeout_ms),
            }
            if speed_mps is not None:
                db["speed"] = float(speed_mps)
            
            resp = self._ws.send_and_wait(
                "liftingColumn/jogStart", db, timeout=3.0
            )
            if not resp or resp.get("err") or resp.get("db") is not True:
                if resp and resp.get("err"):
                    print(f"[liftingColumn] jogStart 失败: {error_text_from_obj(resp.get('err', {}))}")
                elif resp:
                    print(f"[liftingColumn] jogStart 返回非 true: db={resp.get('db')!r}")
                return False
            
            self._jog_device = device
            self._jog_stop.clear()
            interval = max(0.05, min(timeout_ms / 2000.0,
                                     DEFAULT_JOG_HEARTBEAT_INTERVAL_S))
            self._jog_thread = threading.Thread(
                target=self._jog_heartbeat_loop,
                args=(device, interval),
                daemon=True,
            )
            self._jog_thread.start()
            return True

    def stop_jog(self, device: Optional[str] = None) -> bool:
        """
        结束点动
        
        与 C++ LiftingColumnClient::stopJog() 对齐
        """
        with self._jog_lock:
            self._stop_jog_locked()
        
        target = device or (self._jog_device if self._jog_device else "")
        if not target:
            return True
        return self._simple_call("liftingColumn/jogStop", target)

    def _stop_jog_locked(self):
        """持锁时停心跳线程"""
        self._jog_stop.set()
        t = self._jog_thread
        if t and t.is_alive():
            t.join(timeout=0.5)
        self._jog_thread = None
        self._jog_device = None

    def _jog_heartbeat_loop(self, device: str, interval: float):
        """
        心跳线程
        
        与 C++ LiftingColumnClient::_jogHeartbeatLoop() 对齐
        """
        while not self._jog_stop.is_set():
            if not self._ws.connected:
                break
            try:
                resp = self._ws.send_and_wait(
                    "liftingColumn/jogHeartbeat", {"device": device}, timeout=2.0
                )
                if resp and resp.get("err"):
                    print(f"[liftingColumn] jogHeartbeat 失败: {error_text_from_obj(resp.get('err', {}))}")
                    break
            except Exception as e:
                print(f"[liftingColumn] jogHeartbeat 异常: {e}")
                break
            if self._jog_stop.wait(interval):
                break

    # ===================== 工具 =====================

    def _simple_call(self, ty: str, device: str,
                     extra_db: Optional[Dict[str, Any]] = None,
                     timeout: float = 3.0) -> bool:
        """
        通用调用
        
        与 C++ LiftingColumnClient::simpleCall() 对齐
        """
        db = dict(extra_db) if extra_db else {}
        db.setdefault("device", device)
        
        resp = self._ws.send_and_wait(ty, db, timeout=timeout)
        if not resp:
            return False
        if resp.get("err"):
            err = resp["err"]
            code = err.get("code")
            msg = err.get("message", "")
            print(f"[liftingColumn] {ty}({device}) 失败 code={code} {msg}")
            return False
        return resp.get("db") is True

    @property
    def is_jogging(self) -> bool:
        """是否正在点动中"""
        with self._jog_lock:
            return self._jog_thread is not None and self._jog_thread.is_alive()
