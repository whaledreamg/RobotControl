#ifndef COMMUNICATION_REVO2HAND_H
#define COMMUNICATION_REVO2HAND_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QRecursiveMutex>
#include <QStringList>
#include <QVector>
#include <functional>

class ArmWebSocket;

// ============================================================================
// 常量
// ============================================================================
/// 6 个自由度（拇指弯曲、拇指侧摆、食指、中指、无名指、小指）
constexpr int HAND_DOF_COUNT = 6;

/// 各 DOF 的最大角度（度），用于 UI 度 ↔ 控制器归一化 0~1000 换算
/// DOF 0: 拇指弯曲 0~59°  / DOF 1: 拇指侧摆 0~90°
/// DOF 2: 食指 0~81° / DOF 3: 中指 0~81°
/// DOF 4: 无名指 0~81° / DOF 5: 小指 0~81°
constexpr int HAND_DOF_MAX_DEG[HAND_DOF_COUNT] = { 59, 90, 81, 81, 81, 81 };

/// 默认从站地址（左手 126，右手 127）
constexpr int HAND_DEFAULT_SLAVE_LEFT  = 126;
constexpr int HAND_DEFAULT_SLAVE_RIGHT = 127;

/// 默认 RS485 通信板从站名（实际 channel 由 rs485/list 解析）
constexpr const char* HAND_DEFAULT_EC2RS_LEFT  = "EtherCAT485L";
constexpr const char* HAND_DEFAULT_EC2RS_RIGHT = "EtherCAT485R";

/// 寄存器地址
constexpr quint16 HAND_REG_READ_POS   = 0x07D0;  // 2000, 读 6 DOF 位置
constexpr quint16 HAND_REG_WRITE_DOF  = 0x041C;  // 1052, 写单个 DOF
constexpr quint16 HAND_REG_WRITE_ALL  = 0x03F2;  // 1010, 写所有 DOF

/// 写超时默认值
constexpr int HAND_WRITE_TIME_MS_DEFAULT = 100;
constexpr int HAND_WRITE_TIME_MS_MIN    = 1;
constexpr int HAND_WRITE_TIME_MS_MAX    = 2000;

// ============================================================================
// 工具函数
// ============================================================================
/// Modbus RTU CRC16（低字节在前，与 HTML rs485Crc16 完全一致）
quint16 handCrc16(const QByteArray &data);

/// 度 → 归一化 0~1000
int handDegToRaw(double deg, int dofIdx);

/// 归一化 0~1000 → 度
double handRawToDeg(int raw, int dofIdx);

// ============================================================================
// Python 命名兼容别名
//
// Python 版 communication/revo2_hand.py 导出的是 DOF_MAX / DOF_COUNT /
// DOF_NAME_CN_LIST / degToRaw / rawToDeg / WRITE_TIME_MS_DEFAULT 等无前缀
// 符号。handviewdialog.cpp 按 Python 名称书写，这里统一加别名，保证两边
// 的常量表与换算函数始终同源，不会各改一处漏一处。
// ============================================================================
inline constexpr int DOF_COUNT = HAND_DOF_COUNT;

/// 各 DOF 中文名（与 DOF_MAX 同序）
const QStringList &handDofNameCnList();

/// Python 风格包装
inline int degToRaw(double deg, int dofIdx)  { return handDegToRaw(deg, dofIdx); }
inline double rawToDeg(int raw, int dofIdx)  { return handRawToDeg(raw, dofIdx); }
inline constexpr int WRITE_TIME_MS_DEFAULT = HAND_WRITE_TIME_MS_DEFAULT;

/// DOF 最大角度（度），越界时回落到 81°（五指通用上限）
inline int dofMaxDeg(int dofIdx) {
    return (dofIdx >= 0 && dofIdx < HAND_DOF_COUNT) ? HAND_DOF_MAX_DEG[dofIdx] : 81;
}

// ============================================================================
// 数据模型
// ============================================================================
struct HandState {
    QString side;                       // "left" / "right'
    int modbusAddr = 0;                // 从站地址
    QString ec2rsSlave;                // ec2rs485 通道名
    QVector<int> positions;           // 各 DOF 当前位置 0~1000
    QVector<int> targetPositions;      // 最近一次下发目标
    int writeTimeMs = HAND_WRITE_TIME_MS_DEFAULT;
    bool connected = false;
    bool enabled = false;
    bool inited = false;               // ec2rs485 通道是否已 init
    QString lastError;
};

