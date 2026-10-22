#ifndef UI_MESSAGEWIDGET_H
#define UI_MESSAGEWIDGET_H

#include <QWidget>
#include <QString>
#include <QList>
#include <functional>

class QLabel;
class QPushButton;
class QVBoxLayout;

/// 消息类型
enum class MessageType {
    Info = 0,
    Success = 1,
    Warning = 2,
    Error = 3
};

/**
 * @brief 单条消息弹窗
 *
 * 参考 Python 版 ui/message_widget.py：
 *   - 顶部居中堆叠显示
 *   - 4 种类型：INFO / SUCCESS / WARNING / ERROR
 *   - 自动堆叠排序 + 淡入淡出
 *   - 错误类型有手动关闭按钮，其他类型自动消失
 *   - 错误类型去重：相同内容不会重复弹
 */
class MessageItem : public QWidget
{
    Q_OBJECT
public:
    MessageItem(QWidget *parent, MessageType type, const QString &content,
                int durationMs = 2500);

    MessageType type() const { return m_type; }
    QString content() const { return m_content; }

signals:
    void itemRemoved(MessageItem *item);

protected:
    void paintEvent(QPaintEvent *event) override;

public slots:
    void onLifeTimeout();
    void onCloseClicked();

private:
    void buildUi();

    MessageType m_type;
    QString m_content;
    int m_durationMs;
    QLabel *m_label = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QWidget *m_body = nullptr;
};

/**
 * @brief 消息弹窗管理器（挂在主窗口上，确保浮在所有子控件之上）
 */
class MessageManager : public QWidget
{
    Q_OBJECT
public:
    explicit MessageManager(QWidget *parent = nullptr);

    void info(const QString &text);
    void success(const QString &text);
    void warning(const QString &text);
    void error(const QString &text);
    void show(MessageType type, const QString &text, int durationMs = 2500);

signals:
    /// 消息被移除（供主窗口接状态栏）
    void messageShown(MessageType type, const QString &text);

protected:
    /// 跟随宿主窗口尺寸变化
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onItemRemoved(MessageItem *item);

private:
    void relayout();
    void removeOldest();

    QVBoxLayout *m_layout = nullptr;
    QList<MessageItem *> m_items;
    QWidget *m_host = nullptr;
};

#endif // UI_MESSAGEWIDGET_H
