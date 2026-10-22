#include "revo2hand.h"
#include "util.h"
#include "armwebsocket.h"

#include <QDebug>
#include <QJsonDocument>

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
    if (name == QStringLiteral("一把抓"))   return {1000, 0, 1000, 1000, 1000, 1000};
    if (name == QStringLiteral("释放"))     return {0, 0, 0, 0, 0, 0};
    if (name == QStringLiteral("半抓"))     return {500, 0, 500, 500, 500, 500};
    if (name == QStringLiteral("1010"))     return {1000, 0, 100, 1000, 100, 1000};
    if (name == QStringLiteral("两指捏"))   return {800, 0, 200, 0, 0, 0};
    if (name == QStringLiteral("点赞"))     return {0, 0, 1000, 1000, 1000, 1000};
    if (name == QStringLiteral("指点"))     return {1000, 0, 0, 1000, 1000, 1000};
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

    // 1) gripper/list 找设备
    const QVariant r = m_ws->sendAndWait(QStringLiteral("gripper/list"), QJsonObject(), 5000);
    if (!r.isValid()) return {false, QStringLiteral("gripper/list 无响应")};

    const QJsonObject resp = varToObj(r);
    if (resp.contains("err"))
        return {false, QStringLiteral("gripper/list err")};

    const QJsonArray devs = varToArr(resp.value("db"));
    QString targetDevice;
    for (const QJsonValue &v : devs) {
        const QJsonObject d = v.toObject();
        if (d.value("slave").toString() == m_ec2rsSlave) {
            targetDevice = d.value("name").toString();
            if (d.value("type").toString() != QStringLiteral("Generic485")) {
                QJsonObject db2;
                db2.insert("device", targetDevice);
                db2.insert("type", QStringLiteral("Generic485"));
                const QVariant r2 = m_ws->sendAndWait(
                    QStringLiteral("gripper/setType"), db2, 7000);
                const QJsonObject r2o = varToObj(r2);
                if (!r2.isValid() || r2o.contains("err") ||
                    !r2o.value("db").toBool()) {
                    return {false, QStringLiteral("setType 失败: %1").arg(
                        r2o.value("err").toObject().value("message").toString())};
                }
            }
            break;
        }
    }
    if (targetDevice.isEmpty())
        return {false, QStringLiteral("未找到从站 %1 对应的 gripper 设备").arg(m_ec2rsSlave)};

    // 2) ec2rs485/init
    const QString err = initChannel();
    if (!err.isEmpty()) return {false, err};

    m_state.connected = true;
    m_state.enabled = true;
    m_state.inited = true;
    m_state.lastError.clear();
    notifyState();
    return {true, {}};
}

QString Revo2HandClient::initChannel()
{
    QJsonObject db;
    db.insert("slave", m_ec2rsSlave);
    db.insert("baudRate", 115200);
    db.insert("stopBits", 1);
    db.insert("parity", 0);
    const QVariant r = m_ws->sendAndWait(QStringLiteral("ec2rs485/init"), db, 6000);
    if (!r.isValid()) return QStringLiteral("ec2rs485/init 无响应");
    const QJsonObject resp = varToObj(r);
    if (resp.contains("err"))
        return QStringLiteral("init err: %1").arg(
            resp.value("err").toObject().value("message").toString());
    return {};
}

bool Revo2HandClient::isReady() const
{
    return m_state.connected && m_state.inited;
}

void Revo2HandClient::close()
{
    m_state.connected = false;
    m_state.inited = false;
    m_state.enabled = false;
}

int Revo2HandClient::writeTimeMs() const
{
    return m_writeTimeMs;
}

void Revo2HandClient::setWriteTimeMs(int ms)
{
    m_writeTimeMs = qBound(HAND_WRITE_TIME_MS_MIN, ms, HAND_WRITE_TIME_MS_MAX);
}

