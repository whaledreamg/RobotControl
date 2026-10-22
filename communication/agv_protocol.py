"""
AGV TCP 协议封装
包含帧头结构、命令号定义等

与 C++ communication/agv_protocol.h/cpp 对齐
"""
import struct
from enum import IntEnum


class AGVCommand(IntEnum):
    """AGV 命令号枚举"""
    # 控制命令
    TEACH_STOP = 2000           # 停止运动
    TEACH_RELOC = 2002          # 重定位
    CONFIRM_LOCATION = 2003     # 确认定位
    TEACH_MOVE = 2010           # 开环运动控制
    RELOAD_MAP = 2022           # 切换地图
    
    # 导航命令
    PAUSE_NAVIGATION = 3001    # 暂停导航
    CONTINUE_NAVIGATION = 3002  # 继续导航
    CANCEL_NAVIGATION = 3003   # 取消导航
    GO_TARGET_POINT = 3051       # 开始导航
    
    # 状态查询
    GET_AGV_INFO = 1000         # 获取AGV信息
    GET_AGV_LOCATION = 1004      # 获取位置
    GET_AGV_BLOCK = 1006         # 获取阻挡状态
    GET_LASER_DATA = 1009       # 获取激光数据
    GET_NAVIGATION_STATUS = 1020 # 获取导航状态
    GET_RELOCATE_STATUS = 1021   # 获取重定位状态
    GET_REMOTE_MAPS = 1300       # 获取地图列表
    GET_MAP = 4011               # 下载地图数据（含 advancedPointList 导航站点）
    
    # 其他功能
    SET_DO = 6001               # 设置数字输出
    SET_SOFT_EMERGENCY = 6004   # 软急停
    SET_MOTOR_ENABLE = 6201     # 电机使能


class AGVProtocol:
    """AGV TCP 协议处理类
    
    与 C++ communication/agv_protocol.cpp 对齐
    
    真实协议格式（从 SeerAgv 参考项目 SCHeadData.h 确认）：
        | sync(1) | ver(1) | seq(2 BE) | data_len(4 BE) | type/cmd(2 BE) | reserved(6) | JSON |
        总 16 字节 header
    """
    
    HEADER_FORMAT = "!BBHIH6s"  # 网络字节序: 1+1+2+4+2+6 = 16
    HEADER_SIZE = 16

    SYNC_HEADER = 0x5A
    PROTOCOL_VERSION = 0x01
    
    # dlen 异常时的兜底上限（与 C++ kMaxPayloadLen = 1MB 一致）
    MAX_PAYLOAD_LEN = 1024 * 1024

    @staticmethod
    def build_frame(command: int, json_data: str, seq: int = 0) -> bytes:
        """
        构建发送帧

        Args:
            command: 命令号 (type 字段)
            json_data: JSON 字符串数据（空串表示空 body —— Seer 协议要求空 JSON 体）
            seq: 序号

        Returns:
            完整的帧数据 (bytes)
        """
        json_bytes = json_data.encode('utf-8') if isinstance(json_data, str) else json_data
        data_len = len(json_bytes)

        # 构建帧头 - 字段顺序：sync, ver, seq, dlen, type, reserved
        header = struct.pack(
            AGVProtocol.HEADER_FORMAT,
            AGVProtocol.SYNC_HEADER,      # 同步头
            AGVProtocol.PROTOCOL_VERSION, # 协议版本
            seq & 0xFFFF,                 # 序号
            data_len,                     # 数据区长度
            command & 0xFFFF,             # 命令号 (type)
            b'\x00' * 6                   # 保留区域
        )

        return header + json_bytes
    
    @staticmethod
    def create_command(command: int, params: dict = None, seq: int = 0) -> bytes:
        """
        创建命令帧

        Args:
            command: 命令号
            params: 命令参数
            seq: 序号（用于请求-响应匹配）

        Returns:
            完整的帧数据
        """
        import json
        if params is None:
            # Seer 协议对 1300/1004 等无参命令要求空 JSON 体，
            # 用 "null" 会被 AGV 拒绝（ret_code=60002 "data is not JSON object"）
            json_str = ""
        elif isinstance(params, str):
            # 调用方已经构造好 JSON 字符串
            json_str = params
        else:
            json_str = json.dumps(params)

        return AGVProtocol.build_frame(command, json_str, seq)
    
    @staticmethod
    def parse_frame(data: bytes):
        """
        解析接收帧

        与 C++ AgvProtocol::parseFrame() 对齐

        解析策略：
            1) 正常路径：按 data_len 截取
            2) 兜底路径：推送端口的 dlen 字段可能填了端口号等脏值，
               改用花括号配对切 JSON

        Args:
            data: 接收到的数据

        Returns:
            (command, seq, json_data, total_size) 或 None
        """
        HEADER_SIZE = AGVProtocol.HEADER_SIZE

        if len(data) < HEADER_SIZE:
            return None

        # 同步头校验
        if data[0] != AGVProtocol.SYNC_HEADER:
            return None

        try:
            # 字段顺序：sync(1) ver(1) seq(2) dlen(4) type(2) reserved(6)
            sync, version, seq, data_len, command, reserved = struct.unpack(
                AGVProtocol.HEADER_FORMAT, data[:HEADER_SIZE]
            )

            # 协议版本校验
            if version != AGVProtocol.PROTOCOL_VERSION:
                return None

            # 正常路径：dlen 在合理范围内且数据已收全
            if data_len <= AGVProtocol.MAX_PAYLOAD_LEN and len(data) >= HEADER_SIZE + data_len:
                json_bytes = data[HEADER_SIZE:HEADER_SIZE + data_len]
                try:
                    json_data = json_bytes.decode('utf-8')
                except UnicodeDecodeError:
                    json_data = json_bytes.decode('utf-8', errors='replace')
                return command, seq, json_data, HEADER_SIZE + data_len

            # 兜底路径：AGV 推送端口的 dlen 字段可能填了端口号/别的值，不准
            # 改用花括号配对切 JSON（处理字符串内的转义）
            json_start = HEADER_SIZE
            if json_start < len(data) and data[json_start] == ord('{'):
                depth = 0
                json_end = json_start
                in_string = False
                escape = False
                for i in range(json_start, len(data)):
                    b = data[i]
                    if escape:
                        escape = False
                        continue
                    if b == ord('\\'):
                        escape = True
                        continue
                    if b == ord('"'):
                        in_string = not in_string
                        continue
                    if in_string:
                        continue
                    if b == ord('{'):
                        depth += 1
                    elif b == ord('}'):
                        depth -= 1
                        if depth == 0:
                            json_end = i + 1
                            break
                if depth == 0:
                    json_bytes = data[json_start:json_end]
                    try:
                        json_data = json_bytes.decode('utf-8')
                    except UnicodeDecodeError:
                        json_data = json_bytes.decode('utf-8', errors='replace')
                    return command, seq, json_data, json_end

            # dlen 异常大 + JSON 还没收完整 -> 等待更多数据
            return None

        except Exception:
            return None

    @staticmethod
    def resync(buffer: bytes) -> int:
        """
        帧同步：buffer 开头不是 0x5A 时，找到下一个 0x5A 重新对齐
        
        与 C++ AgvProtocol::resync() 对齐
        
        Args:
            buffer: 输入缓冲（可含多帧/半帧）
            
        Returns:
            需要丢弃的字节数（0 表示无需丢弃；buffer 全部无同步头时返回全部长度）
        """
        if not buffer:
            return 0
        if buffer[0] == AGVProtocol.SYNC_HEADER:
            return 0
        
        # 从第 1 字节起找下一个同步头
        try:
            idx = buffer.index(AGVProtocol.SYNC_HEADER, 1)
            return idx
        except ValueError:
            # 没有找到，返回全部长度
            return len(buffer)


