#include "agvtcpclient.h"
#include "../core/settingsmanager.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>
#include <QThread>
#include <QDebug>
#include <QMutexLocker>

/// 安全转换 QVariant → QJsonObject（兼容 Qt 5/6）
static inline QJsonObject varToObj(const QVariant &v)
{
    if (!v.isValid()) return {};
    if (v.canConvert<QJsonObject>()) return v.value<QJsonObject>();
    return v.toJsonValue().toObject();
}

/// 安全转换 QVariant → QJsonArray（兼容 Qt 5/6）
static inline QJsonArray varToArr(const QVariant &v)
{
    if (!v.isValid()) return {};
    if (v.canConvert<QJsonArray>()) return v.value<QJsonArray>();
    return v.toJsonValue().toArray();
}

// 调试开关（与 Python 版 AGV_DEBUG_DUMP 环境变量一致）
static bool agvDebugDump()
{
    static const bool on = qEnvironmentVariableIsSet("AGV_DEBUG_DUMP")
                          && qEnvironmentVariable("AGV_DEBUG_DUMP") == "1";
    return on;
}

// ===========================================================================
// AgvTcpClientBase
// ===========================================================================
AgvTcpClientBase::AgvTcpClientBase(const QString &ip, quint16 port,
                                   const QString &name, QObject *parent)
    : QObject(parent), m_ip(ip), m_port(port), m_name(name)
{
}

AgvTcpClientBase::~AgvTcpClientBase()
{
    disconnectFromHost();
}

quint16 AgvTcpClientBase::nextSeq()
{
    QMutexLocker lock(&m_mutex);
    m_seq = static_cast<quint16>((m_seq + 1) & 0xFFFF);
    return m_seq;
}

void AgvTcpClientBase::setConnected(bool c)
{
    if (m_connected == c)
        return;
    m_connected = c;
    emit connectedChanged(m_name, c);
}

bool AgvTcpClientBase::connectToHost()
{
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState)
        return true;

    if (!m_socket) {
        m_socket = new QTcpSocket(this);
        connect(m_socket, &QTcpSocket::readyRead, this, &AgvTcpClientBase::onReadyRead);
        connect(m_socket, &QTcpSocket::errorOccurred,
                this, &AgvTcpClientBase::onSocketError);
        connect(m_socket, &QTcpSocket::stateChanged,
                this, &AgvTcpClientBase::onSocketStateChanged);
    }

    m_buffer.clear();
    m_socket->connectToHost(m_ip, m_port);
    const bool ok = m_socket->waitForConnected(5000);
    if (ok) {
        qInfo().noquote() << QStringLiteral("[%1] 连接成功 %2:%3")
                                 .arg(m_name, m_ip).arg(m_port);
        setConnected(true);
    } else {
        qWarning().noquote() << QStringLiteral("[%1] 连接失败 %2:%3 — %4")
                                    .arg(m_name, m_ip)
                                    .arg(m_port)
                                    .arg(m_socket->errorString());
        setConnected(false);
    }
    return ok;
}

void AgvTcpClientBase::disconnectFromHost()
{
    // 唤醒所有在条件变量上等待的 send 线程，让它们立即退出
    {
        QMutexLocker lock(&m_mutex);
        m_pending.clear();
        m_respCond.wakeAll();
    }
    if (m_socket) {
        m_socket->abort();
    }
    m_buffer.clear();
    setConnected(false);
}

bool AgvTcpClientBase::writeFrame(const QByteArray &frame)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return false;
    return m_socket->write(frame) == frame.size();
}

void AgvTcpClientBase::dumpTx(const QByteArray &frame, quint16 cmdId) const
{
    const auto *p = reinterpret_cast<const uchar *>(frame.constData());
    const quint32 dlen = qFromBigEndian<quint32>(p + 4);
    const QString body = QString::fromUtf8(frame.mid(AgvProtocol::HEADER_SIZE, dlen));
    qInfo().noquote() << QStringLiteral(
        "\n===== [%1] → AGV 下行报文 =====\n"
        "  同步头:   0x%2\n"
        "  协议版本: %3\n"
        "  序号:     %4\n"
        "  数据长度: %5 字节\n"
        "  命令号:   %6 (0x%7)\n"
        "  JSON体:   %8\n"
        "==============================")
        .arg(m_name)
        .arg(p[0], 2, 16, QChar('0').toUpper())
        .arg(p[1]).arg(qFromBigEndian<quint16>(p + 2)).arg(dlen).arg(cmdId)
        .arg(cmdId, 4, 16, QChar('0'))
        .arg(body);
}

