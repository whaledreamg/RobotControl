#ifndef COMMUNICATION_LIFTINGCOLUMN_H
#define COMMUNICATION_LIFTINGCOLUMN_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include "util.h"
#include <QVector>
#include <functional>

class ArmWebSocket;

/**
 * @brief 升降柱通信客户端（与 Python communication/lifting_column.py 对齐）
 *
 * 复用已有的 ArmWebSocket 连接，不重起 TCP/WS。
 *
 * 设计要点：
 *   - 点动心跳：独立线程在按下按钮期间每 200ms 持续发 jogHeartbeat，
 *     松开 / 异常 / 连接断开时自动停止
 *   - 状态订阅：liftingColumnState/sub（字段在顶层）+ 首帧主动拉取 + 1s 兜底轮询
 *   - 错误码映射：API 文档三十章硬错误码 → QString
 *
 * 高度单位：API 用 m；UI 层负责 mm ↔ m 换算。
 */
class LiftingColumnClient : public QObject
{
    Q_OBJECT
public:
    explicit LiftingColumnClient(ArmWebSocket *ws, QObject *parent = nullptr);
    ~LiftingColumnClient() override;

    // ===== 设备列表 =====
    /// 列出全部升降柱及其软限位 / 速度上限。失败 / 无设备时返回空列表。
    QVector<QJsonObject> listDevices();

    // ===== 状态查询 / 推送 =====
    /// 读一台升降柱的完整状态（与推送字段同构）
    QJsonObject getState(const QString &device);

    /// 订阅 liftingColumn/state 推送（全量周期），启动兜底轮询。
    /// 回调参数为 QVector<QJsonObject>（全量推送形态）。
    bool subscribeState(std::function<void(const QVector<QJsonObject> &)> cb,
                        int tcMs = 100);
    void unsubscribeState();

    /// 主动拉一次首帧（按已知设备逐台 getState）
    QVector<QJsonObject> fetchInitialState(const QStringList &devices = {});

    /// 设置已知设备清单（供 fetchInitialState / 轮询取用）
    void setKnownDevices(const QStringList &devices);

    /// 读最近一次缓存的状态（推送未到达时为 null）
    QJsonObject cachedState(const QString &device);

    // ===== 上电控制 =====
    bool enable(const QString &device);
    bool disable(const QString &device);
    bool clearError(const QString &device);

    // ===== 运动 =====
    bool moveTo(const QString &device, double heightM, double speedMps = -1.0);
    bool stop(const QString &device);
    bool setZero(const QString &device);

    // ===== 点动 =====
    /// 开始点动。direction=1 上升，-1 下降。
    /// 成功后自动起心跳线程。
    bool startJog(const QString &device, int direction,
                  double speedMps = -1.0, int timeoutMs = 500);
    bool stopJog(const QString &device);

    /// 是否正在点动中
    bool isJogging() const;

    // ===== 错误码映射 =====
    static QString errorText(int code);
    static QString errorTextFromObj(const QJsonObject &errObj);

signals:
    void statePushReceived(const QVector<QJsonObject> &states);

private slots:
    void onStatePush(const QString &topic, const QJsonObject &db);
    void onPollTick();

private:
    bool simpleCall(const QString &action, const QString &device,
                   const QJsonObject &extra = QJsonObject(), double timeout = 3.0);
    void startPolling();
    void stopPolling();
    void stopJogHeartbeat();
    void startJogHeartbeat(const QString &device, int timeoutMs);

    ArmWebSocket *m_ws = nullptr;

    // 点动心跳状态
    QString m_jogDevice;
    bool m_jogging = false;

    // 状态推送回调
    std::function<void(const QVector<QJsonObject> &)> m_stateCb;
    // 状态缓存：device -> 最近一次状态
    QJsonObject m_stateCache;
    // 已知设备清单
    QStringList m_knownDevices;
    // 兜底轮询
    QTimer *m_pollTimer = nullptr;
    bool m_subscribed = false;
};

#endif // COMMUNICATION_LIFTINGCOLUMN_H