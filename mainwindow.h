#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include "../common/types.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class AgvTcpClient;
class ArmWebSocket;
class LiftingColumnClient;
class SettingsDialog;
class WebViewDialog;
class MapInfoDialog;
class DualHandManager;
class DualHandPanel;
class MessageManager;
class HandViewDialog;
class QTimer;

/**
 * @brief 主窗口 - 单 .ui 文件风格 (布局参考 Python)
 *
 * 架构对齐 Python ui/main_window.py：
 *   - 机械臂用**单条 WebSocket** 承载左右臂（ArmWebSocket 内部按 armName 区分）
 *   - Revo2 灵巧手复用同一条连接（DualHandManager）
 *   - 全局消息弹窗统一走 MessageManager
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    // ===== 顶部 =====
    void onConnectClicked();
    void onSettingsClicked();
    void onClearErrorClicked();
    void onUpdateClock();

    // ===== AGV =====
    void onAgvState(const AgvState &state);
    void onAgvCtrlConnected(bool connected);
    void onAgvPushConnected(bool connected);
    void onAgvError(const QString &err);

    void onAgvForwardPressed();   void onAgvForwardReleased();
    void onAgvBackwardPressed();  void onAgvBackwardReleased();
    void onAgvLeftPressed();      void onAgvLeftReleased();
    void onAgvRightPressed();     void onAgvRightReleased();
    void onAgvStopClicked();
    void onAgvEmergencyClicked();
    void onAgvNavStartClicked();
    void onAgvNavPauseClicked();
    void onAgvNavCancelClicked();
    void onAgvViewMapClicked();
    void onAgvSpeedChanged(int value);
    void onAgvStationChanged(const QString &text);
    void onRefreshMap();
    void onAgvMapListReceived(const QVariantList &maps, const QString &currentMap);
    void onAgvMapDataReceived(const QString &mapName, const QVariantMap &mapData);

    // ===== 机械臂 =====
    void onArmConnected(bool connected);
    void onArmRobotStatus(const QString &armName, const QJsonObject &db);
    void onArmProjectList(const QVariantList &projects);
    void onArmProjectStatus(const QJsonObject &db);
    void onArmError(const QString &err);

    void onLeftArmEnableClicked(bool checked);
    void onRightArmEnableClicked(bool checked);
    void onArmSpeedChanged(int v);
    void onArmRefreshClicked();
    void onArmViewClicked();
    void onArmProjectStartClicked();
    void onArmProjectPauseClicked();
    void onArmProjectResumeClicked();
    void onArmProjectStopClicked();

    // ===== Revo2 灵巧手 =====
    void onHandInit();
    void onHandViewRequested(const QString &side);
    void onHandGrabRequested(const QString &side);
    void onHandReleaseRequested(const QString &side);
    void onHandStopAllRequested();

    // ===== 升降柱 =====
    void onLiftRefreshClicked();
    void onLiftEnableClicked();
    void onLiftDisableClicked();
    void onLiftClearErrorClicked();
    void onLiftMoveClicked();
    void onLiftStopClicked();
    void onLiftJogUpPressed();
    void onLiftJogUpReleased();
    void onLiftJogDownPressed();
    void onLiftJogDownReleased();
    void onLiftSetZeroClicked();
    void onLiftDeviceChanged(const QString &text);

private:
    void setupCommunication();
    void connectSignals();
    void updateConnectionStatusUi();
    void applyConnectionState();
    void initLiftingColumn();
    void setupHandPanel();

    // AGV UI
    void updateAgvStateUi(const AgvState &state);
    void updateNavButtons(const QString &status);
    QString currentDirection() const;
    void sendAgvDirectionCommand(const QString &dir, bool pressed);

    // Arm UI
    void updateArmStateUi(const QString &armName, const QJsonObject &db);
    void updateProjectStatusUi(const QString &state, const QString &project);
    void updateProjectButtons(const QString &status);
    void updateArmEnableButton(bool left, bool enabled);
    void updateSpeedHint(int value);

    // Gripper UI
    void updateGripperStatus(const QString &text);

    // Lifting UI
    void refreshLiftLabels();
    void setLiftAlert(const QString &text, const QString &bgColor);

    Ui::MainWindow *ui;

    AgvTcpClient *m_agvClient = nullptr;
    ArmWebSocket *m_leftArm   = nullptr;   // 单连接，承载左右臂
    ArmWebSocket *m_rightArm  = nullptr;   // 保留指针 = m_leftArm（单连接架构）

    // Revo2 灵巧手
    DualHandManager *m_handManager = nullptr;
    DualHandPanel   *m_handPanel   = nullptr;

    MessageManager  *m_messageManager = nullptr;
    SettingsDialog  *m_settingsDialog = nullptr;
    MapInfoDialog   *m_mapDialog      = nullptr;

    bool m_armConnected = false;
    bool m_agvConnected = false;

    // AGV 导航状态: "running" / "paused" / "idle"
    QString m_navStatus = QStringLiteral("idle");
    QString m_lastDirection;

    // 机械臂状态缓存
    QJsonObject m_lastLeftArmState;
    QJsonObject m_lastRightArmState;
    bool     m_leftArmEnabled  = false;
    bool     m_rightArmEnabled = false;

    // 工程状态
    QString m_currentProject = QStringLiteral("无");
    QString m_projectState   = QStringLiteral("待机");
    QStringList m_projectList;

    // 地图
    QString       m_currentMapName;
    QVariantList  m_mapList;
    QVariantMap   m_currentMapData;

    // 升降柱
    LiftingColumnClient *m_lifting = nullptr;
    QTimer *m_clockTimer = nullptr;
};

#endif // MAINWINDOW_H