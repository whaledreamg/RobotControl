#include "armwebsocket.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>
#include <QUrl>
#include <QDebug>

// 调试开关（与 Python ARM_DEBUG_PUSH 一致）
static bool armDebugPush()
{
    static const bool on = qEnvironmentVariable("ARM_DEBUG_PUSH") == "1";
    return on;
}

/// 安全转换 QVariant → QJsonObject（兼容 Qt 5/6）
static inline QJsonObject varToObj(const QVariant &v)
{
    if (!v.isValid())
        return {};
    if (v.canConvert<QJsonObject>())
        return v.value<QJsonObject>();
    // Qt 6: QVariant → QJsonValue → QJsonObject
    return v.toJsonValue().toObject();
}

/// 安全转换 QVariant → QJsonArray
static inline QJsonArray varToArr(const QVariant &v)
{
    if (!v.isValid())
        return {};
    if (v.canConvert<QJsonArray>())
        return v.value<QJsonArray>();
    return v.toJsonValue().toArray();
}

ArmWebSocket::ArmWebSocket(QObject *parent) : QObject(parent)
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(1000);
    connect(m_pollTimer, &QTimer::timeout, this, &ArmWebSocket::onPollTick);
}

ArmWebSocket::~ArmWebSocket()
{
    disconnectFromArm();
}

// ---------------------------------------------------------------------------
// 连接管理
// ---------------------------------------------------------------------------
bool ArmWebSocket::connectToArm(const QString &ip, quint16 port)
{
    const QString url = QStringLiteral("ws://%1:%2").arg(ip).arg(port);
    qInfo().noquote() << QStringLiteral("[Arm WS] 正在连接 %1 ...").arg(url);

    if (!m_socket) {
        m_socket = new QWebSocket();
        m_socket->setParent(this);
        connect(m_socket, &QWebSocket::textMessageReceived,
                this, &ArmWebSocket::onTextMessageReceived);
        connect(m_socket, &QWebSocket::connected,
                this, &ArmWebSocket::onConnected);
        connect(m_socket, &QWebSocket::disconnected,
                this, &ArmWebSocket::onDisconnected);
        connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(
                            &QWebSocket::error),
                this, &ArmWebSocket::onErrorOccurred);
    }

    // 等待握手（最多 10s）
    QEventLoop loop;
    QTimer connTimer;
    connTimer.setSingleShot(true);
    bool ok = false;
    connect(&connTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(m_socket, &QWebSocket::connected, &loop, [&loop, &ok]() {
        ok = true;
        loop.quit();
    });
    connect(m_socket, &QWebSocket::disconnected, &loop, &QEventLoop::quit);
    m_socket->open(QUrl(url));
    connTimer.start(10000);
    loop.exec();
    connTimer.stop();

    if (!ok) {
        qWarning().noquote() << QStringLiteral("[Arm WS] 连接超时/失败: %1 — %2")
                                    .arg(url, m_socket->errorString());
        m_socket->abort();
        return false;
    }

    m_connected = true;
    emit connectedChanged(true);
    // 等待连接稳定后再订阅（控制器握手后需要一点时间才能接受订阅）
    QTimer::singleShot(500, this, [this]() { subscribeAll(m_pushInterval); });
    return true;
}

void ArmWebSocket::disconnectFromArm()
{
    stopPolling();
    m_subscribed = false;
    m_pending.clear();
    if (m_socket) {
        m_socket->close();
        m_socket->abort();
    }
    if (m_connected) {
        m_connected = false;
        emit connectedChanged(false);
    }
}

void ArmWebSocket::onConnected()
{
    qInfo().noquote() << QStringLiteral("[Arm WS] 连接已打开");
}

void ArmWebSocket::onDisconnected()
{
    qInfo().noquote() << QStringLiteral("[Arm WS] 连接已关闭");
    if (m_connected) {
        m_connected = false;
        emit connectedChanged(false);
    }
    // 断开后唤醒所有挂起的回调（以"无响应"方式），避免 UI 永久等待
    const auto pendingCopy = m_pending;
    m_pending.clear();
    for (auto it = pendingCopy.constBegin(); it != pendingCopy.constEnd(); ++it) {
        if (it->cb) it->cb(QVariant());
    }
}

void ArmWebSocket::onErrorOccurred(QAbstractSocket::SocketError err)
{
    Q_UNUSED(err)
    qWarning().noquote() << QStringLiteral("[Arm WS] 错误: %1")
                                .arg(m_socket ? m_socket->errorString() : QString());
    if (m_connected) {
        m_connected = false;
        emit connectedChanged(false);
    }
}

