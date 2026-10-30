#ifndef COMMUNICATION_AGVTCPCLIENT_H
#define COMMUNICATION_AGVTCPCLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QWaitCondition>
#include <QVariant>
#include <functional>

#include "agvprotocol.h"
#include "../common/types.h"

Q_DECLARE_METATYPE(AgvProtocol::Frame)

class AgvConnectionManager;   // 前置声明：下方的 AgvTcpClient 门面类持有它

/**
 * @brief AGV TCP 客户端基类（与 Python communication/agv_tcp_client.py 对齐）
 *
 * 设计要点（承接 Python 版的关键行为）：
 *   - 同步 send() 用 QMutex + QWaitCondition 做 seq→响应匹配，带超时，
 *     绝不永久阻塞；disconnect() 会 notifyAll 唤醒所有等待线程
 *   - 接收循环做帧同步：buffer 开头非 0x5A 时丢弃到下一个同步头
 *   - seq 从 1 开始递增（避免 0 被某些协议视为保留值）
 *   - 推送端口的状态做**字段合并**（AGV 推送分多种类型，只有部分字段）
 */
class AgvTcpClientBase : public QObject
{
    Q_OBJECT
public:
    explicit AgvTcpClientBase(const QString &ip, quint16 port,
                              const QString &name, QObject *parent = nullptr);
    ~AgvTcpClientBase() override;

    // ===== 连接 =====
    bool connectToHost();
    void disconnectFromHost();

    bool isConnected() const { return m_connected; }
    QString ip() const   { return m_ip; }
    quint16 port() const { return m_port; }
    QString name() const { return m_name; }

    // ===== 发送 =====
    /// 同步发送并等待 seq 匹配的响应；超时或失败返回无效 QVariant
    QVariant sendSync(uint16_t cmdId, const QJsonObject &obj);
    QVariant sendSync(uint16_t cmdId);                        // 空 body
    QVariant sendSyncRaw(uint16_t cmdId, const QByteArray &json);

    /// 不等待响应直接写 socket（高频运动控制用）
    bool sendNoWait(uint16_t cmdId, const QJsonObject &obj);

    /// 异步发送 + 回调（在事件循环中回调，避免跨线程操作 UI）
    void sendAsync(uint16_t cmdId, const QJsonObject &obj,
                   std::function<void(const QVariant &)> cb = nullptr);

protected:
    /// 子类可覆盖：处理未匹配到 pending 请求的上行报文（即推送）
    virtual void handlePush(const AgvProtocol::Frame &frame);

    /// 子类可调整：同步等待超时（秒）
    void setSendTimeout(double seconds) { m_sendTimeout = seconds; }

    /// 便捷：把 Frame 解包成 object（优先 obj，其次解析 rawJson）
    static QJsonObject frameObject(const AgvProtocol::Frame &frame);

signals:
    /// 连接状态变化
    void connectedChanged(const QString &name, bool connected);
    /// 收到推送（object 形式）
    void pushReceived(const QString &name, const QJsonObject &obj);

private slots:
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError err);
    void onSocketStateChanged(QAbstractSocket::SocketState state);

private:
    void processBuffer();
    bool writeFrame(const QByteArray &frame);
    void setConnected(bool c);
    quint16 nextSeq();
    void dumpTx(const QByteArray &frame, quint16 cmdId) const;

    QString m_ip;
    quint16 m_port = 0;
    QString m_name;

    QTcpSocket *m_socket = nullptr;
    QByteArray m_buffer;
    bool m_connected = false;
    double m_sendTimeout = 3.0;     // 秒

    mutable QMutex m_mutex;
    QWaitCondition m_respCond;
    quint16 m_seq = 1;
    QHash<quint16, QVariant> m_pending;   // seq -> 响应
    int m_rxCount = 0;
};

// ===========================================================================
// 各端口专用客户端
// ===========================================================================

/**
 * @brief 状态推送接收客户端（端口 19301）
 * 关键：AGV 推送分多种类型（DI 局部 / 位置+电池 / 错误…），
 * 必须做**合并**——收到部分数据时不清空已知的字段。
 */
