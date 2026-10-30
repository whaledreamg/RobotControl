#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "communication/agvtcpclient.h"
#include "communication/armwebsocket.h"
#include "communication/liftingcolumn.h"
#include "communication/revo2hand.h"
#include "dialogs/settingsdialog.h"
#include "dialogs/mapinfodialog.h"
#include "dialogs/handviewdialog.h"
#include "ui/handpanel.h"
#include "ui/messagewidget.h"
#include "core/settingsmanager.h"
#include "core/qssmanager.h"
#include "common/global.h"
// QtWebEngineWidgets 仅在桌面平台可用。Android 上该模块不存在，
// 因此 webviewdialog 系列源文件被排除（见 dialogs/dialogs.pri），
// 这里也相应条件包含头文件。
#if !defined(Q_OS_IOS) && !defined(Q_OS_WASM)
#include "dialogs/webviewdialog.h"
#endif

#include <QCloseEvent>
#include <QStatusBar>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QTimer>
#include <QDateTime>
#include <QVariantList>
#include <QVariantMap>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QVector>
#include <cmath>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    QssManager::instance()->loadStyleForWidget(this, QStringLiteral("mainwindow"));

    setupCommunication();
    connectSignals();

    // 顶部 IP 默认值
    auto *s = SettingsManager::instance();
    ui->editArmIp->setText(s->armIp());
    ui->editAgvIp->setText(s->agvIp());

    // AGV 速度滑块
    const int initAgvSpeed = static_cast<int>(s->defaultSpeed() * 100);
    ui->sliderAgvSpeed->setValue(initAgvSpeed);
    ui->lblAgvSpeedVal->setText(QStringLiteral("速度: %1 m/s")
                                    .arg(s->defaultSpeed(), 0, 'f', 2));

    // 机械臂速度滑块
    const int initArmSpeed = static_cast<int>(s->armSpeed() * 100);
    ui->sliderArmSpeed->setValue(initArmSpeed);
    ui->lblArmSpeedVal->setText(QStringLiteral("%1%").arg(initArmSpeed));
    updateSpeedHint(initArmSpeed);

    // 夹爪力度
    ui->sliderGripperForce->setValue(s->defaultGripperForce());
    ui->lblGripperForceVal->setText(QStringLiteral("%1%").arg(s->defaultGripperForce()));

    // 升降柱默认目标高度（mm）
    ui->spinLiftTarget->setValue(s->liftingTargetHeightMm());

    // 全局消息弹窗（浮在所有子控件之上）
    m_messageManager = new MessageManager(this);

    // Revo2 灵巧手面板
    setupHandPanel();

    resize(s->windowSize());
    move(s->windowPos());

    // 状态栏: 时间
    m_clockTimer = new QTimer(this);
    m_clockTimer->setInterval(1000);
    connect(m_clockTimer, &QTimer::timeout, this, &MainWindow::onUpdateClock);
    m_clockTimer->start();
    onUpdateClock();

    updateConnectionStatusUi();
    applyConnectionState();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    auto *s = SettingsManager::instance();
    s->setWindowSize(size());
    s->setWindowPos(pos());
    s->setArmIp(ui->editArmIp->text().trimmed());
    s->setAgvIp(ui->editAgvIp->text().trimmed());
    s->setDefaultGripperForce(ui->sliderGripperForce->value());
    s->setArmSpeed(ui->sliderArmSpeed->value() / 100.0);
    s->saveSettings();

    if (m_agvClient) m_agvClient->disconnectFromHost();
    if (m_handManager) m_handManager->close();
    if (m_leftArm)   m_leftArm->disconnectFromArm();

    QMainWindow::closeEvent(event);
}

void MainWindow::setupCommunication()
{
    m_agvClient = new AgvTcpClient(this);

    // 机械臂：单条 WebSocket 承载左右臂（对齐 Python ArmConnectionManager）
    m_leftArm  = new ArmWebSocket(this);
    m_rightArm = m_leftArm;   // 同一连接的两个别名

    auto *s = SettingsManager::instance();
    m_leftArm->setArmNames(s->leftArmName(), s->rightArmName());
    m_leftArm->setPushInterval(s->pushInterval());
}

void MainWindow::setupHandPanel()
{
    // Revo2 灵巧手面板已由 .ui 提升为 QGroupBox#handGroup 内的占位，
    // 这里动态插入到该 GroupBox 的布局里。
    if (!m_handPanel) {
        QGroupBox *handBox = findChild<QGroupBox*>(QStringLiteral("handGroup"));
        if (!handBox) return;
        m_handPanel = new DualHandPanel(handBox);
        m_handPanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        if (auto *lay = qobject_cast<QVBoxLayout*>(handBox->layout())) {
            lay->addWidget(m_handPanel);
        }
        handBox->setMaximumHeight(280);
    }

    connect(m_handPanel, &DualHandPanel::initRequested,
            this, &MainWindow::onHandInit);
    connect(m_handPanel, &DualHandPanel::viewRequested,
            this, &MainWindow::onHandViewRequested);
    connect(m_handPanel, &DualHandPanel::grabRequested,
            this, &MainWindow::onHandGrabRequested);
    connect(m_handPanel, &DualHandPanel::releaseRequested,
            this, &MainWindow::onHandReleaseRequested);
    connect(m_handPanel, &DualHandPanel::stopAllRequested,
            this, &MainWindow::onHandStopAllRequested);
}

void MainWindow::initLiftingColumn()
{
    if (!m_leftArm || m_lifting) return;
    m_lifting = new LiftingColumnClient(m_leftArm, this);

    auto cb = [this](const QVector<QJsonObject> &states) {
        for (const QJsonObject &st : states) {
            const QString dev = st.value(QStringLiteral("deviceName")).toString();
            const double h = st.value(QStringLiteral("currentPosition")).toDouble();
            if (!dev.isEmpty()) {
                ui->lblLiftHeight->setText(
                    QStringLiteral("%1 mm").arg(h * 1000.0, 0, 'f', 1));
            }
        }
    };
    m_lifting->subscribeState(cb, SettingsManager::instance()->pushInterval());

    QVector<QJsonObject> devs = m_lifting->listDevices();
    QStringList names;
    for (const QJsonObject &d : devs) {
        const QString n = d.value(QStringLiteral("deviceName")).toString();
        if (!n.isEmpty()) names << n;
    }
    if (!names.isEmpty()) {
        m_lifting->setKnownDevices(names);
        ui->cmbLiftDevices->clear();
        ui->cmbLiftDevices->addItems(names);
    }
}

