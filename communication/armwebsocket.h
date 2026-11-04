#ifndef COMMUNICATION_ARMWEBSOCKET_H
#define COMMUNICATION_ARMWEBSOCKET_H

#include <QObject>
#include <QWebSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QVariantList>
#include <QMutex>
#include <QWaitCondition>
#include <QTimer>
#include <functional>

/**
 * @brief 机械臂 WebSocket 通信客户端（与 Python communication/arm_websocket.py 对齐）
 *
 * 关键特性：
 *   - **单连接管理双臂**：Python 版是单条 ws 承载左右臂，C++ 旧版每臂一条，
 *     这里改为单连接，臂名只是报文里的一个字段
 *   - 报文信封：{ "id": n, "ty": "动作名", "db": {...} }
 *   - **订阅类报文字段放顶层**（robotState/sub、projectState/sub、
 *     liftingColumnState/sub），不在 db 内——这是文档硬性要求，
 *     用错会被控制器当调用式处理导致收不到推送
 *   - 订阅 + 首帧主动拉取 + 1s 轮询兜底 三重保障 UI 不空/不卡
 *
 * 升降柱与 Revo2 灵巧手复用本连接（见 liftingcolumn.h / revo2hand.h）。
 *
 * 线程模型：QWebSocket 的信号在其所属线程（即本对象线程）派发，
 * 因此请求-响应匹配采用**回调式异步**；需要同步语义时用 sendAndWait()
 * （内部跑嵌套 QEventLoop，仅可在本对象线程调用）。
 */
class ArmWebSocket : public QObject
{
    Q_OBJECT
public:
    explicit ArmWebSocket(QObject *parent = nullptr);
    ~ArmWebSocket() override;

    // ===== 连接 =====
    /// 建立连接并等待握手成功（最多 10s）
    bool connectToArm(const QString &ip, quint16 port);
    void disconnectFromArm();

    bool isConnected() const { return m_connected; }

    // ===== 基础收发 =====
    /// 发送调用式报文（db 包裹）。done 在本对象线程回调；返回 msgId（失败 -1）
    int send(const QString &action, const QJsonObject &db,
             std::function<void(const QVariant &)> done = nullptr);
    /// 发送订阅式报文（字段放顶层），返回 msgId（失败 -1）
    int sendTopLevel(const QString &action, const QJsonObject &topFields);
    /// 发送调用式报文（db 为 JSON 原始文本）——灵巧手 Modbus 需要整数数组
    int sendRaw(const QString &action, const QByteArray &jsonRaw,
                std::function<void(const QVariant &)> done = nullptr);

    /// 同步发送并等待响应（内部嵌套事件循环；仅可在本对象线程调用）
    QVariant sendAndWait(const QString &action,
                         const QJsonObject &db = QJsonObject(),
                         int timeoutMs = 5000);
    QVariant sendAndWaitRaw(const QString &action,
                            const QByteArray &jsonRaw,
                            int timeoutMs = 5000);

    /// **线程安全**版同步调用，可在任意线程调用。
    ///
    /// sendAndWait() 内部跑嵌套事件循环，而 QWebSocket 的信号只在它所属
    /// 线程派发，所以它只能在本对象线程调用——从别的线程直接调用会导致
    /// 响应信号收不到，等待方必然超时（灵巧手 Modbus 收发曾因此全部失败）。
    ///
    /// 本接口把请求调度回本对象线程执行：
    ///   - 已在该线程：直接调用 sendAndWait（无额外开销）
    ///   - 在其他线程：用 BlockingQueuedConnection 排队过去执行并等结果
    ///
    /// 对应 Python 版 communication/arm_websocket.py：那边 WS 有独立的读线程
    /// + 条件变量，跨线程 send_and_wait 天然安全。
    QVariant sendAndWaitSafe(const QString &action,
                              const QJsonObject &db = QJsonObject(),
                              int timeoutMs = 5000);
    /// 注册推送 topic 回调（recv 时在本对象线程触发）
    void registerCallback(const QString &topic,
                          std::function<void(const QJsonObject &)> cb);

    /// 诊断用：当前挂起的请求数。正常应随「响应到达 / 超时」及时归零；
    /// 残留说明有等待者被遗弃（迟到响应会回调到已失效对象上）。
    int pendingCount() const { return m_pending.size(); }

    // ===== 机械臂使能 =====
    /// 返回 {ok, errStr}（API 3.1 robot/enable、3.2 robot/disable）
    QPair<bool, QString> setArmEnabled(const QString &armName, bool enable);

