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
#include <QIcon>
#include <QStatusBar>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QScrollArea>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QStyle>
#include <QDebug>
#include <QPointer>
#include <QTimer>
#include <QDateTime>
#include <QVariantList>
#include <QVariantMap>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QThreadPool>
#include <QVector>
#include <cmath>
#include <memory>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    QssManager::instance()->loadStyleForWidget(this, QStringLiteral("mainwindow"));

    // 左上角 logo：用图标替换原来的 emoji 文字
    // 按设备像素比缩放源图（96px），高分屏 / 手机屏不发虚
    {
        const QPixmap src(QStringLiteral(":/resources/icons/robot.png"));
        const qreal dpr = devicePixelRatioF();
        const int logical = 28;
        QPixmap logo = src.scaled(QSize(qRound(logical * dpr), qRound(logical * dpr)),
                                  Qt::KeepAspectRatio, Qt::SmoothTransformation);
        logo.setDevicePixelRatio(dpr);
        ui->lblLogo->setPixmap(logo);
        setWindowIcon(QIcon(src));
    }

    // 左(AGV) / 中(机械臂) / 右(灵巧手+升降柱) 三列等分：
    // 外层分割器 1:2，右侧面板内部两列再 1:1
    ui->mainSplitter->setStretchFactor(0, 1);
    ui->mainSplitter->setStretchFactor(1, 2);
    ui->mainSplitter->setSizes({1000, 2000});

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
        // 灵巧手模块与升降柱上下 1:1 等分，内容超出时在模块内部滚动
        auto *handScroll = new QScrollArea(handBox);
        handScroll->setObjectName(QStringLiteral("handScroll"));
        handScroll->setFrameShape(QFrame::NoFrame);
        handScroll->setWidgetResizable(true);
        handScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        handScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

        m_handPanel = new DualHandPanel(handScroll);
        handScroll->setWidget(m_handPanel);

        if (auto *lay = qobject_cast<QVBoxLayout*>(handBox->layout())) {
            lay->addWidget(handScroll);
        }
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
    if (!m_leftArm) return;
    // 客户端只创建一次；断开重连后仍要重新订阅并重新拉列表
    if (!m_lifting)
        m_lifting = new LiftingColumnClient(m_leftArm, this);

    auto cb = [this](const QVector<QJsonObject> &states) {
        for (const QJsonObject &st : states) {
            // 字段名以 API 文档 34.5 为准：设备名 name，当前高度 height（m）
            const QString dev = st.value(QStringLiteral("name")).toString();
            if (dev.isEmpty()) continue;

            // 与 Python update_state 一致：list 没拿到时，推送里出现的设备也补进下拉框
            if (ui->cmbLiftDevices->findText(dev) < 0) {
                const bool wasEmpty = ui->cmbLiftDevices->count() == 0;
                ui->cmbLiftDevices->addItem(dev);
                if (wasEmpty) ui->cmbLiftDevices->setCurrentIndex(0);
            }
            m_liftStates.insert(dev, st);
        }
        // 在线 / 使能 / 运动 / 故障 / 急停 一并刷新，不只是高度
        refreshLiftLabels();
    };

    // 与 Python _init_lifting_column 一致：先拉 list，再订阅
    refreshLiftDevices();
    m_lifting->subscribeState(cb, SettingsManager::instance()->pushInterval());

    // 控制器刚握手时，升降柱所属主站可能还没起来（文档 34.1 最长等 6 s），
    // list 会返回空数组。Python 版在 3 s / 8 s 各补拉一次，这里保持一致。
    for (int delayMs : {3000, 8000}) {
        QTimer::singleShot(delayMs, this, [this]() {
            if (m_leftArm && m_leftArm->isConnected())
                refreshLiftDevices();
        });
    }
}