// ---------------------------------------------------------------------------
// 收发
// ---------------------------------------------------------------------------
int ArmWebSocket::nextMsgId()
{
    m_nextId = (m_nextId + 1) % 100000;
    return m_nextId;
}

QString ArmWebSocket::errText(const QJsonObject &resp)
{
    const QJsonObject err = resp.value("err").toObject();
    if (err.isEmpty())
        return {};
    const QString msg = err.value("message").toString();
    return msg.isEmpty() ? QStringLiteral("未知错误") : msg;
}

int ArmWebSocket::send(const QString &action, const QJsonObject &db,
                       std::function<void(const QVariant &)> done)
{
    return sendRaw(action,
                   QJsonDocument(db).toJson(QJsonDocument::Compact),
                   std::move(done));
}

int ArmWebSocket::sendRaw(const QString &action, const QByteArray &jsonRaw,
                          std::function<void(const QVariant &)> done)
{
    if (!m_connected || !m_socket) {
        if (done) done(QVariant());
        return -1;
    }

    const int id = nextMsgId();

    QJsonObject msg;
    msg.insert("id", id);
    msg.insert("ty", action);
    // jsonRaw 允许调用方传任意合法 JSON（含数组），这里统一塞进 db
    QJsonParseError perr {};
    const QJsonDocument d = QJsonDocument::fromJson(jsonRaw, &perr);
    if (perr.error == QJsonParseError::NoError)
        msg.insert("db", d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object()));
    else
        msg.insert("db", QJsonObject());

    if (done) {
        Pending p;
        p.cb = std::move(done);
        m_pending.insert(id, std::move(p));
    }

    const QByteArray outgoing =
        QJsonDocument(msg).toJson(QJsonDocument::Compact);
    m_socket->sendTextMessage(QString::fromUtf8(outgoing));
    if (armDebugPush())
        qInfo().noquote() << QStringLiteral("[Arm WS] 发送: %1")
                                 .arg(QString::fromUtf8(outgoing));
    return id;
}

int ArmWebSocket::sendTopLevel(const QString &action, const QJsonObject &topFields)
{
    if (!m_connected || !m_socket)
        return -1;

    const int id = nextMsgId();
    QJsonObject msg;
    msg.insert("id", id);
    msg.insert("ty", action);
    // 订阅字段（topics / tc / off / ip / port）在报文**顶层**，不在 db 内
    for (auto it = topFields.constBegin(); it != topFields.constEnd(); ++it)
        msg.insert(it.key(), it.value());

    const QByteArray raw =
        QJsonDocument(msg).toJson(QJsonDocument::Compact);
    m_socket->sendTextMessage(QString::fromUtf8(raw));
    if (armDebugPush())
        qInfo().noquote() << QStringLiteral("[Arm WS] 订阅发送: %1")
                                 .arg(QString::fromUtf8(raw));
    return id;
}

QVariant ArmWebSocket::sendAndWait(const QString &action,
                                   const QJsonObject &db, int timeoutMs)
{
    return sendAndWaitRaw(action,
                          QJsonDocument(db).toJson(QJsonDocument::Compact),
                          timeoutMs);
}

QVariant ArmWebSocket::sendAndWaitRaw(const QString &action,
                                      const QByteArray &jsonRaw, int timeoutMs)
{
    QVariant result;
    bool done = false;

    sendRaw(action, jsonRaw, [&](const QVariant &r) {
        result = r;
        done = true;
    });

    if (!done) {
        // send 失败（未连接）已同步回调，返回空
        return {};
    }

    // 嵌套事件循环等待响应（QWebSocket 信号在事件循环中派发）
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    // 连接断开时立即退出等待
    auto c = connect(m_socket, &QWebSocket::disconnected, &loop, &QEventLoop::quit);
    deadline.start(timeoutMs);
    while (!done && deadline.isActive())
        loop.exec(QEventLoop::AllEvents);
    deadline.stop();
    disconnect(c);

    return done ? result : QVariant();
}

// ---------------------------------------------------------------------------
// 消息处理
// ---------------------------------------------------------------------------
void ArmWebSocket::onTextMessageReceived(const QString &message)
{
    QJsonParseError err {};
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning().noquote() << QStringLiteral("[Arm WS] 解析消息失败: %1")
                                    .arg(message.left(200));
        return;
    }
    handleMessage(doc.object());
}

void ArmWebSocket::completePending(int id, const QJsonObject &obj)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    auto cb = it->cb;
    m_pending.erase(it);
    if (cb) cb(QVariant(obj));
}

