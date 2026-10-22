"""
AGV TCP 通信客户端

与 C++ communication/agvtcpclient.h/cpp 对齐

支持多连接：推送、控制、导航、状态查询等
端口分配：
    pushback 19301 / control 19205 / nav 19206 /
    status 19204 / other 19210 / config 19207

设计要点：
    - 同步 send() 用 Condition + wait/notify 不会永久阻塞；disconnect() 会 notifyAll 唤醒所有等待线程
    - 接收循环做帧同步：buffer 开头非 0x5A 时丢弃到下一个同步头
    - seq 从 1 开始递增（避免 0 被某些协议视为保留值）
    - 推送端口的状态做字段合并（AGV 推送分多种类型，只有部分字段）
"""
import socket
import threading
import json
import time
import os
from typing import Callable, Optional, Dict, Any, Tuple, List

from communication.agv_protocol import AGVProtocol, AGVCommand, command_name


def agv_debug_dump() -> bool:
    """调试开关（与 C++ agvDebugDump() 一致）"""
    return os.environ.get("AGV_DEBUG_DUMP") == "1"


class AGVTCPClientBase:
    """AGV TCP 客户端基类"""
    
    def __init__(self, ip: str, port: int, name: str = ""):
        self.ip = ip
        self.port = port
        self.name = name
        self.socket: Optional[socket.socket] = None
        self.connected = False
        self.recv_thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._seq = 1  # seq 从 1 开始
        self._lock = threading.Lock()
        self._resp_cond = threading.Condition(self._lock)
        self._callbacks = {}
        self._pending_requests: Dict[int, Any] = {}
        self._send_timeout = 3.0  # 秒
        self._rx_count = 0

    def connect(self) -> bool:
        """建立连接"""
        sock = None
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5.0)
            sock.connect((self.ip, self.port))
            self.socket = sock
            self.connected = True
            self._stop_event.clear()
            self.recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
            self.recv_thread.start()
            print(f"[{self.name}] 连接成功，recv_thread 已启动 (daemon={self.recv_thread.daemon})")
            return True
        except Exception as e:
            print(f"[{self.name}] 连接失败: {e}")
            self.connected = False
            if sock is not None:
                try:
                    sock.close()
                except Exception:
                    pass
            self.socket = None
            return False

    def disconnect(self):
        """断开连接"""
        self._stop_event.set()
        # 唤醒所有在条件变量上等待的 send 线程
        with self._resp_cond:
            self._pending_requests.clear()
            self._resp_cond.notify_all()
        sock = self.socket
        self.socket = None
        self.connected = False
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass

    def _dump_tx(self, frame: bytes, cmd_id: int):
        """打印下行报文（调试用）"""
        sync = frame[0]
        ver = frame[1]
        seq = int.from_bytes(frame[2:4], 'big')
        dlen = int.from_bytes(frame[4:8], 'big')
        json_body = frame[16:16+dlen].decode('utf-8', 'replace') if dlen > 0 else ""
        print(f"\n===== [{self.name}] → AGV 下行报文 =====")
        print(f"  同步头:   0x{sync:02X}")
        print(f"  协议版本: {ver}")
        print(f"  序号:     {seq}")
        print(f"  数据长度: {dlen} 字节")
        print(f"  命令号:   {cmd_id} (0x{cmd_id:04X})")
        print(f"  JSON体:   {json_body}")
        print(f"==============================\n")

    def send_sync_raw(self, cmd_id: int, json_body: bytes) -> Optional[dict]:
        """
        同步发送并等待响应（与 C++ AgvTcpClientBase::sendSyncRaw 对齐）
        
        Args:
            cmd_id: 命令号
            json_body: JSON 原始字节
            
        Returns:
            响应数据或 None
        """
        if not self.connected or self.socket is None:
            return None

        with self._resp_cond:
            self._seq = (self._seq + 1) & 0xFFFF
            seq = self._seq
            self._pending_requests[seq] = None

        frame = AGVProtocol.build_frame(cmd_id, json_body, seq)
        
        if agv_debug_dump():
            self._dump_tx(frame, cmd_id)

        sock = self.socket
        if sock is None:
            return None

        try:
            sock.sendall(frame)
        except Exception as e:
            print(f"[{self.name}] 发送失败: {e}")
            with self._resp_cond:
                self._pending_requests.pop(seq, None)
            return None

        # 等待响应
        with self._resp_cond:
            deadline = time.time() + self._send_timeout
            while self._pending_requests.get(seq) is None:
                remaining = deadline - time.time()
                if remaining <= 0:
                    self._pending_requests.pop(seq, None)
                    print(f"[{self.name}] 命令 {cmd_id} 等待响应超时")
                    return None
                # 断开时也要退出
                if not self.connected:
                    self._pending_requests.pop(seq, None)
                    return None
                self._resp_cond.wait(timeout=remaining)

            result = self._pending_requests.pop(seq, None)
            return result

    def send_sync(self, cmd_id: int, obj: Optional[dict] = None) -> Optional[dict]:
        """
        同步发送调用式报文（与 C++ AgvTcpClientBase::sendSync 对齐）
        
        Args:
            cmd_id: 命令号
            obj: JSON 对象（None 表示空 body）
        """
        if obj is None:
            # Seer 协议对无参命令要求空 JSON 体
            return self.send_sync_raw(cmd_id, b"")
        return self.send_sync_raw(cmd_id, json.dumps(obj).encode('utf-8'))

    def send_no_wait(self, cmd_id: int, obj: Optional[dict] = None) -> bool:
        """
        不等待响应直接写 socket（与 C++ AgvTcpClientBase::sendNoWait 对齐）
        用于高频运动控制
        
        Returns:
            True if 发送成功
        """
        if not self.connected or self.socket is None:
            return False

        with self._resp_cond:
            self._seq = (self._seq + 1) & 0xFFFF
            seq = self._seq
            self._pending_requests[seq] = None

        if obj is None:
            frame = AGVProtocol.build_frame(cmd_id, b"", seq)
        else:
            frame = AGVProtocol.build_frame(cmd_id, json.dumps(obj).encode('utf-8'), seq)
        
        sock = self.socket
        if sock is None:
            return False
        try:
            sock.sendall(frame)
            return True
        except Exception as e:
            with self._resp_cond:
                self._pending_requests.pop(seq, None)
            print(f"[{self.name}] send_no_wait 发送失败: {e}")
            return False

    def send_async(self, cmd_id: int, obj: Optional[dict] = None,
                   callback: Callable = None):
        """异步发送命令（不阻塞调用者）"""
        thread = threading.Thread(
            target=self._send_async_task,
            args=(cmd_id, obj, callback),
            daemon=True
        )
        thread.start()

    def _send_async_task(self, cmd_id: int, obj: Optional[dict], callback: Callable):
        try:
            result = self.send_sync(cmd_id, obj)
            if callback:
                callback(result)
        except Exception as e:
            print(f"[{self.name}] 异步发送任务异常: {e}")

    def _recv_loop(self):
        """接收数据循环（与 C++ AgvTcpClientBase::processBuffer 对齐）"""
        buffer = b""
        while not self._stop_event.is_set():
            sock = self.socket
            if sock is None:
                print(f"[{self.name}] socket 为空，退出接收循环")
                break
            try:
                sock.settimeout(0.1)
                data = sock.recv(4096)
                self._rx_count += 1
                if self._rx_count == 1:
                    print(f"[{self.name}] recv_loop 第1次 recv, len={len(data)}, data_hex={data[:100].hex() if data else 'empty'}")
                if not data:
                    print(f"[{self.name}] recv 返回空，连接被远端关闭")
                    break
                buffer += data

                # 帧同步：buffer 开头不是 0x5A 时，找下一个 0x5A 重新对齐
                if buffer and buffer[0] != 0x5A:
                    try:
                        idx = buffer.index(b'\x5A', 1)
                        if idx > 0:
                            print(f"[{self.name}] 帧未对齐，丢弃 {idx} 字节")
                            buffer = buffer[idx:]
                        else:
                            buffer = b""
                            continue
                    except ValueError:
                        buffer = b""
                        continue

                # 解析帧
                while True:
                    frame = AGVProtocol.parse_frame(buffer)
                    if frame is None:
                        if buffer and len(buffer) < 200:
                            print(f"[{self.name}] parse_frame 返回 None, buffer_hex={buffer.hex()}")
                        break

                    command, seq, json_data, total_size = frame
                    buffer = buffer[total_size:]

                    # 上行报文诊断（前 5 条 + 每 50 条）
                    if self._rx_count <= 5 or self._rx_count % 50 == 1:
                        print(f"[{self.name}] ← AGV 上行报文 (第{self._rx_count}条) cmd={command} seq={seq} json={json_data[:300]}")

                    # 处理响应
                    try:
                        payload = json.loads(json_data) if json_data.strip() else {}
                    except Exception:
                        payload = json_data

                    with self._resp_cond:
                        if seq in self._pending_requests:
                            self._pending_requests[seq] = payload
                            self._resp_cond.notify_all()
                        else:
                            # 未匹配 → 推送处理
                            self._handle_push(command, json_data)

            except socket.timeout:
                continue
            except OSError as e:
                if e.errno in (9, 104):
                    break
                print(f"[{self.name}] 接收 OSError: {e}")
                break
            except Exception as e:
                print(f"[{self.name}] 接收异常: {e}")
                break

        self.connected = False
        with self._resp_cond:
            self._resp_cond.notify_all()

    def _handle_push(self, command: int, json_data: str):
        """处理推送数据 - 子类实现"""
        pass

    def register_callback(self, command: int, callback: Callable):
        """注册命令回调"""
        self._callbacks[command] = callback