QByteArray Revo2HandClient::ec2rsWriteRead(const QByteArray &data, int readLength)
{
    // 写
    QJsonObject db;
    db.insert("slave", m_ec2rsSlave);
    QJsonArray arr;
    for (unsigned char c : data)
        arr.append(static_cast<int>(c));
    db.insert("data", arr);
    const QVariant wr = m_ws->sendAndWait(
        QStringLiteral("ec2rs485/write"), db, 3000);
    if (!wr.isValid()) return {};
    const QJsonObject wro = varToObj(wr);
    if (wro.contains("err")) return {};
    ++m_txCount;

    // 读
    QJsonObject dbr;
    dbr.insert("slave", m_ec2rsSlave);
    dbr.insert("timeout", 400);
    if (readLength > 0) dbr.insert("length", readLength);
    const QVariant rd = m_ws->sendAndWait(
        QStringLiteral("ec2rs485/read"), dbr,
        static_cast<int>((400 / 1000.0) + 3.0) * 1000);
    if (!rd.isValid()) return {};
    const QJsonObject rdo = varToObj(rd);
    if (rdo.contains("err")) return {};
    const QJsonValue rxv = rdo.value("db");
    if (!rxv.isArray()) return {};
    const QJsonArray rxa = rxv.toArray();
    QByteArray rx;
    rx.reserve(rxa.size());
    for (const QJsonValue &bv : rxa)
        rx.append(static_cast<char>(bv.toInt(0) & 0xFF));
    ++m_rxCount;
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

    // 写
    QJsonObject db;
    db.insert(QStringLiteral("slave"), m_ec2rsSlave);
    QJsonArray arr;
    for (unsigned char c : frame)
        arr.append(static_cast<int>(c));
    db.insert(QStringLiteral("data"), arr);

    const QVariant wr = m_ws->sendAndWait(QStringLiteral("ec2rs485/write"), db, 3000);
    if (!wr.isValid()) return QStringLiteral("写超时");
    const QString werr = respErr(varToObj(wr));
    if (!werr.isEmpty()) return QStringLiteral("写失败: ") + werr;
    ++m_txCount;

    // 只发不读
    if (readLength <= 0) return {};

    // 读
    QJsonObject dbr;
    dbr.insert(QStringLiteral("slave"), m_ec2rsSlave);
    dbr.insert(QStringLiteral("timeout"), 400);
    dbr.insert(QStringLiteral("length"), readLength);

    const QVariant rd = m_ws->sendAndWait(QStringLiteral("ec2rs485/read"), dbr, 3000);
    if (!rd.isValid()) return QStringLiteral("读超时");
    const QString rerr = respErr(varToObj(rd));
    if (!rerr.isEmpty()) return QStringLiteral("读失败: ") + rerr;

    const QJsonArray rxa = varToObj(rd).value(QStringLiteral("db")).toArray();
    QByteArray rx;
    rx.reserve(rxa.size());
    for (const QJsonValue &bv : rxa)
        rx.append(static_cast<char>(bv.toInt(0) & 0xFF));
    ++m_rxCount;
    return QString::fromLatin1(rx.toHex(' ')).trimmed();
}

QJsonObject Revo2HandClient::ec2rsGetStatus()
{
    if (!m_ws) return {};
    QJsonObject db;
    db.insert(QStringLiteral("slave"), m_ec2rsSlave);
    const QVariant resp = m_ws->sendAndWait(
        QStringLiteral("ec2rs485/getStatus"), db, 3000);
    if (!resp.isValid()) return {};
    return varToObj(resp);
}