void ArmWebSocket::handleMessage(const QJsonObject &obj)
{
    const int id = obj.value("id").toInt(-1);
    const QString ty = obj.value("ty").toString();

    if (armDebugPush()) {
        qInfo().noquote() << QStringLiteral("[Arm WS] 收到: id=%1 ty=%2").arg(id).arg(ty);
    }

    // 1) 响应：msgId 命中 pending
    if (id >= 0 && m_pending.contains(id)) {
        completePending(id, obj);
        return;
    }

    // 2) 订阅 ack / 调用响应：带 id 但无 pending
    if (id >= 0) {
        if (obj.contains("err")) {
            qWarning().noquote()
                << QStringLiteral("[Arm WS] 响应错误: id=%1 ty=%2 err=%3")
                       .arg(id).arg(ty, errText(obj));
        }
        return;
    }

    // 3) 推送：ty 形如 "publish/project/status"
    if (ty.contains("publish")) {
        QString topic = ty;
        topic.remove(QStringLiteral("publish/"));
        const QJsonObject db = obj.value("db").toObject();
        dispatchPush(topic, db);
    }
}

void ArmWebSocket::dispatchPush(const QString &topic, const QJsonObject &db)
{
    emit topicPushReceived(topic, db);

    // 统一调用：命中并执行回调返回 true（std::function 的 operator bool）
    const auto invoke = [&db](const std::function<void(const QJsonObject &)> &f) -> bool {
        if (!f) return false;
        f(db);
        return true;
    };
    const auto tryKey = [this, &invoke](const QString &key) -> bool {
        auto it = m_callbacks.constFind(key);
        return it != m_callbacks.constEnd() && invoke(it.value());
    };

    // 1) 精确匹配注册的 topic
    if (tryKey(topic))
        return;

    // 2) 兼容别名
    if (topic == QStringLiteral("project/status")) {
        if (tryKey(QStringLiteral("project_status")))
            return;
    }
    if (topic == QStringLiteral("robot/%1/status").arg(m_leftArm)) {
        tryKey(QStringLiteral("left_arm_status"));
        return;
    }
    if (topic == QStringLiteral("robot/%1/status").arg(m_rightArm)) {
        tryKey(QStringLiteral("right_arm_status"));
        return;
    }

    if (armDebugPush()) {
        qInfo().noquote() << QStringLiteral("[Arm WS] 推送未匹配: topic=%1").arg(topic);
    }
}

// ---------------------------------------------------------------------------
// 使能
// ---------------------------------------------------------------------------
QPair<bool, QString> ArmWebSocket::setArmEnabled(const QString &armName, bool enable)
{
    if (armName.isEmpty())
        return {false, QStringLiteral("缺少 arm_name")};
    if (!isConnected())
        return {false, QStringLiteral("WebSocket 未连接")};

    const QString action = enable ? QStringLiteral("robot/enable")
                                 : QStringLiteral("robot/disable");
    const QString label  = QString(enable ? "上使能" : "下使能") + QStringLiteral("(") + armName + QStringLiteral(")");

    QJsonObject db;
    db.insert("name", armName);
    const QVariant r = sendAndWait(action, db, 5000);
    if (!r.isValid())
        return {false, QStringLiteral("%1 超时（5s）").arg(label)};

    const QJsonObject resp = varToObj(r);
    if (resp.contains("err"))
        return {false, QStringLiteral("%1 被拒：%2").arg(label, errText(resp))};
    if (resp.value("db").toBool() == false && resp.value("db").isBool())
        return {false, QStringLiteral("%1 失败（db=false）").arg(label)};
    if (!resp.contains("db"))
        return {false, QStringLiteral("%1 响应格式异常").arg(label)};
    return {true, {}};
}

// ---------------------------------------------------------------------------
// 力矩拖动
// ---------------------------------------------------------------------------
QPair<bool, QString> ArmWebSocket::dragArm(const QString &armName)
{
    if (armName.isEmpty())
        return {false, QStringLiteral("缺少 arm_name")};
    if (!isConnected())
        return {false, QStringLiteral("WebSocket 未连接")};

    QJsonObject db;
    db.insert("name", armName);
    const QVariant r = sendAndWait(QStringLiteral("robot/drag"), db, kDragTimeoutMs);
    if (!r.isValid())
        return {false, QStringLiteral("进入拖动(%1) 超时（8s）").arg(armName)};

    const QJsonObject resp = varToObj(r);
    if (resp.contains("err"))
        return {false, QStringLiteral("robot/drag(%1) 被拒：%2")
                        .arg(armName, errText(resp))};
    if (resp.value("db").toBool())
        return {true, {}};
    return {false, QStringLiteral("robot/drag(%1) 响应异常").arg(armName)};
}

