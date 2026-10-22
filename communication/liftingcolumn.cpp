#include "liftingcolumn.h"
#include "util.h"
#include "armwebsocket.h"

#include <QDebug>
#include <QTimer>

// ============================================================================
// 错误码映射（与 Python lifting_column.py 一致）
// ============================================================================
QString LiftingColumnClient::errorText(int code)
{
    switch (code) {
    case 23260: return QStringLiteral("未知设备");
    case 23261: return QStringLiteral("设备离线");
    case 23262: return QStringLiteral("驱动器故障");
    case 23263: return QStringLiteral("设备未使能");
    case 23264: return QStringLiteral("高度超出范围");
    case 23265: return QStringLiteral("速度无效");
    case 23266: return QStringLiteral("急停触发");
    case 23267: return QStringLiteral("使能超时");
    case 23268: return QStringLiteral("设备正在运动");
    case 23269: return QStringLiteral("零点保存失败");
    case 23270: return QStringLiteral("驱动器无应答");
    case 23271: return QStringLiteral("点动方向非法");
    case 23272: return QStringLiteral("清除超时");
    case 23273: return QStringLiteral("已到达限位");
    default:    return QStringLiteral("未知错误(%1)").arg(code);
    }
}

QString LiftingColumnClient::errorTextFromObj(const QJsonObject &errObj)
{
    if (errObj.isEmpty()) return {};
    const int code = errObj.value("code").toInt(0);
    const QString msg = errObj.value("message").toString();
    if (code != 0)
        return errorText(code) + (msg.isEmpty() ? QString() : (QStringLiteral(": ") + msg));
    return msg.isEmpty() ? QStringLiteral("未知错误") : msg;
}

// ============================================================================
// 构造函数
// ============================================================================
LiftingColumnClient::LiftingColumnClient(ArmWebSocket *ws, QObject *parent)
    : QObject(parent), m_ws(ws)
{
    if (m_ws) {
        connect(m_ws, &ArmWebSocket::topicPushReceived,
                this, &LiftingColumnClient::onStatePush);
    }
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(1000);
    connect(m_pollTimer, &QTimer::timeout, this, &LiftingColumnClient::onPollTick);
}

LiftingColumnClient::~LiftingColumnClient()
{
    stopJogHeartbeat();
    stopPolling();
    unsubscribeState();
}

// ============================================================================
// 设备列表
// ============================================================================
QVector<QJsonObject> LiftingColumnClient::listDevices()
{
    if (!m_ws) return {};
    const QVariant r = m_ws->sendAndWait(QStringLiteral("liftingColumn/list"), QJsonObject(), 5000);
    if (!r.isValid()) {
        qWarning().noquote() << QStringLiteral("[liftingColumn] list 无响应");
        return {};
    }
    const QJsonObject resp = varToObj(r);
    if (resp.contains("err")) {
        qWarning().noquote() << QStringLiteral("[liftingColumn] list 失败: %1")
                                    .arg(errorTextFromObj(resp.value("err").toObject()));
        return {};
    }
    return jsonArrayToObjectVector(varToArr(resp.value("db")));
}

// ============================================================================
// 状态查询
// ============================================================================
QJsonObject LiftingColumnClient::getState(const QString &device)
{
    if (!m_ws) return {};
    QJsonObject db;
    db.insert("device", device);
    const QVariant r = m_ws->sendAndWait(QStringLiteral("liftingColumn/getState"), db, 5000);
    if (!r.isValid()) {
        qWarning().noquote() << QStringLiteral("[liftingColumn] getState(%1) 无响应").arg(device);
        return {};
    }
    const QJsonObject resp = varToObj(r);
    if (resp.contains("err")) {
        qWarning().noquote() << QStringLiteral("[liftingColumn] getState(%1) 失败: %2")
                                    .arg(device, errorTextFromObj(resp.value("err").toObject()));
        return {};
    }
    return varToObj(resp.value("db"));
}

// ============================================================================
// 订阅
// ============================================================================
bool LiftingColumnClient::subscribeState(
        std::function<void(const QVector<QJsonObject> &)> cb, int tcMs)
{
    if (!m_ws) return false;
    m_stateCb = std::move(cb);

    // 文档 34.15：订阅字段在报文顶层（不在 db 内）
    QJsonObject top;
    top.insert("topics", QStringLiteral("liftingColumn/state"));
    top.insert("tc", tcMs);
    const int id = m_ws->sendTopLevel(QStringLiteral("liftingColumnState/sub"), top);
    if (id < 0) return false;
    m_subscribed = true;

    fetchInitialState(m_knownDevices);
    startPolling();
    return true;
}

