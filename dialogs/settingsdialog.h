#ifndef DIALOGS_SETTINGSDIALOG_H
#define DIALOGS_SETTINGSDIALOG_H

#include <QDialog>

namespace Ui { class SettingsDialog; }
class BusinessTestPanel;

/**
 * @brief 设置对话框 (网络 / 设备)
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog() override;

    /// 从 SettingsManager 加载当前值到 UI
    void loadFromSettings();

    /// 应用 UI 值到 SettingsManager
    void applyToSettings();

private slots:
    void onAccepted();
    void onRejected();
    void onTabChanged(int index);

private:
    Ui::SettingsDialog *ui;
    BusinessTestPanel *m_testPanel = nullptr;   // iOS / WASM 上为空
    int m_testTabIndex = -1;
};

#endif // DIALOGS_SETTINGSDIALOG_H