void MainWindow::refreshLiftDevices()
{
    if (!m_lifting) return;

    // liftingColumn/list 返回的设备名字段是 name（API 文档 34.4），原样显示，不做改写
    const QVector<QJsonObject> devs = m_lifting->listDevices();
    QStringList names;
    for (const QJsonObject &d : devs) {
        const QString n = d.value(QStringLiteral("name")).toString();
        if (!n.isEmpty()) names << n;
    }
    if (names.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[Main] liftingColumn/list 返回 0 台，等待重试或推送补全");
        statusBar()->showMessage(QStringLiteral("未获取到升降柱设备（详见日志 [liftingColumn]）"), 3000);
        return;
    }

    // 记下每台的软限位 / 速度上限，供目标高度范围和限位显示使用
    m_liftDevInfo.clear();
    for (const QJsonObject &d : devs) {
        const QString n = d.value(QStringLiteral("name")).toString();
        if (!n.isEmpty()) m_liftDevInfo.insert(n, d);
    }

    m_lifting->setKnownDevices(names);

    const QString prev = ui->cmbLiftDevices->currentText();
    ui->cmbLiftDevices->blockSignals(true);
    ui->cmbLiftDevices->clear();
    ui->cmbLiftDevices->addItems(names);
    const int idx = ui->cmbLiftDevices->findText(prev);
    ui->cmbLiftDevices->setCurrentIndex(idx >= 0 ? idx : 0);
    ui->cmbLiftDevices->blockSignals(false);
    // 限位信息每次刷新都要应用（同一台设备的限位也可能变了，如 setZero 之后）
    onLiftDeviceChanged(ui->cmbLiftDevices->currentText());
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
    connect(m_agvClient, &AgvTcpClient::navStatusReceived,   this, &MainWindow::onAgvNavStatus);
    connect(m_agvClient, &AgvTcpClient::lasersReceived,      this, &MainWindow::onAgvLasers);

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
    // 控制权（抢占 / 释放），结果由 AgvTcpClient 信号回传
    connect(ui->btnAgvLock,     &QPushButton::clicked,  this, &MainWindow::onAgvLockClicked);
    connect(ui->btnAgvUnlock,   &QPushButton::clicked,  this, &MainWindow::onAgvUnlockClicked);
    connect(m_agvClient, &AgvTcpClient::controlLockResult,   this, &MainWindow::onAgvLockResult);
    connect(m_agvClient, &AgvTcpClient::controlUnlockResult, this, &MainWindow::onAgvUnlockResult);
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
    connect(ui->btnLeftArmDrag,    &QPushButton::clicked, this, &MainWindow::onLeftArmDragClicked);
    connect(ui->btnRightArmDrag,   &QPushButton::clicked, this, &MainWindow::onRightArmDragClicked);
    connect(ui->sliderArmSpeed,    &QSlider::valueChanged,this, &MainWindow::onArmSpeedChanged);
    connect(ui->btnArmRefresh,     &QPushButton::clicked, this, &MainWindow::onArmRefreshClicked);
    connect(ui->btnArmView,        &QPushButton::clicked, this, &MainWindow::onArmViewClicked);
    connect(ui->btnArmProjectStart, &QPushButton::clicked, this, &MainWindow::onArmProjectStartClicked);
    connect(ui->btnArmProjectPause, &QPushButton::clicked, this, &MainWindow::onArmProjectPauseClicked);
    connect(ui->btnArmProjectResume,&QPushButton::clicked, this, &MainWindow::onArmProjectResumeClicked);
    connect(ui->btnArmProjectStop,  &QPushButton::clicked, this, &MainWindow::onArmProjectStopClicked);

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
    if (!m_leftArm) {
        if (m_messageManager)
            m_messageManager->warning(QStringLiteral("请先连接机械臂"));
        return;
    }
    if (!m_leftArm->isConnected()) {
        if (m_messageManager)
            m_messageManager->warning(QStringLiteral("机械臂未连接"));
        return;
    }

    // 清错要等驱动器反馈确认故障位清除，最长 5 s。底层走 sendAndWait
    // （嵌套事件循环），在 UI 线程调用会冻住界面，因此用异步接口。
    //
    // 臂名必须用控制器 robot/list 解析出的真实名（leftArmName()/
    // rightArmName()），写配置里的名字会被判成未知机器人。
    const QString left  = m_leftArm->leftArmName();
    const QString right = m_leftArm->rightArmName();

    QStringList targets;
    if (!left.isEmpty())  targets << left;
    if (!right.isEmpty()) targets << right;
    if (targets.isEmpty()) {
        if (m_messageManager)
            m_messageManager->warning(QStringLiteral("未获取到机械臂名称，无法清错"));
        return;
    }

    if (ui->btnClearError) ui->btnClearError->setEnabled(false);

    auto *mm = m_messageManager;
    QPointer<MainWindow> self(this);

    // 计数器与失败列表要活到所有回调都回来为止，因此放堆上（shared_ptr）。
    // 用栈变量被异步回调捕获会在函数返回后成为悬垂引用。
    struct ClearState {
        int pending = 0;
        QStringList failures;
    };
    auto st = std::make_shared<ClearState>();
    st->pending = targets.size();

    for (const QString &armName : targets) {
        m_leftArm->clearServoErrorAsync(armName, [self, mm, armName, st](bool ok,
                                                                        const QString &err) {
            if (!ok)
                st->failures << (err.isEmpty()
                                     ? QStringLiteral("%1 清错失败").arg(armName)
                                     : err);
            if (--st->pending > 0)
                return;   // 还没全部回来
            if (!self) return;
            if (self->ui->btnClearError) self->ui->btnClearError->setEnabled(true);
            if (!mm) return;
            if (st->failures.isEmpty()) {
                mm->success(QStringLiteral("清除异常指令已下发"));
            } else {
                mm->error(st->failures.join(QStringLiteral("；")));
            }
        });
    }
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

    // 软急停按钮以 AGV 推送的 soft_emc 为准（物理急停按钮 emergency 不在此处理），
    // 仅在上报值变化时同步：其他控制端触发/解除软急停时按钮跟着变；
    // 上报值不变时不覆盖本地刚做的操作结果
    if (state.softEmergency != m_agvReportedSoftEmc) {
        m_agvReportedSoftEmc = state.softEmergency;
        setAgvEmergencyUi(state.softEmergency);
    }

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

void MainWindow::onAgvPushConnected(bool connected)
{
    m_agvConnected = connected && m_agvClient->isConnected();
    updateConnectionStatusUi();
    applyConnectionState();
    if (!connected) {
        ui->lblAgvState->setText(QStringLiteral("● 未连接"));
        m_navStatus = QStringLiteral("idle");
    }
}

void MainWindow::onAgvNavStatus(int taskStatus)
{
    static const QHash<int, QString> names = {
        {0, QStringLiteral("无任务")}, {1, QStringLiteral("等待中")},
        {2, QStringLiteral("运行中")}, {3, QStringLiteral("已暂停")},
        {4, QStringLiteral("已完成")}, {5, QStringLiteral("失败")},
        {6, QStringLiteral("已取消")},
    };
    ui->lblAgvState->setText(QStringLiteral("● %1")
                                 .arg(names.value(taskStatus, QStringLiteral("未知"))));
    if (taskStatus == 2)      updateNavButtons(QStringLiteral("running"));
    else if (taskStatus == 3) updateNavButtons(QStringLiteral("paused"));
    else                      updateNavButtons(QStringLiteral("idle"));
}

void MainWindow::onAgvLasers(const QVariantList &lasers)
{
    if (m_mapDialog && m_mapDialog->isVisible())
        m_mapDialog->updateLasers(lasers);
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
    if (!m_agvClient) return;
    if (m_agvEmergency) {
        // 当前处于急停 -> 解除
        m_agvClient->releaseSoftEmergency();
        setAgvEmergencyUi(false);
        statusBar()->showMessage(QStringLiteral("已解除急停"), 3000);
    } else {
        m_agvClient->softEmergencyStop();
        setAgvEmergencyUi(true);
        statusBar()->showMessage(QStringLiteral("⛔ 急停已触发"), 3000);
        updateNavButtons(QStringLiteral("idle"));
    }
}

// 急停按钮两种状态：未急停 -> “⛔ 急停”（红）；急停中 -> “解除急停”（绿）
void MainWindow::setAgvEmergencyUi(bool active)
{
    m_agvEmergency = active;
    QPushButton *b = ui->btnAgvEmergency;
    b->setText(active ? QStringLiteral("解除急停") : QStringLiteral("⛔ 急停"));
    b->setToolTip(active ? QStringLiteral("解除 AGV 软急停") : QStringLiteral("触发 AGV 软急停"));
    b->setProperty("estopActive", active);
    // 动态属性变化后需要重新应用样式
    b->style()->unpolish(b);
    b->style()->polish(b);
}

void MainWindow::onAgvLockClicked()
{
    if (!m_agvClient) return;
    const QString nick = ui->editAgvLockNick->text().trimmed();
    if (nick.isEmpty()) {
        if (m_messageManager) m_messageManager->warning(QStringLiteral("请先填写抢占者名称"));
        ui->editAgvLockNick->setFocus();
        return;
    }
    // 不再二次确认：Android 上模态 QMessageBox 无法点击确认/关闭
    ui->btnAgvLock->setEnabled(false);
    statusBar()->showMessage(QStringLiteral("正在抢占控制权..."));
    m_agvClient->lockControl(nick);
}

void MainWindow::onAgvLockResult(bool ok, const QString &msg)
{
    // 按钮使能统一由 applyConnectionState 维护（断线时不应被重新启用）
    ui->btnAgvLock->setEnabled(m_agvConnected);
    if (ok) {
        ui->lblAgvLockState->setText(
            QStringLiteral("控制权: 已抢占（%1）").arg(ui->editAgvLockNick->text().trimmed()));
        statusBar()->showMessage(QStringLiteral("已抢占控制权"), 3000);
    } else {
        ui->lblAgvLockState->setText(QStringLiteral("控制权: 抢占失败"));
        const QString text = QStringLiteral("抢占控制权失败：%1").arg(msg);
        statusBar()->showMessage(text, 5000);
        if (m_messageManager) m_messageManager->error(text);
    }
}

void MainWindow::onAgvUnlockClicked()
{
    if (!m_agvClient) return;
    ui->btnAgvUnlock->setEnabled(false);
    statusBar()->showMessage(QStringLiteral("正在释放控制权..."));
    m_agvClient->unlockControl();
}

void MainWindow::onAgvUnlockResult(bool ok, const QString &msg)
{
    ui->btnAgvUnlock->setEnabled(m_agvConnected);
    if (ok) {
        ui->lblAgvLockState->setText(QStringLiteral("控制权: 已释放"));
        statusBar()->showMessage(QStringLiteral("已释放控制权"), 3000);
    } else {
        // 失败不改标签：最常见原因是控制权不在自己手里（只能释放自己抢占的）
        const QString text = QStringLiteral("释放控制权失败：%1（只能释放自己抢占的控制权）").arg(msg);
        statusBar()->showMessage(text, 5000);
        if (m_messageManager) m_messageManager->error(text);
    }
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
        connect(m_mapDialog, &MapInfoDialog::switchMapRequested,
                this, &MainWindow::onMapSwitchRequested);
        connect(m_mapDialog, &MapInfoDialog::relocateRequested,
                this, &MainWindow::onRelocateRequested);
        connect(m_mapDialog, &MapInfoDialog::confirmLocationRequested,
                m_agvClient, &AgvTcpClient::confirmLocation);
        // AGV 侧结果回填到弹窗（弹窗是 this 的子对象，随主窗口销毁）
        connect(m_agvClient, &AgvTcpClient::mapSwitched, m_mapDialog,
                &MapInfoDialog::setSwitchMapResult);
        connect(m_agvClient, &AgvTcpClient::relocateFinished, m_mapDialog,
                &MapInfoDialog::setRelocateResult);
        connect(m_agvClient, &AgvTcpClient::locationConfirmed, m_mapDialog,
                &MapInfoDialog::setConfirmResult);
        connect(m_agvClient, &AgvTcpClient::relocateStatusReceived,
                this, &MainWindow::onRelocateStatus);

        // 切图成功后重新拉列表：带回新的 current_map，并自动下载新地图数据
        connect(m_agvClient, &AgvTcpClient::mapSwitched, this,
                [this](bool ok, const QString &, const QString &) {
            if (ok) m_agvClient->requestMapList();
        });

        // 重定位指令被 AGV 接受后才开始轮询状态，避免读到上一次的旧结果
        m_relocTimer = new QTimer(this);
        m_relocTimer->setInterval(1000);
        connect(m_relocTimer, &QTimer::timeout,
                m_agvClient, &AgvTcpClient::requestRelocateStatus);
        connect(m_agvClient, &AgvTcpClient::relocateFinished, this,
                [this](bool ok, const QString &) {
            if (ok) m_relocTimer->start(); else m_relocTimer->stop();
        });

        // 弹窗显示时才轮询激光，关闭即停
        connect(m_mapDialog, &QDialog::finished, this, [this]() {
            m_agvClient->setLaserPolling(false);
            m_relocTimer->stop();
        });

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
    m_agvClient->setLaserPolling(true);
    // 已有最新位姿时立刻画出来
    const AgvState st = m_agvClient->lastState();
    m_mapDialog->updateAgvPose(st.x, st.y, qRadiansToDegrees(st.theta));
}

void MainWindow::onRefreshMap()
{
    m_agvClient->requestMapList();
}

void MainWindow::onMapSwitchRequested(const QString &mapName)
{
    // 结果走 mapSwitched 信号：弹窗显示结果，MainWindow 在成功后刷新地图
    m_agvClient->switchMap(mapName);
}

void MainWindow::onRelocateRequested(double x, double y, double angleRad, bool home)
{
    m_agvClient->relocate(x, y, angleRad, home);
}

void MainWindow::onRelocateStatus(int status)
{
    if (m_mapDialog)
        m_mapDialog->setRelocateStatus(status);
    // 0 失败 / 1 成功 / 3 完成待确认：流程结束，停止轮询（2 = 重定位中，继续）
    if (status != 2)
        m_relocTimer->stop();
}

void MainWindow::onAgvMapListReceived(const QVariantList &maps, const QString &currentMap)
{
    // 保存地图信息，请求当前地图数据
    m_currentMapName = currentMap;
    m_mapList = maps;
    
    // 站点下拉框由地图数据（advancedPointList）填充，见 onAgvMapDataReceived

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

    // 导航站点：与 Python _load_agv_stations 一致，取 advancedPointList 的 instanceName
    QVariantList points = mapData.value(QStringLiteral("advancedPointList")).toList();
    if (points.isEmpty())
        points = mapData.value(QStringLiteral("advanced_point_list")).toList();
    if (points.isEmpty())
        points = mapData.value(QStringLiteral("points")).toList();

    const QString prev = ui->cmbAgvStations->currentText();
    ui->cmbAgvStations->blockSignals(true);
    ui->cmbAgvStations->clear();
    for (const QVariant &pv : std::as_const(points)) {
        const QVariantMap p = pv.toMap();
        QString name = p.value(QStringLiteral("instanceName")).toString();
        if (name.isEmpty()) name = p.value(QStringLiteral("instance_name")).toString();
        if (name.isEmpty()) name = p.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) name = p.value(QStringLiteral("className")).toString();
        if (name.isEmpty()) continue;
        ui->cmbAgvStations->addItem(name, name);
    }
    const int idx = ui->cmbAgvStations->findText(prev);
    if (idx >= 0) ui->cmbAgvStations->setCurrentIndex(idx);
    ui->cmbAgvStations->blockSignals(false);

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
    const bool dragging = ArmWebSocket::isDragMode(db.value(QStringLiteral("mode")).toString());
    if (isLeft) {
        m_lastLeftArmState = db;
        updateArmEnableButton(true, enabled);
        updateArmDragButton(true, dragging);
    } else {
        m_lastRightArmState = db;
        updateArmEnableButton(false, enabled);
        updateArmDragButton(false, dragging);
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

void MainWindow::onLeftArmDragClicked()  { toggleArmDrag(true);  }
void MainWindow::onRightArmDragClicked() { toggleArmDrag(false); }

// 拖动模式只能退到 Disabled，所以「退出拖动」= robot/disable；
// 「进入拖动」= robot/drag，成功后不直接改按钮，等 robot/status 推送 mode 回灌，
// 这样界面始终和真机状态一致。
void MainWindow::toggleArmDrag(bool left)
{
    if (!m_leftArm) return;
    bool &busy = left ? m_leftArmDragBusy : m_rightArmDragBusy;
    if (busy) return;

    auto *s = SettingsManager::instance();
    const QString armName = left ? s->leftArmName() : s->rightArmName();
    const QString sideTxt = left ? QStringLiteral("左臂") : QStringLiteral("右臂");
    QPushButton *btn = left ? ui->btnLeftArmDrag : ui->btnRightArmDrag;
    const bool dragging = left ? m_leftArmDragging : m_rightArmDragging;

    busy = true;
    btn->setEnabled(false);

    if (dragging) {
        // 退出：下电（控制器会先走约 100ms 力矩斜坡再进入 Disabled）
        btn->setText(QStringLiteral("退出中…"));
        const auto [ok, err] = m_leftArm->setArmEnabled(armName, false);
        busy = false;
        btn->setEnabled(m_armConnected);
        if (!ok) {
            updateArmDragButton(left, true);   // 回滚文案
            statusBar()->showMessage(QStringLiteral("⚠ %1 退出拖动失败: %2").arg(sideTxt, err), 5000);
            if (m_messageManager) m_messageManager->error(
                QStringLiteral("%1退出拖动失败: %2").arg(sideTxt, err));
            return;
        }
        statusBar()->showMessage(QStringLiteral("%1已退出拖动并下电").arg(sideTxt), 3000);
        return;
    }

    // 进入拖动：请求最长等 8 s，期间按钮置灰防止重复点击
    btn->setText(QStringLiteral("进入中…"));
    QPointer<MainWindow> self(this);
    m_leftArm->dragArmAsync(armName, [self, left, armName, sideTxt](bool ok, const QString &err) {
        if (!self) return;
        bool &busyRef = left ? self->m_leftArmDragBusy : self->m_rightArmDragBusy;
        busyRef = false;
        QPushButton *b = left ? self->ui->btnLeftArmDrag : self->ui->btnRightArmDrag;
        b->setEnabled(self->m_armConnected);
        // 文案先恢复成当前真机状态；成功时 robot/status 推送会随后改成「退出拖动」
        self->updateArmDragButton(left, left ? self->m_leftArmDragging : self->m_rightArmDragging);
        if (ok) {
            b->setToolTip(QStringLiteral("进入力矩拖动（robot/drag）"));
            self->statusBar()->showMessage(
                QStringLiteral("✓ %1 进入力矩拖动模式，可手扶机械臂示教").arg(sideTxt), 3000);
            if (self->m_messageManager) self->m_messageManager->success(
                QStringLiteral("%1已进入拖动模式").arg(sideTxt));
        } else {
            b->setToolTip(QStringLiteral("上次拖动失败: %1").arg(err));
            self->statusBar()->showMessage(
                QStringLiteral("⚠ %1 进入拖动失败: %2").arg(sideTxt, err), 5000);
            if (self->m_messageManager) self->m_messageManager->error(
                QStringLiteral("%1进入拖动失败: %2").arg(sideTxt, err));
        }
    });
}

void MainWindow::updateArmDragButton(bool left, bool dragging)
{
    (left ? m_leftArmDragging : m_rightArmDragging) = dragging;
    const bool busy = left ? m_leftArmDragBusy : m_rightArmDragBusy;
    if (busy) return;   // 请求进行中由 toggleArmDrag 管文案

    QPushButton *btn = left ? ui->btnLeftArmDrag : ui->btnRightArmDrag;
    btn->setText(dragging ? QStringLiteral("退出拖动") : QStringLiteral("拖动"));
    btn->setToolTip(dragging
        ? QStringLiteral("拖动中：手扶机械臂即可示教。\n点此退出拖动并下电（robot/disable）。")
        : QStringLiteral("进入力矩拖动（robot/drag）；\n按机型自动派到 CSTDrag / ServoDrag。\n再点一次退出（= 下电）。"));
    // 拖动中用黄色实心醒目提示；待机时清掉控件级样式，回到全局 QSS
    btn->setStyleSheet(dragging
        ? QStringLiteral("QPushButton{background:#F9C74F;color:#5A3D00;font-weight:bold;}"
                         "QPushButton:disabled{background:#E0E0E0;color:#999;}")
        : QString());
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
    // 地址来自设置页（默认 DEFAULT_ARM_WEB_URL）；设置被修改后再次打开时重新加载
    static WebViewDialog *dlg = nullptr;
    static QString loadedUrl;
    const QString url = SettingsManager::instance()->armWebUrl();
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

    // Revo2HandClient::init() 内部是一串同步 RS485 事务（rs485/list、
    // peripheral/*、rs485/init），累计可能要好几秒。放在主线程执行会把
    // 界面完全冻住，所以移到线程池。
    //
    // 安全性：init() 走 ArmWebSocket::sendAndWaitSafe()，该接口现在在
    // **调用方线程**用条件变量等待，只把「发送」这一步排回 socket 线程，
    // socket 线程全程空闲，因此从工作线程调用是安全的。
    auto *mgr  = m_handManager;
    QPointer<MainWindow> self(this);
    QThreadPool::globalInstance()->start([mgr, self, btn]() {
        const auto r      = mgr->init();
        const bool leftOk = r.first.first;
        const QString leftErr  = r.first.second;
        const bool rightOk = r.second.first;
        const QString rightErr = r.second.second;

        if (!self) return;
        QMetaObject::invokeMethod(self, [self, btn, leftOk, leftErr,
                                         rightOk, rightErr]() {
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
    });
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
    // 写入走 RS485 同步事务，最长可达数秒，不能占用主线程（否则界面卡住）。
    // sendAndWaitSafe() 在调用方线程等待，socket 线程保持空闲，因此安全。
    QPointer<MainWindow> self(this);
    auto *mm = m_messageManager;
    QThreadPool::globalInstance()->start([client, self, mm]() {
        const QString err = client->executePreset(QStringLiteral("一把抓"));
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, mm, err]() {
            if (!mm) return;
            if (err.isEmpty()) mm->success(QStringLiteral("抓取手势已下发"));
            else               mm->error(err);
        }, Qt::QueuedConnection);
    });
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
    QPointer<MainWindow> self(this);
    auto *mm = m_messageManager;
    QThreadPool::globalInstance()->start([client, self, mm]() {
        const QString err = client->executePreset(QStringLiteral("释放"));
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, mm, err]() {
            if (!mm) return;
            if (err.isEmpty()) mm->success(QStringLiteral("释放手势已下发"));
            else               mm->error(err);
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onHandStopAllRequested()
{
    if (!m_handManager) return;
    // 急停：双手写全 0。同样不能在主线程同步等待 RS485 事务。
    QPointer<MainWindow> self(this);
    auto *mgr = m_handManager;
    auto *mm  = m_messageManager;
    QThreadPool::globalInstance()->start([mgr, self, mm]() {
        const QVector<int> zeros(HAND_DOF_COUNT, 0);
        const QString e1 = mgr->left()->writeAll(zeros);
        const QString e2 = mgr->right()->writeAll(zeros);
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, mm, e1, e2]() {
            if (!mm) return;
            if (e1.isEmpty() && e2.isEmpty())
                mm->success(QStringLiteral("灵巧手已急停"));
            else
                mm->error(QStringLiteral("急停失败: %1%2").arg(e1, e2));
        }, Qt::QueuedConnection);
    });
}

// =====================================================================
// 升降柱
// =====================================================================

void MainWindow::onLiftRefreshClicked()
{
    // 与 Python _refresh_lifting_devices 一致：重新拉 liftingColumn/list
    statusBar()->showMessage(QStringLiteral("正在刷新升降柱设备列表..."), 2000);
    refreshLiftDevices();
}

// 取当前选中的设备；没有设备或客户端未就绪时给出提示并返回空串
static QString currentLiftDevice(MainWindow *w, QComboBox *cmb, LiftingColumnClient *lc,
                                 MessageManager *mm)
{
    Q_UNUSED(w)
    const QString dev = cmb->currentText().trimmed();
    if (!lc || dev.isEmpty()) {
        if (mm) mm->warning(QStringLiteral("升降柱: 没有可用设备，请先连接机械臂并刷新设备列表"));
        return {};
    }
    return dev;
}

void MainWindow::reportLiftResult(const QString &what, const QString &dev, bool ok)
{
    const QString err = m_lifting ? m_lifting->lastError() : QString();
    qInfo().noquote() << QStringLiteral("[Main] liftingColumn %1(%2) -> %3 %4")
                             .arg(what, dev,
                                  ok ? QStringLiteral("ok") : QStringLiteral("fail"), err);
    if (ok) {
        statusBar()->showMessage(QStringLiteral("升降柱: %1 %2 成功").arg(what, dev), 2000);
    } else {
        const QString msg = QStringLiteral("升降柱 %1 失败(%2): %3")
                                .arg(what, dev, err.isEmpty() ? QStringLiteral("未知原因") : err);
        statusBar()->showMessage(msg, 5000);
        if (m_messageManager) m_messageManager->error(msg);
    }
    // 立刻读一次状态，不等推送 / 轮询，标签马上反映使能、运动等变化
    if (m_lifting) {
        const QJsonObject s = m_lifting->getState(dev);
        if (!s.isEmpty() && s.value(QStringLiteral("name")).toString() == dev) {
            m_liftStates.insert(dev, s);
            refreshLiftLabels();
        }
    }
}

void MainWindow::onLiftEnableClicked()
{
    const QString dev = currentLiftDevice(this, ui->cmbLiftDevices, m_lifting, m_messageManager);
    if (dev.isEmpty()) return;
    statusBar()->showMessage(QStringLiteral("升降柱: 正在上电 %1 ...").arg(dev));
    reportLiftResult(QStringLiteral("上电"), dev, m_lifting->enable(dev));
}

void MainWindow::onLiftDisableClicked()
{
    const QString dev = currentLiftDevice(this, ui->cmbLiftDevices, m_lifting, m_messageManager);
    if (dev.isEmpty()) return;
    reportLiftResult(QStringLiteral("下电"), dev, m_lifting->disable(dev));
}

void MainWindow::onLiftClearErrorClicked()
{
    const QString dev = currentLiftDevice(this, ui->cmbLiftDevices, m_lifting, m_messageManager);
    if (dev.isEmpty()) return;
    reportLiftResult(QStringLiteral("清错"), dev, m_lifting->clearError(dev));
}

void MainWindow::onLiftMoveClicked()
{
    const QString dev = currentLiftDevice(this, ui->cmbLiftDevices, m_lifting, m_messageManager);
    if (dev.isEmpty()) return;
    const double heightM = ui->spinLiftTarget->value() / 1000.0;   // UI 是 mm，API 是 m
    const double v = ui->spinLiftSpeed->value();                   // 0 = 默认，不传 speed
    reportLiftResult(QStringLiteral("移动到 %1 mm").arg(ui->spinLiftTarget->value(), 0, 'f', 0),
                     dev, m_lifting->moveTo(dev, heightM, v > 0.0 ? v : -1.0));
}

void MainWindow::onLiftStopClicked()
{
    const QString dev = currentLiftDevice(this, ui->cmbLiftDevices, m_lifting, m_messageManager);
    if (dev.isEmpty()) return;
    reportLiftResult(QStringLiteral("停止"), dev, m_lifting->stop(dev));
}

// 点动：按下发 jogStart 并由 LiftingColumnClient 持续发心跳，松开发 jogStop。
// jogStart 内部是阻塞等响应的（嵌套事件循环），等待期间用户可能已经松手，
// 所以用 m_liftJogHeld 记录按键状态，jogStart 返回后若已松手立刻补一个 jogStop，
// 否则会出现「松手后继续运动」。
void MainWindow::startLiftJog(int direction)
{
    const QString dev = currentLiftDevice(this, ui->cmbLiftDevices, m_lifting, m_messageManager);
    if (dev.isEmpty()) return;

    m_liftJogHeld = true;
    const double v = ui->spinLiftJogSpeed->value();   // 0 = 默认
    const bool ok = m_lifting->startJog(dev, direction, v > 0.0 ? v : -1.0,
                                        ui->spinLiftJogTimeout->value());
    if (!ok) {
        m_liftJogHeld = false;
        reportLiftResult(direction > 0 ? QStringLiteral("点动上升") : QStringLiteral("点动下降"),
                         dev, false);
        return;
    }
    if (!m_liftJogHeld)            // 等待响应期间已经松手
        m_lifting->stopJog(dev);
}

void MainWindow::stopLiftJog()
{
    m_liftJogHeld = false;
    if (!m_lifting || !m_lifting->isJogging()) return;
    const QString dev = ui->cmbLiftDevices->currentText().trimmed();
    const bool ok = m_lifting->stopJog(dev);
    if (!ok)
        reportLiftResult(QStringLiteral("点动停止"), dev, false);
}

void MainWindow::onLiftJogUpPressed()     { startLiftJog(1); }
void MainWindow::onLiftJogUpReleased()    { stopLiftJog(); }
void MainWindow::onLiftJogDownPressed()   { startLiftJog(-1); }
void MainWindow::onLiftJogDownReleased()  { stopLiftJog(); }

void MainWindow::onLiftDeviceChanged(const QString &name)
{
    applyLiftDeviceInfo(name);
    refreshLiftLabels();
}

// 按 list 结果设置目标高度 / 速度的取值范围与软限位显示（与 Python _on_device_changed 一致）
void MainWindow::applyLiftDeviceInfo(const QString &device)
{
    const auto it = m_liftDevInfo.constFind(device);
    if (it == m_liftDevInfo.constEnd()) {
        ui->spinLiftTarget->setRange(-2000, 2000);
        ui->lblLiftLimits->setText(QStringLiteral("—"));
        return;
    }
    const QJsonObject &d = it.value();
    const double minMm = d.value(QStringLiteral("minHeight")).toDouble() * 1000.0;
    const double maxMm = d.value(QStringLiteral("maxHeight")).toDouble() * 1000.0;
    const double maxV  = d.value(QStringLiteral("maxVelocity")).toDouble();
    ui->spinLiftTarget->setRange(minMm, maxMm);
    if (maxV > 0.0) {
        ui->spinLiftSpeed->setMaximum(maxV);
        ui->spinLiftJogSpeed->setMaximum(maxV);
    }
    ui->lblLiftLimits->setText(QStringLiteral("%1~%2 mm")
                                   .arg(minMm, 0, 'f', 0).arg(maxMm, 0, 'f', 0));
}

static void setLiftLabel(QLabel *lbl, const QString &text, const QString &color, bool bold)
{
    lbl->setText(text);
    lbl->setStyleSheet(color.isEmpty()
        ? QString()
        : QStringLiteral("color: %1;%2").arg(color, bold ? QStringLiteral(" font-weight: bold;") : QString()));
}

void MainWindow::refreshLiftLabels()
{
    const QString dev = ui->cmbLiftDevices->currentText().trimmed();
    const auto it = m_liftStates.constFind(dev);
    if (dev.isEmpty() || it == m_liftStates.constEnd()) {
        ui->lblLiftHeight->setText(QStringLiteral("—"));
        setLiftLabel(ui->lblLiftOnline,  QStringLiteral("—"), {}, false);
        setLiftLabel(ui->lblLiftEnabled, QStringLiteral("—"), {}, false);
        setLiftLabel(ui->lblLiftMoving,  QStringLiteral("—"), {}, false);
        setLiftLabel(ui->lblLiftFault,   QStringLiteral("—"), {}, false);
        setLiftAlert({}, {});
        return;
    }

    const QJsonObject &s = it.value();
    const QString kOk = QStringLiteral("#2E7D32"), kMuted = QStringLiteral("#999999"),
                  kBlue = QStringLiteral("#1976D2"), kCyan = QStringLiteral("#00838F"),
                  kDanger = QStringLiteral("#C62828"), kWarn = QStringLiteral("#EF6C00");

    ui->lblLiftHeight->setText(QStringLiteral("%1 mm")
        .arg(qRound(s.value(QStringLiteral("height")).toDouble() * 1000.0)));

    if (s.value(QStringLiteral("online")).toBool())
        setLiftLabel(ui->lblLiftOnline, QStringLiteral("●在线"), kOk, true);
    else
        setLiftLabel(ui->lblLiftOnline, QStringLiteral("○离线"), kMuted, false);

    const bool enabled = s.value(QStringLiteral("enabled")).toBool();
    if (enabled)
        setLiftLabel(ui->lblLiftEnabled, QStringLiteral("✓使能"), kOk, true);
    else
        setLiftLabel(ui->lblLiftEnabled, QStringLiteral("○未使能"), kMuted, false);

    if (s.value(QStringLiteral("jogging")).toBool())
        setLiftLabel(ui->lblLiftMoving, QStringLiteral("▶点动中"), kCyan, true);
    else if (s.value(QStringLiteral("moving")).toBool())
        setLiftLabel(ui->lblLiftMoving, QStringLiteral("▶运动中"), kBlue, true);
    else if (enabled && s.value(QStringLiteral("targetReached")).toBool())
        setLiftLabel(ui->lblLiftMoving, QStringLiteral("●已到位"), kOk, false);
    else
        setLiftLabel(ui->lblLiftMoving, QStringLiteral("○静止"), kMuted, false);

    const bool fault = s.value(QStringLiteral("fault")).toBool();
    const int  code  = s.value(QStringLiteral("errorCode")).toInt();
    if (fault)
        setLiftLabel(ui->lblLiftFault, QStringLiteral("⚠码 %1").arg(code), kDanger, true);
    else
        setLiftLabel(ui->lblLiftFault, QStringLiteral("无"), kMuted, false);

    if (s.value(QStringLiteral("emergencyStopActive")).toBool())
        setLiftAlert(QStringLiteral("⛔ 机器人急停生效中，本升降柱联动下电"), kDanger);
    else if (fault)
        setLiftAlert(QStringLiteral("⚠ 驱动器故障 (码 %1)，请清错后重新使能").arg(code), kWarn);
    else
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
    if (!agvOn) {
        // 断开后状态未知：复位为“急停”，并清掉上报缓存，重连后以 AGV 上报为准
        m_agvReportedSoftEmc = false;
        if (m_agvEmergency) setAgvEmergencyUi(false);
    }
    ui->editAgvLockNick ->setEnabled(agvOn);
    ui->btnAgvLock      ->setEnabled(agvOn);
    ui->btnAgvUnlock    ->setEnabled(agvOn);
    ui->cmbAgvStations  ->setEnabled(agvOn);
    ui->btnViewMap      ->setEnabled(agvOn);
    // 导航控件依赖导航状态再叠加（运行中不能再次开始等）
    updateNavButtons(m_navStatus);

    // ===== 机械臂控件 =====
    const bool armOn = m_armConnected;
    ui->btnLeftArmEnable   ->setEnabled(armOn);
    ui->btnRightArmEnable  ->setEnabled(armOn);
    ui->btnLeftArmDrag     ->setEnabled(armOn && !m_leftArmDragBusy);
    ui->btnRightArmDrag    ->setEnabled(armOn && !m_rightArmDragBusy);
    if (!armOn) {
        // 断线后真机状态未知，拖动按钮回到初始态
        updateArmDragButton(true,  false);
        updateArmDragButton(false, false);
    }
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
    ui->btnLiftRefresh     ->setEnabled(armOn);
    ui->cmbLiftDevices     ->setEnabled(armOn);
    ui->spinLiftTarget     ->setEnabled(armOn);
    ui->spinLiftSpeed      ->setEnabled(armOn);
}
