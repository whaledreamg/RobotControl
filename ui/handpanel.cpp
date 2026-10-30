#include "handpanel.h"

#include "../core/qssmanager.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace {
// 切换动态属性后需要重新抛光，QSS 的 [prop="x"] 选择器才会生效
void setDynamicProp(QWidget *w, const char *name, const QString &value)
{
    if (!w || w->property(name).toString() == value) return;
    w->setProperty(name, value);
    w->style()->unpolish(w);
    w->style()->polish(w);
}

// 在线状态 -> state 属性值（样式见 controls/handpanel.qss）
void setHandState(QLabel *dot, QLabel *text, const QString &state)
{
    setDynamicProp(dot, "state", state);
    setDynamicProp(text, "state", state);
}
} // namespace

// ============================================================================
// HandPanel
// ============================================================================
HandPanel::HandPanel(const QString &side, QWidget *parent)
    : QFrame(parent), m_side(side)
{
    setObjectName(QStringLiteral("handPanel"));
    buildUi();
    setEnabled(false);
}

void HandPanel::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    const QString sideCn = (m_side == QStringLiteral("left"))
                          ? QStringLiteral("左手") : QStringLiteral("右手");
    const QString sideEn = m_side.left(1).toUpper() + m_side.mid(1);

    // ---- 标题行 ----
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    titleRow->setContentsMargins(0, 0, 0, 0);

    auto *title = new QLabel(QStringLiteral("🤚  %1  %2").arg(sideCn, sideEn));
    title->setObjectName(QStringLiteral("handTitle"));
    titleRow->addWidget(title);
    titleRow->addStretch(1);

    m_statusDot = new QLabel(QStringLiteral("●"));
    m_statusDot->setObjectName(QStringLiteral("handStatusDot"));
    titleRow->addWidget(m_statusDot);

    m_statusLabel = new QLabel(QStringLiteral("未初始化"));
    m_statusLabel->setObjectName(QStringLiteral("handStatusText"));
    titleRow->addWidget(m_statusLabel);

    m_btnView = new QPushButton(QStringLiteral("View"));
    m_btnView->setFixedHeight(24);
    m_btnView->setMinimumWidth(50);
    m_btnView->setToolTip(QStringLiteral("6 自由度微调 + Modbus 调试"));
    m_btnView->setCursor(Qt::PointingHandCursor);
    // 淡紫胶囊小按钮；样式见 controls/handpanel.qss（#handBtnView 清零了全局 padding / min-height）
    m_btnView->setObjectName(QStringLiteral("handBtnView"));
    m_btnView->setProperty("role", QStringLiteral("secondary"));
    connect(m_btnView, &QPushButton::clicked, this, [this]() {
        emit viewRequested(m_side);
    });
    titleRow->addWidget(m_btnView);

    layout->addLayout(titleRow);

    // ---- 开 / 合按钮 ----
    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    btnRow->setContentsMargins(0, 0, 0, 0);

    m_btnGrab = new QPushButton(QStringLiteral("✊  开  抓取"));
    m_btnGrab->setMinimumHeight(36);
    m_btnGrab->setCursor(Qt::PointingHandCursor);
    m_btnGrab->setProperty("role", QStringLiteral("success"));
    connect(m_btnGrab, &QPushButton::clicked, this, [this]() {
        emit grabRequested(m_side);
    });
    btnRow->addWidget(m_btnGrab, 1);

    m_btnRelease = new QPushButton(QStringLiteral("🖐  合  释放"));
    m_btnRelease->setMinimumHeight(36);
    m_btnRelease->setCursor(Qt::PointingHandCursor);
    m_btnRelease->setProperty("role", QStringLiteral("warning"));
    connect(m_btnRelease, &QPushButton::clicked, this, [this]() {
        emit releaseRequested(m_side);
    });
    btnRow->addWidget(m_btnRelease, 1);

    layout->addLayout(btnRow);

    // ---- 错误提示 ----
    m_errLabel = new QLabel;
    m_errLabel->setObjectName(QStringLiteral("handError"));
    m_errLabel->setWordWrap(true);
    layout->addWidget(m_errLabel);
}

void HandPanel::bindClient(Revo2HandClient *client)
{
    m_client = client;
    if (m_client) {
        m_client->registerStateCallback(
            [this](const HandState &s) { onStateChanged(s); });
        applyState(m_client->state());
    }
}

void HandPanel::onStateChanged(const HandState &state)
{
    applyState(state);
}

