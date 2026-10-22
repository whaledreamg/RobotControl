#ifndef CORE_SETTINGSMANAGER_H
#define CORE_SETTINGSMANAGER_H

#include <QObject>
#include <QSettings>
#include <QString>
#include <QSize>
#include <QPoint>
#include "../common/constants.h"

/**
 * @brief 配置管理器 (单例, 基于 QSettings)
 *
 * 封装所有用户配置的读取与持久化。
 * 键名与 Python 版 core/settings_manager.py 保持语义一致
 * （Python 用 "connection/ip" 等斜杠键，C++ 用 "network/armIp" 等，
 *  两者各自独立存储，互不影响）。
 */
class SettingsManager : public QObject
{
    Q_OBJECT
public:
    static SettingsManager* instance();

    // ------ 加载与保存 ------
    void loadSettings();
    void saveSettings();

    // ------ IP 与端口 ------
    QString armIp() const;
    void setArmIp(const QString &ip);

    QString agvIp() const;
    void setAgvIp(const QString &ip);

    quint16 armWsPort() const;
    void setArmWsPort(quint16 port);

    quint16 agvPushPort() const;
    void setAgvPushPort(quint16 port);

    quint16 agvCtrlPort() const;
    void setAgvCtrlPort(quint16 port);

    quint16 agvNavPort() const;
    void setAgvNavPort(quint16 port);

    quint16 agvStatusPort() const;
    void setAgvStatusPort(quint16 port);

    quint16 agvOtherPort() const;
    void setAgvOtherPort(quint16 port);

    /// 配置端口（19207）—— 4011 GET_MAP 必须走这个端口
    quint16 agvConfigPort() const;
    void setAgvConfigPort(quint16 port);

    // ------ 机械臂名称 ------
    QString leftArmName() const;
    void setLeftArmName(const QString &name);

    QString rightArmName() const;
    void setRightArmName(const QString &name);

    // ------ 推送频率 ------
    int pushInterval() const;
    void setPushInterval(int ms);

    // ------ 速度 / 力度 ------
    double defaultSpeed() const;
    void setDefaultSpeed(double speed);

    double armSpeed() const;
    void setArmSpeed(double ratio);

    int defaultGripperForce() const;
    void setDefaultGripperForce(int force);

    // ------ 升降柱（liftingColumn）------
    /// 默认升降柱设备名（出厂 "Column"）
    QString liftingDevice() const;
    void setLiftingDevice(const QString &name);

    /// UI 上次输入的目标高度（mm）；与控制器 m 单位间由界面换算
    int liftingTargetHeightMm() const;
    void setLiftingTargetHeightMm(int mm);

    /// 运动缺省速度（m/s）；<=0 表示交给控制器用 maxVelocity
    double liftingSpeedMps() const;
    void setLiftingSpeedMps(double v);

    /// 点动缺省速度（m/s）
    double liftingJogSpeedMps() const;
    void setLiftingJogSpeedMps(double v);

    /// 点动心跳超时（ms）；范围 100~5000
    int liftingJogTimeoutMs() const;
    void setLiftingJogTimeoutMs(int ms);

    // ------ 窗口几何信息 ------
    QSize windowSize() const;
    void setWindowSize(const QSize &size);

    QPoint windowPos() const;
    void setWindowPos(const QPoint &pos);

    // ------ 恢复默认 ------
    void resetToDefault();

signals:
    void settingsChanged();

private:
    explicit SettingsManager(QObject *parent = nullptr);
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    QSettings m_settings;
};

#endif // CORE_SETTINGSMANAGER_H