void LiftingColumnClient::unsubscribeState()
{
    if (!m_ws) return;
    stopPolling();
    if (m_subscribed) {
        QJsonObject top;
        top.insert("topics", QStringLiteral("liftingColumn/state"));
        top.insert("off", 1);
        m_ws->sendTopLevel(QStringLiteral("liftingColumnState/sub"), top);
        m_subscribed = false;
    }
    m_stateCb = nullptr;
    m_stateCache = QJsonObject();
    m_knownDevices.clear();
}

QVector<QJsonObject> LiftingColumnClient::fetchInitialState(const QStringList &devices)
{
    QStringList names = devices.isEmpty() ? m_knownDevices : devices;
    if (names.isEmpty()) return {};

    QVector<QJsonObject> states;
    for (const QString &name : names) {
        const QJsonObject s = getState(name);
        if (!s.isEmpty() && s.value("name") == name) {
            m_stateCache = s;
            states.append(s);
        }
    }
    if (!states.isEmpty() && m_stateCb)
        m_stateCb(states);
    return states;
}

void LiftingColumnClient::setKnownDevices(const QStringList &devices)
{
    const bool wasEmpty = m_knownDevices.isEmpty();
    m_knownDevices = devices;
    if (wasEmpty && !devices.isEmpty() && m_stateCb && m_subscribed)
        fetchInitialState(devices);
}

QJsonObject LiftingColumnClient::cachedState(const QString &device)
{
    if (m_stateCache.value("name") == device)
        return m_stateCache;
    return {};
}

// ============================================================================
// 状态推送处理
// ============================================================================
void LiftingColumnClient::onStatePush(const QString &topic, const QJsonObject &db)
{
    Q_UNUSED(db)
    if (topic != QStringLiteral("liftingColumn/state"))
        return;

    // 推送的 db 字段本身是 array
    const QJsonValue dv = db.value("db");
    const QJsonArray arr = dv.isArray() ? dv.toArray() : QJsonArray();
    if (arr.isEmpty()) return;

    for (const QJsonValue &v : arr) {
        const QJsonObject s = v.toObject();
        if (!s.isEmpty() && s.contains("name"))
            m_stateCache = s;
    }

    for (const QJsonValue &v : arr) {
        const QJsonObject s = v.toObject();
        const QString n = s.value("name").toString();
        if (!n.isEmpty() && !m_knownDevices.contains(n))
            m_knownDevices.append(n);
    }

    const QVector<QJsonObject> states = jsonArrayToObjectVector(arr);
    if (m_stateCb) m_stateCb(states);
    emit statePushReceived(states);
}

// ============================================================================
// 轮询
// ============================================================================
void LiftingColumnClient::startPolling()
{
    if (!m_pollTimer->isActive())
        m_pollTimer->start();
}

void LiftingColumnClient::stopPolling()
{
    if (m_pollTimer) m_pollTimer->stop();
}

void LiftingColumnClient::onPollTick()
{
    if (!m_ws || !m_ws->isConnected() || m_knownDevices.isEmpty()) return;

    QVector<QJsonObject> states;
    for (const QString &name : m_knownDevices) {
        const QJsonObject s = getState(name);
        if (!s.isEmpty() && s.value("name") == name) {
            m_stateCache = s;
            states.append(s);
        }
    }
    if (!states.isEmpty() && m_stateCb)
        m_stateCb(states);
}

// ============================================================================
// 上电 / 下电 / 清错
// ============================================================================
bool LiftingColumnClient::enable(const QString &device)
{
    return simpleCall(QStringLiteral("liftingColumn/enable"), device, QJsonObject());
}

bool LiftingColumnClient::disable(const QString &device)
{
    return simpleCall(QStringLiteral("liftingColumn/disable"), device, QJsonObject());
}

bool LiftingColumnClient::clearError(const QString &device)
{
    return simpleCall(QStringLiteral("liftingColumn/clearError"), device, QJsonObject());
}

// ============================================================================
// 运动
// ============================================================================
bool LiftingColumnClient::moveTo(const QString &device, double heightM, double speedMps)
{
    if (!m_ws) return false;
    QJsonObject db;
    db.insert("device", device);
    db.insert("height", heightM);
    if (speedMps > 0.0) db.insert("speed", speedMps);
    const QVariant r = m_ws->sendAndWait(QStringLiteral("liftingColumn/moveTo"), db, 3000);
    if (!r.isValid()) return false;
    const QJsonObject resp = varToObj(r);
    if (resp.contains("err")) {
        qWarning().noquote() << QStringLiteral("[liftingColumn] moveTo(%1) 失败: %2")
                                    .arg(device, errorTextFromObj(resp.value("err").toObject()));
        return false;
    }
    return resp.value("db").toBool();
}