void MainWindow::connectSignals()
{
    // ===== 顶部 =====
    connect(ui->btnConnect,    &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(ui->btnSettings,   &QPushButton::clicked, this, &MainWindow::onSettingsClicked);
    connect(ui->btnClearError, &QPushButton::clicked, this, &MainWindow::onClearErrorClicked);

    // ===== AGV 通信 =====
    connect(m_agvClient, &AgvTcpClient::stateReceived,        this, &MainWindow::onAgvState);
    connect(m_agvClient, &AgvTcpClient::ctrlConnectedChanged, this, &MainWindow::onAgvCtrlConnected);
    connect(m_agvClient, &AgvTcpClient::pushConnectedChanged, this, &MainWindow::onAgvPushConnected);
    connect(m_agvClient, &AgvTcpClient::errorOccurred,        this, &MainWindow::onAgvError);
    connect(m_agvClient, &AgvTcpClient::mapListReceived,     this, &MainWindow::onAgvMapListReceived);
    connect(m_agvClient, &AgvTcpClient::mapDataReceived,     this, &MainWindow::onAgvMapDataReceived);

    // ===== AGV 遥控 =====
    connect(ui->btnAgvForward,  &QPushButton::pressed,  this, &MainWindow::onAgvForwardPressed);
    connect(ui->btnAgvForward,  &QPushButton::released, this, &MainWindow::onAgvForwardReleased);
    connect(ui->btnAgvBackward, &QPushButton::pressed,  this, &MainWindow::onAgvBackwardPressed);
    connect(ui->btnAgvBackward, &QPushButton::released, this, &MainWindow::onAgvBackwardReleased);
    connect(ui->btnAgvLeft,     &QPushButton::pressed,  this, &MainWindow::onAgvLeftPressed);
    connect(ui->btnAgvLeft,     &QPushButton::released, this, &MainWindow::onAgvLeftReleased);
    connect(ui->btnAgvRight,    &QPushButton::pressed,  this, &MainWindow::onAgvRightPressed);
    connect(ui->btnAgvRight,    &QPushButton::released, this, &MainWindow::onAgvRightReleased);
    connect(ui->btnAgvStop,     &QPushButton::clicked,  this, &MainWindow::onAgvStopClicked);
    connect(ui->btnAgvEmergency,&QPushButton::clicked,  this, &MainWindow::onAgvEmergencyClicked);
    connect(ui->sliderAgvSpeed, &QSlider::valueChanged, this, &MainWindow::onAgvSpeedChanged);
    connect(ui->btnAgvNavStart, &QPushButton::clicked,  this, &MainWindow::onAgvNavStartClicked);
    connect(ui->btnAgvNavPause, &QPushButton::clicked,  this, &MainWindow::onAgvNavPauseClicked);
    connect(ui->btnAgvNavCancel,&QPushButton::clicked,  this, &MainWindow::onAgvNavCancelClicked);
    connect(ui->btnViewMap,     &QPushButton::clicked,  this, &MainWindow::onAgvViewMapClicked);
    connect(ui->cmbAgvStations, &QComboBox::currentTextChanged,
            this, &MainWindow::onAgvStationChanged);

    // ===== 机械臂通信（单连接，按 armName 分流）=====
    connect(m_leftArm, &ArmWebSocket::connectedChanged,  this, &MainWindow::onArmConnected);
    connect(m_leftArm, &ArmWebSocket::robotStatusReceived,this, &MainWindow::onArmRobotStatus);
    connect(m_leftArm, &ArmWebSocket::projectListReceived,this, &MainWindow::onArmProjectList);
    connect(m_leftArm, &ArmWebSocket::projectStatusReceived,this, &MainWindow::onArmProjectStatus);
    connect(m_leftArm, &ArmWebSocket::errorOccurred,     this, &MainWindow::onArmError);

    connect(ui->btnLeftArmEnable,  &QPushButton::toggled, this, &MainWindow::onLeftArmEnableClicked);
    connect(ui->btnRightArmEnable, &QPushButton::toggled, this, &MainWindow::onRightArmEnableClicked);
    connect(ui->sliderArmSpeed,    &QSlider::valueChanged,this, &MainWindow::onArmSpeedChanged);
    connect(ui->btnArmRefresh,     &QPushButton::clicked, this, &MainWindow::onArmRefreshClicked);
    connect(ui->btnArmView,        &QPushButton::clicked, this, &MainWindow::onArmViewClicked);
    connect(ui->btnArmProjectStart, &QPushButton::clicked, this, &MainWindow::onArmProjectStartClicked);
    connect(ui->btnArmProjectPause, &QPushButton::clicked, this, &MainWindow::onArmProjectPauseClicked);
    connect(ui->btnArmProjectResume,&QPushButton::clicked, this, &MainWindow::onArmProjectResumeClicked);
    connect(ui->btnArmProjectStop,  &QPushButton::clicked, this, &MainWindow::onArmProjectStopClicked);

    // ===== 夹爪 (占位: 通过 statusBar 显示提示) =====
    auto gripperOpen = [this]() {
        statusBar()->showMessage(QStringLiteral("夹爪: 打开"), 2000);
    };
    auto gripperClose = [this]() {
        const int f = ui->sliderGripperForce->value();
        statusBar()->showMessage(QStringLiteral("夹爪: 闭合 (%1%)").arg(f), 2000);
    };
    auto gripperStop = [this]() {
        statusBar()->showMessage(QStringLiteral("夹爪: 停止"), 2000);
        updateGripperStatus(QStringLiteral("■ 停止"));
    };
    connect(ui->btnGripperOpen,  &QPushButton::clicked, this, [gripperOpen, this]() {
        gripperOpen(); updateGripperStatus(QStringLiteral("→ 打开中…"));
    });
    connect(ui->btnGripperClose, &QPushButton::clicked, this, [gripperClose, this]() {
        gripperClose();
        updateGripperStatus(QStringLiteral("→ 闭合中 (%1%)").arg(ui->sliderGripperForce->value()));
    });
    connect(ui->btnGripperStop,  &QPushButton::clicked, this, gripperStop);
    connect(ui->sliderGripperForce, &QSlider::valueChanged, this, [this](int v) {
        ui->lblGripperForceVal->setText(QStringLiteral("%1%").arg(v));
    });

    // ===== 升降柱 =====
    connect(ui->btnLiftRefresh,     &QPushButton::clicked,  this, &MainWindow::onLiftRefreshClicked);
    connect(ui->btnLiftEnable,      &QPushButton::clicked,  this, &MainWindow::onLiftEnableClicked);
    connect(ui->btnLiftDisable,     &QPushButton::clicked,  this, &MainWindow::onLiftDisableClicked);
    connect(ui->btnLiftClearError,  &QPushButton::clicked,  this, &MainWindow::onLiftClearErrorClicked);
    connect(ui->btnLiftMove,        &QPushButton::clicked,  this, &MainWindow::onLiftMoveClicked);
    connect(ui->btnLiftStop,        &QPushButton::clicked,  this, &MainWindow::onLiftStopClicked);
    connect(ui->btnLiftJogUp,       &QPushButton::pressed,  this, &MainWindow::onLiftJogUpPressed);
    connect(ui->btnLiftJogUp,       &QPushButton::released, this, &MainWindow::onLiftJogUpReleased);
    connect(ui->btnLiftJogDown,     &QPushButton::pressed,  this, &MainWindow::onLiftJogDownPressed);
    connect(ui->btnLiftJogDown,     &QPushButton::released, this, &MainWindow::onLiftJogDownReleased);
    connect(ui->btnLiftSetZero,     &QPushButton::clicked,  this, &MainWindow::onLiftSetZeroClicked);
    connect(ui->cmbLiftDevices,     &QComboBox::currentTextChanged,
            this, &MainWindow::onLiftDeviceChanged);
}

// =====================================================================
// 顶部
// =====================================================================

void MainWindow::onConnectClicked()
{
    const bool anyConnected = m_armConnected || m_agvConnected;
    if (anyConnected) {
        if (m_agvClient) m_agvClient->disconnectFromHost();
        if (m_leftArm)   m_leftArm->disconnectFromArm();
        if (m_rightArm)  m_rightArm->disconnectFromArm();
        m_armConnected = m_agvConnected = false;
        updateConnectionStatusUi();
        ui->btnConnect->setText(QStringLiteral("连接"));
        statusBar()->showMessage(QStringLiteral("已断开连接"), 2000);
        return;
    }

    const QString armIp = ui->editArmIp->text().trimmed();
    const QString agvIp = ui->editAgvIp->text().trimmed();
    if (armIp.isEmpty() || agvIp.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("请输入机械臂和 AGV 的 IP 地址"), 3000);
        return;
    }

    auto *s = SettingsManager::instance();
    s->setArmIp(armIp);
    s->setAgvIp(agvIp);

    m_agvClient->connectToHost(agvIp, s->agvCtrlPort(), s->agvNavPort(), s->agvPushPort());

    // 单连接：内部按 left/right armName 区分左右臂
    m_leftArm->setArmNames(s->leftArmName(), s->rightArmName());
    m_leftArm->setPushInterval(s->pushInterval());
    if (m_leftArm->connectToArm(armIp, s->armWsPort())) {
        initLiftingColumn();
        // Revo2 灵巧手复用同一条 WS 通道
        if (!m_handManager) {
            m_handManager = new DualHandManager(m_leftArm,
                                               HAND_DEFAULT_EC2RS_LEFT,
                                               HAND_DEFAULT_EC2RS_RIGHT,
                                               HAND_DEFAULT_SLAVE_LEFT,
                                               HAND_DEFAULT_SLAVE_RIGHT,
                                               this);
            if (m_handPanel) m_handPanel->bindManager(m_handManager);
        }
    }

    statusBar()->showMessage(QStringLiteral("正在连接  机械臂:%1  AGV:%2").arg(armIp, agvIp), 3000);
}

