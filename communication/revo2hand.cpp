#include "revo2hand.h"
#include "util.h"
#include "armwebsocket.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QStringList>
#include <QThread>

// ============================================================================
// 工具函数
// ============================================================================
quint16 handCrc16(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (unsigned char byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

int handDegToRaw(double deg, int dofIdx)
{
    if (dofIdx < 0 || dofIdx >= HAND_DOF_COUNT) return 0;
    const int maxDeg = HAND_DOF_MAX_DEG[dofIdx];
    return qBound(0, qRound(deg / maxDeg * 1000.0), 1000);
}

double handRawToDeg(int raw, int dofIdx)
{
    if (dofIdx < 0 || dofIdx >= HAND_DOF_COUNT) return 0.0;
    const int maxDeg = HAND_DOF_MAX_DEG[dofIdx];
    return qBound(0.0, (raw / 1000.0) * maxDeg, static_cast<double>(maxDeg));
}

// ============================================================================
// Modbus RTU 帧构造
// ============================================================================
static QByteArray buildModbusFrame(int slave, int fc, const QVector<int> &payload)
{
    QByteArray body;
    body.reserve(payload.size() + 2);
    body.append(static_cast<char>(slave & 0xFF));
    body.append(static_cast<char>(fc & 0xFF));
    for (int b : payload)
        body.append(static_cast<char>(b & 0xFF));
    const quint16 crc = handCrc16(body);
    body.append(static_cast<char>(crc & 0xFF));
    body.append(static_cast<char>((crc >> 8) & 0xFF));
    return body;
}

static QByteArray buildReadFrame(int slave, quint16 startReg, int count)
{
    QVector<int> p;
    p.append((startReg >> 8) & 0xFF); p.append(startReg & 0xFF);
    p.append((count >> 8) & 0xFF);     p.append(count & 0xFF);
    return buildModbusFrame(slave, 0x04, p);
}

static QByteArray buildWriteDofFrame(int slave, int dofIdx, int position, int timeMs)
{
    QVector<int> p;
    p.append((HAND_REG_WRITE_DOF >> 8) & 0xFF); p.append(HAND_REG_WRITE_DOF & 0xFF);
    p.append(0x00); p.append(0x03);               // 数量 3
    p.append(0x06);                                // 字节数 6
    p.append((dofIdx >> 8) & 0xFF); p.append(dofIdx & 0xFF);
    p.append((position >> 8) & 0xFF); p.append(position & 0xFF);
    p.append((timeMs >> 8) & 0xFF);   p.append(timeMs & 0xFF);
    return buildModbusFrame(slave, 0x10, p);
}

static QByteArray buildWriteAllFrame(int slave, const QVector<int> &positions, int timeMs)
{
    QVector<int> p;
    p.append((HAND_REG_WRITE_ALL >> 8) & 0xFF); p.append(HAND_REG_WRITE_ALL & 0xFF);
    p.append(0x00); p.append(0x0C);              // 数量 12
    p.append(0x18);                                // 字节数 24
    for (int pos : positions) {
        p.append((pos >> 8) & 0xFF); p.append(pos & 0xFF);
        p.append((timeMs >> 8) & 0xFF); p.append(timeMs & 0xFF);
    }
    return buildModbusFrame(slave, 0x10, p);
}

static bool verifyCrc(const QByteArray &data)
{
    if (data.size() < 4) return false;
    const quint16 crc = handCrc16(data.left(data.size() - 2));
    const quint16 expect = (static_cast<unsigned char>(data.at(data.size() - 1)) << 8)
                         | static_cast<unsigned char>(data.at(data.size() - 2));
    return crc == expect;
}

static QString checkSlaveErrorByte(const QByteArray &rx)
{
    if (rx.size() >= 3 && (rx.at(1) & 0xFF) == 0x90)
        return QStringLiteral("从站异常码 0x%1").arg(
            static_cast<unsigned char>(rx.at(2)), 2, 16, QChar('0').toUpper());
    return {};
}

// ============================================================================
// 公开的 Modbus 帧接口（供 HandViewDialog 调试页使用）
// 与 Python revo2_hand.py 同名函数一一对应。
// ============================================================================
QByteArray handCrcBytes(const QByteArray &data)
{
    const quint16 crc = handCrc16(data);
    QByteArray out;
    out.append(static_cast<char>(crc & 0xFF));
    out.append(static_cast<char>((crc >> 8) & 0xFF));
    return out;
}

bool handVerifyCrc(const QByteArray &frame)
{
    return verifyCrc(frame);
}

QByteArray handBuildFrame(int slave, int func, const QByteArray &payload)
{
    QVector<int> p;
    for (char c : payload)
        p.append(static_cast<unsigned char>(c));
    return buildModbusFrame(slave, func, p);
}

QByteArray handReadPositionsFrame(int slave)
{
    return buildReadFrame(slave, HAND_REG_READ_POS, HAND_DOF_COUNT);
}

QByteArray handWriteOneDofFrame(int slave, int dofIdx, int position, int timeMs)
{
    return buildWriteDofFrame(slave, dofIdx, position, timeMs);
}

QByteArray handWriteAllDofsFrame(int slave, const QVector<int> &positions, int timeMs)
{
    return buildWriteAllFrame(slave, positions, timeMs);
}

// ============================================================================
// 预设手势
// ============================================================================
const QStringList &handDofNameCnList()
{
    // 与 Python DOF_INFO[].name_cn 同序（拇指弯曲 / 拇指侧摆 / 食指 / 中指 / 无名指 / 小指）
    static const QStringList kNames = {
        QStringLiteral("拇指弯曲"),
        QStringLiteral("拇指侧摆"),
        QStringLiteral("食指"),
        QStringLiteral("中指"),
        QStringLiteral("无名指"),
        QStringLiteral("小指"),
    };
    return kNames;
}

const QStringList &handDofNameList()
{
    // 与 Python DOF_INFO[].name 同序
    static const QStringList kNames = {
        QStringLiteral("thumb_bend"),
        QStringLiteral("thumb_swing"),
        QStringLiteral("index"),
        QStringLiteral("middle"),
        QStringLiteral("ring"),
        QStringLiteral("pinky"),
    };
    return kNames;
}

QVector<int> handPreset(const QString &name)
{
    // 预设名与 Python communication/revo2_hand.py 的 PRESETS 完全一致。
    // 额外接受英文别名（grab / release ...）：调用方（主界面按钮）历史上
    // 传的是英文名，直接查中文名表会落空并返回「未知预设」。
    static const QHash<QString, QVector<int>> kPresets = {
        {QStringLiteral("一把抓"), {1000, 0, 1000, 1000, 1000, 1000}},
        {QStringLiteral("释放"),   {0, 0, 0, 0, 0, 0}},
        {QStringLiteral("半抓"),   {500, 0, 500, 500, 500, 500}},
        {QStringLiteral("1010"),   {1000, 0, 100, 1000, 100, 1000}},
        {QStringLiteral("两指捏"), {800, 0, 200, 0, 0, 0}},
        {QStringLiteral("点赞"),   {0, 0, 1000, 1000, 1000, 1000}},
        {QStringLiteral("指点"),   {1000, 0, 0, 1000, 1000, 1000}},
        // 英文别名
        {QStringLiteral("grab"),      {1000, 0, 1000, 1000, 1000, 1000}},
        {QStringLiteral("release"),   {0, 0, 0, 0, 0, 0}},
        {QStringLiteral("half"),      {500, 0, 500, 500, 500, 500}},
        {QStringLiteral("pinch"),     {800, 0, 200, 0, 0, 0}},
        {QStringLiteral("thumbsup"),  {0, 0, 1000, 1000, 1000, 1000}},
        {QStringLiteral("point"),     {1000, 0, 0, 1000, 1000, 1000}},
    };

    const auto it = kPresets.constFind(name.trimmed());
    if (it != kPresets.constEnd())
        return it.value();
    return {};
}

// ============================================================================
// Revo2HandClient
// ============================================================================
Revo2HandClient::Revo2HandClient(ArmWebSocket *ws, const QString &side,
                                 const QString &ec2rsSlave, int modbusAddr,
                                 QObject *parent)
    : QObject(parent), m_ws(ws), m_side(side),
      m_ec2rsSlave(ec2rsSlave), m_modbusAddr(modbusAddr)
{
    m_state.side = side;
    m_state.modbusAddr = modbusAddr;
    m_state.ec2rsSlave = ec2rsSlave;
    m_state.positions.fill(0, HAND_DOF_COUNT);
    m_state.targetPositions.fill(0, HAND_DOF_COUNT);
}

Revo2HandClient::~Revo2HandClient()
{
    close();
}

QPair<bool, QString> Revo2HandClient::init(bool force)
{
    Q_UNUSED(force)

    // 1) rs485/list 解析 channel
    QString channel;
    QString cerr = resolveChannel(&channel, true);
    if (!cerr.isEmpty()) return {false, cerr};

    // 2) rs485/init
    QString err = initChannel();

    // 通道被厂商夹爪协议占用时，init 会返回 23252「厂商夹爪占用」。
    // 这时才走释放流程：停用该通道上的夹爪协议，等占用解除后重新 init。
    //
    // 注意：释放流程**不能**作为连接的必经步骤。peripheral/getConfig 与
    // rs485/getStatus 在很多正常工况下都会返回错误或空 revision（例如
    // 通道未初始化、配置服务未装载、持久化诊断），一旦把它们当致命错误，
    // 灵巧手就再也连不上。ActionPlanner 的可用实现就是不做这一步：
    // 直接 rs485/list → rs485/init。
    if (err.contains(QStringLiteral("23252"))) {
        const QString rerr = releaseChannel(channel);
        if (!rerr.isEmpty()) return {false, rerr};

        err = initChannel();
        if (!err.isEmpty()) return {false, err};
    }

    if (!err.isEmpty()) return {false, err};

    {
        QMutexLocker locker(&m_mutex);
        m_state.connected = true;
        m_state.enabled = true;
        m_state.inited = true;
        m_state.lastError.clear();
    }
    notifyState();
    return {true, {}};
}

QString Revo2HandClient::resolveChannel(QString *channel, bool force)
{
    {
        QMutexLocker locker(&m_mutex);
        if (!force && !m_channel.isEmpty()) {
            if (channel) *channel = m_channel;
            return {};
        }
    }
    const QVariant r = m_ws->sendAndWaitSafe(QStringLiteral("rs485/list"), QJsonObject(), 5000);
    if (!r.isValid()) return QStringLiteral("rs485/list 无响应");
    const QJsonObject resp = varToObj(r);
    if (resp.contains("err"))
        return QStringLiteral("rs485/list err: %1").arg(
            resp.value("err").toObject().value("message").toString());

    const QString want = ec2rsSlave();
    const QJsonArray chans = varToArr(resp.value("db"));
    QStringList names;
    for (const QJsonValue &v : chans) {
        const QJsonObject c = v.toObject();
        const QString ch = c.value("channel").toString();
        names << ch;
        const QString slave = c.value("backendStatus").toObject().value("slave").toString();
        if (slave == want || ch == want
            || ch.section('/', -1) == want) {
            if (c.contains("supported") && !c.value("supported").toBool())
                return QStringLiteral("通道 %1 当前平台不支持").arg(ch);
            {
                QMutexLocker locker(&m_mutex);
                m_channel = ch;
            }
            if (channel) *channel = ch;
            return {};
        }
    }
    return QStringLiteral("rs485/list 中未找到从站 %1 对应的通道（现有: %2）")
        .arg(want, names.isEmpty() ? QStringLiteral("无") : names.join(", "));
}

QString Revo2HandClient::releaseChannel(const QString &channel)
{
    // 这是**兜底**路径：仅当 rs485/init 报 23252（厂商夹爪占用）时才走到。
    // 因此这里的每一步都不能把「查不到 / 读不出」当成致命错误——
    // 查不到就说明不需要释放，直接返回成功即可。
    QJsonObject sq;
    sq.insert("channel", channel);
    const QJsonObject st = varToObj(m_ws->sendAndWaitSafe(
        QStringLiteral("rs485/getStatus"), sq, 3000));
    if (st.isEmpty())
        return QStringLiteral("rs485/getStatus 无响应，无法确认通道占用情况");
    if (st.contains("err"))
        return QStringLiteral("rs485/getStatus err(%1): %2")
            .arg(st.value("err").toObject().value("code").toInt())
            .arg(st.value("err").toObject().value("message").toString());
    if (st.value("db").toObject().value("occupiedBy").toString() != QStringLiteral("gripper"))
        return {};

    const QJsonObject cfg = varToObj(m_ws->sendAndWaitSafe(
        QStringLiteral("peripheral/getConfig"), QJsonObject(), 5000));
    if (cfg.isEmpty() || cfg.contains("err"))
        return QStringLiteral("peripheral/getConfig 失败，无法释放被夹爪占用的 RS485 通道");
    const QJsonObject cdb = cfg.value("db").toObject();
    QString revision = cdb.value("revision").toString();
    QString applied  = cdb.value("appliedRevision").toString();
    if (revision.isEmpty())
        return QStringLiteral("peripheral/getConfig 返回空版本，配置读取异常，无法释放通道");

    const QJsonArray grippers =
        cdb.value("runtime").toObject().value("gripper").toArray();
    const QString want = ec2rsSlave();

    // runtime 里找不到本从站上已启用的夹爪协议时，没什么可停用的：
    // 占用很可能来自别的从站或已被别的进程处理。直接返回成功，
    // 让上层的 rs485/init 再试一次，由它给出权威结论。
    bool found = false;
    for (const QJsonValue &gv : grippers) {
        const QJsonObject g = gv.toObject();
        if (g.value("slave").toString() != want || !g.value("enabled").toBool())
            continue;
        found = true;

        QJsonObject db;
        db.insert("revision", revision);
        db.insert("appliedRevision", applied);
        db.insert("device", g.value("name").toString());
        db.insert("enabled", false);
        db.insert("confirmStationary", true);
        const QJsonObject r = varToObj(m_ws->sendAndWaitSafe(
            QStringLiteral("peripheral/setDeviceEnabled"), db, 8000));
        if (r.isEmpty())
            return QStringLiteral("停用夹爪协议 %1 无响应").arg(g.value("name").toString());
        if (r.contains("err"))
            return QStringLiteral("停用夹爪协议 %1 失败: %2").arg(
                g.value("name").toString(),
                r.value("err").toObject().value("message").toString());
        const QJsonObject nd = r.value("db").toObject();
        if (nd.contains("revision")) revision = nd.value("revision").toString();
        if (nd.contains("appliedRevision")) applied = nd.value("appliedRevision").toString();
    }

    // runtime 里没有匹配的已启用夹爪协议 → 无需等待释放，直接返回。
    // 否则下面的轮询会白等满 10s 才报错。
    if (!found)
        return {};

    // 等收尾结束并解除占用（最长 10s）
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 10000) {
        const QJsonObject c2 = varToObj(m_ws->sendAndWaitSafe(
            QStringLiteral("peripheral/getConfig"), QJsonObject(), 3000));
        bool releasing = false;
        const QJsonArray rt = c2.value("db").toObject().value("runtime")
                                .toObject().value("gripper").toArray();
        for (const QJsonValue &gv : rt) {
            const QJsonObject g = gv.toObject();
            if (g.value("slave").toString() == want && g.value("releasing").toBool())
                releasing = true;
        }
        const QJsonObject s2 = varToObj(m_ws->sendAndWaitSafe(
            QStringLiteral("rs485/getStatus"), sq, 3000));
        const QString occ = s2.value("db").toObject().value("occupiedBy")
                              .toString(QStringLiteral("gripper"));
        if (!releasing && occ != QStringLiteral("gripper")) return {};
        QThread::msleep(300);
    }
    return QStringLiteral("等待夹爪协议释放 RS485 通道超时（occupiedBy 仍为 gripper）");
}

QString Revo2HandClient::initChannel()
{
    QString channel;
    const QString cerr = resolveChannel(&channel, true);
    if (!cerr.isEmpty()) return cerr;

    QJsonObject db;
    db.insert("channel", channel);
    db.insert("baudRate", 115200);
    db.insert("stopBits", 1);
    db.insert("parity", 0);
    const QVariant r = m_ws->sendAndWaitSafe(QStringLiteral("rs485/init"), db, 6000);
    if (!r.isValid()) return QStringLiteral("rs485/init 无响应");
    const QJsonObject resp = varToObj(r);
    if (resp.isEmpty()) return QStringLiteral("rs485/init 返回格式错误");
    if (resp.contains("err"))
        return QStringLiteral("init err(%1): %2")
            .arg(resp.value("err").toObject().value("code").toInt())
            .arg(resp.value("err").toObject().value("message").toString());

    // writeDof()/writeAll()/readPositions() 也会按需调用 initChannel()。
    // 成功后必须和 Python _init_channel() 一样保留 inited 标志，否则每次
    // 收发都会重复 rs485/init，容易造成通道重置并表现为灵巧手不稳定。
    QMutexLocker locker(&m_mutex);
    m_state.inited = true;
    return {};
}

bool Revo2HandClient::isReady() const
{
    QMutexLocker locker(&m_mutex);
    return m_state.connected && m_state.inited;
}

void Revo2HandClient::close()
{
    QMutexLocker locker(&m_mutex);
    m_state.connected = false;
    m_state.inited = false;
    m_state.enabled = false;
}

int Revo2HandClient::writeTimeMs() const
{
    QMutexLocker locker(&m_mutex);
    return m_writeTimeMs;
}

void Revo2HandClient::setWriteTimeMs(int ms)
{
    QMutexLocker locker(&m_mutex);
    m_writeTimeMs = qBound(HAND_WRITE_TIME_MS_MIN, ms, HAND_WRITE_TIME_MS_MAX);
}

QString Revo2HandClient::ec2rsSlave() const
{
    QMutexLocker locker(&m_mutex);
    return m_ec2rsSlave;
}

int Revo2HandClient::modbusAddr() const
{
    QMutexLocker locker(&m_mutex);
    return m_modbusAddr;
}

int Revo2HandClient::txCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_txCount;
}