class AGVPushbackClient(AGVTCPClientBase):
    """
    状态推送接收客户端（端口 19301）
    
    与 C++ AgvPushbackClient 对齐
    
    关键：AGV 推送分多种类型（DI 局部 / 位置+电池 / 错误…），
    必须做字段合并——收到部分数据时不清空已知的字段。
    """
    
    def __init__(self, ip: str, port: int):
        super().__init__(ip, port, "Pushback")
        self._state_callback: Optional[Callable] = None
        self._last_state: dict = {}
        self._push_count = 0

    def set_state_callback(self, callback: Callable):
        """设置状态回调"""
        self._state_callback = callback

    def _handle_push(self, command: int, json_data: str):
        """处理状态推送（与 C++ AgvPushbackClient::handlePush 对齐）"""
        try:
            data = json.loads(json_data) if json_data.strip() else {}
            self._push_count += 1

            # 关键：合并到 _last_state，保留之前已知的字段
            if isinstance(data, dict):
                self._last_state.update(data)

            if self._state_callback:
                self._state_callback(self._last_state)
        except Exception as e:
            print(f"[Pushback] 解析推送数据失败: {e}")


class AGVControlClient(AGVTCPClientBase):
    """
    控制客户端（端口 19205）
    
    与 C++ AgvControlClient 对齐
    
    持续运动：每 100ms 重发 2010（duration=200），实现"按住一直动、松开即停"。
    """
    
    def __init__(self, ip: str, port: int):
        super().__init__(ip, port, "Control")
        self._send_timeout = 1.5  # 控制类命令超时短
        self._continuous_vx = 0.0
        self._continuous_vy = 0.0
        self._continuous_w = 0.0
        self._continuous_timer: Optional[threading.Thread] = None

    def start_continuous_move(self, vx: float, vy: float, w: float):
        """
        启动持续运动（与 C++ AgvControlClient::startContinuousMove 对齐）
        后台线程每 100ms 重新发送指令
        """
        self._stop_continuous_move_timer()
        self._continuous_vx = vx
        self._continuous_vy = vy
        self._continuous_w = w
        if vx != 0.0 or vy != 0.0 or w != 0.0:
            self._continuous_timer = threading.Thread(
                target=self._continuous_move_task, daemon=True
            )
            self._continuous_timer.start()

    def _stop_continuous_move_timer(self):
        """停止持续运动定时器"""
        self._continuous_vx = 0.0
        self._continuous_vy = 0.0
        self._continuous_w = 0.0

    def stop_continuous_move(self):
        """
        停止持续运动（与 C++ AgvControlClient::stopContinuousMove 对齐）
        异步发停止指令，不阻塞
        """
        self._stop_continuous_move_timer()
        threading.Thread(target=lambda: self.send_sync(AGVCommand.TEACH_STOP), daemon=True).start()

    def _continuous_move_task(self):
        """
        持续运动任务（与 C++ AgvControlClient::onContinuousTick 对齐）
        关键：使用 send_no_wait 不等待响应，否则会导致 AGV 走起来"一抖一抖"
        """
        while self.connected and (
            self._continuous_vx != 0 or
            self._continuous_vy != 0 or
            self._continuous_w != 0
        ):
            try:
                params = {
                    "vx": self._continuous_vx,
                    "vy": self._continuous_vy,
                    "w": self._continuous_w,
                    "duration": 200  # 200ms 后过期，需要重发
                }
                self.send_no_wait(AGVCommand.TEACH_MOVE, params)
            except Exception as e:
                print(f"[Control] 持续运动出错: {e}")
                break
            time.sleep(0.1)  # 100ms 一次

    def teach_move_once(self, vx: float, vy: float, w: float, duration_ms: int = 100):
        """
        单次开环运动（与 C++ AgvControlClient::teachMoveOnce 对齐）
        duration 不能为 0，给最小值 100ms
        """
        if duration_ms <= 0:
            duration_ms = 100
        params = {"vx": vx, "vy": vy, "w": w, "duration": duration_ms}
        self.send_no_wait(AGVCommand.TEACH_MOVE, params)

    def set_soft_emergency(self, status: bool) -> Optional[dict]:
        """软急停（与 C++ AgvControlClient::setSoftEmergency 对齐）"""
        params = {"status": status}
        return self.send_sync(AGVCommand.SET_SOFT_EMERGENCY, params)