void ArmWebSocket::dragArmAsync(const QString &armName,
                                std::function<void(bool, const QString &)> onResult)
{
    // 避免阻塞调用线程（UI 按钮），走异步回调
    QTimer::singleShot(0, this, [this, armName, onResult]() {
        const auto r = dragArm(armName);
        if (onResult) onResult(r.first, r.second);
    });
}

bool ArmWebSocket::isDragMode(const QString &mode)
{
    return mode == QStringLiteral("CSTDrag") || mode == QStringLiteral("ServoDrag");
}

// ---------------------------------------------------------------------------
// 速度
// ---------------------------------------------------------------------------
bool ArmWebSocket::setSpeedOverride(double ratio)
{
    ratio = qBound(0.0, ratio, 1.0);
    QJsonObject db;
    db.insert("ratio", ratio);
    const QVariant r = sendAndWait(QStringLiteral("device/setSpeedOverride"), db);
    return r.isValid() && varToObj(r).value("db").toBool();
}

double ArmWebSocket::getSpeedOverride(bool *ok)
{
    const QVariant r = sendAndWait(QStringLiteral("device/getSpeedOverride"));
    if (ok) *ok = r.isValid();
    if (r.isValid())
        return varToObj(r).value("db").toObject().value("ratio").toDouble(1.0);
    return 1.0;
}

// ---------------------------------------------------------------------------
// 清错
// ---------------------------------------------------------------------------
bool ArmWebSocket::clearServoError(const QString &armName)
{
    if (armName.isEmpty())
        return false;
    QJsonObject db;
    db.insert("name", armName);
    const QVariant r = sendAndWait(QStringLiteral("robot/clearServoError"), db, 3000);
    if (!r.isValid()) {
        qWarning().noquote() << QStringLiteral("[Arm WS] clearServoError(%1) 无响应").arg(armName);
        return false;
    }
    const QJsonObject resp = varToObj(r);
    if (resp.contains("err")) {
        qWarning().noquote() << QStringLiteral("[Arm WS] clearServoError(%1) 失败: %2")
                                    .arg(armName, errText(resp));
        return false;
    }
    return resp.value("db").toBool();
}

QHash<QString, bool> ArmWebSocket::clearAllServoErrors(const QString &left,
                                                       const QString &right)
{
    QHash<QString, bool> results;
    if (!left.isEmpty())  results.insert(left,  clearServoError(left));
    if (!right.isEmpty()) results.insert(right, clearServoError(right));
    return results;
}

// ---------------------------------------------------------------------------
// 工程
// ---------------------------------------------------------------------------
QVariantList ArmWebSocket::listProjects()
{
    const QVariant r = sendAndWait(QStringLiteral("project/listProjects"));
    if (!r.isValid())
        return {};
    return varToObj(r).value("db").toArray().toVariantList();
}

bool ArmWebSocket::runProject(const QString &projectName)
{
    QJsonObject db;
    db.insert("name", projectName);
    const QVariant r = sendAndWait(QStringLiteral("project/run"), db);
    return r.isValid() && varToObj(r).value("db").toBool();
}

bool ArmWebSocket::pauseProject()
{
    const QVariant r = sendAndWait(QStringLiteral("project/pause"));
    return r.isValid() && varToObj(r).value("db").toBool();
}

bool ArmWebSocket::resumeProject()
{
    const QVariant r = sendAndWait(QStringLiteral("project/resume"));
    return r.isValid() && varToObj(r).value("db").toBool();
}

bool ArmWebSocket::stopProject()
{
    const QVariant r = sendAndWait(QStringLiteral("project/stop"));
    return r.isValid() && varToObj(r).value("db").toBool();
}

// ---------------------------------------------------------------------------
// 订阅
// ---------------------------------------------------------------------------
void ArmWebSocket::setArmNames(const QString &left, const QString &right)
{
    m_leftArm  = left;
    m_rightArm = right;
}