int Revo2HandClient::rxCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_rxCount;
}

int Revo2HandClient::errCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_errCount;
}

void Revo2HandClient::setModbusAddr(int addr)
{
    QMutexLocker locker(&m_mutex);
    if (m_modbusAddr != addr) m_state.inited = false;
    m_modbusAddr = addr;
    m_state.modbusAddr = addr;
}

void Revo2HandClient::setEc2rsSlave(const QString &slave)
{
    QMutexLocker locker(&m_mutex);
    if (m_ec2rsSlave != slave) { m_state.inited = false; m_channel.clear(); }
    m_ec2rsSlave = slave;
    m_state.ec2rsSlave = slave;
}

void Revo2HandClient::resetInited()
{
    QMutexLocker locker(&m_mutex);
    m_state.inited = false;
}

QByteArray Revo2HandClient::ec2rsWriteRead(const QByteArray &data, int readLength)
{
    // 同一路 RS485 通道上的「写 + 读」必须原子完成：轮询线程与写入线程
    // 若并发，读到的可能是别人写的残留数据，或响应与请求错位。
    // 对应 Python 版 _ecrs_write_read() 的 self._lock。
    //
    // 锁在调用方线程获取。等待响应期间，socket 线程（主线程）保持空闲，
    // 能正常接收并派发 QWebSocket 信号——不会阻塞 UI，也不会死锁。
    QMutexLocker ioLocker(&m_ioMutex);

    QString channel;
    if (!resolveChannel(&channel).isEmpty()) return QByteArray();

    // 写
    QJsonObject db;
    db.insert("channel", channel);
    QJsonArray arr;
    for (unsigned char c : data)
        arr.append(static_cast<int>(c));
    db.insert("data", arr);
    const QVariant wr = m_ws->sendAndWaitSafe(
        QStringLiteral("rs485/write"), db, 3000);
    if (!wr.isValid()) return QByteArray();
    const QJsonObject wro = varToObj(wr);
    if (wro.contains("err")) return QByteArray();
    {
        QMutexLocker locker(&m_mutex);
        ++m_txCount;
    }

    // 读
    QJsonObject dbr;
    dbr.insert("channel", channel);
    dbr.insert("timeout", 400);
    if (readLength > 0) dbr.insert("length", readLength);
    const QVariant rd = m_ws->sendAndWaitSafe(
        QStringLiteral("rs485/read"), dbr,
        static_cast<int>((400 / 1000.0) + 3.0) * 1000);
    if (!rd.isValid()) return QByteArray();
    const QJsonObject rdo = varToObj(rd);
    if (rdo.contains("err")) return QByteArray();
    const QJsonValue rxv = rdo.value("db");
    if (!rxv.isArray()) return QByteArray();
    const QJsonArray rxa = rxv.toArray();
    QByteArray rx;
    rx.reserve(rxa.size());
    for (const QJsonValue &bv : rxa)
        rx.append(static_cast<char>(bv.toInt(0) & 0xFF));
    {
        QMutexLocker locker(&m_mutex);
        ++m_rxCount;
    }
    return rx;
}

