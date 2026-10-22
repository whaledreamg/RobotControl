"""
机械臂 WebSocket 通信客户端

与 C++ communication/armwebsocket.h/cpp 对齐

关键特性：
    - **单连接管理双臂**：Python 版是单条 ws 承载左右臂，
      报文里的 arm_name 字段区分左右臂
    - 报文信封：{ "id": n, "ty": "动作名", "db": {...} }
    - **订阅类报文字段放顶层**（robotState/sub、projectState/sub、
      liftingColumnState/sub），不在 db 内——这是文档硬性要求
    - 订阅 + 首帧主动拉取 + 1s 轮询兜底 三重保障 UI 不空/不卡

升降柱与 Revo2 灵巧手复用本连接（见 liftingcolumn.py / revo2_hand.py）。
"""
import json
import threading
import time
import os
from typing import Callable, Optional, Dict, Any, Tuple

import websocket

from core.settings_manager import SettingsManager


def arm_debug_push() -> bool:
    """调试开关（与 C++ ARM_DEBUG_PUSH 环境变量一致）"""
    return os.environ.get("ARM_DEBUG_PUSH") == "1"


class ArmWebSocketClient:
    """机械臂 WebSocket 客户端"""
    
    def __init__(self, ip: str, port: int):
        self.ip = ip
        self.port = port
        self.url = f"ws://{ip}:{port}"
        self.ws: Optional[websocket.WebSocketApp] = None
        self.connected = False
        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._id = 0
        self._lock = threading.Lock()
        self._callbacks: Dict[str, Callable] = {}
        self._pending_requests: Dict[int, Any] = {}
        self._resp_cond = threading.Condition(self._lock)
        
        # 机械臂配置
        self.settings = SettingsManager()
        self.left_arm_name = self.settings.left_arm_name
        self.right_arm_name = self.settings.right_arm_name
        self.push_interval = self.settings.arm_push_interval
        
        # 订阅状态
        self._subscribed = False
        
        # 轮询相关
        self._polling_stop: Optional[threading.Event] = None
        self._polling_thread: Optional[threading.Thread] = None
    
    def connect(self) -> bool:
        """建立连接并等待握手成功（最多 10s）"""
        print(f"[Arm WS] 正在连接 {self.url} ...")
        try:
            self.ws = websocket.WebSocketApp(
                self.url,
                on_open=self._on_open,
                on_message=self._on_message,
                on_error=self._on_error,
                on_close=self._on_close
            )

            self._stop_event.clear()
            self._thread = threading.Thread(target=self._run, daemon=True)
            self._thread.start()

            # 等待连接成功
            timeout = 10.0
            start = time.time()
            while time.time() - start < timeout:
                if self.connected:
                    print(f"[Arm WS] 连接已建立，等待稳定后订阅推送...")
                    time.sleep(0.5)  # 等待连接稳定
                    self._subscribe_all()
                    return True
                time.sleep(0.1)

            print(f"[Arm WS] 连接超时 ({timeout}s)，仍未 connected=True")
            return False

        except Exception as e:
            print(f"[Arm WS] 连接失败: {e}")
            return False
    
    def disconnect(self):
        """断开连接"""
        self._stop_event.set()
        if self._polling_stop:
            self._polling_stop.set()
        if self.ws:
            try:
                self.ws.close()
            except:
                pass
        self.connected = False
        self._subscribed = False
        with self._resp_cond:
            self._pending_requests.clear()
            self._resp_cond.notify_all()
    
    def _run(self):
        """WebSocket 运行循环"""
        try:
            self.ws.run_forever(ping_interval=30, ping_timeout=10)
        except Exception as e:
            print(f"[Arm WS] 运行异常: {e}")
        finally:
            self.connected = False
    
    def _on_open(self, ws):
        """连接打开回调"""
        print(f"[Arm WS] 连接已打开")
        self.connected = True
    
    def _on_message(self, ws, message):
        """收到消息回调"""
        try:
            data = json.loads(message)
            msg_id = data.get("id")
            msg_type = data.get("ty")

            if arm_debug_push():
                if not hasattr(self, '_msg_total'):
                    self._msg_total = 0
                self._msg_total += 1
                print(f"[Arm WS] 收到第 {self._msg_total} 条: msg_id={msg_id}, msg_type={msg_type!r}")

            # 处理响应
            if msg_id and msg_id in self._pending_requests:
                with self._resp_cond:
                    self._pending_requests[msg_id]["data"] = data
                    self._pending_requests[msg_id]["event"].set()
                    self._resp_cond.notify_all()
            # 订阅 ack / 调用响应
            elif msg_id and not (msg_type and "publish" in msg_type):
                if "err" in data:
                    err = data.get("err", {})
                    msg = err.get("message", "未知错误") if isinstance(err, dict) else str(err)
                    print(f"[Arm WS] 响应错误: id={msg_id}, ty={msg_type!r}, err={msg}")
            # 处理推送
            elif msg_type and "publish" in msg_type:
                self._handle_push(msg_type, data)

        except json.JSONDecodeError:
            print(f"[Arm WS] 解析消息失败: {message}")
        except Exception as e:
            print(f"[Arm WS] 处理消息异常: {e}")
    
    def _on_error(self, ws, error):
        """错误回调"""
        print(f"[Arm WS] 错误: {error}")
        self.connected = False
    
    def _on_close(self, ws, close_status_code, close_msg):
        """关闭回调"""
        print(f"[Arm WS] 连接已关闭: {close_status_code} {close_msg}")
        if self.connected:
            self.connected = False
            with self._resp_cond:
                self._pending_requests.clear()
                self._resp_cond.notify_all()
    
    def _handle_push(self, msg_type: str, data: dict):
        """处理推送消息"""
        # 提取实际的 topic 类型
        topic = msg_type.replace("publish/", "")
        db = data.get("db", {})

        if arm_debug_push():
            print(f"[Arm WS] 收到推送: topic={topic!r}, keys={list(db.keys()) if isinstance(db, dict) else type(db).__name__}")

        # 调用注册的回调
        if topic in self._callbacks:
            self._callbacks[topic](db)
        elif "robot" in topic and "status" in topic:
            # 机械臂状态推送
            if self.left_arm_name in topic:
                cb = self._callbacks.get(f"robot/{self.left_arm_name}/status") or self._callbacks.get("left_arm_status")
                if cb:
                    cb(db)
            elif self.right_arm_name in topic:
                cb = self._callbacks.get(f"robot/{self.right_arm_name}/status") or self._callbacks.get("right_arm_status")
                if cb:
                    cb(db)
        elif "project/status" in topic:
            cb = self._callbacks.get("project/status") or self._callbacks.get("project_status")
            if cb:
                cb(db)
        else:
            if arm_debug_push():
                print(f"[Arm WS] 收到推送但未匹配: topic={topic!r}")
    
    def _send(self, msg_type: str, params: dict = None) -> int:
        """发送消息，返回消息ID"""
        if not self.connected:
            return -1

        with self._lock:
            self._id = (self._id + 1) % 100000
            msg_id = self._id

        message = {
            "id": msg_id,
            "ty": msg_type,
            "db": params if params is not None else {}
        }

        try:
            self.ws.send(json.dumps(message))
            if arm_debug_push():
                print(f"[Arm WS] 发送: {message}")
            return msg_id
        except Exception as e:
            print(f"[Arm WS] 发送失败: {e}")
            return -1

    def _send_top_level(self, msg_type: str, **fields) -> int:
        """
        发送订阅类消息，字段放在报文顶层（不在 db 内）
        
        与 C++ ArmWebSocket::sendTopLevel() 对齐
        
        用于 robotState/sub / projectState/sub / liftingColumnState/sub 等订阅类动作。
        文档规定：订阅字段（id / topics / tc / off / ip / port）在报文顶层。
        """
        if not self.connected:
            return -1

        with self._lock:
            self._id = (self._id + 1) % 100000
            msg_id = self._id

        message = {"id": msg_id, "ty": msg_type}
        message.update(fields)

        try:
            self.ws.send(json.dumps(message))
            if arm_debug_push():
                print(f"[Arm WS] 订阅发送: {message}")
            return msg_id
        except Exception as e:
            print(f"[Arm WS] 订阅发送失败: {e}")
            return -1
    
    def send_and_wait(self, msg_type: str, params: dict = None, timeout: float = 5.0) -> Optional[dict]:
        """
        发送消息并等待响应
        
        与 C++ ArmWebSocket::sendAndWait() 对齐
        """
        if not self.connected:
            return None
        
        msg_id = self._send(msg_type, params)
        if msg_id < 0:
            return None
        
        # 等待响应
        event = threading.Event()
        with self._resp_cond:
            self._pending_requests[msg_id] = {"event": event, "data": None}
        
        if event.wait(timeout):
            with self._resp_cond:
                return self._pending_requests.get(msg_id, {}).get("data")
        else:
            # 超时
            with self._resp_cond:
                self._pending_requests.pop(msg_id, None)
            return None
    
    def send_and_wait_raw(self, msg_type: str, json_raw: str, timeout: float = 5.0) -> Optional[dict]:
        """
        发送原始 JSON 并等待响应
        
        与 C++ ArmWebSocket::sendAndWaitRaw() 对齐
        """
        if not self.connected:
            return None
        
        with self._lock:
            self._id = (self._id + 1) % 100000
            msg_id = self._id
        
        try:
            db_data = json.loads(json_raw) if json_raw.strip() else {}
        except:
            db_data = {}
        
        message = {
            "id": msg_id,
            "ty": msg_type,
            "db": db_data if isinstance(db_data, dict) else {}
        }
        
        # 如果是数组，也放入 db
        if isinstance(db_data, list):
            message["db"] = db_data
        
        try:
            self.ws.send(json.dumps(message))
        except Exception as e:
            print(f"[Arm WS] send_and_wait_raw 发送失败: {e}")
            return None
        
        event = threading.Event()
        with self._resp_cond:
            self._pending_requests[msg_id] = {"event": event, "data": None}
        
        if event.wait(timeout):
            with self._resp_cond:
                return self._pending_requests.get(msg_id, {}).get("data")
        else:
            with self._resp_cond:
                self._pending_requests.pop(msg_id, None)
            return None

    def register_callback(self, topic: str, callback: Callable):
        """注册推送回调"""
        self._callbacks[topic] = callback
        if arm_debug_push():
            print(f"[Arm WS] 注册回调: topic={topic!r}")
    
    def unregister_callback(self, topic: str):
        """取消注册回调"""
        self._callbacks.pop(topic, None)
    
    # ========== 机械臂使能控制 ==========

    def _set_arm_enabled(self, arm_name: str, enable: bool, timeout: float = 5.0) -> Tuple[bool, str]:
        """
        上使能 / 下使能 通用实现
        
        与 C++ ArmWebSocket::setArmEnabled() 对齐
        
        返回: (ok: bool, err_str: str)
        """
        if not arm_name:
            return False, "缺少 arm_name"
        if not self.connected:
            return False, "WebSocket 未连接"

        ty = "robot/enable" if enable else "robot/disable"
        upper_or_lower = "上" if enable else "下"
        msg = f"{upper_or_lower}使能({arm_name})"

        result = self.send_and_wait(ty, {"name": arm_name}, timeout=timeout)
        if not result:
            return False, f"{msg} 超时（{timeout:.0f}s）"

        if result.get("err"):
            err = result["err"]
            err_msg = err.get('message', '未知错误') if isinstance(err, dict) else str(err)
            return False, f"{msg} 被拒：{err_msg}"

        if result.get("db") is True:
            return True, ""
        if result.get("db") is False:
            return False, f"{msg} 失败（db=false）"

        return False, f"{msg} 响应格式异常"

    def enable_arm(self, arm_name: str = None) -> bool:
        """上使能（向后兼容）"""
        if arm_name is None:
            arm_name = self.left_arm_name
        ok, _ = self._set_arm_enabled(arm_name, enable=True)
        return ok

    def disable_arm(self, arm_name: str = None) -> bool:
        """下使能（向后兼容）"""
        if arm_name is None:
            arm_name = self.left_arm_name
        ok, _ = self._set_arm_enabled(arm_name, enable=False)
        return ok

    def enable_arm_ex(self, arm_name: str) -> str:
        """上使能（返回错误字符串）"""
        ok, err = self._set_arm_enabled(arm_name, enable=True)
        return err if not ok else ""

    def disable_arm_ex(self, arm_name: str) -> str:
        """下使能（返回错误字符串）"""
        ok, err = self._set_arm_enabled(arm_name, enable=False)
        return err if not ok else ""

    # ========== 单臂力矩拖动 ==========
    
    _DRAG_TIMEOUT_S = 8.0  # 模式切到位可能较慢
    _DRAG_STATUS_TIMEOUT_S = 2.5

    def drag_arm(self, arm_name: str) -> Tuple[bool, str]:
        """
        同步力矩拖动
        
        与 C++ ArmWebSocket::dragArm() 对齐
        """
        if not arm_name:
            return False, "缺少 arm_name"
        if not self.connected:
            return False, "WebSocket 未连接"

        result = self.send_and_wait(
            "robot/drag", {"name": arm_name}, timeout=self._DRAG_TIMEOUT_S,
        )
        if not result:
            return False, f"进入拖动({arm_name}) 超时（{self._DRAG_TIMEOUT_S:.0f}s）"
        if result.get("err"):
            err = result["err"]
            err_msg = err.get('message', '未知错误') if isinstance(err, dict) else str(err)
            return False, f"robot/drag({arm_name}) 被拒：{err_msg}"
        if result.get("db") is True:
            return True, ""
        return False, f"robot/drag({arm_name}) 响应异常"

    def drag_arm_async(self, arm_name: str, on_result: Callable[[bool, str], None] = None):
        """
        异步进入拖动
        
        与 C++ ArmWebSocket::dragArmAsync() 对齐
        """
        def _worker():
            ok, err = self.drag_arm(arm_name)
            if on_result is not None:
                try:
                    on_result(ok, err)
                except Exception as e:
                    print(f"[robot/drag] 回调异常: {e}")

        threading.Thread(target=_worker, daemon=True).start()

    @staticmethod
    def is_drag_mode(mode: str) -> bool:
        """
        判断 mode 是不是拖动模式
        
        与 C++ ArmWebSocket::isDragMode() 对齐
        """
        return mode in ("CSTDrag", "ServoDrag")
    
    # ========== 速度控制 ==========
    
    def set_speed_override(self, ratio: float) -> bool:
        """设置速度倍率"""
        ratio = max(0.0, min(1.0, ratio))
        result = self.send_and_wait("device/setSpeedOverride", {"ratio": ratio})
        return result is not None and result.get("db") is True
    
    def get_speed_override(self) -> Optional[float]:
        """获取速度倍率"""
        result = self.send_and_wait("device/getSpeedOverride", {})
        if result and "db" in result:
            return result["db"].get("ratio", 1.0)
        return None

    # ========== 清除伺服错误 ==========
    
    def clear_servo_error(self, arm_name: str) -> bool:
        """
        清除伺服错误
        
        与 C++ ArmWebSocket::clearServoError() 对齐
        """
        if not arm_name:
            print("[Arm WS] clear_servo_error 缺少 arm_name")
            return False
        result = self.send_and_wait(
            "robot/clearServoError", {"name": arm_name}, timeout=3.0
        )
        if not result:
            print(f"[Arm WS] clear_servo_error({arm_name}) 无响应")
            return False
        if result.get("err"):
            err = result["err"]
            print(f"[Arm WS] clear_servo_error({arm_name}) 失败: {err}")
            return False
        return result.get("db") is True

    def clear_all_servo_errors(self) -> dict:
        """清左右两条臂的伺服错误"""
        results = {}
        for arm in [self.left_arm_name, self.right_arm_name]:
            if arm:
                results[arm] = self.clear_servo_error(arm)
        return results
    
    # ========== 工程控制 ==========
    
    def list_projects(self) -> list:
        """获取工程列表"""
        result = self.send_and_wait("project/listProjects", {})
        if result and "db" in result:
            return result["db"]
        return []
    
    def run_project(self, project_name: str) -> bool:
        """运行工程"""
        result = self.send_and_wait("project/run", {"name": project_name})
        return result is not None and result.get("db") is True
    
    def pause_project(self) -> bool:
        """暂停工程"""
        result = self.send_and_wait("project/pause", {})
        return result is not None and result.get("db") is True
    
    def resume_project(self) -> bool:
        """恢复工程"""
        result = self.send_and_wait("project/resume", {})
        return result is not None and result.get("db") is True
    
    def stop_project(self) -> bool:
        """停止工程"""
        result = self.send_and_wait("project/stop", {})
        return result is not None and result.get("db") is True

    # ========== 订阅 ==========
    
    def _subscribe_all(self):
        """
        订阅所有推送
        
        与 C++ ArmWebSocket::subscribeAll() 对齐
        """
        if not self.connected:
            return
        
        self._subscribed = True
        tc_ms = int(self.push_interval)
        
        print(
            f"[Arm WS] 订阅推送: project/status, "
            f"robot/{self.left_arm_name}/status, "
            f"robot/{self.right_arm_name}/status, "
            f"interval={tc_ms}ms"
        )

        # 工程状态订阅
        self._send_top_level(
            "projectState/sub",
            topics=["project/status"],
            tc=tc_ms,
        )

        # 机械臂状态订阅
        self._send_top_level(
            "robotState/sub",
            topics=f"robot/{self.left_arm_name}/status",
            tc=tc_ms,
        )
        self._send_top_level(
            "robotState/sub",
            topics=f"robot/{self.right_arm_name}/status",
            tc=tc_ms,
        )

        # 主动拉一次首帧
        self._fetch_initial_status()
        
        # 兜底轮询
        self._start_polling()

    def _fetch_initial_status(self):
        """
        主动拉一次首帧
        
        与 C++ ArmWebSocket::fetchInitialStatus() 对齐
        """
        # 工程状态首帧
        try:
            result = self.send_and_wait("project/status", {}, timeout=2.0)
            if result and isinstance(result, dict) and result.get("db"):
                db = result["db"]
                cb = self._callbacks.get("project/status") or self._callbacks.get("project_status")
                if cb:
                    try:
                        cb(db)
                    except Exception as e:
                        print(f"[Arm] project/status 首帧回调异常: {e}")
        except Exception as e:
            print(f"[Arm] 拉取项目首帧异常: {e}")

        # 左臂首帧
        try:
            result = self.send_and_wait("robot/status", {"name": self.left_arm_name}, timeout=self._DRAG_STATUS_TIMEOUT_S)
            if result and isinstance(result, dict) and result.get("db"):
                db = result["db"]
                cb = self._callbacks.get(f"robot/{self.left_arm_name}/status") or self._callbacks.get("left_arm_status")
                if cb:
                    try:
                        cb(db)
                    except Exception as e:
                        print(f"[Arm WS] left_arm_status 首帧回调异常: {e}")
        except Exception as e:
            print(f"[Arm WS] 拉取左臂首帧异常: {e}")

        # 右臂首帧
        try:
            result = self.send_and_wait("robot/status", {"name": self.right_arm_name}, timeout=self._DRAG_STATUS_TIMEOUT_S)
            if result and isinstance(result, dict) and result.get("db"):
                db = result["db"]
                cb = self._callbacks.get(f"robot/{self.right_arm_name}/status") or self._callbacks.get("right_arm_status")
                if cb:
                    try:
                        cb(db)
                    except Exception as e:
                        print(f"[Arm WS] right_arm_status 首帧回调异常: {e}")
        except Exception as e:
            print(f"[Arm WS] 拉取右臂首帧异常: {e}")

    def _start_polling(self):
        """启动兜底轮询（1s/次）"""
        if self._polling_thread and self._polling_thread.is_alive():
            return

        self._polling_stop = threading.Event()

        def _poll_loop():
            # 第一次延迟 1s 启动，让订阅先完成
            if self._polling_stop.wait(1.0):
                return
            while not self._polling_stop.is_set():
                try:
                    self._poll_once()
                except Exception as e:
                    print(f"[Arm WS] 轮询异常: {e}")
                if self._polling_stop.wait(1.0):
                    return

        self._polling_thread = threading.Thread(target=_poll_loop, daemon=True)
        self._polling_thread.start()
        print(f"[Arm WS] 状态轮询已启动 (1s/次)")

    def _poll_once(self):
        """执行一次状态轮询"""
        # 工程状态
        try:
            result = self.send_and_wait("project/status", {}, timeout=2.0)
            if result and isinstance(result, dict) and result.get("db"):
                db = result["db"]
                if isinstance(db, dict) and db:
                    cb = self._callbacks.get("project/status") or self._callbacks.get("project_status")
                    if cb:
                        try:
                            cb(db)
                        except Exception as e:
                            print(f"[Arm] project/status 轮询回调异常: {e}")
        except Exception as e:
            print(f"[Arm] 轮询 project/status 异常: {e}")

        # 左臂状态
        result = self.send_and_wait("robot/status", {"name": self.left_arm_name}, timeout=2.0)
        if result and isinstance(result, dict) and result.get("db"):
            db = result["db"]
            if isinstance(db, dict) and db:
                cb = self._callbacks.get(f"robot/{self.left_arm_name}/status") or self._callbacks.get("left_arm_status")
                if cb:
                    try:
                        cb(db)
                    except Exception as e:
                        print(f"[Arm WS] left_arm_status 轮询回调异常: {e}")

        # 右臂状态
        result = self.send_and_wait("robot/status", {"name": self.right_arm_name}, timeout=2.0)
        if result and isinstance(result, dict) and result.get("db"):
            db = result["db"]
            if isinstance(db, dict) and db:
                cb = self._callbacks.get(f"robot/{self.right_arm_name}/status") or self._callbacks.get("right_arm_status")
                if cb:
                    try:
                        cb(db)
                    except Exception as e:
                        print(f"[Arm WS] right_arm_status 轮询回调异常: {e}")

    def unsubscribe_all(self):
        """退订所有推送"""
        if not self.connected:
            return
        
        # 工程状态退订
        self._send_top_level(
            "projectState/sub",
            topics="project/status",
            off=1,
        )
        # 机械臂状态退订
        self._send_top_level(
            "robotState/sub",
            topics=f"robot/{self.left_arm_name}/status",
            off=1,
        )
        self._send_top_level(
            "robotState/sub",
            topics=f"robot/{self.right_arm_name}/status",
            off=1,
        )
        
        self._subscribed = False
        
        # 停止轮询
        if self._polling_stop:
            self._polling_stop.set()
        if self._polling_thread:
            self._polling_thread.join(timeout=2.0)
        self._polling_thread = None
        self._polling_stop = None

    def set_arm_names(self, left: str, right: str):
        """设置臂名"""
        self.left_arm_name = left
        self.right_arm_name = right

    @property
    def is_subscribed(self) -> bool:
        return self._subscribed


