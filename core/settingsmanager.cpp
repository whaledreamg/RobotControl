#include "settingsmanager.h"

namespace {
constexpr const char* K_ARM_IP            = "network/armIp";
constexpr const char* K_AGV_IP            = "network/agvIp";
constexpr const char* K_ARM_WEB_URL       = "network/armWebUrl";
constexpr const char* K_ARM_WS_PORT       = "network/armWsPort";
constexpr const char* K_AGV_PUSH_PORT     = "network/agvPushPort";
constexpr const char* K_AGV_CTRL_PORT     = "network/agvCtrlPort";
constexpr const char* K_AGV_NAV_PORT      = "network/agvNavPort";
constexpr const char* K_AGV_STATUS_PORT   = "network/agvStatusPort";
constexpr const char* K_AGV_OTHER_PORT    = "network/agvOtherPort";
constexpr const char* K_AGV_CONFIG_PORT   = "network/agvConfigPort";
constexpr const char* K_LEFT_ARM_NAME     = "device/leftArmName";
constexpr const char* K_RIGHT_ARM_NAME    = "device/rightArmName";
constexpr const char* K_PUSH_INTERVAL     = "device/pushInterval";
constexpr const char* K_DEFAULT_SPEED     = "control/defaultSpeed";
constexpr const char* K_ARM_SPEED         = "control/armSpeed";
constexpr const char* K_GRIPPER_FORCE     = "control/gripperForce";
constexpr const char* K_LIFT_DEVICE       = "liftingColumn/device";
constexpr const char* K_LIFT_TARGET_MM    = "liftingColumn/targetHeightMm";
constexpr const char* K_LIFT_SPEED        = "liftingColumn/speedMps";
constexpr const char* K_LIFT_JOG_SPEED    = "liftingColumn/jogSpeedMps";
constexpr const char* K_LIFT_JOG_TIMEOUT  = "liftingColumn/jogTimeoutMs";
constexpr const char* K_WINDOW_SIZE       = "ui/windowSize";
constexpr const char* K_WINDOW_POS        = "ui/windowPos";
}

SettingsManager* SettingsManager::instance()
{
    static SettingsManager inst;
    return &inst;
}

SettingsManager::SettingsManager(QObject *parent)
    : QObject(parent)
{
}

// ===== 加载/保存 =====
void SettingsManager::loadSettings()
{
    m_settings.sync();
}

void SettingsManager::saveSettings()
{
    m_settings.sync();
    emit settingsChanged();
}

// ===== IP 与端口 =====
QString SettingsManager::armIp() const
{
    return m_settings.value(K_ARM_IP, QString::fromLatin1(Common::DEFAULT_ARM_IP)).toString();
}

void SettingsManager::setArmIp(const QString &ip)
{
    m_settings.setValue(K_ARM_IP, ip);
}

QString SettingsManager::agvIp() const
{
    return m_settings.value(K_AGV_IP, QString::fromLatin1(Common::DEFAULT_AGV_IP)).toString();
}

void SettingsManager::setAgvIp(const QString &ip)
{
    m_settings.setValue(K_AGV_IP, ip);
}

QString SettingsManager::armWebUrl() const
{
    const QString url = m_settings.value(K_ARM_WEB_URL).toString().trimmed();
    return url.isEmpty() ? QString::fromLatin1(Common::DEFAULT_ARM_WEB_URL) : url;
}

void SettingsManager::setArmWebUrl(const QString &url)
{
    m_settings.setValue(K_ARM_WEB_URL, url.trimmed());
}

quint16 SettingsManager::armWsPort() const
{
    return static_cast<quint16>(m_settings.value(K_ARM_WS_PORT, Common::DEFAULT_ARM_WS_PORT).toUInt());
}

void SettingsManager::setArmWsPort(quint16 port)
{
    m_settings.setValue(K_ARM_WS_PORT, port);
}

quint16 SettingsManager::agvPushPort() const
{
    return static_cast<quint16>(m_settings.value(K_AGV_PUSH_PORT, Common::DEFAULT_AGV_PUSH_PORT).toUInt());
}

void SettingsManager::setAgvPushPort(quint16 port)
{
    m_settings.setValue(K_AGV_PUSH_PORT, port);
}