// ============================================================================
// 调试通道（供 HandViewDialog 的「Modbus 调试」页）
// ============================================================================

/// 提取 resp.err.message（与本文件其他调用点一致的错误处理）
static QString respErr(const QJsonObject &resp)
{
    if (!resp.contains(QStringLiteral("err"))) return {};
    return resp.value(QStringLiteral("err")).toObject()
             .value(QStringLiteral("message")).toString();
}

QString Revo2HandClient::rawExchange(const QByteArray &frame, int readLength)
{
    if (!m_ws) return QStringLiteral("未连接机械臂");

    // 与 ec2rsWriteRead() 一样：锁在调用方线程获取，写和读串行执行。
    // 等待响应期间 socket 线程保持空闲，不会阻塞 UI。
    QMutexLocker ioLocker(&m_ioMutex);

    QString channel;
    const QString cerr = resolveChannel(&channel);
    if (!cerr.isEmpty()) return cerr;

    // 写
    QJsonObject db;
    db.insert(QStringLiteral("channel"), channel);
    QJsonArray arr;
    for (unsigned char c : frame)
        arr.append(static_cast<int>(c));
    db.insert(QStringLiteral("data"), arr);

    const QVariant wr = m_ws->sendAndWaitSafe(
        QStringLiteral("rs485/write"), db, 3000);
    if (!wr.isValid()) return QStringLiteral("写超时");
    const QString werr = respErr(varToObj(wr));
    if (!werr.isEmpty()) return QStringLiteral("写失败: ") + werr;
    {
        QMutexLocker locker(&m_mutex);
        ++m_txCount;
    }

    // 只发不读
    if (readLength <= 0) return QString();

    // 读
    QJsonObject dbr;
    dbr.insert(QStringLiteral("channel"), channel);
    dbr.insert(QStringLiteral("timeout"), 400);
    dbr.insert(QStringLiteral("length"), readLength);

    const QVariant rd = m_ws->sendAndWaitSafe(
        QStringLiteral("rs485/read"), dbr, 3000);
    if (!rd.isValid()) return QStringLiteral("读超时");
    const QString rerr = respErr(varToObj(rd));
    if (!rerr.isEmpty()) return QStringLiteral("读失败: ") + rerr;

    const QJsonArray rxa = varToObj(rd).value(QStringLiteral("db")).toArray();
    QByteArray rx;
    rx.reserve(rxa.size());
    for (const QJsonValue &bv : rxa)
        rx.append(static_cast<char>(bv.toInt(0) & 0xFF));
    {
        QMutexLocker locker(&m_mutex);
        ++m_rxCount;
    }
    return QString::fromLatin1(rx.toHex(' ')).trimmed();
}

