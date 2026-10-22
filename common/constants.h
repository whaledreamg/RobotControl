#ifndef COMMON_CONSTANTS_H
#define COMMON_CONSTANTS_H

#include <QString>

namespace Common {

// 默认网络配置
inline constexpr const char* DEFAULT_IP              = "192.168.100.16";
inline constexpr const char* DEFAULT_ARM_IP          = "192.168.100.16";
inline constexpr const char* DEFAULT_AGV_IP          = "192.168.100.101";
inline constexpr quint16        DEFAULT_ARM_WS_PORT   = 9000;
inline constexpr quint16        DEFAULT_AGV_PUSH_PORT = 19301;
inline constexpr quint16        DEFAULT_AGV_CTRL_PORT = 19205;
inline constexpr quint16        DEFAULT_AGV_NAV_PORT  = 19206;
inline constexpr quint16        DEFAULT_AGV_STATE_PORT= 19204;
inline constexpr quint16        DEFAULT_AGV_OTHER_PORT= 19210;
inline constexpr quint16        DEFAULT_AGV_CONFIG_PORT= 19207;

// 机械臂名称
inline constexpr const char* DEFAULT_LEFT_ARM_NAME  = "LeftArm";
inline constexpr const char* DEFAULT_RIGHT_ARM_NAME = "RightArm";

// 推送频率
inline constexpr int DEFAULT_PUSH_INTERVAL_MS = 100;   // 10Hz

// AGV 协议字段
inline constexpr quint8  AGV_SYNC_HEADER   = 0x5A;
inline constexpr quint8  AGV_PROTOCOL_VER  = 0x01;
inline constexpr int     AGV_HEADER_SIZE   = 16;
inline constexpr int     AGV_RESERVED_SIZE = 6;

// 速度限制
inline constexpr double MIN_SPEED = 0.0;
inline constexpr double MAX_SPEED = 1.0;
inline constexpr double DEFAULT_SPEED = 0.5;

// 升降柱范围 (mm)
inline constexpr int LIFTING_MIN_HEIGHT = 0;
inline constexpr int LIFTING_MAX_HEIGHT = 2000;
inline constexpr int LIFTING_DEFAULT_HEIGHT = 500;

// 升降柱默认参数（与 Python settings_manager 对齐）
inline constexpr const char* LIFTING_DEFAULT_DEVICE  = "Column";
inline constexpr double LIFTING_DEFAULT_SPEED_MPS   = 0.05;
inline constexpr double LIFTING_DEFAULT_JOG_SPEED_MPS = 0.03;
inline constexpr int    LIFTING_DEFAULT_JOG_TIMEOUT_MS = 500;

// 夹爪力度
inline constexpr int GRIPPER_MIN_FORCE = 0;
inline constexpr int GRIPPER_MAX_FORCE = 100;
inline constexpr int GRIPPER_DEFAULT_FORCE = 50;

} // namespace Common

#endif // COMMON_CONSTANTS_H