class AgvPushbackClient : public AgvTcpClientBase
{
    Q_OBJECT
public:
    explicit AgvPushbackClient(const QString &ip, quint16 port,
                               QObject *parent = nullptr);

    void setStateCallback(std::function<void(const QJsonObject &)> cb);

protected:
    void handlePush(const AgvProtocol::Frame &frame) override;

private:
    std::function<void(const QJsonObject &)> m_stateCb;
    QJsonObject m_lastState;       // 合并缓存
    int m_pushCount = 0;
};

/**
 * @brief 控制客户端（端口 19205）
 * 持续运动：每 100ms 重发 2010（duration=200），实现"按住一直动、松开即停"。
 * 这是工业 AGV 通用做法——单次指令通常 100-500ms 后被 AGV 自动停止。
 */
class AgvControlClient : public AgvTcpClientBase
{
    Q_OBJECT
public:
    explicit AgvControlClient(const QString &ip, quint16 port,
                              QObject *parent = nullptr);
    ~AgvControlClient() override;

    /// 启动持续运动（后台 QTimer 重发）
    void startContinuousMove(double vx, double vy, double w);
    /// 停止持续运动（异步发 2000，不阻塞）
    void stopContinuousMove();

    /// 单次开环运动（不等待响应）
    void teachMoveOnce(double vx, double vy, double w, int durationMs = 100);

    /// 软急停
    void setSoftEmergency(bool status);

private slots:
    void onContinuousTick();

private:
    void stopContinuousMoveTimerOnly();

    QTimer *m_moveTimer = nullptr;
    double m_vx = 0.0, m_vy = 0.0, m_w = 0.0;
};

/**
 * @brief 导航客户端（端口 19206）
 */
class AgvNavigationClient : public AgvTcpClientBase
{
    Q_OBJECT
public:
    explicit AgvNavigationClient(const QString &ip, quint16 port,
                                 QObject *parent = nullptr);

    void goTargetPoint(const QString &stationId,
                       const QString &method = QStringLiteral("forward"),
                       double maxSpeed  = 1.0,
                       double maxWSpeed = 0.5);
    void pauseNavigation();
    void continueNavigation();
    void cancelNavigation();
};

/**
 * @brief 状态查询客户端（端口 19204）
 */
class AgvStatusClient : public AgvTcpClientBase
{
    Q_OBJECT
public:
    explicit AgvStatusClient(const QString &ip, quint16 port,
                             QObject *parent = nullptr);

    QJsonObject getAgvLocation();
    QJsonObject getLaserData();
    QJsonObject getNavigationStatus(bool simple = true);
    QJsonObject getRemoteMaps();
};

/**
 * @brief 配置客户端（端口 19207）
 *
 * 注意：4011 GET_MAP（下载地图）在 status 端口会被 AGV 拒绝
 * (ret_code=60000 "error api type")，必须走 config 端口。
 */
class AgvConfigClient : public AgvTcpClientBase
{
    Q_OBJECT
public:
    explicit AgvConfigClient(const QString &ip, quint16 port,
                             QObject *parent = nullptr);

    /// 下载指定地图数据（含 advancedPointList 导航站点）
    QJsonObject getMap(const QString &mapName);
};

/**
 * @brief 其他功能客户端（端口 19210：电机使能 / 数字输出）
 */
class AgvOtherClient : public AgvTcpClientBase
{
    Q_OBJECT
public:
    explicit AgvOtherClient(const QString &ip, quint16 port,
                            QObject *parent = nullptr);

    void setMotorEnable(const QString &motorName, bool enable);
    void setSoftEmergency(bool status);
};

// ===========================================================================
// AGV 主客户端门面（兼容 mainwindow 使用的旧接口）
// ===========================================================================