quint16 SettingsManager::agvCtrlPort() const
{
    return static_cast<quint16>(m_settings.value(K_AGV_CTRL_PORT, Common::DEFAULT_AGV_CTRL_PORT).toUInt());
}

void SettingsManager::setAgvCtrlPort(quint16 port)
{
    m_settings.setValue(K_AGV_CTRL_PORT, port);
}

quint16 SettingsManager::agvNavPort() const
{
    return static_cast<quint16>(m_settings.value(K_AGV_NAV_PORT, Common::DEFAULT_AGV_NAV_PORT).toUInt());
}

void SettingsManager::setAgvNavPort(quint16 port)
{
    m_settings.setValue(K_AGV_NAV_PORT, port);
}

quint16 SettingsManager::agvStatusPort() const
{
    return static_cast<quint16>(m_settings.value(K_AGV_STATUS_PORT, Common::DEFAULT_AGV_STATE_PORT).toUInt());
}

void SettingsManager::setAgvStatusPort(quint16 port)
{
    m_settings.setValue(K_AGV_STATUS_PORT, port);
}

quint16 SettingsManager::agvOtherPort() const
{
    return static_cast<quint16>(m_settings.value(K_AGV_OTHER_PORT, Common::DEFAULT_AGV_OTHER_PORT).toUInt());
}

void SettingsManager::setAgvOtherPort(quint16 port)
{
    m_settings.setValue(K_AGV_OTHER_PORT, port);
}

quint16 SettingsManager::agvConfigPort() const
{
    return static_cast<quint16>(m_settings.value(K_AGV_CONFIG_PORT, Common::DEFAULT_AGV_CONFIG_PORT).toUInt());
}

void SettingsManager::setAgvConfigPort(quint16 port)
{
    m_settings.setValue(K_AGV_CONFIG_PORT, port);
}

// ===== 机械臂名称 =====
QString SettingsManager::leftArmName() const
{
    return m_settings.value(K_LEFT_ARM_NAME, QString::fromLatin1(Common::DEFAULT_LEFT_ARM_NAME)).toString();
}

void SettingsManager::setLeftArmName(const QString &name)
{
    m_settings.setValue(K_LEFT_ARM_NAME, name);
}

QString SettingsManager::rightArmName() const
{
    return m_settings.value(K_RIGHT_ARM_NAME, QString::fromLatin1(Common::DEFAULT_RIGHT_ARM_NAME)).toString();
}

void SettingsManager::setRightArmName(const QString &name)
{
    m_settings.setValue(K_RIGHT_ARM_NAME, name);
}

// ===== 推送频率 =====
int SettingsManager::pushInterval() const
{
    return m_settings.value(K_PUSH_INTERVAL, Common::DEFAULT_PUSH_INTERVAL_MS).toInt();
}

void SettingsManager::setPushInterval(int ms)
{
    m_settings.setValue(K_PUSH_INTERVAL, ms);
}

// ===== 控制参数 =====
double SettingsManager::defaultSpeed() const
{
    return m_settings.value(K_DEFAULT_SPEED, Common::DEFAULT_SPEED).toDouble();
}

void SettingsManager::setDefaultSpeed(double speed)
{
    m_settings.setValue(K_DEFAULT_SPEED, speed);
}

double SettingsManager::armSpeed() const
{
    return m_settings.value(K_ARM_SPEED, 1.0).toDouble();
}

void SettingsManager::setArmSpeed(double ratio)
{
    m_settings.setValue(K_ARM_SPEED, ratio);
}

int SettingsManager::defaultGripperForce() const
{
    return m_settings.value(K_GRIPPER_FORCE, Common::GRIPPER_DEFAULT_FORCE).toInt();
}

void SettingsManager::setDefaultGripperForce(int force)
{
    m_settings.setValue(K_GRIPPER_FORCE, force);
}

// ===== 升降柱 =====
QString SettingsManager::liftingDevice() const
{
    return m_settings.value(K_LIFT_DEVICE, QStringLiteral("Column")).toString();
}

void SettingsManager::setLiftingDevice(const QString &name)
{
    m_settings.setValue(K_LIFT_DEVICE, name);
}

int SettingsManager::liftingTargetHeightMm() const
{
    return m_settings.value(K_LIFT_TARGET_MM, 300).toInt();
}