class AGVNavigationClient(AGVTCPClientBase):
    """导航客户端（端口 19206，与 C++ AgvNavigationClient 对齐）"""
    
    def __init__(self, ip: str, port: int):
        super().__init__(ip, port, "Navigation")

    def go_target_point(self, station_id: str, method: str = "forward",
                        max_speed: float = 1.0, max_wspeed: float = 0.5) -> Optional[dict]:
        """开始导航到目标点（与 C++ AgvNavigationClient::goTargetPoint 对齐）"""
        params = {
            "id": station_id,
            "method": method,
            "max_speed": max_speed,
            "max_wspeed": max_wspeed
        }
        return self.send_sync(AGVCommand.GO_TARGET_POINT, params)

    def pause_navigation(self) -> Optional[dict]:
        """暂停导航"""
        return self.send_sync(AGVCommand.PAUSE_NAVIGATION)

    def continue_navigation(self) -> Optional[dict]:
        """继续导航"""
        return self.send_sync(AGVCommand.CONTINUE_NAVIGATION)

    def cancel_navigation(self) -> Optional[dict]:
        """取消导航"""
        return self.send_sync(AGVCommand.CANCEL_NAVIGATION)


class AGVStatusClient(AGVTCPClientBase):
    """状态查询客户端（端口 19204，与 C++ AgvStatusClient 对齐）"""
    
    def __init__(self, ip: str, port: int):
        super().__init__(ip, port, "Status")

    def get_agv_location(self) -> Optional[dict]:
        """获取AGV位置"""
        return self.send_sync(AGVCommand.GET_AGV_LOCATION)

    def get_laser_data(self) -> Optional[dict]:
        """获取激光数据"""
        return self.send_sync(AGVCommand.GET_LASER_DATA)

    def get_navigation_status(self, simple: bool = True) -> Optional[dict]:
        """获取导航状态"""
        params = {"simple": simple}
        return self.send_sync(AGVCommand.GET_NAVIGATION_STATUS, params)

    def get_remote_maps(self) -> Optional[dict]:
        """获取地图列表（与 C++ AgvStatusClient::getRemoteMaps 对齐）"""
        print(f"[get_remote_maps] status.connected={self.connected}, "
              f"socket={self.socket is not None}")
        result = self.send_sync(AGVCommand.GET_REMOTE_MAPS)
        print(f"[get_remote_maps] send result={result}")
        if not result:
            return None
        # 兼容 ret_code != 0 的情况
        if isinstance(result, dict) and result.get("ret_code", 0) != 0:
            print(f"[get_remote_maps] 业务失败 ret_code={result.get('ret_code')} "
                  f"err_msg={result.get('err_msg')}")
            return None
        return result