QString Revo2HandClient::writeDof(int dofIdx, int position)
{
    if (dofIdx < 0 || dofIdx >= HAND_DOF_COUNT)
        return QStringLiteral("DOF 索引越界: %1").arg(dofIdx);
    position = qBound(0, position, 1000);

    if (!m_state.inited) {
        const QString err = initChannel();
        if (!err.isEmpty()) { m_state.lastError = err; ++m_errCount; return err; }
    }

    const QByteArray frame = buildWriteDofFrame(
        m_modbusAddr, dofIdx, position, m_writeTimeMs);
    const QByteArray rx = ec2rsWriteRead(frame, 8);
    if (rx.isEmpty()) { m_state.lastError = QStringLiteral("无应答"); ++m_errCount; return m_state.lastError; }
    if (!verifyCrc(rx)) { m_state.lastError = QStringLiteral("CRC 错误"); ++m_errCount; return m_state.lastError; }
    const QString serr = checkSlaveErrorByte(rx);
    if (!serr.isEmpty()) { m_state.lastError = serr; ++m_errCount; return serr; }
    if ((rx.at(1) & 0xFF) != 0x10) {
        m_state.lastError = QStringLiteral("非预期功能码 0x%1").arg(
            static_cast<unsigned char>(rx.at(1)), 2, 16, QChar('0').toUpper());
        ++m_errCount;
        return m_state.lastError;
    }

    m_state.positions[dofIdx] = position;
    m_state.targetPositions[dofIdx] = position;
    m_state.lastError.clear();
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

    if (!m_state.inited) {
        const QString err = initChannel();
        if (!err.isEmpty()) { m_state.lastError = err; ++m_errCount; return err; }
    }

    const QByteArray frame = buildWriteAllFrame(m_modbusAddr, clamped, m_writeTimeMs);
    const QByteArray rx = ec2rsWriteRead(frame, 8);
    if (rx.isEmpty()) { m_state.lastError = QStringLiteral("无应答"); ++m_errCount; return m_state.lastError; }
    if (!verifyCrc(rx)) { m_state.lastError = QStringLiteral("CRC 错误"); ++m_errCount; return m_state.lastError; }
    const QString serr = checkSlaveErrorByte(rx);
    if (!serr.isEmpty()) { m_state.lastError = serr; ++m_errCount; return serr; }
    if ((rx.at(1) & 0xFF) != 0x10) {
        m_state.lastError = QStringLiteral("非预期功能码 0x%1").arg(
            static_cast<unsigned char>(rx.at(1)), 2, 16, QChar('0').toUpper());
        ++m_errCount;
        return m_state.lastError;
    }

    m_state.positions = clamped;
    m_state.targetPositions = clamped;
    m_state.lastError.clear();
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
    if (!m_state.inited) {
        const QString err = initChannel();
        if (!err.isEmpty()) { m_state.lastError = err; return err; }
    }

    const QByteArray frame = buildReadFrame(m_modbusAddr, HAND_REG_READ_POS, HAND_DOF_COUNT);
    const QByteArray rx = ec2rsWriteRead(frame, 17);
    if (rx.isEmpty()) { m_state.lastError = QStringLiteral("无应答"); return m_state.lastError; }
    if (!verifyCrc(rx)) { m_state.lastError = QStringLiteral("CRC 错误"); return m_state.lastError; }
    if ((rx.at(1) & 0xFF) != 0x04) {
        m_state.lastError = QStringLiteral("非预期功能码 0x%1").arg(
            static_cast<unsigned char>(rx.at(1)), 2, 16, QChar('0').toUpper());
        return m_state.lastError;
    }
    if ((rx.at(2) & 0xFF) != HAND_DOF_COUNT * 2) {
        m_state.lastError = QStringLiteral("字节数不匹配: %1").arg(rx.at(2) & 0xFF);
        return m_state.lastError;
    }

    QVector<int> pos;
    pos.reserve(HAND_DOF_COUNT);
    for (int i = 0; i < HAND_DOF_COUNT; ++i)
        pos.append(((rx.at(3 + i*2) & 0xFF) << 8) | (rx.at(4 + i*2) & 0xFF));
    m_state.positions = pos;
    m_state.lastError.clear();
    notifyState();
    return {};
}

HandState Revo2HandClient::state() const
{
    return m_state;
}

void Revo2HandClient::registerStateCallback(
        std::function<void(const HandState &)> cb)
{
    if (cb) m_callbacks.append(cb);
}

void Revo2HandClient::notifyState()
{
    const QVector<std::function<void(const HandState &)>> cbs = m_callbacks;
    const HandState s = m_state;
    for (const auto &f : cbs) {
        try { f(s); } catch (...) {}
    }
    emit stateChanged(m_state);
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