/**
 * @brief AgvTcpClient — 兼容 mainwindow 旧接口的门面类
 *
 * 旧代码期望的信号：
 *   void ctrlConnectedChanged(bool)
 *   void pushConnectedChanged(bool)
 *   void stateReceived(const AgvState &)
 *   void mapListReceived(const QVariantList &, const QString &)
 *   void mapDataReceived(const QString &, const QVariantMap &)
 *   void errorOccurred(const QString &)
 *
 * 旧代码期望的方法：
 *   void connectToHost(ip, ctrlPort, navPort, pushPort)
 *   void disconnectFromHost()
 *   void stopMotion()
 *   void softEmergencyStop()
 *   void openLoopControl(linear, angular)
 *   void startNavigation(id)
 *   void pauseNavigation()
 *   void cancelNavigation()
 *   void requestMapList()
 *   void requestMapData(mapName)
 *   bool isConnected()
 */
class AgvTcpClient : public QObject
{
    Q_OBJECT
public:
    explicit AgvTcpClient(QObject *parent = nullptr);
    ~AgvTcpClient() override;

    void connectToHost(const QString &ip,
                       quint16 ctrlPort,
                       quint16 navPort,
                       quint16 pushPort);
    void disconnectFromHost();
    bool isConnected() const;

    // 运动控制
    void stopMotion();
    void softEmergencyStop();        // 触发软急停
    void releaseSoftEmergency();     // 解除软急停
    void openLoopControl(double linear, double angular);

    // 导航
    void startNavigation(const QString &stationId);
    void pauseNavigation();
    void cancelNavigation();

    // 地图
    void requestMapList();
    void requestMapData(const QString &mapName);

signals:
    void ctrlConnectedChanged(bool);
    void pushConnectedChanged(bool);
    void stateReceived(const AgvState &);
    void mapListReceived(const QVariantList &, const QString &);
    void mapDataReceived(const QString &, const QVariantMap &);
    void errorOccurred(const QString &);

private:
    void setupManager(const QString &ip);

    AgvConnectionManager *m_mgr = nullptr;
    AgvState m_lastState;
    bool m_ctrlConnected = false;
    bool m_pushConnected = false;
};

// ===========================================================================
// Python 兼容类型别名（AGVTCPClient = AgvTcpClient）
// ===========================================================================
using AGVTCPClient = AgvTcpClient;

// ===========================================================================
// 连接管理器
// ===========================================================================

/**
 * @brief AGV 连接管理器 - 统一管理 6 路 AGV 连接
 *
 * 端口分配（与 Python 一致）：
 *   pushback 19301 / control 19205 / nav 19206 /
 *   status 19204 / other 19210 / config 19207
 */
class AgvConnectionManager : public QObject
{
    Q_OBJECT
public:
    struct Ports {
        quint16 pushback = 19301;
        quint16 control  = 19205;
        quint16 nav      = 19206;
        quint16 status   = 19204;
        quint16 other    = 19210;
        quint16 config   = 19207;
    };

    explicit AgvConnectionManager(QObject *parent = nullptr);
    ~AgvConnectionManager() override;

    /// 连接所有端口。推送端口必须成功，其余可选。返回推送端口是否成功
    bool connectAll(const QString &ip, const Ports &ports);
    void disconnectAll();

    bool isConnected() const;

    AgvPushbackClient *pushback() const { return m_pushback; }
    AgvControlClient  *control()  const { return m_control;  }
    AgvNavigationClient *navigation() const { return m_nav;  }
    AgvStatusClient   *status()   const { return m_status;   }
    AgvOtherClient    *others()   const { return m_others;   }
    AgvConfigClient   *config()   const { return m_config;   }

    void setStateCallback(std::function<void(const QJsonObject &)> cb);

signals:
    /// 各端口连接诊断信息
    void portsDiagnostic(const QStringList &lines);

private:
    QString m_ip;
    AgvPushbackClient   *m_pushback = nullptr;
    AgvControlClient    *m_control  = nullptr;
    AgvNavigationClient *m_nav      = nullptr;
    AgvStatusClient     *m_status   = nullptr;
    AgvOtherClient      *m_others   = nullptr;
    AgvConfigClient     *m_config   = nullptr;
    bool m_connected = false;
};

#endif // COMMUNICATION_AGVTCPCLIENT_H