void ArmWebSocket::subscribeAll(int tcMs)
{
    if (!isConnected())
        return;
    m_pushInterval = tcMs;
    m_subscribed = true;

    qInfo().noquote() << QStringLiteral(
        "[Arm WS] 订阅推送: project/status, robot/%1/status, robot/%2/status, interval=%3ms")
        .arg(m_leftArm, m_rightArm).arg(tcMs);

    // 工程状态订阅（一次带多主题；topics 接受 string[]）
    {
        QJsonObject top;
        QJsonArray topics;
        topics.append(QStringLiteral("project/status"));
        top.insert("topics", topics);
        top.insert("tc", tcMs);
        sendTopLevel(QStringLiteral("projectState/sub"), top);
    }
    // 机械臂状态订阅：左右臂各一条（topic 含臂名，不能并入工程那批）
    {
        QJsonObject top;
        top.insert("topics", QStringLiteral("robot/%1/status").arg(m_leftArm));
        top.insert("tc", tcMs);
        sendTopLevel(QStringLiteral("robotState/sub"), top);
    }
    {
        QJsonObject top;
        top.insert("topics", QStringLiteral("robot/%1/status").arg(m_rightArm));
        top.insert("tc", tcMs);
        sendTopLevel(QStringLiteral("robotState/sub"), top);
    }

    // 主动拉一次首帧，让 UI 立刻拿到当下真实值
    fetchInitialStatus();
    // 兜底轮询
    startPolling(1000);
}

void ArmWebSocket::fetchInitialStatus()
{
    const auto dispatch = [this](const QString &topic, const QJsonObject &db) {
        if (!db.isEmpty())
            dispatchPush(topic, db);
    };

    // 工程状态首帧
    if (const QVariant r = sendAndWait(QStringLiteral("project/status"), {}, 2000);
        r.isValid()) {
        dispatch(QStringLiteral("project/status"), varToObj(r).value("db").toObject());
    }
    // 左右臂首帧
    if (const QVariant r = sendAndWait(QStringLiteral("robot/status"),
                                       [&] { QJsonObject d; d.insert("name", m_leftArm); return d; }(),
                                       kDragStatusTimeoutMs);
        r.isValid()) {
        dispatch(QStringLiteral("robot/%1/status").arg(m_leftArm),
                 varToObj(r).value("db").toObject());
    }
    if (const QVariant r = sendAndWait(QStringLiteral("robot/status"),
                                       [&] { QJsonObject d; d.insert("name", m_rightArm); return d; }(),
                                       kDragStatusTimeoutMs);
        r.isValid()) {
        dispatch(QStringLiteral("robot/%1/status").arg(m_rightArm),
                 varToObj(r).value("db").toObject());
    }
}

void ArmWebSocket::unsubscribeAll()
{
    if (!isConnected())
        return;
    {
        QJsonObject top;
        top.insert("topics", QStringLiteral("project/status"));
        top.insert("off", 1);
        sendTopLevel(QStringLiteral("projectState/sub"), top);
    }
    {
        QJsonObject top;
        top.insert("topics", QStringLiteral("robot/%1/status").arg(m_leftArm));
        top.insert("off", 1);
        sendTopLevel(QStringLiteral("robotState/sub"), top);
    }
    {
        QJsonObject top;
        top.insert("topics", QStringLiteral("robot/%1/status").arg(m_rightArm));
        top.insert("off", 1);
        sendTopLevel(QStringLiteral("robotState/sub"), top);
    }
    m_subscribed = false;
}

void ArmWebSocket::startPolling(int intervalMs)
{
    m_pollTimer->setInterval(intervalMs);
    if (!m_pollTimer->isActive())
        m_pollTimer->start();
}

void ArmWebSocket::stopPolling()
{
    if (m_pollTimer)
        m_pollTimer->stop();
}

void ArmWebSocket::onPollTick()
{
    if (!isConnected())
        return;
    // 兜底：即使推送丢帧/不变化，UI 也能拿到真实值
    const auto poll = [this](const QString &topic, const QJsonObject &db) {
        if (!db.isEmpty())
            dispatchPush(topic, db);
    };

    if (const QVariant r = sendAndWait(QStringLiteral("project/status"), {}, 2000);
        r.isValid())
        poll(QStringLiteral("project/status"), varToObj(r).value("db").toObject());

    QJsonObject dl; dl.insert("name", m_leftArm);
    if (const QVariant r = sendAndWait(QStringLiteral("robot/status"), dl, 2000);
        r.isValid())
        poll(QStringLiteral("robot/%1/status").arg(m_leftArm),
             varToObj(r).value("db").toObject());

    QJsonObject dr; dr.insert("name", m_rightArm);
    if (const QVariant r = sendAndWait(QStringLiteral("robot/status"), dr, 2000);
        r.isValid())
        poll(QStringLiteral("robot/%1/status").arg(m_rightArm),
             varToObj(r).value("db").toObject());
}

void ArmWebSocket::registerCallback(const QString &topic,
                                    std::function<void(const QJsonObject &)> cb)
{
    if (cb) m_callbacks.insert(topic, std::move(cb));
    else    m_callbacks.remove(topic);
    if (armDebugPush())
        qInfo().noquote() << QStringLiteral("[Arm WS] 注册回调 topic=%1").arg(topic);
}