class AGVConfigClient(AGVTCPClientBase):
    """
    配置客户端（端口 19207，与 C++ AgvConfigClient 对齐）
    
    4011 GET_MAP（下载地图数据）在 status 端口会被 AGV 拒绝，
    必须走 config 端口。
    """
    
    def __init__(self, ip: str, port: int):
        super().__init__(ip, port, "Config")

    def get_map(self, map_name: str) -> Optional[dict]:
        """下载指定地图的数据（含 advancedPointList 导航站点）"""
        params = {"map_name": map_name}
        print(f"[get_map] config.connected={self.connected}, "
              f"socket={self.socket is not None}, map_name={map_name}")
        result = self.send_sync(AGVCommand.GET_MAP, params)
        print(f"[get_map] send result ret_code="
              f"{result.get('ret_code') if isinstance(result, dict) else None}")
        if not result or not isinstance(result, dict):
            return None
        if result.get("ret_code", 0) != 0:
            return None
        return result


class AGVOtherClient(AGVTCPClientBase):
    """其他功能客户端（端口 19210：电机使能/数字输出，与 C++ AgvOtherClient 对齐）"""
    
    def __init__(self, ip: str, port: int):
        super().__init__(ip, port, "Others")

    def set_motor_enable(self, motor_name: str, enable: bool) -> Optional[dict]:
        """电机使能"""
        params = {"motor_name": motor_name, "enable": enable}
        return self.send_sync(AGVCommand.SET_MOTOR_ENABLE, params)

    def set_soft_emergency(self, status: bool) -> Optional[dict]:
        """软急停"""
        params = {"status": status}
        return self.send_sync(AGVCommand.SET_SOFT_EMERGENCY, params)