QVariant AgvTcpClientBase::sendSyncRaw(uint16_t cmdId, const QByteArray &json)
{
    if (!isConnected())
        return {};

    const quint16 seq = nextSeq();
    {
        QMutexLocker lock(&m_mutex);
        m_pending.insert(seq, QVariant());      // 提前注册占位
    }

    const QByteArray frame = AgvProtocol::buildFrame(cmdId, json, seq);
    if (agvDebugDump())
        dumpTx(frame, cmdId);

    if (!writeFrame(frame)) {
        QMutexLocker lock(&m_mutex);
        m_pending.remove(seq);
        qWarning().noquote() << QStringLiteral("[%1] 发送失败 cmd=%2").arg(m_name).arg(cmdId);
        return {};
    }

    QMutexLocker lock(&m_mutex);
    QElapsedTimer timer;
    timer.start();
    while (!m_pending.contains(seq) || !m_pending[seq].isValid()) {
        const qint64 remain = m_sendTimeout * 1000 - timer.elapsed();
        if (remain <= 0) {
            m_pending.remove(seq);
            qWarning().noquote() << QStringLiteral("[%1] 命令 %2 等待响应超时")
                                        .arg(m_name).arg(cmdId);
            return {};
        }
        // 断开了也要退出等待
        if (!m_connected) {
            m_pending.remove(seq);
            return {};
        }
        m_respCond.wait(&m_mutex, static_cast<unsigned long>(remain));
    }
    return m_pending.take(seq);
}