QJsonObject Revo2HandClient::ec2rsGetStatus()
{
    if (!m_ws) return {};
    QString channel;
    if (!resolveChannel(&channel).isEmpty()) return {};
    QJsonObject db;
    db.insert(QStringLiteral("channel"), channel);
    const QVariant resp = m_ws->sendAndWaitSafe(
        QStringLiteral("rs485/getStatus"), db, 3000);
    if (!resp.isValid()) return {};
    return varToObj(resp);
}

QString Revo2HandClient::writeDof(int dofIdx, int position)
{
    if (dofIdx < 0 || dofIdx >= HAND_DOF_COUNT)
        return QStringLiteral("DOF 索引越界: %1").arg(dofIdx);
    position = qBound(0, position, 1000);

    // 未 init 时按需 init（Python 版 write_dof 同样在内部自动 init）
    if (!isChannelInited()) {
        const QString err = initChannel();
        if (!err.isEmpty()) { setError(err); return err; }
    }

    QByteArray frame;
    {
        QMutexLocker locker(&m_mutex);
        frame = buildWriteDofFrame(m_modbusAddr, dofIdx, position, m_writeTimeMs);
    }

    const QByteArray rx = ec2rsWriteRead(frame, 8);
    if (rx.isEmpty()) { setError(QStringLiteral("无应答")); return QStringLiteral("无应答"); }
    if (!verifyCrc(rx)) { setError(QStringLiteral("CRC 错误")); return QStringLiteral("CRC 错误"); }
    const QString serr = checkSlaveErrorByte(rx);
    if (!serr.isEmpty()) { setError(serr); return serr; }
    if ((rx.at(1) & 0xFF) != 0x10) {
        const QString err = QStringLiteral("非预期功能码 0x%1").arg(
            static_cast<unsigned char>(rx.at(1)), 2, 16, QChar('0').toUpper());
        setError(err);
        return err;
    }

    {
        QMutexLocker locker(&m_mutex);
        m_state.positions[dofIdx] = position;
        m_state.targetPositions[dofIdx] = position;
        m_state.lastError.clear();
    }
    notifyState();
    return {};
}

