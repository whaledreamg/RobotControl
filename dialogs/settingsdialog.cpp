#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include "../core/settingsmanager.h"
#include "../core/qssmanager.h"
#include "dialogutil.h"

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SettingsDialog)
{
    ui->setupUi(this);
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
