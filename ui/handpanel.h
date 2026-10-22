#ifndef UI_HANDPANEL_H
#define UI_HANDPANEL_H

#include <QFrame>
#include <QWidget>
#include <QVector>
#include <functional>

#include "../communication/revo2hand.h"

class QLabel;
class QPushButton;

/**
 * @brief 单只 Revo2 灵巧手的紧凑控制面板（主界面）
 *
 * ┌────────────────────────────────────────┐
 * │ 🤚 左手 Left  ● 离线        [View]      │
 * │ [✊ 开 抓取]            [🖐 合 释放]     │
 * │ ⚠ 错误信息                             │
 * └────────────────────────────────────────┘
 *
 * 6 个自由度的精确微调放在 View 弹窗里。
 */
class HandPanel : public QFrame
{
    Q_OBJECT
public:
    explicit HandPanel(const QString &side, QWidget *parent = nullptr);

    void bindClient(Revo2HandClient *client);
    void setEnabled(bool enabled);
    void applyState(const HandState &state);

    QString side() const { return m_side; }

signals:
    void grabRequested(const QString &side);
    void releaseRequested(const QString &side);
    void viewRequested(const QString &side);

private:
    void buildUi();
    void onStateChanged(const HandState &state);

    QString m_side;
    Revo2HandClient *m_client = nullptr;

    QLabel *m_statusDot = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_errLabel = nullptr;
    QPushButton *m_btnGrab = nullptr;
    QPushButton *m_btnRelease = nullptr;
    QPushButton *m_btnView = nullptr;
};

/**
 * @brief 双手灵巧手总面板
 */
class DualHandPanel : public QWidget
{
    Q_OBJECT
public:
    explicit DualHandPanel(QWidget *parent = nullptr);

    void bindManager(DualHandManager *manager);
    void setArmConnected(bool connected);
    void onInitDone(bool leftOk, bool rightOk);

    QPushButton *initButton() const { return m_btnInit; }

signals:
    void initRequested();
    void grabRequested(const QString &side);
    void releaseRequested(const QString &side);
    void viewRequested(const QString &side);
    void stopAllRequested();

private:
    void buildUi();

    HandPanel *m_leftPanel = nullptr;
    HandPanel *m_rightPanel = nullptr;
    QPushButton *m_btnInit = nullptr;
    QPushButton *m_btnStopAll = nullptr;
};

#endif // UI_HANDPANEL_H