void HandPanel::applyState(const HandState &state)
{
    if (state.connected && state.inited) {
        setHandState(m_statusDot, m_statusLabel, QStringLiteral("online"));
        m_statusLabel->setText(QStringLiteral("在线"));
    } else if (state.connected) {
        setHandState(m_statusDot, m_statusLabel, QStringLiteral("partial"));
        m_statusLabel->setText(QStringLiteral("已连接（通道未 init）"));
    } else {
        setHandState(m_statusDot, m_statusLabel, QStringLiteral("offline"));
        m_statusLabel->setText(QStringLiteral("未初始化"));
    }
    m_errLabel->setText(state.lastError.isEmpty()
                        ? QString()
                        : QStringLiteral("⚠ %1").arg(state.lastError));
}

void HandPanel::setEnabled(bool enabled)
{
    m_btnGrab->setEnabled(enabled);
    m_btnRelease->setEnabled(enabled);
    m_btnView->setEnabled(true);
    if (!enabled) {
        setHandState(m_statusDot, m_statusLabel, QStringLiteral("offline"));
        m_statusLabel->setText(QStringLiteral("未初始化"));
        m_errLabel->clear();
    }
}

// ============================================================================
// DualHandPanel
// ============================================================================
DualHandPanel::DualHandPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("dualHandPanel"));
    buildUi();
    // 放在 buildUi 之后：样式一次性应用到全部子控件（含两个 HandPanel）
    QssManager::instance()->loadStyleForWidget(this, QStringLiteral("controls/handpanel"));
}

void DualHandPanel::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_leftPanel  = new HandPanel(QStringLiteral("left"));
    m_rightPanel = new HandPanel(QStringLiteral("right"));

    connect(m_leftPanel,  &HandPanel::grabRequested,   this, &DualHandPanel::grabRequested);
    connect(m_leftPanel,  &HandPanel::releaseRequested, this, &DualHandPanel::releaseRequested);
    connect(m_leftPanel,  &HandPanel::viewRequested,    this, &DualHandPanel::viewRequested);
    connect(m_rightPanel, &HandPanel::grabRequested,   this, &DualHandPanel::grabRequested);
    connect(m_rightPanel, &HandPanel::releaseRequested, this, &DualHandPanel::releaseRequested);
    connect(m_rightPanel, &HandPanel::viewRequested,    this, &DualHandPanel::viewRequested);

    layout->addWidget(m_leftPanel);
    layout->addWidget(m_rightPanel);

    // ---- 底部按钮 ----
    auto *bottom = new QHBoxLayout;
    bottom->setSpacing(8);
    bottom->setContentsMargins(0, 0, 0, 0);

    m_btnInit = new QPushButton(QStringLiteral("连接双手"));
    m_btnInit->setMinimumHeight(30);
    m_btnInit->setCursor(Qt::PointingHandCursor);
    m_btnInit->setToolTip(QStringLiteral("setType(Generic485) + ec2rs485/init"));
    m_btnInit->setProperty("role", QStringLiteral("primary"));
    connect(m_btnInit, &QPushButton::clicked, this, &DualHandPanel::initRequested);
    bottom->addWidget(m_btnInit, 1);

    m_btnStopAll = new QPushButton(QStringLiteral("⏹  急停"));
    m_btnStopAll->setMinimumHeight(30);
    m_btnStopAll->setCursor(Qt::PointingHandCursor);
    m_btnStopAll->setEnabled(false);
    m_btnStopAll->setProperty("role", QStringLiteral("danger"));
    connect(m_btnStopAll, &QPushButton::clicked, this, &DualHandPanel::stopAllRequested);
    bottom->addWidget(m_btnStopAll, 1);

    layout->addLayout(bottom);
}

void DualHandPanel::bindManager(DualHandManager *manager)
{
    if (!manager) return;
    m_leftPanel->bindClient(manager->left());
    m_rightPanel->bindClient(manager->right());
}

void DualHandPanel::setArmConnected(bool connected)
{
    if (!connected) {
        m_leftPanel->setEnabled(false);
        m_rightPanel->setEnabled(false);
        m_btnStopAll->setEnabled(false);
        m_btnInit->setEnabled(false);
    } else {
        m_btnInit->setEnabled(true);
    }
}

void DualHandPanel::onInitDone(bool leftOk, bool rightOk)
{
    m_leftPanel->setEnabled(leftOk);
    m_rightPanel->setEnabled(rightOk);
    m_btnStopAll->setEnabled(leftOk || rightOk);
    m_btnInit->setText(QStringLiteral("重新连接"));
}
