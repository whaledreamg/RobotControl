#include "agvprotocol.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QtEndian>
#include <QDebug>

namespace {
// dlen 异常时的兜底上限（与 Python 的 1MB 一致）
constexpr quint32 kMaxPayloadLen = 1024u * 1024u;
}

// ---------------------------------------------------------------------------
// 构建帧
// ---------------------------------------------------------------------------
QByteArray AgvProtocol::buildFrame(uint16_t cmdId,
                                   const QByteArray &json,
                                   uint16_t seq)
{
    const QByteArray payload = json;

    QByteArray frame;
    frame.reserve(HEADER_SIZE + payload.size());

    // 1) 同步头 + 协议版本
    frame.append(static_cast<char>(SYNC_HEADER));
    frame.append(static_cast<char>(PROTOCOL_VER));

    // 2) 序号 (2B, big-endian)
    char seqBytes[2];
    qToBigEndian<quint16>(seq, seqBytes);
    frame.append(seqBytes, 2);

    // 3) 数据长度 (4B, big-endian)
    char lenBytes[4];
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()), lenBytes);
    frame.append(lenBytes, 4);

    // 4) 命令号 (2B, big-endian)
    char cmdBytes[2];
    qToBigEndian<quint16>(cmdId, cmdBytes);
    frame.append(cmdBytes, 2);

    // 5) 保留 6 字节
    frame.append(QByteArray(RESERVED_SIZE, '\0'));

    // 6) payload
    frame.append(payload);

    return frame;
}

QByteArray AgvProtocol::buildFrame(uint16_t cmdId,
                                   const QJsonObject &obj,
                                   uint16_t seq)
{
    return buildFrame(cmdId,
                      QJsonDocument(obj).toJson(QJsonDocument::Compact),
                      seq);
}

// ---------------------------------------------------------------------------
// 解析帧
// ---------------------------------------------------------------------------
bool AgvProtocol::parseFrame(const QByteArray &data, Frame &out)
{
    if (data.size() < HEADER_SIZE)
        return false;

    const auto *p = reinterpret_cast<const uchar *>(data.constData());

    // 同步头校验
    if (p[0] != SYNC_HEADER)
        return false;

    // 协议版本校验
    if (p[1] != PROTOCOL_VER)
        return false;

    const quint16 seq = qFromBigEndian<quint16>(p + 2);
    const quint32 dataLen = qFromBigEndian<quint32>(p + 4);
    const quint16 cmd = qFromBigEndian<quint16>(p + 8);

    // 正常路径：dlen 在合理范围内且数据已收全
    if (dataLen <= kMaxPayloadLen && data.size() >= HEADER_SIZE + static_cast<int>(dataLen)) {
        out.command  = cmd;
        out.seq      = seq;
        out.rawJson  = data.mid(HEADER_SIZE, static_cast<int>(dataLen));
        out.totalSize = HEADER_SIZE + static_cast<int>(dataLen);
        // 尝试按 JSON 对象解析（失败则保留原文，调用方按字符串处理）
        if (!out.rawJson.trimmed().isEmpty()) {
            QJsonParseError err {};
            const QJsonDocument doc = QJsonDocument::fromJson(out.rawJson, &err);
            if (err.error == QJsonParseError::NoError && doc.isObject()) {
                out.obj      = doc.object();
                out.objValid = true;
            } else {
                out.objValid = false;
            }
        }
        return true;
    }

    // 兜底路径：AGV 推送端口的 dlen 字段可能填了端口号 / 别的值，不准。
    // 改用花括号配对切 JSON（需正确处理字符串内的转义）。
    if (data.size() > HEADER_SIZE && data.at(HEADER_SIZE) == '{') {
        int depth = 0;
        int jsonEnd = HEADER_SIZE;
        bool inString = false;
        bool escape = false;

        for (int i = HEADER_SIZE; i < data.size(); ++i) {
            const char b = data.at(i);
            if (escape)          { escape = false; continue; }
            if (b == '\\')       { escape = true;  continue; }
            if (b == '"')        { inString = !inString; continue; }
            if (inString)        continue;
            if (b == '{')        { ++depth; }
            else if (b == '}') {
                if (--depth == 0) { jsonEnd = i + 1; break; }
            }
        }

        if (depth == 0) {
            out.command   = cmd;
            out.seq       = seq;
            out.rawJson   = data.mid(HEADER_SIZE, jsonEnd - HEADER_SIZE);
            out.totalSize = jsonEnd;
            QJsonParseError err {};
            const QJsonDocument doc = QJsonDocument::fromJson(out.rawJson, &err);
            if (err.error == QJsonParseError::NoError && doc.isObject()) {
                out.obj      = doc.object();
                out.objValid = true;
            } else {
                out.objValid = false;
            }
            return true;
        }
    }

    // dlen 异常大 + JSON 还没收完整 → 等待更多数据
    return false;
}

int AgvProtocol::resync(const QByteArray &buffer)
{
    if (buffer.isEmpty())
        return 0;
    if (static_cast<quint8>(buffer.at(0)) == SYNC_HEADER)
        return 0;

    // 从第 1 字节起找下一个同步头
    const int idx = buffer.indexOf(static_cast<char>(SYNC_HEADER), 1);
    if (idx > 0)
        return idx;              // 丢弃 idx 字节后重新对齐
    return buffer.size();        // 全是脏数据，全部丢弃
}

// ---------------------------------------------------------------------------
// 命令名
// ---------------------------------------------------------------------------
QString AgvProtocol::commandName(uint16_t cmdId)
{
    switch (cmdId) {
    case CMD_TEACH_STOP:           return QStringLiteral("停止运动");
    case CMD_TEACH_RELOC:          return QStringLiteral("重定位");
    case CMD_CONFIRM_LOCATION:     return QStringLiteral("确认定位");
    case CMD_TEACH_MOVE:           return QStringLiteral("开环运动控制");
    case CMD_RELOAD_MAP:           return QStringLiteral("切换地图");
    case CMD_PAUSE_NAVIGATION:     return QStringLiteral("暂停导航");
    case CMD_CONTINUE_NAVIGATION:  return QStringLiteral("继续导航");
    case CMD_CANCEL_NAVIGATION:    return QStringLiteral("取消导航");
    case CMD_GO_TARGET_POINT:      return QStringLiteral("开始导航");
    case CMD_GET_AGV_INFO:         return QStringLiteral("获取AGV信息");
    case CMD_GET_AGV_LOCATION:     return QStringLiteral("获取位置");
    case CMD_GET_AGV_BLOCK:        return QStringLiteral("获取阻挡状态");
    case CMD_GET_LASER_DATA:       return QStringLiteral("获取激光数据");
    case CMD_GET_NAVIGATION_STATUS:return QStringLiteral("获取导航状态");
    case CMD_GET_RELOCATE_STATUS:  return QStringLiteral("获取重定位状态");
    case CMD_GET_REMOTE_MAPS:      return QStringLiteral("获取地图列表");
    case CMD_GET_MAP:              return QStringLiteral("下载地图数据");
    case CMD_SET_DO:               return QStringLiteral("设置数字输出");
    case CMD_SET_SOFT_EMERGENCY:   return QStringLiteral("软急停");
    case CMD_SET_MOTOR_ENABLE:     return QStringLiteral("电机使能");
    default:                       return QStringLiteral("未知命令(%1)").arg(cmdId);
    }
}