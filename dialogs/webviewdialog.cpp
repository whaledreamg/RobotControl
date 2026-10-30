#include "webviewdialog.h"
#include "core/qssmanager.h"
#include "dialogs/dialogutil.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QUrl>
#include <QDebug>
#include <QTimer>
#include <QShowEvent>

#ifdef Q_OS_ANDROID
#include <QQuickWidget>
#include <QQuickItem>
#include <QMetaObject>
#else
#include <QWebEngineView>
#endif

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
    DialogUtil::adaptForMobile(this);

    if (!m_initialUrl.isEmpty()) {
        loadUrl(m_initialUrl);
    }
}

void WebViewDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (m_shown) return;
    m_shown = true;

#ifdef Q_OS_ANDROID
    initQuickView();
    // 原生 WebView 还要等布局、几何都稳定后才可用，延迟一拍再加载地址
    QTimer::singleShot(300, this, [this]() {
        if (!m_pendingUrl.isEmpty())
            loadUrl(m_pendingUrl);
    });
#endif
}

#ifdef Q_OS_ANDROID
void WebViewDialog::initQuickView()
{
    // showEvent 时对话框的原生窗口已创建，Qt WebView 才能挂到正确的父窗口上
    if (!windowHandle())
        qWarning() << "WebViewDialog: windowHandle() 为空，WebView 可能无法显示";

    m_view->setSource(QUrl(QStringLiteral("qrc:/resources/qml/WebViewPage.qml")));
    if (m_view->status() == QQuickWidget::Error) {
        qWarning() << "WebViewPage.qml 加载失败:" << m_view->errors();
        m_statusLabel->setText(QStringLiteral("WebView 未初始化（QML 加载失败）"));
        return;
    }
    if (QQuickItem *root = m_view->rootObject()) {
        connect(root, SIGNAL(urlUpdated(QString)),   this, SLOT(onViewUrlChanged(QString)));
        connect(root, SIGNAL(titleUpdated(QString)), this, SLOT(onViewTitleChanged(QString)));
        connect(root, SIGNAL(loadStatus(int,QString,QString)),
                this, SLOT(onLoadStatus(int,QString,QString)));
    }
}
#endif

void WebViewDialog::onLoadStatus(int status, const QString &url, const QString &error)
{
    // 0 开始 / 1 停止 / 2 成功 / 3 失败
    switch (status) {
    case 0: m_statusLabel->setText(QStringLiteral("加载中… %1").arg(url)); break;
    case 2: m_statusLabel->setText(QStringLiteral("已加载 %1").arg(url));  break;
    case 3: m_statusLabel->setText(QStringLiteral("加载失败：%1（%2）").arg(error, url)); break;
    default: break;
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
    m_urlEdit->setPlaceholderText(QStringLiteral("输入 URL，例如 http://192.168.100.16:9198/cocontrol/robotviewmultiarm"));
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

    // --- 网页内容区 ---
#ifdef Q_OS_ANDROID
    // 这里只创建 QQuickWidget，不加载 QML。
    // Qt WebView 在 QML 加载完成时会通过 QQuickRenderControl::renderWindowFor()
    // 取对话框的原生窗口（windowHandle()）作为父窗口。构造函数阶段对话框还没有
    // 原生窗口，windowHandle() 为空，WebView 会被挂到永远不显示的离屏窗口上：
    // 页面能加载成功（状态栏显示“已加载”），但画面不会出现在对话框里。
    // 因此 QML 推迟到 showEvent（原生窗口已创建）之后再加载，见 initQuickView()。
    m_view = new QQuickWidget(this);
    m_view->setResizeMode(QQuickWidget::SizeRootObjectToView);
#else
    m_view = new QWebEngineView(this);
    connect(m_view, &QWebEngineView::urlChanged, this,
            [this](const QUrl &u) { onViewUrlChanged(u.toString()); });
    connect(m_view, &QWebEngineView::titleChanged, this, &WebViewDialog::onViewTitleChanged);
#endif
    mainLayout->addWidget(m_view, 1);

    // 状态栏：显示加载进度/失败原因（Android 无法看 logcat 时用于排查）
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    mainLayout->addWidget(m_statusLabel);
#ifndef Q_OS_ANDROID
    m_statusLabel->hide();
#endif
}

void WebViewDialog::loadUrl(const QString &url)
{
    if (!m_view || url.isEmpty()) return;

    QString u = url.trimmed();
    if (!u.contains(QLatin1String("://"))) {
        u = QStringLiteral("http://") + u;
    }
    m_urlEdit->setText(u);
#ifdef Q_OS_ANDROID
    m_pendingUrl = u;
    // 窗口还没显示时先记下，showEvent 之后再加载
    if (!m_shown) return;
    if (QQuickItem *root = m_view->rootObject())
        QMetaObject::invokeMethod(root, "load", Q_ARG(QVariant, QVariant(QUrl(u))));
    else
        m_statusLabel->setText(QStringLiteral("WebView 未初始化（QML 加载失败）"));
#else
    m_view->setUrl(QUrl(u));
#endif
}

void WebViewDialog::onUrlEdited()
{
    loadUrl(m_urlEdit->text());
}

void WebViewDialog::onBack()
{
#ifdef Q_OS_ANDROID
    if (m_view && m_view->rootObject())
        QMetaObject::invokeMethod(m_view->rootObject(), "goBack");
#else
    if (m_view) m_view->back();
#endif
}

void WebViewDialog::onForward()
{
#ifdef Q_OS_ANDROID
    if (m_view && m_view->rootObject())
        QMetaObject::invokeMethod(m_view->rootObject(), "goForward");
#else
    if (m_view) m_view->forward();
#endif
}

void WebViewDialog::onReload()
{
#ifdef Q_OS_ANDROID
    if (m_view && m_view->rootObject())
        QMetaObject::invokeMethod(m_view->rootObject(), "reload");
#else
    if (m_view) m_view->reload();
#endif
}

void WebViewDialog::onViewUrlChanged(const QString &url)
{
    m_urlEdit->setText(url);
}

void WebViewDialog::onViewTitleChanged(const QString &title)
{
    if (!title.isEmpty() && !title.trimmed().isEmpty()) {
        setWindowTitle(QStringLiteral("%1 — WebView").arg(title));
    }
}