void MainWindow::onSettingsClicked()
{
    if (!m_settingsDialog) m_settingsDialog = new SettingsDialog(this);
    m_settingsDialog->loadFromSettings();
    m_settingsDialog->exec();
}

void MainWindow::onClearErrorClicked()
{
    // 实际项目: 通过 m_agvClient 调用复位/清除异常接口
    statusBar()->showMessage(QStringLiteral("已清除异常"), 2000);
}

void MainWindow::onUpdateClock()
{
    statusBar()->showMessage(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss  系统就绪")), 1500);
}

// =====================================================================
// AGV
// =====================================================================

void MainWindow::onAgvState(const AgvState &state) {
    updateAgvStateUi(state);

    // 推送到地图弹窗
    if (m_mapDialog && m_mapDialog->isVisible()) {
        m_mapDialog->updateAgvPose(state.x, state.y, qRadiansToDegrees(state.theta));
    }
}

void MainWindow::updateAgvStateUi(const AgvState &state)
{
    if (state.x != 0.0 || state.y != 0.0) {
        ui->lblAgvX->setText(QStringLiteral("X: %1 m").arg(state.x, 0, 'f', 2));
        ui->lblAgvY->setText(QStringLiteral("Y: %1 m").arg(state.y, 0, 'f', 2));
    }
    if (state.theta != 0.0) {
        ui->lblAgvAngle->setText(QStringLiteral("θ: %1°").arg(qRadiansToDegrees(state.theta), 0, 'f', 1));
    }
    if (state.battery > 0) {
        ui->lblAgvBattery->setText(QStringLiteral("电池: %1%").arg(state.battery));
    }
    if (!state.currentStation.isEmpty()) {
        ui->lblAgvStation->setText(QStringLiteral("站点: %1").arg(state.currentStation));
    }
    if (!state.status.isEmpty()) {
        ui->lblAgvState->setText(QStringLiteral("● %1").arg(state.status));
    }
}

void MainWindow::onAgvCtrlConnected(bool)
{
    m_agvConnected = m_agvClient->isConnected();
    updateConnectionStatusUi();
    applyConnectionState();
}

void MainWindow::onAgvPushConnected(bool)
{
    m_agvConnected = m_agvClient->isConnected();
    updateConnectionStatusUi();
    applyConnectionState();
}

void MainWindow::onAgvError(const QString &err)
{
    statusBar()->showMessage(QStringLiteral("AGV: %1").arg(err), 5000);
}

void MainWindow::onAgvSpeedChanged(int value)
{
    const double speed = value / 100.0;
    ui->lblAgvSpeedVal->setText(QStringLiteral("速度: %1 m/s").arg(speed, 0, 'f', 2));
    SettingsManager::instance()->setDefaultSpeed(speed);
    // 重发当前方向
    if (!m_lastDirection.isEmpty() && m_agvClient && m_agvClient->isConnected()) {
        sendAgvDirectionCommand(m_lastDirection, true);
    }
}

QString MainWindow::currentDirection() const { return m_lastDirection; }

void MainWindow::sendAgvDirectionCommand(const QString &dir, bool pressed)
{
    if (!m_agvClient) return;
    if (!pressed) {
        m_agvClient->stopMotion();
        if (m_lastDirection == dir) m_lastDirection.clear();
        return;
    }
    m_lastDirection = dir;
    const double speed = ui->sliderAgvSpeed->value() / 100.0;
    double linear = 0, angular = 0;
    if      (dir == "forward")  linear =  speed;
    else if (dir == "backward") linear = -speed;
    else if (dir == "left")     angular =  speed;
    else if (dir == "right")    angular = -speed;
    m_agvClient->openLoopControl(linear, angular);
}

void MainWindow::onAgvForwardPressed()   { sendAgvDirectionCommand("forward",  true);  }
void MainWindow::onAgvForwardReleased()  { sendAgvDirectionCommand("forward",  false); }
void MainWindow::onAgvBackwardPressed()  { sendAgvDirectionCommand("backward", true);  }
void MainWindow::onAgvBackwardReleased() { sendAgvDirectionCommand("backward", false); }
void MainWindow::onAgvLeftPressed()      { sendAgvDirectionCommand("left",     true);  }
void MainWindow::onAgvLeftReleased()     { sendAgvDirectionCommand("left",     false); }
void MainWindow::onAgvRightPressed()     { sendAgvDirectionCommand("right",    true);  }
void MainWindow::onAgvRightReleased()    { sendAgvDirectionCommand("right",    false); }
void MainWindow::onAgvStopClicked()
{
    if (m_agvClient) m_agvClient->stopMotion();
    m_lastDirection.clear();
}
void MainWindow::onAgvEmergencyClicked()
{
    if (m_agvClient) m_agvClient->softEmergencyStop();
    statusBar()->showMessage(QStringLiteral("⛔ 急停已触发"), 3000);
    updateNavButtons(QStringLiteral("idle"));
}

void MainWindow::onAgvNavStartClicked()
{
    if (!m_agvClient) return;
    const QString station = ui->cmbAgvStations->currentData().toString();
    const QString id = station.isEmpty() ? ui->cmbAgvStations->currentText() : station;
    if (id.isEmpty()) return;
    m_agvClient->startNavigation(id);
    updateNavButtons(QStringLiteral("running"));
}

void MainWindow::onAgvNavPauseClicked()
{
    if (m_agvClient) m_agvClient->pauseNavigation();
    updateNavButtons(QStringLiteral("paused"));
}

void MainWindow::onAgvNavCancelClicked()
{
    if (m_agvClient) m_agvClient->cancelNavigation();
    updateNavButtons(QStringLiteral("idle"));
}

void MainWindow::onAgvViewMapClicked()
{
    // AGV 地图弹窗
    if (!m_mapDialog) {
        m_mapDialog = new MapInfoDialog(this);
        connect(m_mapDialog, &MapInfoDialog::refreshClicked, this, &MainWindow::onRefreshMap);

        // 如果已有地图数据，先设置
        if (!m_mapList.isEmpty() || !m_currentMapData.isEmpty()) {
            m_mapDialog->setData(m_mapList, m_currentMapName, m_currentMapData);
        }

        // 触发一次地图数据请求
        m_agvClient->requestMapList();
    }
    m_mapDialog->show();
    m_mapDialog->raise();
    m_mapDialog->activateWindow();
}

void MainWindow::onRefreshMap()
{
    m_agvClient->requestMapList();
}

void MainWindow::onAgvMapListReceived(const QVariantList &maps, const QString &currentMap)
{
    // 保存地图信息，请求当前地图数据
    m_currentMapName = currentMap;
    m_mapList = maps;
    
    // 更新 UI
    if (ui->cmbAgvStations) {
        ui->cmbAgvStations->clear();
        for (const QVariant &m : maps) {
            ui->cmbAgvStations->addItem(m.toString());
        }
    }
    
    // 请求地图数据
    if (!currentMap.isEmpty()) {
        m_agvClient->requestMapData(currentMap);
    }
    
    // 如果地图弹窗已打开，更新它
    if (m_mapDialog) {
        m_mapDialog->setData(maps, currentMap, m_currentMapData);
    }
}

void MainWindow::onAgvMapDataReceived(const QString &mapName, const QVariantMap &mapData)
{
    m_currentMapName = mapName;
    m_currentMapData = mapData;
    
    // 如果地图弹窗已打开，更新它
    if (m_mapDialog) {
        m_mapDialog->setData(m_mapList, mapName, mapData);
    }
}

void MainWindow::onAgvStationChanged(const QString &) {}

void MainWindow::updateNavButtons(const QString &status)
{
    m_navStatus = status;
    ui->btnAgvNavStart ->setEnabled(status == "idle");
    ui->btnAgvNavPause ->setEnabled(status == "running");
    ui->btnAgvNavCancel->setEnabled(status == "running" || status == "paused");
}

// =====================================================================
// 机械臂
// =====================================================================

void MainWindow::onArmConnected(bool connected)
{
    if (connected == m_armConnected) return;
    m_armConnected = connected;
    updateConnectionStatusUi();
    applyConnectionState();

    if (connected) {
        m_lastDirection.clear();
        m_leftArm->listProjects();
        m_leftArm->subscribeAll(SettingsManager::instance()->pushInterval());
        m_leftArm->fetchInitialStatus();
        m_leftArm->startPolling(1000);
        if (m_handPanel) m_handPanel->setArmConnected(true);
    } else {
        if (m_handPanel) m_handPanel->setArmConnected(false);
    }
}

void MainWindow::onArmRobotStatus(const QString &armName, const QJsonObject &db)
{
    auto *s = SettingsManager::instance();
    const bool isLeft = (armName == s->leftArmName());

    const bool enabled = db.value(QStringLiteral("enabled")).toBool();
    if (isLeft) {
        m_lastLeftArmState = db;
        updateArmEnableButton(true, enabled);
    } else {
        m_lastRightArmState = db;
        updateArmEnableButton(false, enabled);
    }
    updateArmStateUi(armName, db);
}

void MainWindow::updateArmStateUi(const QString &armName, const QJsonObject &db)
{
    const bool isLeft = (armName == SettingsManager::instance()->leftArmName());
    QLabel *jLabels[6] = {
        isLeft ? ui->lblLeftJ1 : ui->lblRightJ1,
        isLeft ? ui->lblLeftJ2 : ui->lblRightJ2,
        isLeft ? ui->lblLeftJ3 : ui->lblRightJ3,
        isLeft ? ui->lblLeftJ4 : ui->lblRightJ4,
        isLeft ? ui->lblLeftJ5 : ui->lblRightJ5,
        isLeft ? ui->lblLeftJ6 : ui->lblRightJ6,
    };
    QLabel *poseLabels[6] = {
        isLeft ? ui->lblLeftPoseX : ui->lblRightPoseX,
        isLeft ? ui->lblLeftPoseY : ui->lblRightPoseY,
        isLeft ? ui->lblLeftPoseZ : ui->lblRightPoseZ,
        isLeft ? ui->lblLeftPoseA : ui->lblRightPoseA,
        isLeft ? ui->lblLeftPoseB : ui->lblRightPoseB,
        isLeft ? ui->lblLeftPoseC : ui->lblRightPoseC,
    };

    // joints: 弧度数组
    const QJsonArray joints = db.value(QStringLiteral("joints")).toArray();
    for (int i = 0; i < 6 && i < joints.size(); ++i) {
        const double deg = qRadiansToDegrees(joints.at(i).toDouble());
        jLabels[i]->setText(QStringLiteral("J%1 %2°").arg(i + 1).arg(deg, 0, 'f', 1));
    }

    // cartesian: 位姿字典（x/y/z 单位 m，a/b/c 单位 rad）
    const QJsonObject car = db.value(QStringLiteral("cartesian")).toObject();
    if (!car.isEmpty()) {
        poseLabels[0]->setText(QStringLiteral("X %1mm").arg(car.value(QStringLiteral("x")).toDouble() * 1000, 0, 'f', 1));
        poseLabels[1]->setText(QStringLiteral("Y %1mm").arg(car.value(QStringLiteral("y")).toDouble() * 1000, 0, 'f', 1));
        poseLabels[2]->setText(QStringLiteral("Z %1mm").arg(car.value(QStringLiteral("z")).toDouble() * 1000, 0, 'f', 1));
        poseLabels[3]->setText(QStringLiteral("A %1°").arg(qRadiansToDegrees(car.value(QStringLiteral("a")).toDouble()), 0, 'f', 1));
        poseLabels[4]->setText(QStringLiteral("B %1°").arg(qRadiansToDegrees(car.value(QStringLiteral("b")).toDouble()), 0, 'f', 1));
        poseLabels[5]->setText(QStringLiteral("C %1°").arg(qRadiansToDegrees(car.value(QStringLiteral("c")).toDouble()), 0, 'f', 1));
    }
}

void MainWindow::onArmProjectList(const QVariantList &projects)
{
    m_projectList.clear();
    ui->cmbArmProjects->clear();
    for (const QVariant &pv : projects) {
        const QVariantMap pm = pv.toMap();
        // Python 版优先取 name，其次 note；无 name 时用 note 兜底
        QString name = pm.value(QStringLiteral("name")).toString();
        const QString note = pm.value(QStringLiteral("note")).toString();
        if (name.isEmpty()) name = note;
        if (name.isEmpty()) continue;
        m_projectList << name;
        ui->cmbArmProjects->addItem(name, name);
    }
}

void MainWindow::onArmProjectStatus(const QJsonObject &db)
{
    // project/status 推送字段（对齐 Python arm_control.update_project_status）
    const int state = db.value(QStringLiteral("state")).toInt(0);
    QString name = db.value(QStringLiteral("name")).toString();
    if (name.isEmpty()) name = QStringLiteral("无");
    const bool isStep = db.value(QStringLiteral("isStep")).toBool();
    const int errorLine = db.value(QStringLiteral("errorLine")).toInt(-1);
    const QString message = db.value(QStringLiteral("message")).toString().trimmed();

    static const QStringList kStateNames = {
        QStringLiteral("待机"), QStringLiteral("运行中"), QStringLiteral("已暂停")
    };
    QString stateText = (state >= 0 && state < kStateNames.size())
                            ? kStateNames.at(state) : QStringLiteral("未知");
    if (state == 1 && isStep) stateText += QStringLiteral("(单步)");

    QString display;
    if (errorLine >= 0 && !message.isEmpty()) {
        display = QStringLiteral("当前工程: %1 — %2 · L%3: %4")
                      .arg(name, stateText).arg(errorLine).arg(message);
    } else if (state == 2 && !message.isEmpty()) {
        display = QStringLiteral("当前工程: %1 — %2 · %3").arg(name, stateText, message);
    } else {
        display = QStringLiteral("当前工程: %1 (%2)").arg(name, stateText);
    }
    updateProjectStatusUi(stateText, name);
    ui->lblArmProjectStatus->setText(display);
    ui->lblArmProjectStatus->setToolTip(
        QString::fromUtf8(QJsonDocument(db).toJson(QJsonDocument::Indented)));
}

void MainWindow::onArmError(const QString &err)
{
    statusBar()->showMessage(QStringLiteral("机械臂: %1").arg(err), 5000);
}

void MainWindow::onLeftArmEnableClicked(bool checked)
{
    if (!m_leftArm) return;
    auto *s = SettingsManager::instance();
    auto [ok, err] = m_leftArm->setArmEnabled(s->leftArmName(), checked);
    if (!ok) {
        updateArmEnableButton(true, m_leftArmEnabled);   // 回滚 UI
        if (m_messageManager) m_messageManager->error(
            QStringLiteral("左臂使能失败: %1").arg(err));
        return;
    }
    updateArmEnableButton(true, checked);
    if (m_messageManager) {
        m_messageManager->success(checked ? QStringLiteral("左臂已使能")
                                          : QStringLiteral("左臂已下使能"));
    }
}

void MainWindow::onRightArmEnableClicked(bool checked)
{
    if (!m_leftArm) return;
    auto *s = SettingsManager::instance();
    auto [ok, err] = m_leftArm->setArmEnabled(s->rightArmName(), checked);
    if (!ok) {
        updateArmEnableButton(false, m_rightArmEnabled);   // 回滚 UI
        if (m_messageManager) m_messageManager->error(
            QStringLiteral("右臂使能失败: %1").arg(err));
        return;
    }
    updateArmEnableButton(false, checked);
    if (m_messageManager) {
        m_messageManager->success(checked ? QStringLiteral("右臂已使能")
                                          : QStringLiteral("右臂已下使能"));
    }
}

void MainWindow::updateArmEnableButton(bool left, bool enabled)
{
    if (left)  m_leftArmEnabled  = enabled; else m_rightArmEnabled = enabled;
    QPushButton *btn = left ? ui->btnLeftArmEnable : ui->btnRightArmEnable;
    QLabel      *lbl = left ? ui->lblLeftArmEnabled : ui->lblRightArmEnabled;
    btn->blockSignals(true);
    btn->setChecked(enabled);
    btn->setText(enabled ? QStringLiteral("已使能") : QStringLiteral("启用"));
    btn->blockSignals(false);
    if (enabled) {
        lbl->setText(QStringLiteral("✓ 已使能"));
        lbl->setStyleSheet(QStringLiteral("color: #2E7D32; font-weight: bold;"));
    } else {
        lbl->setText(QStringLiteral("未使能"));
        lbl->setStyleSheet(QStringLiteral("color: #777; font-size: 11px;"));
    }
}

void MainWindow::onArmSpeedChanged(int v)
{
    ui->lblArmSpeedVal->setText(QStringLiteral("%1%").arg(v));
    updateSpeedHint(v);
    const double ratio = v / 100.0;
    SettingsManager::instance()->setArmSpeed(ratio);
    // 单连接：速度覆盖对双臂同时生效
    if (m_leftArm) m_leftArm->setSpeedOverride(ratio);
}

void MainWindow::updateSpeedHint(int value)
{
    QString hint;
    if      (value <= 25) hint = QStringLiteral("慢速 - 精细操作");
    else if (value <= 50) hint = QStringLiteral("标准 - 一般作业");
    else if (value <= 75) hint = QStringLiteral("快速 - 提高效率");
    else                  hint = QStringLiteral("极速 - 测试用");
    ui->lblArmSpeedHint->setText(hint);
}

void MainWindow::onArmRefreshClicked()
{
    if (m_leftArm) m_leftArm->listProjects();
    statusBar()->showMessage(QStringLiteral("正在刷新工程列表..."), 2000);
}

void MainWindow::onArmViewClicked()
{
    // 机械臂 Web 界面 URL
    // 桌面用 QtWebEngine，Android 用 Qt WebView；iOS / WASM 未适配。
#if !defined(Q_OS_IOS) && !defined(Q_OS_WASM)
    static WebViewDialog *dlg = nullptr;
    if (!dlg) {
        dlg = new WebViewDialog(url, QStringLiteral("机械臂 Web"), this);
        loadedUrl = url;
    } else if (url != loadedUrl) {
        dlg->loadUrl(url);
        loadedUrl = url;
    }
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
#else
    Q_UNUSED(this);
#endif
}

void MainWindow::onArmProjectStartClicked()
{
    const QString name = ui->cmbArmProjects->currentData().toString();
    if (name.isEmpty() || !m_leftArm) return;
    m_leftArm->runProject(name);
    updateProjectButtons(QStringLiteral("running"));
}
void MainWindow::onArmProjectPauseClicked()
{
    if (m_leftArm) m_leftArm->pauseProject();
    updateProjectButtons(QStringLiteral("paused"));
}
void MainWindow::onArmProjectResumeClicked()
{
    if (m_leftArm) m_leftArm->resumeProject();
    updateProjectButtons(QStringLiteral("running"));
}
void MainWindow::onArmProjectStopClicked()
{
    if (m_leftArm) m_leftArm->stopProject();
    updateProjectButtons(QStringLiteral("idle"));
}

void MainWindow::updateProjectStatusUi(const QString &state, const QString &project)
{
    m_projectState = state;
    if (!project.isEmpty()) m_currentProject = project;
    ui->lblArmProjectStatus->setText(
        QStringLiteral("当前工程: %1 (%2)").arg(m_currentProject, m_projectState));
    if      (state == "运行中") updateProjectButtons(QStringLiteral("running"));
    else if (state == "已暂停") updateProjectButtons(QStringLiteral("paused"));
    else                        updateProjectButtons(QStringLiteral("idle"));
}

void MainWindow::updateProjectButtons(const QString &status)
{
    ui->btnArmProjectStart ->setEnabled(status == "idle");
    ui->btnArmProjectPause ->setEnabled(status == "running");
    ui->btnArmProjectResume->setEnabled(status == "paused");
    ui->btnArmProjectStop  ->setEnabled(status == "running" || status == "paused");
}

// =====================================================================
// Revo2 灵巧手
// =====================================================================

void MainWindow::onHandInit()
{
    if (!m_handManager) {
        if (m_messageManager)
            m_messageManager->warning(QStringLiteral("请先连接机械臂"));
        return;
    }

    QPushButton *btn = m_handPanel ? m_handPanel->initButton() : nullptr;
    if (btn) {
        btn->setEnabled(false);
        btn->setText(QStringLiteral("初始化中…"));
    }

    // init() 内部走 sendAndWait（阻塞式），放后台线程避免卡 UI
    auto *mgr  = m_handManager;
    auto *self = this;
    QThread::create([mgr, self, btn]() {
        const auto r      = mgr->init();
        const bool leftOk = r.first.first;
        const QString leftErr  = r.first.second;
        const bool rightOk = r.second.first;
        const QString rightErr = r.second.second;

        QMetaObject::invokeMethod(self, [self, leftOk, rightOk, leftErr, rightErr, btn]() {
            if (btn) {
                btn->setEnabled(true);
                btn->setText(QStringLiteral("重新连接"));
            }
            if (self->m_handPanel)
                self->m_handPanel->onInitDone(leftOk, rightOk);

            QStringList errs;
            if (!leftOk)  errs << (QStringLiteral("左手: ") + leftErr);
            if (!rightOk) errs << (QStringLiteral("右手: ") + rightErr);

            const QString detail = errs.join(QStringLiteral("; "));
            if (!self->m_messageManager) return;
            if (errs.isEmpty())
                self->m_messageManager->success(QStringLiteral("灵巧手初始化成功"));
            else
                self->m_messageManager->error(
                    QStringLiteral("灵巧手初始化失败: ") + detail);
        }, Qt::QueuedConnection);
    })->start();
}

void MainWindow::onHandViewRequested(const QString &side)
{
    if (!m_handManager) {
        if (m_messageManager)
            m_messageManager->warning(QStringLiteral("请先连接机械臂并初始化灵巧手"));
        return;
    }
    Revo2HandClient *client = (side == QStringLiteral("left"))
                                  ? m_handManager->left() : m_handManager->right();
    auto *dlg = new HandViewDialog(side, client, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MainWindow::onHandGrabRequested(const QString &side)
{
    if (!m_handManager) {
        if (m_messageManager) m_messageManager->warning(QStringLiteral("请先连接机械臂"));
        return;
    }
    Revo2HandClient *client = (side == QStringLiteral("left"))
                                  ? m_handManager->left() : m_handManager->right();
    if (!client->state().inited) {
        if (m_messageManager)
            m_messageManager->warning(
                QStringLiteral("%1手未初始化，请先点击「连接双手」")
                    .arg(side == QStringLiteral("left") ? QStringLiteral("左") : QStringLiteral("右")));
        return;
    }
    QString err = client->executePreset(QStringLiteral("grab"));
    if (err.isEmpty()) {
        if (m_messageManager) m_messageManager->success(QStringLiteral("抓取手势已下发"));
    } else {
        if (m_messageManager) m_messageManager->error(err);
    }
}

void MainWindow::onHandReleaseRequested(const QString &side)
{
    if (!m_handManager) {
        if (m_messageManager) m_messageManager->warning(QStringLiteral("请先连接机械臂"));
        return;
    }
    Revo2HandClient *client = (side == QStringLiteral("left"))
                                  ? m_handManager->left() : m_handManager->right();
    if (!client->state().inited) {
        if (m_messageManager)
            m_messageManager->warning(
                QStringLiteral("%1手未初始化，请先点击「连接双手」")
                    .arg(side == QStringLiteral("left") ? QStringLiteral("左") : QStringLiteral("右")));
        return;
    }
    QString err = client->executePreset(QStringLiteral("release"));
    if (err.isEmpty()) {
        if (m_messageManager) m_messageManager->success(QStringLiteral("释放手势已下发"));
    } else {
        if (m_messageManager) m_messageManager->error(err);
    }
}

void MainWindow::onHandStopAllRequested()
{
    if (!m_handManager) return;
    // 急停：双手写全 0
    const QVector<int> zeros(HAND_DOF_COUNT, 0);
    QString e1 = m_handManager->left()->writeAll(zeros);
    QString e2 = m_handManager->right()->writeAll(zeros);
    // 地址来自设置页（默认 DEFAULT_ARM_WEB_URL）；设置被修改后再次打开时重新加载
    if (m_messageManager) {
    static QString loadedUrl;
    const QString url = SettingsManager::instance()->armWebUrl();
        if (e1.isEmpty() && e2.isEmpty())
            m_messageManager->success(QStringLiteral("灵巧手已急停"));
        else
            m_messageManager->error(QStringLiteral("急停失败: %1%2").arg(e1, e2));
    }
}

// =====================================================================
// 夹爪
// =====================================================================
void MainWindow::updateGripperStatus(const QString &text)
{
    ui->lblGripperStatus->setText(text);
}

// =====================================================================
// 升降柱
// =====================================================================

void MainWindow::onLiftRefreshClicked()
{
    // 真实项目: 从 arm_manager 拉取设备列表
    // 此处使用占位数据 (与 Python _refresh_lifting_devices 流程一致)
    statusBar()->showMessage(QStringLiteral("正在刷新升降柱设备列表..."), 2000);
    // 占位: 添加一个示例设备
    if (ui->cmbLiftDevices->count() == 0) {
        ui->cmbLiftDevices->addItem(QStringLiteral("liftingColumn-1"));
    }
}

void MainWindow::onLiftEnableClicked()
{
    const QString dev = ui->cmbLiftDevices->currentText();
    statusBar()->showMessage(QStringLiteral("升降柱: 上电 %1 (待实现)").arg(dev), 2000);
    refreshLiftLabels();
}

void MainWindow::onLiftDisableClicked()
{
    const QString dev = ui->cmbLiftDevices->currentText();
    statusBar()->showMessage(QStringLiteral("升降柱: 下电 %1 (待实现)").arg(dev), 2000);
    refreshLiftLabels();
}

void MainWindow::onLiftClearErrorClicked()
{
    const QString dev = ui->cmbLiftDevices->currentText();
    statusBar()->showMessage(QStringLiteral("升降柱: 清错 %1 (待实现)").arg(dev), 2000);
    refreshLiftLabels();
}

void MainWindow::onLiftMoveClicked()
{
    const QString dev = ui->cmbLiftDevices->currentText();
    const double h_mm = ui->spinLiftTarget->value();
    const double v    = ui->spinLiftSpeed->value();
    Q_UNUSED(v);
    statusBar()->showMessage(QStringLiteral("升降柱: 移动 %1 -> %2 mm (待实现)").arg(dev).arg(h_mm, 0, 'f', 0), 3000);
}

void MainWindow::onLiftStopClicked()
{
    const QString dev = ui->cmbLiftDevices->currentText();
    statusBar()->showMessage(QStringLiteral("升降柱: 停止 %1 (待实现)").arg(dev), 2000);
}

void MainWindow::onLiftJogUpPressed()
{
    const QString dev = ui->cmbLiftDevices->currentText();
    statusBar()->showMessage(QStringLiteral("升降柱: 上升 %1 (待实现)").arg(dev), 1500);
}
void MainWindow::onLiftJogUpReleased()
{
    const QString dev = ui->cmbLiftDevices->currentText();
    statusBar()->showMessage(QStringLiteral("升降柱: 停止 %1 (待实现)").arg(dev), 1500);
}
void MainWindow::onLiftJogDownPressed()
{
    const QString dev = ui->cmbLiftDevices->currentText();
    statusBar()->showMessage(QStringLiteral("升降柱: 下降 %1 (待实现)").arg(dev), 1500);
}
void MainWindow::onLiftJogDownReleased()
{
    const QString dev = ui->cmbLiftDevices->currentText();
    statusBar()->showMessage(QStringLiteral("升降柱: 停止 %1 (待实现)").arg(dev), 1500);
}

void MainWindow::onLiftSetZeroClicked()
{
    const QString dev = ui->cmbLiftDevices->currentText();
    statusBar()->showMessage(QStringLiteral("升降柱: 置零 %1 (待实现)").arg(dev), 2000);
}

void MainWindow::onLiftDeviceChanged(const QString &)
{
    refreshLiftLabels();
}

void MainWindow::refreshLiftLabels()
{
    // 占位: 全部 reset
    ui->lblLiftHeight->setText(QStringLiteral("—"));
    ui->lblLiftLimits->setText(QStringLiteral("—"));
    ui->lblLiftOnline ->setText(QStringLiteral("○—"));
    ui->lblLiftEnabled->setText(QStringLiteral("○—"));
    ui->lblLiftMoving ->setText(QStringLiteral("○—"));
    ui->lblLiftFault  ->setText(QStringLiteral("无"));
    setLiftAlert({}, {});
}

void MainWindow::setLiftAlert(const QString &text, const QString &bgColor)
{
    if (text.isEmpty()) {
        ui->lblLiftAlert->setText({});
        ui->lblLiftAlert->setStyleSheet({});
    } else {
        ui->lblLiftAlert->setText(text);
        ui->lblLiftAlert->setStyleSheet(
            QStringLiteral("color: white; background: %1; border-radius: 4px; padding: 2px 8px; font-weight: bold;").arg(bgColor));
    }
}

// =====================================================================
// 状态指示
// =====================================================================

void MainWindow::updateConnectionStatusUi()
{
    // 机械臂
    ui->armStatusLabel->setText(m_armConnected ? QStringLiteral("机械臂: 已连接") : QStringLiteral("机械臂: 未连接"));
    ui->armStatusDot->setStyleSheet(m_armConnected
        ? QStringLiteral("color: #2E7D32; font-size: 14px;")
        : QStringLiteral("color: #999999; font-size: 14px;"));

    // AGV
    ui->agvStatusLabel->setText(m_agvConnected ? QStringLiteral("AGV: 已连接") : QStringLiteral("AGV: 未连接"));
    ui->agvStatusDot->setStyleSheet(m_agvConnected
        ? QStringLiteral("color: #2E7D32; font-size: 14px;")
        : QStringLiteral("color: #999999; font-size: 14px;"));

    ui->btnConnect->setText(m_armConnected || m_agvConnected ? QStringLiteral("断开") : QStringLiteral("连接"));
}

// =====================================================================
// 按钮使能：根据 AGV/机械臂连接状态批量启用/禁用控件
// =====================================================================
// 规则：
//   - AGV 控件（速度、方向、停止、站点、View、开始/暂停/取消/急停）：
//       仅当 m_agvConnected=true 时启用
//   - 机械臂控件（左/右臂使能、速度、工程选择/刷新/View、开始/暂停/继续/停止工程）：
//       仅当 m_armConnected=true 时启用（即左+右双臂都已连接）
//   - 导航按钮的 4 态锁定（idle/running/paused）和工程按钮的 3 态锁定，
//     通过 updateNavButtons / updateProjectButtons 在各自状态回调里再叠加。
void MainWindow::applyConnectionState()
{
    // ===== AGV 控件 =====
    const bool agvOn = m_agvConnected;
    ui->sliderAgvSpeed  ->setEnabled(agvOn);
    ui->btnAgvForward   ->setEnabled(agvOn);
    ui->btnAgvBackward  ->setEnabled(agvOn);
    ui->btnAgvLeft      ->setEnabled(agvOn);
    ui->btnAgvRight     ->setEnabled(agvOn);
    ui->btnAgvStop      ->setEnabled(agvOn);
    ui->btnAgvEmergency ->setEnabled(agvOn);
    ui->cmbAgvStations  ->setEnabled(agvOn);
    ui->btnViewMap      ->setEnabled(agvOn);
    // 导航控件依赖导航状态再叠加（运行中不能再次开始等）
    updateNavButtons(m_navStatus);

    // ===== 机械臂控件 =====
    const bool armOn = m_armConnected;
    ui->btnLeftArmEnable   ->setEnabled(armOn);
    ui->btnRightArmEnable  ->setEnabled(armOn);
    ui->sliderArmSpeed     ->setEnabled(armOn);
    ui->cmbArmProjects     ->setEnabled(armOn);
    ui->btnArmRefresh      ->setEnabled(armOn);
    ui->btnArmView         ->setEnabled(armOn);
    // 工程控件依赖工程状态再叠加（运行中不能再次开始等）
    updateProjectButtons(m_projectState == QStringLiteral("运行中") ? QStringLiteral("running")
                        : m_projectState == QStringLiteral("已暂停") ? QStringLiteral("paused")
                                                                    : QStringLiteral("idle"));

    // ===== 升降柱控件（走机械臂 ws 通道，未连接时禁用动作按钮）=====
    ui->btnLiftEnable      ->setEnabled(armOn);
    ui->btnLiftDisable     ->setEnabled(armOn);
    ui->btnLiftClearError  ->setEnabled(armOn);
    ui->btnLiftMove        ->setEnabled(armOn);
    ui->btnLiftStop        ->setEnabled(armOn);
    ui->btnLiftJogUp       ->setEnabled(armOn);
    ui->btnLiftJogDown     ->setEnabled(armOn);
    ui->btnLiftSetZero     ->setEnabled(armOn);
    ui->btnLiftRefresh     ->setEnabled(armOn);
    ui->cmbLiftDevices     ->setEnabled(armOn);
    ui->spinLiftTarget     ->setEnabled(armOn);
    ui->spinLiftSpeed      ->setEnabled(armOn);
}