    // ===== 力矩拖动（robot/drag，API 3.13）=====
    // 进入拖动模式（CSTDrag / ServoDrag，机型档案决定）；退出只能走
    // robot/disable。UI 状态靠 robot/status 推送回灌。
    static constexpr int kDragTimeoutMs       = 8000;   // 模式切到位较慢
    static constexpr int kDragStatusTimeoutMs = 2500;
    QPair<bool, QString> dragArm(const QString &armName);
    /// 异步拖动，完成后在本对象线程回调 onResult(ok, err)
    void dragArmAsync(const QString &armName,
                      std::function<void(bool, const QString &)> onResult);
    /// 判断 robot/status 的 mode 是否为拖动模式
    static bool isDragMode(const QString &mode);

    // ===== 速度 =====
    bool setSpeedOverride(double ratio);
    double getSpeedOverride(bool *ok = nullptr);

    // ===== 清错（API 3.23 robot/clearServoError）=====
    // 对指定机器人全部关节下发驱动器故障复位；每台臂的故障独立、各清各的
    bool clearServoError(const QString &armName);
    QHash<QString, bool> clearAllServoErrors(const QString &left,
                                             const QString &right);

    // ===== 工程 =====
    QVariantList listProjects();
    bool runProject(const QString &projectName);
    bool pauseProject();
    bool resumeProject();
    bool stopProject();

    // ===== 订阅 =====
    /// 订阅工程状态 + 左右臂状态（tcMs 为推送周期，文档下限 10ms）
    void subscribeAll(int tcMs);
    void unsubscribeAll();
    /// 首帧主动拉取（订阅端只在新订阅者到达时给最新一帧，重连后可能是上一会话的）
    void fetchInitialStatus();
    void startPolling(int intervalMs = 1000);
    void stopPolling();

    QString leftArmName() const  { return m_leftArm;  }
    QString rightArmName() const { return m_rightArm; }
    void setArmNames(const QString &left, const QString &right);
    int pushInterval() const { return m_pushInterval; }
    void setPushInterval(int ms) { m_pushInterval = ms; }

signals:
    void connectedChanged(bool connected);
    void robotStatusReceived(const QString &armName, const QJsonObject &db);
    void projectStatusReceived(const QJsonObject &db);
    void projectListReceived(const QVariantList &projects);
    void speedReceived(double ratio);
    void errorOccurred(const QString &error);
    /// robot/list 解析出的真实臂名（左、右），供 UI 校正显示
    void armNamesResolved(const QString &left, const QString &right);
    /// 任意 topic 推送（升降柱 / 灵巧手等复用）
    void topicPushReceived(const QString &topic, const QJsonObject &db);

private slots:
    void onTextMessageReceived(const QString &message);
    void onConnected();
    void onDisconnected();
    void onErrorOccurred(QAbstractSocket::SocketError err);
    void onPollTick();

private:
    void handleMessage(const QJsonObject &obj);
    void dispatchPush(const QString &topic, const QJsonObject &db);
    void completePending(int id, const QJsonObject &obj);
    int  nextMsgId();
    /// 错误提取：err.message / err.code
    static QString errText(const QJsonObject &resp);
    /// 用 robot/list 的真实臂名改写 m_leftArm / m_rightArm。
    /// 文档 §2.1/§4.3：订阅主题必须用真实臂名，写错则整条订阅被拒（25004），
    /// 此时连接正常但永远收不到推送。
    void resolveArmNames();

    QWebSocket *m_socket = nullptr;
    bool m_connected = false;

    int m_nextId = 0;
    struct Pending {
        QJsonObject obj;
        bool done = false;
        std::function<void(const QVariant &)> cb;
    };
    QHash<int, Pending> m_pending;

    /// 轮询重入保护：onPollTick 内部是阻塞式 sendAndWait，嵌套事件循环
    /// 会让 1s 定时器在等待期间再次触发本槽，导致轮询递归堆积
    bool m_polling = false;

    QHash<QString, std::function<void(const QJsonObject &)>> m_callbacks;

    QString m_leftArm  = QStringLiteral("LeftArm");
    QString m_rightArm = QStringLiteral("RightArm");
    int m_pushInterval = 100;

    QTimer *m_pollTimer = nullptr;
    bool m_subscribed = false;
};

#endif // COMMUNICATION_ARMWEBSOCKET_H