QString Revo2HandClient::writeAll(const QVector<int> &positions)
{
    if (positions.size() != HAND_DOF_COUNT)
        return QStringLiteral("positions 长度必须为 %1，实际 %2")
                .arg(HAND_DOF_COUNT).arg(positions.size());

    QVector<int> clamped;
    clamped.reserve(HAND_DOF_COUNT);
    for (int p : positions) clamped.append(qBound(0, p, 1000));

    if (!isChannelInited()) {
        const QString err = initChannel();
        if (!err.isEmpty()) { setError(err); return err; }
    }

    QByteArray frame;
    {
        QMutexLocker locker(&m_mutex);
        frame = buildWriteAllFrame(m_modbusAddr, clamped, m_writeTimeMs);
    }

    const QByteArray rx = ec2rsWriteRead(frame, 8);
    if (rx.isEmpty()) { setError(QStringLiteral("无应答")); return QStringLiteral("无应答"); }
    if (!verifyCrc(rx)) { setError(QStringLiteral("CRC 错误")); return QStringLiteral("CRC 错误"); }
    const QString serr = checkSlaveErrorByte(rx);
    if (!serr.isEmpty()) { setError(serr); return serr; }
    if ((rx.at(1) & 0xFF) != 0x10) {
        const QString err = QStringLiteral("非预期功能码 0x%1").arg(
            static_cast<unsigned char>(rx.at(1)), 2, 16, QChar('0').toUpper());
        setError(err);
        return err;
    }

    {
        QMutexLocker locker(&m_mutex);
        m_state.positions = clamped;
        m_state.targetPositions = clamped;
        m_state.lastError.clear();
    }
    notifyState();
    return {};
}

