#ifndef DIALOGS_HANDVIEWDIALOG_H
#define DIALOGS_HANDVIEWDIALOG_H

#include <QDialog>
#include <QString>

class Revo2HandClient;
class QTabWidget;
class QWidget;

/**
 * @brief Revo2 灵巧手 6-DOF 微调弹窗
 *
 * 复刻 ui/hand_view_dialog.py 的 5 个 Tab：
 *   1. 6 自由度微调（双滑块：发送 + 当前只读）
 *   2. 预设手势（一把抓/释放/半抓/1010/两指捏/点赞/指点）
 *   3. Modbus 调试（自由 0x04/0x10 报文 + 收发记录）
 *   4. 参数配置（从站/ec2rs485 通道/写时间）
 *   5. 关节序列（多步位置时间编辑 + 捕获 + 播放）
 */
class HandViewDialog : public QDialog
{
    Q_OBJECT
public:
    explicit HandViewDialog(const QString &side,
                            Revo2HandClient *client,
                            QWidget *parent = nullptr);
    ~HandViewDialog();

private:
    void buildUi();

    QString m_side;
    Revo2HandClient *m_client;
    QTabWidget *m_tabs;
};

#endif // DIALOGS_HANDVIEWDIALOG_H