QVariant AgvTcpClientBase::sendSync(uint16_t cmdId, const QJsonObject &obj)
{
    return sendSyncRaw(cmdId, QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QVariant AgvTcpClientBase::sendSync(uint16_t cmdId)
{
    // Seer 协议对无参命令（1300/1004 等）要求空 JSON 体，
    // 用 "null" 会被 AGV 拒绝（ret_code=60002 "data is not JSON object"）
    return sendSyncRaw(cmdId, QByteArray());
}

bool AgvTcpClientBase::sendNoWait(uint16_t cmdId, const QJsonObject &obj)
{
    if (!isConnected())
        return false;

    const quint16 seq = nextSeq();
    {
        QMutexLocker lock(&m_mutex);
        m_pending.insert(seq, QVariant());   // 响应到达后被填上然后丢弃
    }
    const QByteArray frame =
        AgvProtocol::buildFrame(cmdId, obj, seq);
    if (!writeFrame(frame)) {
        QMutexLocker lock(&m_mutex);
        m_pending.remove(seq);
        return false;
    }
    return true;
}

void AgvTcpClientBase::sendAsync(uint16_t cmdId, const QJsonObject &obj,
                                 std::function<void(const QVariant &)> cb)
{
    QTimer::singleShot(0, this, [this, cmdId, obj, cb]() {
        const QVariant r = sendSync(cmdId, obj);
        if (cb) cb(r);
    });
}

QJsonObject AgvTcpClientBase::frameObject(const AgvProtocol::Frame &frame)
{
    if (frame.objValid)
        return frame.obj;
    QJsonParseError err {};
    const QJsonDocument doc = QJsonDocument::fromJson(frame.rawJson, &err);
    if (err.error == QJsonParseError::NoError && doc.isObject())
        return doc.object();
    return {};
}

void AgvTcpClientBase::handlePush(const AgvProtocol::Frame &) {}

void AgvTcpClientBase::onReadyRead()
{
    if (!m_socket)
        return;
    m_buffer.append(m_socket->readAll());

    // 帧同步：buffer 开头不是 0x5A 时，丢弃到下一个同步头重新对齐
    const int drop = AgvProtocol::resync(m_buffer);
    if (drop > 0) {
        qWarning().noquote() << QStringLiteral("[%1] 帧未对齐，丢弃 %2 字节")
                                    .arg(m_name).arg(drop);
        m_buffer.remove(0, drop);
    }

    processBuffer();
}

void AgvTcpClientBase::processBuffer()
{
    while (true) {
        AgvProtocol::Frame frame;
        if (!AgvProtocol::parseFrame(m_buffer, frame)) {
            // 不完整帧，等待更多数据
            break;
        }
        m_buffer.remove(0, frame.totalSize);
        ++m_rxCount;

        // 上行报文诊断（前 5 条 + 每 50 条）
        if (m_rxCount <= 5 || m_rxCount % 50 == 1) {
            qInfo().noquote() << QStringLiteral("[%1] ← AGV 上行报文 (第%2条) cmd=%3 seq=%4 json=%5")
                                        .arg(m_name).arg(m_rxCount).arg(frame.command)
                                        .arg(frame.seq)
                                        .arg(QString::fromUtf8(frame.rawJson.left(300)));
        }

        // 先尝试匹配 pending 请求
        bool matched = false;
        {
            QMutexLocker lock(&m_mutex);
            if (m_pending.contains(frame.seq)) {
                const QJsonObject obj = frameObject(frame);
                m_pending[frame.seq] = obj.isEmpty() ? QVariant(QString::fromUtf8(frame.rawJson))
                                                    : QVariant(obj);
                m_respCond.wakeAll();
                matched = true;
            }
        }
        if (matched)
            continue;

        // 未匹配 → 按推送处理
        handlePush(frame);
        const QJsonObject obj = frameObject(frame);
        if (!obj.isEmpty())
            emit pushReceived(m_name, obj);
    }
}

void AgvTcpClientBase::onSocketError(QAbstractSocket::SocketError err)
{
    // RemoteHostClosedError / ConnectionResetError 属于正常断开
    if (err == QAbstractSocket::RemoteHostClosedError) {
        setConnected(false);
        QMutexLocker lock(&m_mutex);
        m_pending.clear();
        m_respCond.wakeAll();
        return;
    }
    qWarning().noquote() << QStringLiteral("[%1] Socket 错误: %2")
                                .arg(m_name, m_socket ? m_socket->errorString() : QString());
    setConnected(false);
}

void AgvTcpClientBase::onSocketStateChanged(QAbstractSocket::SocketState state)
{
    if (state == QAbstractSocket::ConnectedState) {
        setConnected(true);
    } else if (state == QAbstractSocket::UnconnectedState) {
        setConnected(false);
        QMutexLocker lock(&m_mutex);
        m_pending.clear();
        m_respCond.wakeAll();
    }
}

// ===========================================================================
// AgvPushbackClient
// ===========================================================================
AgvPushbackClient::AgvPushbackClient(const QString &ip, quint16 port, QObject *parent)
    : AgvTcpClientBase(ip, port, QStringLiteral("Pushback"), parent)
{
}

void AgvPushbackClient::setStateCallback(std::function<void(const QJsonObject &)> cb)
{
    m_stateCb = std::move(cb);
}

void AgvPushbackClient::handlePush(const AgvProtocol::Frame &frame)
{
    const QJsonObject obj = frameObject(frame);
    if (obj.isEmpty())
        return;

    ++m_pushCount;

    // 关键：合并到 m_lastState，保留之前已知的字段。
    // AGV 推送的多个类型（DI 局部 / 位置+电池 / 错误…）合并成完整状态
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
        m_lastState.insert(it.key(), it.value());

    if (m_stateCb)
        m_stateCb(m_lastState);
}

// ===========================================================================
// AgvControlClient
// ===========================================================================
AgvControlClient::AgvControlClient(const QString &ip, quint16 port, QObject *parent)
    : AgvTcpClientBase(ip, port, QStringLiteral("Control"), parent)
{
    // 控制类命令超时短一些，避免长按方向键累积请求
    setSendTimeout(1.5);
    m_moveTimer = new QTimer(this);
    m_moveTimer->setInterval(100);      // 100ms 一次
    connect(m_moveTimer, &QTimer::timeout, this, &AgvControlClient::onContinuousTick);
}

AgvControlClient::~AgvControlClient() = default;

void AgvControlClient::startContinuousMove(double vx, double vy, double w)
{
    stopContinuousMoveTimerOnly();
    m_vx = vx; m_vy = vy; m_w = w;
    if (vx != 0.0 || vy != 0.0 || w != 0.0)
        m_moveTimer->start();
}

void AgvControlClient::stopContinuousMoveTimerOnly()
{
    if (m_moveTimer)
        m_moveTimer->stop();
    m_vx = m_vy = m_w = 0.0;
}

void AgvControlClient::stopContinuousMove()
{
    stopContinuousMoveTimerOnly();
    // 异步发停止指令，不阻塞
    QTimer::singleShot(0, this, [this]() { sendSync(AgvProtocol::CMD_TEACH_STOP); });
}

void AgvControlClient::onContinuousTick()
{
    if (!isConnected()) {
        stopContinuousMoveTimerOnly();
        return;
    }
    // 关键：不等待响应（运动指令是 fire-and-forget），
    // 否则 send() 阻塞会导致指令间隔不均匀，AGV 走起来"一抖一抖"
    QJsonObject p;
    p.insert("vx", m_vx);
    p.insert("vy", m_vy);
    p.insert("w",  m_w);
    p.insert("duration", 200);          // 200ms 后过期，需要重发
    sendNoWait(AgvProtocol::CMD_TEACH_MOVE, p);
}

void AgvControlClient::teachMoveOnce(double vx, double vy, double w, int durationMs)
{
    // duration 不能为 0，否则部分 AGV 协议会忽略；给最小值 100ms
    if (durationMs <= 0) durationMs = 100;
    QJsonObject p;
    p.insert("vx", vx);
    p.insert("vy", vy);
    p.insert("w",  w);
    p.insert("duration", durationMs);
    sendNoWait(AgvProtocol::CMD_TEACH_MOVE, p);
}

void AgvControlClient::setSoftEmergency(bool status)
{
    QJsonObject p;
    p.insert("status", status);
    sendSync(AgvProtocol::CMD_SET_SOFT_EMERGENCY, p);
}

// ===========================================================================
// AgvNavigationClient
// ===========================================================================
AgvNavigationClient::AgvNavigationClient(const QString &ip, quint16 port, QObject *parent)
    : AgvTcpClientBase(ip, port, QStringLiteral("Navigation"), parent)
{
}

void AgvNavigationClient::goTargetPoint(const QString &stationId,
                                        const QString &method,
                                        double maxSpeed, double maxWSpeed)
{
    QJsonObject p;
    p.insert("id", stationId);
    p.insert("method", method);
    p.insert("max_speed", maxSpeed);
    p.insert("max_wspeed", maxWSpeed);
    sendSync(AgvProtocol::CMD_GO_TARGET_POINT, p);
}

void AgvNavigationClient::pauseNavigation()
{
    sendSync(AgvProtocol::CMD_PAUSE_NAVIGATION);
}

void AgvNavigationClient::continueNavigation()
{
    sendSync(AgvProtocol::CMD_CONTINUE_NAVIGATION);
}

void AgvNavigationClient::cancelNavigation()
{
    sendSync(AgvProtocol::CMD_CANCEL_NAVIGATION);
}

// ===========================================================================
// AgvStatusClient
// ===========================================================================
AgvStatusClient::AgvStatusClient(const QString &ip, quint16 port, QObject *parent)
    : AgvTcpClientBase(ip, port, QStringLiteral("Status"), parent)
{
}

QJsonObject AgvStatusClient::getAgvLocation()
{
    return varToObj(sendSync(AgvProtocol::CMD_GET_AGV_LOCATION));
}

QJsonObject AgvStatusClient::getLaserData()
{
    return varToObj(sendSync(AgvProtocol::CMD_GET_LASER_DATA));
}

QJsonObject AgvStatusClient::getNavigationStatus(bool simple)
{
    QJsonObject p;
    p.insert("simple", simple);
    return varToObj(sendSync(AgvProtocol::CMD_GET_NAVIGATION_STATUS, p));
}

QJsonObject AgvStatusClient::getRemoteMaps()
{
    const QVariant r = sendSync(AgvProtocol::CMD_GET_REMOTE_MAPS);
    if (!r.isValid())
        return {};
    const QJsonObject obj = varToObj(r);
    // 兼容 ret_code != 0 的情况（业务失败）
    if (obj.value("ret_code").toInt(0) != 0) {
        qWarning().noquote() << QStringLiteral("[get_remote_maps] 业务失败 ret_code=%1 %2")
                                    .arg(obj.value("ret_code").toInt())
                                    .arg(obj.value("err_msg").toString());
        return {};
    }
    return obj;
}

// ===========================================================================
// AgvConfigClient
// ===========================================================================
AgvConfigClient::AgvConfigClient(const QString &ip, quint16 port, QObject *parent)
    : AgvTcpClientBase(ip, port, QStringLiteral("Config"), parent)
{
}

QJsonObject AgvConfigClient::getMap(const QString &mapName)
{
    QJsonObject p;
    p.insert("map_name", mapName);
    const QVariant r = sendSync(AgvProtocol::CMD_GET_MAP, p);
    if (!r.isValid())
        return {};
    const QJsonObject obj = varToObj(r);
    if (obj.value("ret_code").toInt(0) != 0)
        return {};
    return obj;
}

// ===========================================================================
// AgvOtherClient
// ===========================================================================
AgvOtherClient::AgvOtherClient(const QString &ip, quint16 port, QObject *parent)
    : AgvTcpClientBase(ip, port, QStringLiteral("Others"), parent)
{
}

void AgvOtherClient::setMotorEnable(const QString &motorName, bool enable)
{
    QJsonObject p;
    p.insert("motor_name", motorName);
    p.insert("enable", enable);
    sendSync(AgvProtocol::CMD_SET_MOTOR_ENABLE, p);
}

void AgvOtherClient::setSoftEmergency(bool status)
{
    QJsonObject p;
    p.insert("status", status);
    sendSync(AgvProtocol::CMD_SET_SOFT_EMERGENCY, p);
}

// ===========================================================================
// AgvTcpClient — 兼容 mainwindow 旧接口的门面
// ===========================================================================
AgvTcpClient::AgvTcpClient(QObject *parent)
    : QObject(parent), m_mgr(new AgvConnectionManager(this))
{
    // pushback → stateReceived + pushConnectedChanged
    m_mgr->setStateCallback([this](const QJsonObject &obj) {
        // 推送分多种类型，通过 type 字段区分
        int type = obj.value("type").toInt(-1);
        if (type == 1 || type == 2 || type == 3) {
            m_lastState = AgvState::fromJson(obj);
            emit stateReceived(m_lastState);
        }
    });

    // pushback connected
    if (auto *pb = m_mgr->pushback()) {
        connect(pb, &AgvPushbackClient::connectedChanged,
                this, [this](const QString &, bool c) {
            m_pushConnected = c;
            emit pushConnectedChanged(c);
        });
    }

    // control connected
    if (auto *ctrl = m_mgr->control()) {
        connect(ctrl, &AgvControlClient::connectedChanged,
                this, [this](const QString &, bool c) {
            m_ctrlConnected = c;
            emit ctrlConnectedChanged(c);
        });
    }
}

AgvTcpClient::~AgvTcpClient() = default;

void AgvTcpClient::disconnectFromHost()
{
    if (m_mgr) m_mgr->disconnectAll();
}

bool AgvTcpClient::isConnected() const
{
    return m_mgr && m_mgr->isConnected();
}

void AgvTcpClient::pauseNavigation()
{
    if (m_mgr) m_mgr->navigation()->pauseNavigation();
}

void AgvTcpClient::cancelNavigation()
{
    if (m_mgr) m_mgr->navigation()->cancelNavigation();
}

void AgvTcpClient::setupManager(const QString &ip)
{
    AgvConnectionManager::Ports p;
    auto *s = SettingsManager::instance();
    p.control  = s->agvCtrlPort();
    p.nav      = s->agvNavPort();
    p.pushback = s->agvPushPort();
    p.status   = s->agvStatusPort();
    p.config   = s->agvConfigPort();
    p.other    = 19210;
    m_mgr->connectAll(ip, p);
}

void AgvTcpClient::connectToHost(const QString &ip,
                                quint16, quint16, quint16)
{
    Q_UNUSED(ip);
    // 忽略旧参数，使用 SettingsManager 端口
    setupManager(ip);
}

void AgvTcpClient::stopMotion()
{
    if (m_mgr) m_mgr->control()->stopContinuousMove();
}

void AgvTcpClient::softEmergencyStop()
{
    if (m_mgr) m_mgr->others()->setSoftEmergency(true);
}

void AgvTcpClient::openLoopControl(double linear, double angular)
{
    if (m_mgr) m_mgr->control()->startContinuousMove(linear, 0, angular);
}

void AgvTcpClient::startNavigation(const QString &stationId)
{
    if (m_mgr) {
        auto *nav = m_mgr->navigation();
        nav->goTargetPoint(stationId);
    }
}

void AgvTcpClient::requestMapList()
{
    if (!m_mgr) return;
    auto *st = m_mgr->status();
    QJsonObject resp = st->getRemoteMaps();
    QVariantList maps;
    QString current;
    if (resp.contains("map_list")) {
        QJsonArray arr = varToArr(resp.value("map_list"));
        for (const QJsonValue &v : arr) maps.append(v.toString());
    }
    current = resp.value("current_map").toString();
    emit mapListReceived(maps, current);
}

void AgvTcpClient::requestMapData(const QString &mapName)
{
    if (!m_mgr) return;
    QJsonObject resp = m_mgr->config()->getMap(mapName);
    QVariantMap vm;
    for (auto it = resp.constBegin(); it != resp.constEnd(); ++it)
        vm.insert(it.key(), it.value().toVariant());
    emit mapDataReceived(mapName, vm);
}

// ===========================================================================
// AgvConnectionManager
// ===========================================================================
AgvConnectionManager::AgvConnectionManager(QObject *parent)
    : QObject(parent)
{
}

AgvConnectionManager::~AgvConnectionManager()
{
    disconnectAll();
}

bool AgvConnectionManager::connectAll(const QString &ip, const Ports &ports)
{
    m_ip = ip;
    delete m_pushback; m_pushback = nullptr;
    delete m_control;  m_control  = nullptr;
    delete m_nav;      m_nav      = nullptr;
    delete m_status;   m_status   = nullptr;
    delete m_others;   m_others   = nullptr;
    delete m_config;   m_config   = nullptr;

    m_pushback = new AgvPushbackClient(ip, ports.pushback, this);
    m_control  = new AgvControlClient (ip, ports.control,  this);
    m_nav      = new AgvNavigationClient(ip, ports.nav,     this);
    m_status   = new AgvStatusClient  (ip, ports.status,  this);
    m_others   = new AgvOtherClient   (ip, ports.other,   this);
    m_config   = new AgvConfigClient  (ip, ports.config,  this);

    QStringList diag;

    // 推送端口必须连接成功
    const bool pushOk = m_pushback->connectToHost();
    diag << QStringLiteral("pushback: connected=%1, port=%2")
                .arg(m_pushback->isConnected()).arg(ports.pushback);
    if (!pushOk) {
        emit portsDiagnostic(diag);
        return false;
    }

    // 其他端口可选
    m_control->connectToHost();
    m_nav->connectToHost();
    m_status->connectToHost();
    m_others->connectToHost();
    m_config->connectToHost();

    diag << QStringLiteral("control: connected=%1, port=%2")
                .arg(m_control->isConnected()).arg(ports.control)
         << QStringLiteral("nav: connected=%1, port=%2")
                .arg(m_nav->isConnected()).arg(ports.nav)
         << QStringLiteral("status: connected=%1, port=%2")
                .arg(m_status->isConnected()).arg(ports.status)
         << QStringLiteral("other: connected=%1, port=%2")
                .arg(m_others->isConnected()).arg(ports.other)
         << QStringLiteral("config: connected=%1, port=%2")
                .arg(m_config->isConnected()).arg(ports.config);

    emit portsDiagnostic(diag);
    m_connected = true;
    return true;
}

void AgvConnectionManager::disconnectAll()
{
    if (m_pushback) m_pushback->disconnectFromHost();
    if (m_control)  m_control->disconnectFromHost();
    if (m_nav)      m_nav->disconnectFromHost();
    if (m_status)   m_status->disconnectFromHost();
    if (m_others)   m_others->disconnectFromHost();
    if (m_config)   m_config->disconnectFromHost();
    m_connected = false;
}

bool AgvConnectionManager::isConnected() const
{
    return m_connected && m_pushback && m_pushback->isConnected();
}

void AgvConnectionManager::setStateCallback(std::function<void(const QJsonObject &)> cb)
{
    if (m_pushback)
        m_pushback->setStateCallback(std::move(cb));
}