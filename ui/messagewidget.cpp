#include "messagewidget.h"

#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kMessageMargin   = 12;
constexpr int kDefaultDuration = 2500;

struct StyleConf {
    const char *border;
    const char *bg;
    const char *color;
    const char *icon;
};

StyleConf styleFor(MessageType t)
{
    switch (t) {
    case MessageType::Success: return {"#67C23A", "rgba(240,249,235,225)", "rgb(60,160,40)",  "✓"};
    case MessageType::Error:   return {"#F56C6C", "rgba(254,240,240,225)", "rgb(200,80,80)",  "✕"};
    case MessageType::Warning: return {"#E6A23C", "rgba(253,246,236,225)", "rgb(190,130,40)", "!"};
    case MessageType::Info:
    default:                   return {"#409EFF", "rgba(240,247,255,225)", "rgb(50,120,210)", "i"};
    }
}
}

// ============================================================================
// MessageItem
// ============================================================================
MessageItem::MessageItem(QWidget *parent, MessageType type,
                         const QString &content, int durationMs)
    : QWidget(parent), m_type(type), m_content(content),
      m_durationMs(durationMs > 0 ? durationMs : kDefaultDuration)
{
    setObjectName(QStringLiteral("messageItem"));
    // 注意：作为 QWidget 子类的弹窗提示，不要加 FramelessWindowHint——
    //       加了会让 widget 脱离父窗口变成独立 top-level。
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    buildUi();

    auto *effect = new QGraphicsDropShadowEffect(this);
    effect->setBlurRadius(18);
    effect->setOffset(0, 3);
    effect->setColor(QColor(0, 0, 0, 60));
    setGraphicsEffect(effect);
}

void MessageItem::buildUi()
{
    const StyleConf sc = styleFor(m_type);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 10, 10, 10);
    layout->setSpacing(10);

    // 左侧图标
    auto *icon = new QLabel(QString::fromUtf8(sc.icon));
    icon->setFixedWidth(20);
    icon->setAlignment(Qt::AlignCenter);
    icon->setStyleSheet(QStringLiteral("color: %1; font-size: 15px; font-weight: bold;")
                             .arg(QString::fromUtf8(sc.color)));
    layout->addWidget(icon);

    // 文本
    m_label = new QLabel(m_content);
    m_label->setWordWrap(true);
    m_label->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;")
                               .arg(QString::fromUtf8(sc.color)));
    layout->addWidget(m_label, 1);

    // 错误类型有手动关闭按钮
    if (m_type == MessageType::Error) {
        m_closeBtn = new QPushButton(QStringLiteral("✕"));
        m_closeBtn->setFixedSize(20, 20);
        m_closeBtn->setCursor(Qt::PointingHandCursor);
        m_closeBtn->setFlat(true);
        m_closeBtn->setStyleSheet(QStringLiteral(
            "QPushButton { border: none; color: %1; font-size: 13px; }"
            "QPushButton:hover { color: #000; }").arg(QString::fromUtf8(sc.color)));
        connect(m_closeBtn, &QPushButton::clicked, this, &MessageItem::onCloseClicked);
        layout->addWidget(m_closeBtn);
    }
}

void MessageItem::paintEvent(QPaintEvent *)
{
    const StyleConf sc = styleFor(m_type);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath path;
    path.addRoundedRect(rect().adjusted(0, 0, -1, -1), 10, 10);
    p.fillPath(path, QColor(QString::fromUtf8(sc.bg)));
    p.setPen(QPen(QColor(QString::fromUtf8(sc.border)), 1.2));
    p.drawPath(path);
}

void MessageItem::onLifeTimeout()
{
    emit itemRemoved(this);
}

void MessageItem::onCloseClicked()
{
    emit itemRemoved(this);
}

// ============================================================================
// MessageManager
// ============================================================================
MessageManager::MessageManager(QWidget *parent)
    : QWidget(parent), m_host(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(kMessageMargin);
    m_layout->addStretch(1);

    if (m_host) {
        m_host->installEventFilter(this);
        resize(m_host->size());
        m_host->installEventFilter(this);
    }
}

void MessageManager::show(MessageType type, const QString &text, int durationMs)
{
    if (text.isEmpty())
        return;

    // 错误类型去重：相同内容不重复弹
    if (type == MessageType::Error) {
        for (MessageItem *it : std::as_const(m_items)) {
            if (it->type() == type && it->content() == text) {
                it->raise();
                return;
            }
        }
    }

    auto *item = new MessageItem(this, type, text, durationMs);
    connect(item, &MessageItem::itemRemoved, this, &MessageManager::onItemRemoved);

    m_layout->insertWidget(m_layout->count() - 1, item);
    m_items.append(item);

    // 错误不自动消失
    if (type != MessageType::Error) {
        QTimer::singleShot(durationMs > 0 ? durationMs : kDefaultDuration,
                           item, &MessageItem::onLifeTimeout);
    }

    emit messageShown(type, text);
    relayout();
}

void MessageManager::info(const QString &text)    { show(MessageType::Info, text); }
void MessageManager::success(const QString &text) { show(MessageType::Success, text); }
void MessageManager::warning(const QString &text) { show(MessageType::Warning, text); }
void MessageManager::error(const QString &text)   { show(MessageType::Error, text, 8000); }

void MessageManager::onItemRemoved(MessageItem *item)
{
    m_items.removeAll(item);
    item->deleteLater();
    relayout();
}

void MessageManager::removeOldest()
{
    if (m_items.isEmpty()) return;
    MessageItem *it = m_items.first();
    m_items.removeFirst();
    it->deleteLater();
    relayout();
}

void MessageManager::relayout()
{
    if (m_host) {
        resize(m_host->size());
        raise();
    }
    // 最多同时显示 6 条
    while (m_items.size() > 6)
        removeOldest();
}

bool MessageManager::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_host && event->type() == QEvent::Resize) {
        const QResizeEvent *re = static_cast<const QResizeEvent *>(event);
        resize(re->size());
    }
    return QWidget::eventFilter(obj, event);
}
