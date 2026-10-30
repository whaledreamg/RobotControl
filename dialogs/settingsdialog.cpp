#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include "../core/settingsmanager.h"
#include "../core/qssmanager.h"
#include "dialogutil.h"

#include <QDialogButtonBox>
#include <QPushButton>

// 与 dialogs.pri 一致：iOS / WASM 不参与 Web 相关代码的构建
#if !defined(Q_OS_IOS) && !defined(Q_OS_WASM)
#define ROBOT_HAS_WEBVIEW 1
#include "businesstestpanel.h"
#endif

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SettingsDialog)
{
    ui->setupUi(this);

#ifdef ROBOT_HAS_WEBVIEW
    // 「业务调试」标签：内嵌 business-test.html，首次切到该标签时才加载
    m_testPanel = new BusinessTestPanel(this);
    m_testTabIndex = ui->tabs->addTab(m_testPanel, QStringLiteral("业务调试"));
    connect(ui->tabs, &QTabWidget::currentChanged, this, &SettingsDialog::onTabChanged);
#endif

    // 底部按钮的配色由 QSS 的 role 属性决定
    if (auto *ok = ui->btnBox->button(QDialogButtonBox::Ok))
        ok->setProperty("role", QStringLiteral("primary"));
    if (auto *cancel = ui->btnBox->button(QDialogButtonBox::Cancel))
        cancel->setProperty("role", QStringLiteral("close"));

    QssManager::instance()->loadStyleForWidget(this, QStringLiteral("dialogs/settingsdialog"));
    loadFromSettings();
    DialogUtil::adaptForMobile(this);

    connect(ui->btnBox, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(ui->btnBox, &QDialogButtonBox::rejected, this, &SettingsDialog::onRejected);
}

SettingsDialog::~SettingsDialog() { delete ui; }

void SettingsDialog::loadFromSettings()
{
    auto *s = SettingsManager::instance();
    ui->editIp->setText(s->agvIp());
    ui->editArmWebUrl->setText(s->armWebUrl());
    ui->spinArmPort->setValue(s->armWsPort());
    ui->spinAgvPush->setValue(s->agvPushPort());
    ui->spinAgvCtrl->setValue(s->agvCtrlPort());
    ui->spinAgvNav->setValue(s->agvNavPort());
    ui->editLeftArm->setText(s->leftArmName());
    ui->editRightArm->setText(s->rightArmName());
    ui->spinPushInterval->setValue(s->pushInterval());
}

void SettingsDialog::applyToSettings()
{
    auto *s = SettingsManager::instance();
    s->setAgvIp(ui->editIp->text());
    s->setArmWebUrl(ui->editArmWebUrl->text());
    s->setArmWsPort(static_cast<quint16>(ui->spinArmPort->value()));
    s->setAgvPushPort(static_cast<quint16>(ui->spinAgvPush->value()));
    s->setAgvCtrlPort(static_cast<quint16>(ui->spinAgvCtrl->value()));
    s->setAgvNavPort(static_cast<quint16>(ui->spinAgvNav->value()));
    s->setLeftArmName(ui->editLeftArm->text());
    s->setRightArmName(ui->editRightArm->text());
    s->setPushInterval(ui->spinPushInterval->value());
}

void SettingsDialog::onAccepted()
{
    applyToSettings();
    SettingsManager::instance()->saveSettings();
    accept();
}

void SettingsDialog::onRejected()
{
    reject();
}

void SettingsDialog::onTabChanged(int index)
{
#ifdef ROBOT_HAS_WEBVIEW
    if (!m_testPanel || index != m_testTabIndex) return;
#ifndef Q_OS_ANDROID
    // 默认的设置窗口太小，放不下调试页面（Android 上对话框本来就是最大化的）
    resize(qMax(width(), 1100), qMax(height(), 760));
#endif
    m_testPanel->ensureLoaded();
#else
    Q_UNUSED(index);
#endif
}
