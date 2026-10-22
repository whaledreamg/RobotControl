"""
Revo2 灵巧手控制客户端（与 C++ communication/revo2hand.h/cpp 对齐）

基于 CoMatrix 业务测试台 HTML 的实际协议实现。

关键协议：
- 6 主动自由度（DOF），按强脑官方行程：
    DOF 0: 拇指弯曲     0~59°
    DOF 1: 拇指侧摆     0~90°
    DOF 2: 食指         0~81°
    DOF 3: 中指         0~81°
    DOF 4: 无名指       0~81°
    DOF 5: 小指         0~81°
- 角度转换：UI 用度，线上用归一化 0~1000
- 读当前位置：Modbus 0x04 (Read Input Registers)
    起始寄存器 0x07D0 (2000), 数量 6 → 6×u16be
- 写单 DOF：Modbus 0x10 (Write Multiple Registers)
    起始寄存器 0x041C (1052), 数量 3 → [dof_idx, position(0-1000), time_ms(1-2000)]
- 写所有 DOF：Modbus 0x10
    起始寄存器 0x03F2 (1010), 数量 12 (24字节) → 6×(position, time_ms)
- 从站地址：左手 126, 右手 127
- ec2rs485 通道：左手 EtherCAT485L, 右手 EtherCAT485R
- 应答错误码：响应字节 [1] = 0x90 表示从站异常
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

from communication.arm_websocket import ArmWebSocketClient


# ============================================================
# 6 自由度定义（与 C++ HAND_DOF_MAX_DEG 对齐）
# ============================================================
DOF_COUNT = 6

DOF_INFO = [
    {"idx": 0, "name": "thumb_bend",  "name_cn": "拇指弯曲", "max": 59},
    {"idx": 1, "name": "thumb_swing", "name_cn": "拇指侧摆", "max": 90},
    {"idx": 2, "name": "index",       "name_cn": "食指",     "max": 81},
    {"idx": 3, "name": "middle",      "name_cn": "中指",     "max": 81},
    {"idx": 4, "name": "ring",        "name_cn": "无名指",   "max": 81},
    {"idx": 5, "name": "pinky",       "name_cn": "小指",     "max": 81},
]
DOF_NAME_LIST = [d["name"] for d in DOF_INFO]
DOF_NAME_CN_LIST = [d["name_cn"] for d in DOF_INFO]
DOF_MAX = [d["max"] for d in DOF_INFO]

# 默认从站地址（左手 126，右手 127）
DEFAULT_SLAVE_ADDR = {"left": 126, "right": 127}
# 默认 ec2rs485 通道
DEFAULT_EC2RS_SLAVE = {"left": "EtherCAT485L", "right": "EtherCAT485R"}


# ============================================================
# 寄存器（与 C++ 常量对齐）
# ============================================================
# 读当前位置（输入寄存器）
REG_READ_POS    = 0x07D0   # 2000
# 写单个 DOF（保持寄存器）
REG_WRITE_DOF   = 0x041C   # 1052
# 写所有 DOF（保持寄存器）
REG_WRITE_ALL   = 0x03F2   # 1010

# 写超时（与 C++ HAND_WRITE_TIME_MS_DEFAULT 对齐）
WRITE_TIME_MS_DEFAULT = 100
WRITE_TIME_MS_MIN = 1
WRITE_TIME_MS_MAX = 2000


# ============================================================
# Modbus RTU 工具（与 C++ handCrc16 完全一致）
# ============================================================
def crc16(data: bytes) -> int:
    """
    Modbus RTU CRC16（多项式 0xA001，低字节在前）
    与 C++ handCrc16() 完全一致
    """
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def crc_bytes(data: bytes) -> List[int]:
    """返回 CRC 的低字节、高字节"""
    c = crc16(data)
    return [c & 0xFF, (c >> 8) & 0xFF]


def verify_crc(data: List[int]) -> bool:
    """验证 CRC（与 C++ verifyCrc() 对齐）"""
    if len(data) < 4:
        return False
    body = bytes(data[:-2])
    expected = (data[-1] << 8) | data[-2]
    return crc16(body) == expected


def check_slave_error(rx: List[int]) -> Optional[str]:
    """
    检查从站异常（与 C++ checkSlaveErrorByte() 对齐）
    bytes[1] = 0x90 表示异常
    """
    if not rx:
        return None
    if len(rx) >= 3 and (rx[1] & 0xFF) == 0x90:
        return f"从站异常码 0x{rx[2] & 0xFF:02X}"
    return None


def build_frame(slave: int, fc: int, payload: List[int]) -> List[int]:
    """通用 Modbus RTU 帧构造"""
    body = [(slave & 0xFF), (fc & 0xFF)] + [(b & 0xFF) for b in payload]
    return body + crc_bytes(bytes(body))


def read_positions_frame(slave: int) -> List[int]:
    """读 6 DOF 当前位置：0x04, 起始 0x07D0, 数量 6"""
    payload = [(REG_READ_POS >> 8) & 0xFF, REG_READ_POS & 0xFF,
               0x00, DOF_COUNT]
    return build_frame(slave, 0x04, payload)


def write_one_dof_frame(slave: int, dof_idx: int, position: int,
                        time_ms: int) -> List[int]:
    """写单个 DOF：0x10, 起始 0x041C, 数量 3 → [dof_idx, position, time_ms]"""
    payload = [(REG_WRITE_DOF >> 8) & 0xFF, REG_WRITE_DOF & 0xFF,
               0x00, 0x03,        # 数量 = 3
               0x06,              # 字节数 = 6
               (dof_idx >> 8) & 0xFF, dof_idx & 0xFF,
               (position >> 8) & 0xFF, position & 0xFF,
               (time_ms >> 8) & 0xFF, time_ms & 0xFF]
    return build_frame(slave, 0x10, payload)


def write_all_dofs_frame(slave: int, positions: List[int],
                         time_ms: int) -> List[int]:
    """
    写所有 6 DOF：0x10, 起始 0x03F2 (1010), 数量 12 → 6×(position, time_ms)
    
    寄存器 1010~1021 映射：
        1010: 大拇指Flex pos  1011: Flex time
        1012: 大拇指Aux  pos  1013: Aux  time
        1014: 食指       pos  1015: 食指 time
        1016: 中指       pos  1017: 中指 time
        1018: 无名指     pos  1019: 无名指 time
        1020: 小指       pos  1021: 小指 time
    数据段必须按寄存器顺序 (pos_i, time_i) 交替发送
    """
    if len(positions) != DOF_COUNT:
        raise ValueError(f"positions 长度必须为 {DOF_COUNT}")
    payload = [(REG_WRITE_ALL >> 8) & 0xFF, REG_WRITE_ALL & 0xFF,
               0x00, 0x0C,        # 数量 = 12
               0x18]              # 字节数 = 24
    for p in positions:
        payload += [(p >> 8) & 0xFF, p & 0xFF,
                    (time_ms >> 8) & 0xFF, time_ms & 0xFF]
    return build_frame(slave, 0x10, payload)


def parse_read_response(data: List[int]) -> Optional[List[int]]:
    """
    解析读 6 DOF 响应
    返回 6 个 0-1000 的归一化值
    """
    if not data or len(data) < 17:
        return None
    if not verify_crc(data):
        return None
    if (data[1] & 0xFF) != 0x04:
        return None
    if (data[2] & 0xFF) != DOF_COUNT * 2:
        return None
    values = []
    for i in range(DOF_COUNT):
        values.append(((data[3 + i * 2] & 0xFF) << 8) | (data[4 + i * 2] & 0xFF))
    return values


# ============================================================
# 角度转换（与 C++ handDegToRaw/handRawToDeg 对齐）
# ============================================================
def deg_to_raw(deg: float, dof_idx: int) -> int:
    """度 → 0-1000 归一化（与 C++ handDegToRaw 对齐）"""
    if dof_idx < 0 or dof_idx >= DOF_COUNT:
        return 0
    max_d = DOF_MAX[dof_idx]
    n = round(deg / max_d * 1000)
    return max(0, min(1000, n))


def raw_to_deg(raw: int, dof_idx: int) -> float:
    """0-1000 归一化 → 度（与 C++ handRawToDeg 对齐）"""
    if dof_idx < 0 or dof_idx >= DOF_COUNT:
        return 0.0
    max_d = DOF_MAX[dof_idx]
    d = (raw / 1000.0) * max_d
    return max(0.0, min(max_d, d))


# ============================================================
# 数据模型
# ============================================================
@dataclass
class HandState:
    """单只手状态（与 C++ HandState 对齐）"""
    side: str                              # "left" / "right"
    modbus_addr: int
    ec2rs485_slave: str
    positions: List[int] = field(default_factory=lambda: [0] * DOF_COUNT)
    target_positions: List[int] = field(default_factory=lambda: [0] * DOF_COUNT)
    write_time_ms: int = WRITE_TIME_MS_DEFAULT
    connected: bool = False
    enabled: bool = False
    inited: bool = False        # ec2rs485 是否已 init
    last_error: str = ""


# ============================================================
# 预设手势（与 C++ handPreset() 对齐）
# ============================================================
PRESETS = {
    "一把抓":   [1000, 0, 1000, 1000, 1000, 1000],
    "释放":     [0, 0, 0, 0, 0, 0],
    "半抓":     [500, 0, 500, 500, 500, 500],
    "1010":     [1000, 0, 100, 1000, 100, 1000],
    "两指捏":   [800, 0, 200, 0, 0, 0],
    "点赞":     [0, 0, 1000, 1000, 1000, 1000],
    "指点":     [1000, 0, 0, 1000, 1000, 1000],
}


# ============================================================
# Revo2HandClient（单只手）
# ============================================================
class Revo2HandClient:
    """
    Revo2 灵巧手控制客户端（单只手，6 自由度）
    
    与 C++ Revo2HandClient 对齐
    """

    def __init__(self, ws: ArmWebSocketClient, side: str,
                 slave: str, addr: int,
                 write_time_ms: int = WRITE_TIME_MS_DEFAULT):
        self._ws = ws
        self.side = side
        self.ec2rs_slave = slave
        self.modbus_addr = addr
        self._write_time_ms = max(WRITE_TIME_MS_MIN,
                                  min(WRITE_TIME_MS_MAX, int(write_time_ms)))
        self._lock = threading.Lock()
        self._state = HandState(side=side, modbus_addr=addr,
                                ec2rs485_slave=slave,
                                write_time_ms=self._write_time_ms)
        self._state_lock = threading.Lock()
        self._callbacks: List[Callable[[HandState], None]] = []
        # 轮询
        self._poll_stop: Optional[threading.Event] = None
        self._poll_thread: Optional[threading.Thread] = None
        # 统计
        self._tx_count = 0
        self._rx_count = 0
        self._err_count = 0

    # ---------- 内部 ----------

    def _ecrs_write_read(self, data: List[int], read_length: int = 0,
                         timeout: int = 400) -> Tuple[List[int], Optional[str]]:
        """
        ec2rs485/write + read
        
        与 C++ Revo2HandClient::ec2rsWriteRead() 对齐
        """
        with self._lock:
            # 写
            wr = self._ws.send_and_wait("ec2rs485/write", {
                "slave": self.ec2rs_slave,
                "data": data,
            }, timeout=3.0)
            if not wr:
                return [], "ec2rs485/write 无响应"
            if wr.get("err"):
                code = wr["err"].get("code")
                msg = wr["err"].get("message", "未知")
                return [], f"write err({code}): {msg}"
            self._tx_count += 1
            
            # 读
            db: Dict = {"slave": self.ec2rs_slave, "timeout": timeout}
            if read_length > 0:
                db["length"] = read_length
            rd = self._ws.send_and_wait("ec2rs485/read", db,
                                        timeout=(timeout / 1000.0) + 3.0)
            if not rd:
                return [], "ec2rs485/read 无响应"
            if rd.get("err"):
                code = rd["err"].get("code")
                msg = rd["err"].get("message", "未知")
                return [], f"read err({code}): {msg}"
            rx = rd.get("db")
            if not isinstance(rx, list):
                return [], "read 返回格式错误"
            self._rx_count += 1
            return list(rx), None

    def _init_channel(self) -> Optional[str]:
        """
        ec2rs485/init
        
        与 C++ Revo2HandClient::initChannel() 对齐
        """
        resp = self._ws.send_and_wait("ec2rs485/init", {
            "slave": self.ec2rs_slave,
            "baudRate": 115200,
            "stopBits": 1,
            "parity": 0,
        }, timeout=6.0)
        if not resp:
            return "ec2rs485/init 无响应"
        if resp.get("err"):
            code = resp["err"].get("code")
            msg = resp["err"].get("message", "未知")
            return f"init err({code}): {msg}"
        with self._state_lock:
            self._state.inited = True
        return None

    # ---------- 公开 API ----------

    def init(self, force: bool = False) -> Tuple[bool, str]:
        """
        初始化：gripper/setType(Generic485) + ec2rs485/init
        
        与 C++ Revo2HandClient::init() 对齐
        """
        # 1) gripper/list 找设备
        resp = self._ws.send_and_wait("gripper/list", {}, timeout=5.0)
        if not resp:
            return False, "gripper/list 无响应"
        if resp.get("err"):
            return False, f"gripper/list err: {resp['err']}"
        devs = resp.get("db")
        if not isinstance(devs, list):
            return False, "gripper/list 响应格式错误"
        
        target_device = None
        for d in devs:
            if isinstance(d, dict) and d.get("slave") == self.ec2rs_slave:
                target_device = d.get("name")
                if d.get("type") != "Generic485":
                    setr = self._ws.send_and_wait("gripper/setType", {
                        "device": target_device,
                        "type": "Generic485",
                    }, timeout=7.0)
                    if not setr or setr.get("err") or setr.get("db") is not True:
                        msg = (setr or {}).get("err", {}).get("message", "未知错误")
                        return False, f"setType 失败: {msg}"
                break
        
        if not target_device:
            return False, f"未找到从站 {self.ec2rs_slave} 对应的 gripper 设备"

        # 2) init 通道
        with self._state_lock:
            need_init = force or not self._state.inited
        if need_init:
            err = self._init_channel()
            if err:
                return False, err

        with self._state_lock:
            self._state.connected = True
            self._state.enabled = True
            self._state.last_error = ""
        return True, ""

    def is_ready(self) -> bool:
        """是否就绪"""
        with self._state_lock:
            return self._state.connected and self._state.inited

    def close(self):
        """关闭（与 C++ Revo2HandClient::close() 对齐）"""
        self.stop_polling()
        with self._state_lock:
            self._state.connected = False
            self._state.inited = False
            self._state.enabled = False

    def get_write_time_ms(self) -> int:
        return self._state.write_time_ms

    def set_write_time_ms(self, t: int) -> None:
        t = max(WRITE_TIME_MS_MIN, min(WRITE_TIME_MS_MAX, int(t)))
        with self._state_lock:
            self._state.write_time_ms = t

    # ---------- 写入 ----------

    def write_dof(self, dof_idx: int, position: int,
                  clamp: bool = True) -> Optional[str]:
        """
        写单个 DOF
        
        与 C++ Revo2HandClient::writeDof() 对齐
        """
        if dof_idx < 0 or dof_idx >= DOF_COUNT:
            return f"DOF 索引越界: {dof_idx}"
        if clamp:
            position = max(0, min(1000, int(position)))
        
        with self._state_lock:
            need_init = not self._state.inited
            time_ms = self._state.write_time_ms
        if need_init:
            err = self._init_channel()
            if err:
                with self._state_lock:
                    self._state.last_error = err
                return err
        
        frame = write_one_dof_frame(self.modbus_addr, dof_idx, position, time_ms)
        rx, err = self._ecrs_write_read(frame, read_length=8, timeout=400)
        if err:
            with self._state_lock:
                self._state.last_error = err
                self._err_count += 1
            return err
        if not rx:
            with self._state_lock:
                self._state.last_error = "无应答"
                self._err_count += 1
            return "无应答"
        if not verify_crc(rx):
            with self._state_lock:
                self._state.last_error = "CRC 错误"
                self._err_count += 1
            return "CRC 错误"
        serr = check_slave_error(rx)
        if serr:
            with self._state_lock:
                self._state.last_error = serr
                self._err_count += 1
            return serr
        if (rx[1] & 0xFF) != 0x10:
            with self._state_lock:
                self._state.last_error = f"非预期功能码 0x{rx[1] & 0xFF:02X}"
                self._err_count += 1
            return self._state.last_error
        
        with self._state_lock:
            self._state.positions[dof_idx] = position
            self._state.target_positions[dof_idx] = position
            self._state.last_error = ""
        self._notify()
        return None

    def write_all(self, positions: List[int],
                  clamp: bool = True) -> Optional[str]:
        """
        写所有 6 DOF
        
        与 C++ Revo2HandClient::writeAll() 对齐
        """
        if len(positions) != DOF_COUNT:
            return f"positions 长度必须为 {DOF_COUNT}，实际 {len(positions)}"
        if clamp:
            positions = [max(0, min(1000, int(p))) for p in positions]
        
        with self._state_lock:
            need_init = not self._state.inited
            time_ms = self._state.write_time_ms
        if need_init:
            err = self._init_channel()
            if err:
                with self._state_lock:
                    self._state.last_error = err
                return err
        
        frame = write_all_dofs_frame(self.modbus_addr, positions, time_ms)
        rx, err = self._ecrs_write_read(frame, read_length=8, timeout=400)
        if err:
            with self._state_lock:
                self._state.last_error = err
                self._err_count += 1
            return err
        if not rx:
            with self._state_lock:
                self._state.last_error = "无应答"
                self._err_count += 1
            return "无应答"
        if not verify_crc(rx):
            with self._state_lock:
                self._state.last_error = "CRC 错误"
                self._err_count += 1
            return "CRC 错误"
        serr = check_slave_error(rx)
        if serr:
            with self._state_lock:
                self._state.last_error = serr
                self._err_count += 1
            return serr
        if (rx[1] & 0xFF) != 0x10:
            with self._state_lock:
                self._state.last_error = f"非预期功能码 0x{rx[1] & 0xFF:02X}"
                self._err_count += 1
            return self._state.last_error
        
        with self._state_lock:
            self._state.positions = list(positions)
            self._state.target_positions = list(positions)
            self._state.last_error = ""
        self._notify()
        return None

    def execute_preset(self, name: str) -> Optional[str]:
        """
        执行预设手势
        
        与 C++ Revo2HandClient::executePreset() 对齐
        """
        if name not in PRESETS:
            return f"未知预设: {name}"
        return self.write_all(PRESETS[name])

    # ---------- 读取 ----------

    def read_positions(self) -> Optional[str]:
        """
        读 6 DOF 当前位置
        
        与 C++ Revo2HandClient::readPositions() 对齐
        """
        with self._state_lock:
            need_init = not self._state.inited
        if need_init:
            err = self._init_channel()
            if err:
                with self._state_lock:
                    self._state.last_error = err
                return err
        
        frame = read_positions_frame(self.modbus_addr)
        rx, err = self._ecrs_write_read(frame, read_length=17, timeout=400)
        if err:
            with self._state_lock:
                self._state.last_error = err
            return err
        if not rx:
            with self._state_lock:
                self._state.last_error = "无应答"
            return "无应答"
        if not verify_crc(rx):
            with self._state_lock:
                self._state.last_error = "CRC 错误"
            return "CRC 错误"
        if (rx[1] & 0xFF) != 0x04:
            with self._state_lock:
                self._state.last_error = f"非预期功能码 0x{rx[1] & 0xFF:02X}"
            return self._state.last_error
        if (rx[2] & 0xFF) != DOF_COUNT * 2:
            with self._state_lock:
                self._state.last_error = f"字节数不匹配: {rx[2] & 0xFF}"
            return self._state.last_error
        
        positions = []
        for i in range(DOF_COUNT):
            positions.append(((rx[3 + i * 2] & 0xFF) << 8) | (rx[4 + i * 2] & 0xFF))
        with self._state_lock:
            self._state.positions = positions
            self._state.last_error = ""
        self._notify()
        return None

    def get_state(self) -> HandState:
        return self._state

    def register_state_callback(self, cb: Callable[[HandState], None]):
        self._callbacks.append(cb)

    def _notify(self):
        """通知状态变化"""
        with self._state_lock:
            callbacks = list(self._callbacks)
        if not callbacks:
            return
        state = self.get_state()
        for cb in callbacks:
            try:
                cb(state)
            except Exception as e:
                print(f"[Revo2-{self.side}] 状态回调异常: {e}")

    def get_tx_count(self) -> int:
        return self._tx_count

    def get_rx_count(self) -> int:
        return self._rx_count

    def get_err_count(self) -> int:
        return self._err_count

    # ---------- 轮询 ----------

    def start_polling(self, interval_s: float = 0.2):
        """启动轮询"""
        if self._poll_thread and self._poll_thread.is_alive():
            return
        self._poll_stop = threading.Event()
        self._poll_thread = threading.Thread(
            target=self._poll_loop, args=(interval_s,),
            daemon=True, name=f"revo2-poll-{self.side}")
        self._poll_thread.start()

    def stop_polling(self):
        """停止轮询"""
        if self._poll_stop:
            self._poll_stop.set()
        if self._poll_thread:
            self._poll_thread.join(timeout=2.0)
        self._poll_thread = None
        self._poll_stop = None

    def _poll_loop(self, interval_s: float):
        """轮询循环"""
        while self._poll_stop and not self._poll_stop.is_set():
            try:
                self.read_positions()
            except Exception as e:
                print(f"[Revo2-{self.side}] 轮询异常: {e}")
            state = self.get_state()
            for cb in self._callbacks:
                try:
                    cb(state)
                except Exception as e:
                    print(f"[Revo2-{self.side}] 状态回调异常: {e}")
            self._poll_stop.wait(interval_s)


# ============================================================
# DualHandManager（双手）
# ============================================================
class DualHandManager:
    """
    双手灵巧手管理器
    
    与 C++ DualHandManager 对齐
    """

    def __init__(self, ws: ArmWebSocketClient,
                 left_slave: str = DEFAULT_EC2RS_SLAVE["left"],
                 right_slave: str = DEFAULT_EC2RS_SLAVE["right"],
                 left_addr: int = DEFAULT_SLAVE_ADDR["left"],
                 right_addr: int = DEFAULT_SLAVE_ADDR["right"],
                 write_time_ms: int = WRITE_TIME_MS_DEFAULT):
        self._ws = ws
        self.left = Revo2HandClient(ws, "left", left_slave, left_addr, write_time_ms)
        self.right = Revo2HandClient(ws, "right", right_slave, right_addr, write_time_ms)

    def init(self) -> Tuple[bool, str, bool, str]:
        """
        初始化双手 + 启动轮询
        
        与 C++ DualHandManager::init() 对齐
        """
        # 先停止旧轮询
        try:
            self.left.stop_polling()
            self.right.stop_polling()
        except Exception:
            pass
        
        lo, le = self.left.init()
        ro, re_ = self.right.init()
        
        # 启动轮询
        if lo:
            self.left.start_polling(0.2)
        elif self.left.get_state().connected:
            self.left.start_polling(0.2)
        if ro:
            self.right.start_polling(0.2)
        elif self.right.get_state().connected:
            self.right.start_polling(0.2)
        
        return lo, le, ro, re_

    def close(self):
        """关闭（与 C++ DualHandManager::close() 对齐）"""
        self.left.close()
        self.right.close()

    def set_arm_connected(self, connected: bool):
        """设置连接状态（与 C++ DualHandManager::setArmConnected() 对齐）"""
        if not connected:
            self.left.stop_polling()
            self.right.stop_polling()
        with self.left._state_lock:
            self.left._state.connected = connected
        with self.right._state_lock:
            self.right._state.connected = connected