class AGVConnectionManager:
    """
    AGV 连接管理器 - 统一管理所有 AGV 连接
    
    与 C++ AgvConnectionManager 对齐
    
    端口分配（与 C++ 一致）：
        pushback 19301 / control 19205 / nav 19206 /
        status 19204 / other 19210 / config 19207
    """
    
    def __init__(self, ip: str, ports: Optional[dict] = None):
        self.ip = ip
        self.ports = ports or {
            "pushback": 19301,
            "control": 19205,
            "nav": 19206,
            "status": 19204,
            "other": 19210,
            "config": 19207,
        }
        self.pushback: Optional[AGVPushbackClient] = None
        self.control: Optional[AGVControlClient] = None
        self.navigation: Optional[AGVNavigationClient] = None
        self.status: Optional[AGVStatusClient] = None
        self.others: Optional[AGVOtherClient] = None
        self.config: Optional[AGVConfigClient] = None
        self._connected = False

    def connect_all(self) -> bool:
        """
        连接所有端口（与 C++ AgvConnectionManager::connectAll 对齐）
        推送端口必须成功，其余可选
        """
        self.pushback = AGVPushbackClient(self.ip, self.ports.get("pushback", 19301))
        self.control = AGVControlClient(self.ip, self.ports.get("control", 19205))
        self.navigation = AGVNavigationClient(self.ip, self.ports.get("nav", 19206))
        self.status = AGVStatusClient(self.ip, self.ports.get("status", 19204))
        self.others = AGVOtherClient(self.ip, self.ports.get("other", 19210))
        self.config = AGVConfigClient(self.ip, self.ports.get("config", 19207))

        # 推送端口必须连接成功
        if not self.pushback.connect():
            print(f"[AGV] 推送端口连接失败")
            return False

        # 其他端口可选
        self.control.connect()
        self.navigation.connect()
        self.status.connect()
        self.others.connect()
        self.config.connect()

        self._connected = True
        return True

    def disconnect_all(self):
        """断开所有连接（与 C++ AgvConnectionManager::disconnectAll 对齐）"""
        if self.pushback:
            self.pushback.disconnect()
        if self.control:
            self.control.disconnect()
        if self.navigation:
            self.navigation.disconnect()
        if self.status:
            self.status.disconnect()
        if self.others:
            self.others.disconnect()
        if self.config:
            self.config.disconnect()
        self._connected = False

    def set_state_callback(self, callback: Callable):
        """设置状态推送回调"""
        if self.pushback:
            self.pushback.set_state_callback(callback)

    @property
    def connected(self) -> bool:
        return self._connected and self.pushback is not None and self.pushback.connected


# ===========================================================================
# AGV 主客户端门面（兼容旧接口）
# ===========================================================================

class AgvState:
    """AGV 状态（与 C++ AgvState 对齐）"""
    
    def __init__(self):
        self.x = 0.0
        self.y = 0.0
        self.angle = 0.0
        self.battery = 0
        self.error_code = 0
        self.status = ""
        self.speed = 0.0
        self.target_station = ""
        
    @staticmethod
    def from_json(obj: dict) -> 'AgvState':
        """从 JSON 创建 AgvState"""
        state = AgvState()
        if not obj:
            return state
        state.x = obj.get("x", 0.0)
        state.y = obj.get("y", 0.0)
        state.angle = obj.get("angle", 0.0)
        state.battery = obj.get("battery", 0)
        state.error_code = obj.get("error_code", 0)
        state.status = obj.get("status", "")
        state.speed = obj.get("speed", 0.0)
        state.target_station = obj.get("target_station", "")
        return state