// ============================================================================
// 预设手势
// ============================================================================
/// 预设手势映射（按 0~1000 归一化值）
/// 所有动作下拇指侧摆(swing) = 0
QVector<int> handPreset(const QString &name);

// ============================================================================
// Modbus RTU 帧构造 / 校验
// 与 Python revo2_hand.py 的 crc_bytes / verify_crc / build_frame /
// read_positions_frame / write_one_dof_frame / write_all_dofs_frame 一一对应，
// 供 HandViewDialog 的「Modbus 调试」页自由拼报文使用。
// ============================================================================

/// CRC 低字节 + 高字节（Modbus RTU 小端在前）
QByteArray handCrcBytes(const QByteArray &data);

/// 校验报文尾部两字节 CRC（等价 Python verify_crc）
bool handVerifyCrc(const QByteArray &frame);

/// 通用帧构造：slave | fc | payload + CRC
QByteArray handBuildFrame(int slave, int func, const QByteArray &payload);

/// 读 6 DOF 当前位置：0x04, 起始 0x07D0, 数量 6
QByteArray handReadPositionsFrame(int slave);

/// 写单个 DOF：0x10, 起始 0x041C, 数量 3 → [dof_idx, position, time_ms]
QByteArray handWriteOneDofFrame(int slave, int dofIdx, int position, int timeMs);

/// 写所有 6 DOF：0x10, 起始 0x03F2 (1010), 数量 12
/// 寄存器 1010~1021 按 (pos_i, time_i) 交替排列
QByteArray handWriteAllDofsFrame(int slave, const QVector<int> &positions, int timeMs);

// --- Python 侧寄存器名别名 ---
inline constexpr quint16 REG_READ_POS  = HAND_REG_READ_POS;
inline constexpr quint16 REG_WRITE_DOF = HAND_REG_WRITE_DOF;
inline constexpr quint16 REG_WRITE_ALL = HAND_REG_WRITE_ALL;

/// 各 DOF 英文名（thumb_bend / thumb_swing / index / middle / ring / pinky）
const QStringList &handDofNameList();

// ============================================================================
// 单手客户端
// ============================================================================
class Revo2HandClient : public QObject
{
    Q_OBJECT
public:
    explicit Revo2HandClient(ArmWebSocket *ws,
                             const QString &side,
                             const QString &ec2rsSlave,
                             int modbusAddr,
                             QObject *parent = nullptr);
    ~Revo2HandClient() override;

    /// 初始化：rs485/list 解析 channel + 释放夹爪占用 + rs485/init
    /// （API 1.212.0 起 ec2rs485/* 迁为 rs485/*，gripper/setType 已删除）
    /// 返回 {ok, errStr}
    QPair<bool, QString> init(bool force = false);

    bool isReady() const;
    void close();

    int writeTimeMs() const;
    void setWriteTimeMs(int ms);

    // 写入
    /// 写单个 DOF：返回 null 表示成功，QString 为错误信息
    QString writeDof(int dofIdx, int position);
    /// 写所有 6 DOF：返回 null 表示成功
    QString writeAll(const QVector<int> &positions);
    /// 执行预设手势
    QString executePreset(const QString &name);

    // 读取
    /// 读 6 DOF 当前位置
    QString readPositions();

    HandState state() const;
    /// 注册状态回调，返回句柄（用于注销）
    using StateCallback = std::function<void(const HandState &)>;
    int registerStateCallback(StateCallback cb);
    /// 注销状态回调（必须在回调所属对象销毁前调用）。
    ///
    /// 回调以裸 std::function 保存，若对象销毁后仍留在列表里，
    /// 客户端下一次状态变化就会调用到已释放的对象 → 闪退。
    /// Python 版靠 PySide Signal 在控件销毁时自动断开；C++ 需显式注销。
    void unregisterStateCallback(int handle);

    // --- 供 UI（HandViewDialog 调试页）读取的内部参数 ---
    QString ec2rsSlave() const;
    int modbusAddr() const;
    int txCount() const;
    int rxCount() const;
    int errCount() const;

