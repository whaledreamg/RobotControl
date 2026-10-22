#include "webviewdialog.h"
#include "core/qssmanager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QUrl>

WebViewDialog::WebViewDialog(const QString &url, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_initialUrl(url)
{
    setWindowTitle(title.isEmpty() ? QStringLiteral("WebView") : title);
    setMinimumSize(900, 600);
    resize(1100, 750);
    setModal(false);

    setupUi();

    // 加载对话框专用样式
    QssManager::instance()->loadStyleForWidget(this, QStringLiteral("dialogs/webviewdialog"));

    if (!m_initialUrl.isEmpty()) {
        loadUrl(m_initialUrl);
    }
}

void WebViewDialog::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(8);

    // --- 顶部工具栏 ---
    QHBoxLayout *toolbar = new QHBoxLayout();
    toolbar->setSpacing(6);

    m_btnBack = new QPushButton(QStringLiteral("◀"), this);
    m_btnBack->setProperty("role", "navBtn");
    m_btnBack->setFixedWidth(36);
    m_btnBack->setToolTip(QStringLiteral("后退"));
    connect(m_btnBack, &QPushButton::clicked, this, &WebViewDialog::onBack);
    toolbar->addWidget(m_btnBack);

    m_btnForward = new QPushButton(QStringLiteral("▶"), this);
    m_btnForward->setProperty("role", "navBtn");
    m_btnForward->setFixedWidth(36);
    m_btnForward->setToolTip(QStringLiteral("前进"));
    connect(m_btnForward, &QPushButton::clicked, this, &WebViewDialog::onForward);
    toolbar->addWidget(m_btnForward);

    m_btnReload = new QPushButton(QStringLiteral("↻"), this);
    m_btnReload->setProperty("role", "navBtn");
    m_btnReload->setFixedWidth(36);
    m_btnReload->setToolTip(QStringLiteral("刷新"));
    connect(m_btnReload, &QPushButton::clicked, this, &WebViewDialog::onReload);
    toolbar->addWidget(m_btnReload);

    m_urlEdit = new QLineEdit(this);
    m_urlEdit->setPlaceholderText(QStringLiteral("输入 URL，例如 http://192.168.100.16:9198/"));
    m_urlEdit->setMinimumHeight(30);
    connect(m_urlEdit, &QLineEdit::returnPressed, this, &WebViewDialog::onUrlEdited);
    toolbar->addWidget(m_urlEdit, 1);

    QPushButton *btnGo = new QPushButton(QStringLiteral("跳转"));
    btnGo->setProperty("role", "primary");
    connect(btnGo, &QPushButton::clicked, this, &WebViewDialog::onUrlEdited);
    toolbar->addWidget(btnGo);

    QPushButton *btnClose = new QPushButton(QStringLiteral("关闭"));
    btnClose->setProperty("role", "close");
    // WebViewDialog 通过 show() 显示，QDialog::accept() 在 show() 模式
    // 下不会自动隐藏窗口（要 close() 才行）。
    connect(btnClose, &QPushButton::clicked, this, &QWidget::close);
    toolbar->addWidget(btnClose);

    mainLayout->addLayout(toolbar);

    // --- WebEngineView 内容区 ---
    m_view = new QWebEngineView(this);
    connect(m_view, &QWebEngineView::urlChanged, this, &WebViewDialog::onViewUrlChanged);
    connect(m_view, &QWebEngineView::titleChanged, this, &WebViewDialog::onViewTitleChanged);
    mainLayout->addWidget(m_view, 1);
}

void WebViewDialog::loadUrl(const QString &url)
{
    if (!m_view || url.isEmpty()) return;

    QString u = url.trimmed();
    if (!u.contains(QLatin1String("://"))) {
        u = QStringLiteral("http://") + u;
    }
    m_urlEdit->setText(u);
    m_view->setUrl(QUrl(u));
}

void WebViewDialog::onUrlEdited()
{
    loadUrl(m_urlEdit->text());
}

void WebViewDialog::onBack()
{
    if (m_view) m_view->back();
}

void WebViewDialog::onForward()
{
    if (m_view) m_view->forward();
}

void WebViewDialog::onReload()
{
    if (m_view) m_view->reload();
}

void WebViewDialog::onViewUrlChanged(const QUrl &url)
{
    m_urlEdit->setText(url.toString());
}

void WebViewDialog::onViewTitleChanged(const QString &title)
{
    if (!title.isEmpty() && !title.trimmed().isEmpty()) {
        setWindowTitle(QStringLiteral("%1 — WebView").arg(title));
    }
}