class AgvTcpClient:
    """
    兼容旧接口的门面类
    
    与 C++ AgvTcpClient 对齐
    """
    
    def __init__(self):
        self._mgr: Optional[AGVConnectionManager] = None
        self._last_state = AgvState()
        self._ctrl_connected = False
        self._push_connected = False
        self._state_callback: Optional[Callable] = None
        self._map_list_callback: Optional[Callable] = None
        self._map_data_callback: Optional[Callable] = None
        self._error_callback: Optional[Callable] = None

    def set_state_callback(self, cb: Callable[[AgvState], None]):
        """设置状态回调"""
        self._state_callback = cb

    def set_map_list_callback(self, cb: Callable[[List[str], str], None]):
        """设置地图列表回调"""
        self._map_list_callback = cb

    def set_map_data_callback(self, cb: Callable[[str, dict], None]):
        """设置地图数据回调"""
        self._map_data_callback = cb

    def set_error_callback(self, cb: Callable[[str], None]):
        """设置错误回调"""
        self._error_callback = cb

    def connect_to_host(self, ip: str, ctrl_port: int = 19205,
                        nav_port: int = 19206, push_port: int = 19301):
        """
        连接 AGV（与 C++ AgvTcpClient::connectToHost 对齐）
        忽略旧参数，使用默认端口
        """
        ports = {
            "pushback": push_port,
            "control": ctrl_port,
            "nav": nav_port,
            "status": 19204,
            "other": 19210,
            "config": 19207,
        }
        self._mgr = AGVConnectionManager(ip, ports)
        
        # 状态推送回调
        self._mgr.set_state_callback(self._on_state_push)
        
        return self._mgr.connect_all()

    def disconnect_from_host(self):
        """断开连接"""
        if self._mgr:
            self._mgr.disconnect_all()
        self._ctrl_connected = False
        self._push_connected = False

    def is_connected(self) -> bool:
        return self._mgr is not None and self._mgr.connected

    def _on_state_push(self, obj: dict):
        """处理状态推送"""
        # 推送分多种类型，通过 type 字段区分
        obj_type = obj.get("type", -1)
        if obj_type in (1, 2, 3):
            self._last_state = AgvState.from_json(obj)
            if self._state_callback:
                self._state_callback(self._last_state)

    # 运动控制
    def stop_motion(self):
        """停止运动"""
        if self._mgr:
            self._mgr.control.stop_continuous_move()

    def soft_emergency_stop(self):
        """软急停"""
        if self._mgr:
            self._mgr.others.set_soft_emergency(True)

    def open_loop_control(self, linear: float, angular: float):
        """开环控制"""
        if self._mgr:
            self._mgr.control.start_continuous_move(linear, 0, angular)

    # 导航
    def start_navigation(self, station_id: str):
        """开始导航"""
        if self._mgr:
            self._mgr.navigation.go_target_point(station_id)

    def pause_navigation(self):
        """暂停导航"""
        if self._mgr:
            self._mgr.navigation.pause_navigation()

    def cancel_navigation(self):
        """取消导航"""
        if self._mgr:
            self._mgr.navigation.cancel_navigation()

    # 地图
    def request_map_list(self):
        """请求地图列表"""
        if not self._mgr:
            return
        resp = self._mgr.status.get_remote_maps()
        maps = []
        current = ""
        if resp and isinstance(resp, dict):
            if "map_list" in resp:
                maps = resp["map_list"]
            current = resp.get("current_map", "")
        if self._map_list_callback:
            self._map_list_callback(maps, current)

    def request_map_data(self, map_name: str):
        """请求地图数据"""
        if not self._mgr:
            return
        resp = self._mgr.config.get_map(map_name)
        if self._map_data_callback and resp:
            self._map_data_callback(map_name, resp)

    @property
    def ctrl_connected(self) -> bool:
        return self._ctrl_connected

    @property
    def push_connected(self) -> bool:
        return self._push_connected


# ===========================================================================
# Python 兼容类型别名（AGVTCPClient = AgvTcpClient）
# 与 C++ communication/agvtcpclient.h 中的 using AGVTCPClient = AgvTcpClient 对齐
# ===========================================================================
AGVTCPClient = AgvTcpClient