    // --- 调试通道 ---
    /// 原始 Modbus 收发（读长度 <0 表示只发不读）
    QString rawExchange(const QByteArray &frame, int readLength);
    /// 查询 RS485 通道状态（rs485/getStatus）
    QJsonObject ec2rsGetStatus();
    /// 把从站名解析为 rs485 channel 标识（rs485/list）。成功返回空串并写入 channel。
    QString resolveChannel(QString *channel, bool force = false);
    /// 实时轮询间隔（ms），供 HandViewDialog 的「实时模式」开关切换
    void setPollInterval(int ms) { m_pollIntervalMs = ms; }
    int pollInterval() const   { return m_pollIntervalMs; }

    // --- 运行时改参数（ParamConfigTab「保存（立即生效）」）---
    void setModbusAddr(int addr);
    void setEc2rsSlave(const QString &slave);
    /// 参数变化后清掉 init 标记，下次写/读时自动重新 init
    void resetInited();

signals:
    void stateChanged(const HandState &state);

private:
    QByteArray ec2rsWriteRead(const QByteArray &data, int readLength);
    QString initChannel();
    /// 通道被夹爪协议占用时，经 peripheral/setDeviceEnabled 停用本从站上的夹爪协议
    QString releaseChannel(const QString &channel);
    void notifyState();
    /// 通道是否已 init（线程安全读取）
    bool isChannelInited() const;
    /// 记录错误并计数（线程安全）
    void setError(const QString &err);
    ArmWebSocket *m_ws = nullptr;
    QString m_side;
    QString m_ec2rsSlave;           // 通信板从站名（用于定位 channel）
    QString m_channel;              // rs485/list 解析出的 channel 标识
    int m_modbusAddr;
    int m_writeTimeMs = HAND_WRITE_TIME_MS_DEFAULT;
    int m_pollIntervalMs = 200;
    HandState m_state;
    /// 保护 m_state / m_channel / m_txCount 等共享状态。
    /// 微调写入走后台线程（QtConcurrent::run），同时 UI 线程会读状态，
    /// 必须加锁否则读到撕裂值。递归锁：notifyState() 的回调里会再调 state()。
    mutable QRecursiveMutex m_mutex;
    /// 串行化同一路 RS485 通道上的「写 + 读」收发
    /// （对应 Python _ecrs_write_read() 的 self._lock）。
    ///
    /// 锁在**调用方线程**获取，等待响应的过程也在调用方线程
    /// （ArmWebSocket::sendAndWaitSafe 内部用条件变量），
    /// socket 所属线程（主线程）全程空闲，因此不会阻塞 UI，也不会死锁。
    QMutex m_ioMutex;
    /// 状态回调表：{句柄, 回调}
    struct StateCbEntry {
        int handle;
        std::function<void(const HandState &)> fn;
    };
    QVector<StateCbEntry> m_callbacks;
    int m_nextCbHandle = 1;
    int m_txCount = 0, m_rxCount = 0, m_errCount = 0;
};

// ============================================================================
// 双手管理器
// ============================================================================
class DualHandManager : public QObject
{
    Q_OBJECT
public:
    explicit DualHandManager(ArmWebSocket *ws,
                            const QString &leftSlave  = HAND_DEFAULT_EC2RS_LEFT,
                            const QString &rightSlave = HAND_DEFAULT_EC2RS_RIGHT,
                            int leftAddr  = HAND_DEFAULT_SLAVE_LEFT,
                            int rightAddr = HAND_DEFAULT_SLAVE_RIGHT,
                            QObject *parent = nullptr);
    ~DualHandManager() override;

    Revo2HandClient *left()  { return m_left; }
    Revo2HandClient *right() { return m_right; }

    /// 初始化双手（停止旧轮询 + init + 启动轮询）
    /// 返回 {leftOk, leftErr, rightOk, rightErr}
    QPair<QPair<bool, QString>, QPair<bool, QString>> init();

    void close();
    void setArmConnected(bool connected);

private:
    ArmWebSocket *m_ws = nullptr;
    Revo2HandClient *m_left  = nullptr;
    Revo2HandClient *m_right = nullptr;
};

#endif // COMMUNICATION_REVO2HAND_H