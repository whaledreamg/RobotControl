#include "messagewidget.h"

#include <QColor>
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
constexpr int kMinLabelWidth   = 160;   // 文字区最小宽度（短消息也不会太窄）
constexpr int kMaxLabelWidth   = 640;   // 文字区最大宽度（同 Python 版），超出则换行
constexpr int kLabelSlack      = 6;     // 文本宽度之外的固定余量，避免字体取整导致末尾被换行/截断

/// 错误类型的存活时长（ms）。
///
/// 错误弹窗必须能自己消失。旧实现给 Error 类型单独开了「不自动消失」的
/// 分支，导致 error() 里传的时长完全失效，弹窗只能靠手点 ✕ 关闭——
/// 在 Android 上那个 ✕ 按钮还可能收不到触摸事件，弹窗就永久挂在界面上。
constexpr int kErrorDuration = 10000;

struct StyleConf {
    const char *border;
    QColor      bg;     // 半透明背景。注意必须用 QColor(r,g,b,a)：
                        // QColor(QString) 不识别 CSS 的 "rgba(...)"，会得到无效色并被画成纯黑
    const char *color;
    const char *icon;
};

StyleConf styleFor(MessageType t)
{
    switch (t) {
    case MessageType::Success: return {"#67C23A", QColor(240, 249, 235, 220), "rgb(60,160,40)",  "✓"};
    case MessageType::Error:   return {"#F56C6C", QColor(254, 240, 240, 225), "rgb(200,80,80)",  "✕"};
    case MessageType::Warning: return {"#E6A23C", QColor(253, 246, 236, 225), "rgb(190,130,40)", "!"};
    case MessageType::Info:
    default:                   return {"#409EFF", QColor(240, 247, 255, 225), "rgb(50,120,210)", "i"};
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

    m_lifeTimer = new QTimer(this);
    m_lifeTimer->setSingleShot(true);
    connect(m_lifeTimer, &QTimer::timeout, this, &MessageItem::onLifeTimeout);
}

void MessageItem::startLifeTimer()
{
    // 用单次定时器而不是直接删控件，是为了让这一帧的绘制先完成，
    // 弹窗不会闪一下就消失。
    m_lifeTimer->stop();
    m_lifeTimer->start(m_durationMs);
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

    // 文本：宽度 = 文本实际宽度 + 固定余量，不再被拉成整行宽。
    // 全局 QSS 会覆盖 QLabel 的字体，所以这里先把标签挂到 this 上、写好样式并 ensurePolished，
    // 再用它最终生效的字体去量宽度，这样量出来的值和实际绘制一致。
    m_label = new QLabel(m_content, this);
    m_label->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;")
                               .arg(QString::fromUtf8(sc.color)));
    m_label->ensurePolished();

    // 单行放得下就不换行；超过上限（随宿主窗口宽度，最多 kMaxLabelWidth）才自动换行
    const int hostW = parentWidget() ? parentWidget()->width() : 800;
    const int maxLabelW = qBound(kMinLabelWidth, hostW - 160, kMaxLabelWidth);
    const QRect textRect = m_label->fontMetrics().boundingRect(
        QRect(0, 0, maxLabelW, 100000), Qt::TextWordWrap, m_content);
    const int labelW = qMax(kMinLabelWidth, textRect.width() + kLabelSlack);
    m_label->setWordWrap(true);
    m_label->setFixedWidth(labelW);
    layout->addWidget(m_label);

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
    p.fillPath(path, sc.bg);
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

    // 水平居中，宽度取消息自身大小，不被布局拉伸成整行
    m_layout->insertWidget(m_layout->count() - 1, item, 0, Qt::AlignHCenter);
    m_items.append(item);

    // 所有类型都自动消失（含错误）。旧实现给 Error 开了「不自动消失」的
    // 分支，error() 里传的时长被完全忽略，弹窗只能手动关闭——
    // Android 上那个 ✕ 还可能收不到触摸事件，弹窗就永久挂在界面上。
    item->startLifeTimer();

    emit messageShown(type, text);
    relayout();
}

void MessageManager::info(const QString &text)    { show(MessageType::Info, text); }
void MessageManager::success(const QString &text) { show(MessageType::Success, text); }
void MessageManager::warning(const QString &text) { show(MessageType::Warning, text); }
// 错误弹窗停留 10 秒后自动消失
void MessageManager::error(const QString &text)   { show(MessageType::Error, text, kErrorDuration); }

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