class AGVTaskStatus(IntEnum):
    """导航任务状态"""
    NONE = 0       # 无任务
    WAITING = 1    # 等待中
    RUNNING = 2    # 运行中
    PAUSED = 3     # 已暂停
    FINISHED = 4   # 已完成
    FAILED = 5     # 失败
    CANCELLED = 6  # 已取消


# 命令名辅助（与 C++ AgvProtocol::commandName() 对齐）
AGV_COMMAND_NAMES = {
    AGVCommand.TEACH_STOP: "停止运动",
    AGVCommand.TEACH_RELOC: "重定位",
    AGVCommand.CONFIRM_LOCATION: "确认定位",
    AGVCommand.TEACH_MOVE: "开环运动控制",
    AGVCommand.RELOAD_MAP: "切换地图",
    AGVCommand.PAUSE_NAVIGATION: "暂停导航",
    AGVCommand.CONTINUE_NAVIGATION: "继续导航",
    AGVCommand.CANCEL_NAVIGATION: "取消导航",
    AGVCommand.GO_TARGET_POINT: "开始导航",
    AGVCommand.GET_AGV_INFO: "获取AGV信息",
    AGVCommand.GET_AGV_LOCATION: "获取位置",
    AGVCommand.GET_AGV_BLOCK: "获取阻挡状态",
    AGVCommand.GET_LASER_DATA: "获取激光数据",
    AGVCommand.GET_NAVIGATION_STATUS: "获取导航状态",
    AGVCommand.GET_RELOCATE_STATUS: "获取重定位状态",
    AGVCommand.GET_REMOTE_MAPS: "获取地图列表",
    AGVCommand.GET_MAP: "下载地图数据",
    AGVCommand.SET_DO: "设置数字输出",
    AGVCommand.SET_SOFT_EMERGENCY: "软急停",
    AGVCommand.SET_MOTOR_ENABLE: "电机使能",
}


def command_name(cmd_id: int) -> str:
    """获取命令号对应的可读名称"""
    return AGV_COMMAND_NAMES.get(cmd_id, f"未知命令({cmd_id})")
