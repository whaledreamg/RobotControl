#ifndef COMMUNICATION_AGVPROTOCOL_H
#define COMMUNICATION_AGVPROTOCOL_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <cstdint>

/**
 * @brief AGV TCP 协议封装（与 Python communication/agv_protocol.py 对齐）
 *
 * 帧格式（16 字节头 + JSON 数据，网络字节序 / 大端）：
 *   | sync(1) | ver(1) | seq(2) | data_len(4) | type/cmd(2) | reserved(6) | JSON |
 *     0x5A       0x01      0-65535   变长          命令号        填充0
 *
 * 解析策略与 Python 版一致：
 *   1) 正常路径：按 data_len 截取
 *   2) 兜底路径：推送端口的 dlen 字段可能填了端口号等脏值，改用花括号配对切 JSON
 */
class AgvProtocol
{
public:
    // ===== 控制命令 =====
    static constexpr uint16_t CMD_TEACH_STOP            = 2000;  // 停止运动
    static constexpr uint16_t CMD_TEACH_RELOC           = 2002;  // 重定位
    static constexpr uint16_t CMD_CONFIRM_LOCATION      = 2003;  // 确认定位
    static constexpr uint16_t CMD_TEACH_MOVE            = 2010;  // 开环运动控制
    static constexpr uint16_t CMD_RELOAD_MAP            = 2022;  // 切换地图

    // ===== 导航命令 =====
    static constexpr uint16_t CMD_PAUSE_NAVIGATION      = 3001;  // 暂停导航
    static constexpr uint16_t CMD_CONTINUE_NAVIGATION   = 3002;  // 继续导航
    static constexpr uint16_t CMD_CANCEL_NAVIGATION     = 3003;  // 取消导航
    static constexpr uint16_t CMD_GO_TARGET_POINT       = 3051;  // 开始导航

    // ===== 状态查询 =====
    static constexpr uint16_t CMD_GET_AGV_INFO          = 1000;  // 获取 AGV 信息
    static constexpr uint16_t CMD_GET_AGV_LOCATION      = 1004;  // 获取位置
    static constexpr uint16_t CMD_GET_AGV_BLOCK         = 1006;  // 获取阻挡状态
    static constexpr uint16_t CMD_GET_LASER_DATA        = 1009;  // 获取激光数据
    static constexpr uint16_t CMD_GET_NAVIGATION_STATUS = 1020;  // 获取导航状态
    static constexpr uint16_t CMD_GET_RELOCATE_STATUS   = 1021;  // 获取重定位状态
    static constexpr uint16_t CMD_GET_REMOTE_MAPS       = 1300;  // 获取地图列表
    static constexpr uint16_t CMD_GET_MAP               = 4011;  // 下载地图数据

    // ===== 其他功能 =====
    static constexpr uint16_t CMD_SET_DO                = 6001;  // 设置数字输出
    static constexpr uint16_t CMD_SET_SOFT_EMERGENCY    = 6004;  // 软急停
    static constexpr uint16_t CMD_SET_MOTOR_ENABLE      = 6201;  // 电机使能

    // ===== 协议常量 =====
    static constexpr int  HEADER_SIZE   = 16;
    static constexpr int  RESERVED_SIZE = 6;
    static constexpr quint8 SYNC_HEADER   = 0x5A;
    static constexpr quint8 PROTOCOL_VER  = 0x01;

    /// 解析结果：单个完整帧的信息
    struct Frame {
        uint16_t command  = 0;
        uint16_t seq      = 0;
        QByteArray rawJson;        // 原始 JSON 文本（可能不是对象）
        QJsonObject obj;           // 当 rawJson 是 JSON 对象时的解析结果
        bool objValid = false;     // obj 是否有效
        int totalSize  = 0;        // 本帧总字节数
    };

    /**
     * @brief 构建一帧 AGV 协议数据
     * @param cmdId  命令号
     * @param json   JSON 文本（空串表示空 body —— Seer 协议要求空 JSON 体，
     *               用 "null" 会被 AGV 以 ret_code=60002 拒绝）
     * @param seq    帧序号
     */
    static QByteArray buildFrame(uint16_t cmdId,
                                 const QByteArray &json,
                                 uint16_t seq = 0);

    /// 便捷重载：JSON 对象入参
    static QByteArray buildFrame(uint16_t cmdId,
                                 const QJsonObject &obj,
                                 uint16_t seq = 0);

    /**
     * @brief 解析缓冲区中的首个完整帧
     * @param buffer   输入缓冲（可含多帧 / 半帧）
     * @param out      [out] 解析结果
     * @return true 表示成功解析出一个完整帧
     */
    static bool parseFrame(const QByteArray &buffer, Frame &out);

    /**
     * @brief 帧同步：buffer 开头不是 0x5A 时，找到下一个 0x5A 重新对齐
     * @return 需要丢弃的字节数（0 表示无需丢弃；buffer 全部无同步头时返回全部长度）
     */
    static int resync(const QByteArray &buffer);

    /// 命令名辅助（用于日志）
    static QString commandName(uint16_t cmdId);

    /// 任务状态枚举（与 Python AGVTaskStatus 一致）
    enum class TaskStatus {
        None = 0, Waiting = 1, Running = 2,
        Paused = 3, Finished = 4, Failed = 5, Cancelled = 6
    };
};

#endif // COMMUNICATION_AGVPROTOCOL_H