void SettingsManager::setLiftingTargetHeightMm(int mm)
{
    m_settings.setValue(K_LIFT_TARGET_MM, mm);
}

double SettingsManager::liftingSpeedMps() const
{
    return m_settings.value(K_LIFT_SPEED, 0.05).toDouble();
}

void SettingsManager::setLiftingSpeedMps(double v)
{
    m_settings.setValue(K_LIFT_SPEED, v);
}

double SettingsManager::liftingJogSpeedMps() const
{
    return m_settings.value(K_LIFT_JOG_SPEED, 0.03).toDouble();
}

void SettingsManager::setLiftingJogSpeedMps(double v)
{
    m_settings.setValue(K_LIFT_JOG_SPEED, v);
}

int SettingsManager::liftingJogTimeoutMs() const
{
    return m_settings.value(K_LIFT_JOG_TIMEOUT, 500).toInt();
}

void SettingsManager::setLiftingJogTimeoutMs(int ms)
{
    m_settings.setValue(K_LIFT_JOG_TIMEOUT, qBound(100, ms, 5000));
}

// ===== 窗口几何信息 =====
QSize SettingsManager::windowSize() const
{
    return m_settings.value(K_WINDOW_SIZE, QSize(1280, 800)).toSize();
}

void SettingsManager::setWindowSize(const QSize &size)
{
    m_settings.setValue(K_WINDOW_SIZE, size);
}

QPoint SettingsManager::windowPos() const
{
    return m_settings.value(K_WINDOW_POS, QPoint(100, 100)).toPoint();
}

void SettingsManager::setWindowPos(const QPoint &pos)
{
    m_settings.setValue(K_WINDOW_POS, pos);
}

// ===== 恢复默认 =====
void SettingsManager::resetToDefault()
{
    m_settings.setValue(K_ARM_IP,          QString::fromLatin1(Common::DEFAULT_ARM_IP));
    m_settings.setValue(K_AGV_IP,          QString::fromLatin1(Common::DEFAULT_AGV_IP));
    m_settings.setValue(K_ARM_WEB_URL,     QString::fromLatin1(Common::DEFAULT_ARM_WEB_URL));
    m_settings.setValue(K_ARM_WS_PORT,     Common::DEFAULT_ARM_WS_PORT);
    m_settings.setValue(K_AGV_PUSH_PORT,   Common::DEFAULT_AGV_PUSH_PORT);
    m_settings.setValue(K_AGV_CTRL_PORT,   Common::DEFAULT_AGV_CTRL_PORT);
    m_settings.setValue(K_AGV_NAV_PORT,    Common::DEFAULT_AGV_NAV_PORT);
    m_settings.setValue(K_AGV_STATUS_PORT, Common::DEFAULT_AGV_STATE_PORT);
    m_settings.setValue(K_AGV_OTHER_PORT,  Common::DEFAULT_AGV_OTHER_PORT);
    m_settings.setValue(K_AGV_CONFIG_PORT, Common::DEFAULT_AGV_CONFIG_PORT);
    m_settings.setValue(K_LEFT_ARM_NAME,   QString::fromLatin1(Common::DEFAULT_LEFT_ARM_NAME));
    m_settings.setValue(K_RIGHT_ARM_NAME,  QString::fromLatin1(Common::DEFAULT_RIGHT_ARM_NAME));
    m_settings.setValue(K_PUSH_INTERVAL,   Common::DEFAULT_PUSH_INTERVAL_MS);
    m_settings.setValue(K_DEFAULT_SPEED,   Common::DEFAULT_SPEED);
    m_settings.setValue(K_ARM_SPEED,       1.0);
    m_settings.setValue(K_GRIPPER_FORCE,   Common::GRIPPER_DEFAULT_FORCE);
    m_settings.setValue(K_LIFT_DEVICE,     QStringLiteral("Column"));
    m_settings.setValue(K_LIFT_TARGET_MM,  300);
    m_settings.setValue(K_LIFT_SPEED,      0.05);
    m_settings.setValue(K_LIFT_JOG_SPEED,  0.03);
    m_settings.setValue(K_LIFT_JOG_TIMEOUT, 500);
    m_settings.sync();
    emit settingsChanged();
}