# ===========================================================================
# 连接管理器
# ===========================================================================

class ArmConnectionManager:
    """机械臂连接管理器"""
    
    def __init__(self, ip: str, port: int):
        self.client = ArmWebSocketClient(ip, port)
        self._connected = False
    
    def connect(self) -> bool:
        """连接"""
        if self.client.connect():
            self._connected = True
            return True
        self._connected = False
        return False
    
    def disconnect(self):
        """断开连接"""
        self.client.disconnect()
        self._connected = False
    
    def set_state_callback(self, topic: str, callback: Callable):
        """设置状态回调"""
        self.client.register_callback(topic, callback)
    
    def enable_arm(self, arm_name: str = None) -> bool:
        return self.client.enable_arm(arm_name)

    def disable_arm(self, arm_name: str = None) -> bool:
        return self.client.disable_arm(arm_name)

    def enable_arm_ex(self, arm_name: str) -> str:
        return self.client.enable_arm_ex(arm_name)

    def disable_arm_ex(self, arm_name: str) -> str:
        return self.client.disable_arm_ex(arm_name)

    def drag_arm(self, arm_name: str) -> Tuple[bool, str]:
        return self.client.drag_arm(arm_name)

    def drag_arm_async(self, arm_name: str,
                       on_result: Callable[[bool, str], None] = None):
        return self.client.drag_arm_async(arm_name, on_result)

    @staticmethod
    def is_drag_mode(mode: str) -> bool:
        return ArmWebSocketClient.is_drag_mode(mode)

    def set_speed_override(self, ratio: float) -> bool:
        return self.client.set_speed_override(ratio)

    def get_speed_override(self) -> Optional[float]:
        return self.client.get_speed_override()

    def clear_servo_error(self, arm_name: str) -> bool:
        return self.client.clear_servo_error(arm_name)

    def clear_all_servo_errors(self) -> dict:
        return self.client.clear_all_servo_errors()

    def list_projects(self) -> list:
        return self.client.list_projects()
    
    def run_project(self, project_name: str) -> bool:
        return self.client.run_project(project_name)
    
    def pause_project(self) -> bool:
        return self.client.pause_project()
    
    def resume_project(self) -> bool:
        return self.client.resume_project()

    def stop_project(self) -> bool:
        return self.client.stop_project()
    
    @property
    def connected(self) -> bool:
        return self._connected

    # ========== 升降柱（liftingColumn） ==========
    def get_lifting_column(self):
        """取得升降柱客户端（懒加载）"""
        mgr = getattr(self, "_lifting_column", None)
        if mgr is None:
            from communication.lifting_column import LiftingColumnClient
            mgr = LiftingColumnClient(self.client)
            self._lifting_column = mgr
        return mgr
    
    # ========== Revo2 灵巧手 ==========
    def get_revo2_hand(self):
        """取得灵巧手管理器（懒加载）"""
        mgr = getattr(self, "_revo2_hand", None)
        if mgr is None:
            from communication.revo2_hand import DualHandManager
            mgr = DualHandManager(self.client)
            self._revo2_hand = mgr
        return mgr