QString Revo2HandClient::executePreset(const QString &name)
{
    const QVector<int> pos = handPreset(name);
    if (pos.isEmpty())
        return QStringLiteral("未知预设: %1").arg(name);
    return writeAll(pos);
}

QString Revo2HandClient::readPositions()
{
    if (!isChannelInited()) {
        const QString err = initChannel();
        if (!err.isEmpty()) { setError(err); return err; }
    }

    QByteArray frame;
    {
        QMutexLocker locker(&m_mutex);
        frame = buildReadFrame(m_modbusAddr, HAND_REG_READ_POS, HAND_DOF_COUNT);
    }

    const QByteArray rx = ec2rsWriteRead(frame, 17);
    if (rx.isEmpty()) { setError(QStringLiteral("无应答")); return QStringLiteral("无应答"); }
    if (!verifyCrc(rx)) { setError(QStringLiteral("CRC 错误")); return QStringLiteral("CRC 错误"); }
    if ((rx.at(1) & 0xFF) != 0x04) {
        const QString err = QStringLiteral("非预期功能码 0x%1").arg(
            static_cast<unsigned char>(rx.at(1)), 2, 16, QChar('0').toUpper());
        setError(err);
        return err;
    }
    if ((rx.at(2) & 0xFF) != HAND_DOF_COUNT * 2) {
        const QString err = QStringLiteral("字节数不匹配: %1").arg(rx.at(2) & 0xFF);
        setError(err);
        return err;
    }

    QVector<int> pos;
    pos.reserve(HAND_DOF_COUNT);
    for (int i = 0; i < HAND_DOF_COUNT; ++i)
        pos.append(((rx.at(3 + i*2) & 0xFF) << 8) | (rx.at(4 + i*2) & 0xFF));
    {
        QMutexLocker locker(&m_mutex);
        m_state.positions = pos;
        m_state.lastError.clear();
    }
    notifyState();
    return {};
}