bool LiftingColumnClient::stop(const QString &device)
{
    return simpleCall(QStringLiteral("liftingColumn/stop"), device, QJsonObject());
}

bool LiftingColumnClient::setZero(const QString &device)
{
    return simpleCall(QStringLiteral("liftingColumn/setZero"), device, QJsonObject(), 4.0);
}

// ============================================================================
// 点动
// ============================================================================
bool LiftingColumnClient::startJog(const QString &device, int direction,
                                  double speedMps, int timeoutMs)
{
    if (direction != 1 && direction != -1) {
        qWarning().noquote() << QStringLiteral("[liftingColumn] startJog 方向非法: %1").arg(direction);
        return false;
    }
    if (!m_ws) return false;

    stopJogHeartbeat();

    QJsonObject db;
    db.insert("device", device);
    db.insert("direction", direction);
    db.insert("timeoutMs", timeoutMs);
    if (speedMps > 0.0) db.insert("speed", speedMps);

    const QVariant r = m_ws->sendAndWait(QStringLiteral("liftingColumn/jogStart"), db, 3000);
    if (!r.isValid() || !varToObj(r).value("db").toBool()) {
        if (r.isValid() && varToObj(r).contains("err"))
            qWarning().noquote() << QStringLiteral("[liftingColumn] jogStart 失败: %1")
                                        .arg(errorTextFromObj(varToObj(r).value("err").toObject()));
        return false;
    }

    m_jogDevice = device;
    m_jogging = true;

    // 心跳用递归 QTimer 实现（每 timeoutMs/2 发一次 jogHeartbeat）
    QTimer::singleShot(0, this, [this, device, timeoutMs]() {
        if (!m_jogging || !m_ws || !m_ws->isConnected()) {
            m_jogging = false;
            return;
        }
        QJsonObject db2;
        db2.insert("device", device);
        m_ws->sendAndWait(QStringLiteral("liftingColumn/jogHeartbeat"), db2, 2000);
        if (m_jogging && m_ws && m_ws->isConnected())
            QTimer::singleShot(timeoutMs / 2, this, [this, device, timeoutMs]() {
                if (!m_jogging || !m_ws || !m_ws->isConnected()) {
                    m_jogging = false;
                    return;
                }
                QJsonObject d3; d3.insert("device", device);
                m_ws->sendAndWait(QStringLiteral("liftingColumn/jogHeartbeat"), d3, 2000);
                if (m_jogging && m_ws && m_ws->isConnected())
                    QTimer::singleShot(timeoutMs / 2, this, [this, device, timeoutMs]() {
                        startJogHeartbeat(device, timeoutMs);
                    });
                else
                    m_jogging = false;
            });
        else
            m_jogging = false;
    });
    return true;
}

void LiftingColumnClient::startJogHeartbeat(const QString &device, int timeoutMs)
{
    if (!m_jogging || !m_ws || !m_ws->isConnected())
        return;
    QJsonObject db;
    db.insert("device", device);
    m_ws->sendAndWait(QStringLiteral("liftingColumn/jogHeartbeat"), db, 2000);
    if (m_jogging && m_ws && m_ws->isConnected())
        QTimer::singleShot(timeoutMs / 2, this, [this, device, timeoutMs]() {
            startJogHeartbeat(device, timeoutMs);
        });
}

bool LiftingColumnClient::stopJog(const QString &device)
{
    stopJogHeartbeat();
    return simpleCall(QStringLiteral("liftingColumn/jogStop"), device, QJsonObject());
}

void LiftingColumnClient::stopJogHeartbeat()
{
    m_jogging = false;
    m_jogDevice.clear();
}

bool LiftingColumnClient::isJogging() const
{
    return m_jogging;
}

// ============================================================================
// 通用调用
// ============================================================================
bool LiftingColumnClient::simpleCall(const QString &action, const QString &device,
                                    const QJsonObject &extra, double timeout)
{
    if (!m_ws) return false;
    QJsonObject db;
    db.insert("device", device);
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        db.insert(it.key(), it.value());

    const int timeoutMs = static_cast<int>(timeout * 1000);
    const QVariant r = m_ws->sendAndWait(action, db, timeoutMs);
    if (!r.isValid()) return false;
    const QJsonObject resp = varToObj(r);
    if (resp.contains("err")) {
        qWarning().noquote() << QStringLiteral("[liftingColumn] %1(%2) 失败: %3")
                                    .arg(action, device,
                                         errorTextFromObj(resp.value("err").toObject()));
        return false;
    }
    return resp.value("db").toBool();
}