HandState Revo2HandClient::state() const
{
    QMutexLocker locker(&m_mutex);
    return m_state;
}

bool Revo2HandClient::isChannelInited() const
{
    QMutexLocker locker(&m_mutex);
    return m_state.inited;
}

void Revo2HandClient::setError(const QString &err)
{
    {
        QMutexLocker locker(&m_mutex);
        m_state.lastError = err;
        ++m_errCount;
    }
    notifyState();
}

int Revo2HandClient::registerStateCallback(StateCallback cb)
{
    if (!cb) return 0;
    QMutexLocker locker(&m_mutex);
    const int handle = m_nextCbHandle++;
    m_callbacks.append({handle, std::move(cb)});
    return handle;
}

void Revo2HandClient::unregisterStateCallback(int handle)
{
    if (handle <= 0) return;
    QMutexLocker locker(&m_mutex);
    m_callbacks.removeIf([handle](const StateCbEntry &e) {
        return e.handle == handle;
    });
}

void Revo2HandClient::notifyState()
{
    // notifyState() 可能从工作线程被调用（微调写入走后台线程），
    // 而注册的回调里会直接操作 UI 控件（如 HandPanel::applyState），
    // 跨线程改控件会导致崩溃或界面不刷新。
    //
    // 这里把回调统一切回本对象（客户端）所属线程再执行。
    // 另外回调里用 QTimer::singleShot(0, ...) 投递的更新也依赖事件循环，
    // 在没有事件循环的工作线程里永远不会触发——切回主线程后即恢复正常。
    QVector<StateCallback> cbs;
    HandState s;
    {
        QMutexLocker locker(&m_mutex);
        cbs.reserve(m_callbacks.size());
        for (const StateCbEntry &e : m_callbacks)
            cbs.append(e.fn);
        s = m_state;
    }

    const auto runCallbacks = [this, cbs, s]() {
        for (const auto &f : cbs) {
            try { f(s); } catch (...) {}
        }
        emit stateChanged(s);
    };

    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, runCallbacks, Qt::QueuedConnection);
        return;
    }
    runCallbacks();
}

// ============================================================================
// DualHandManager
// ============================================================================
DualHandManager::DualHandManager(ArmWebSocket *ws,
                               const QString &leftSlave,
                               const QString &rightSlave,
                               int leftAddr, int rightAddr,
                               QObject *parent)
    : QObject(parent), m_ws(ws)
{
    m_left  = new Revo2HandClient(ws, QStringLiteral("left"),  leftSlave,  leftAddr,  this);
    m_right = new Revo2HandClient(ws, QStringLiteral("right"), rightSlave, rightAddr, this);
}

DualHandManager::~DualHandManager()
{
    close();
}

QPair<QPair<bool, QString>, QPair<bool, QString>> DualHandManager::init()
{
    m_left->close();
    m_right->close();
    const auto lo = m_left->init();
    const auto ro = m_right->init();
    return { lo, ro };
}

void DualHandManager::close()
{
    m_left->close();
    m_right->close();
}

void DualHandManager::setArmConnected(bool connected)
{
    if (!connected) {
        m_left->close();
        m_right->close();
    }
}
