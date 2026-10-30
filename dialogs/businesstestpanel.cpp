#include "businesstestpanel.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#ifdef Q_OS_ANDROID
#include <QMetaObject>
#include <QQuickItem>
#include <QQuickWidget>
#include <QTimer>
#else
#include <QWebEngineSettings>
#include <QWebEngineView>
#endif

namespace {
const char *const kPageName = "business-test.html";
const char *const kResourceDir = ":/resources/web/";
}

BusinessTestPanel::BusinessTestPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

#ifdef Q_OS_ANDROID
    // 只创建控件，QML 推迟到 ensureLoaded()：Qt WebView 需要宿主窗口已有原生句柄
    m_view = new QQuickWidget(this);
    m_view->setResizeMode(QQuickWidget::SizeRootObjectToView);
#else
    m_view = new QWebEngineView(this);
#endif
    layout->addWidget(m_view, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->hide();
    layout->addWidget(m_status);
}

void BusinessTestPanel::setStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

QString BusinessTestPanel::extractWebFiles(QString *error) const
{
    const QString dirPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/web");
    if (!QDir().mkpath(dirPath)) {
        *error = QStringLiteral("无法创建目录：%1").arg(dirPath);
        return {};
    }

    const QStringList names = { QString::fromLatin1(kPageName),
                                QStringLiteral("business-friction.js") };
    for (const QString &name : names) {
        const QString src = QString::fromLatin1(kResourceDir) + name;
        const QString dst = dirPath + QLatin1Char('/') + name;
        // 每次都用打包进程序的版本覆盖，保证和程序版本一致
        QFile::remove(dst);
        if (!QFile::copy(src, dst)) {
            *error = QStringLiteral("释放 %1 失败（%2）").arg(name, dst);
            return {};
        }
        // 从 qrc 复制出来的文件是只读的，改成可写以便下次覆盖
        QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    return dirPath + QLatin1Char('/') + QString::fromLatin1(kPageName);
}

void BusinessTestPanel::ensureLoaded()
{
    if (m_loaded) return;
    m_loaded = true;

    QString error;
    const QString file = extractWebFiles(&error);
    if (file.isEmpty()) {
        qWarning() << "BusinessTestPanel:" << error;
        setStatus(error);
        return;
    }
    const QUrl url = QUrl::fromLocalFile(file);

#ifdef Q_OS_ANDROID
    m_view->setSource(QUrl(QStringLiteral("qrc:/resources/qml/WebViewPage.qml")));
    QQuickItem *root = m_view->rootObject();
    if (m_view->status() == QQuickWidget::Error || !root) {
        qWarning() << "WebViewPage.qml 加载失败:" << m_view->errors();
        setStatus(QStringLiteral("WebView 未初始化（QML 加载失败）"));
        return;
    }
    connect(root, SIGNAL(loadStatus(int,QString,QString)),
            this, SLOT(onLoadStatus(int,QString,QString)));
    // Android 11+ 默认禁止 WebView 读本地文件，需显式放开（仅 allowFileAccess，
    // 不放开 file:// 页面之间互相读取）
    root->setProperty("allowLocalFiles", true);
    // 原生 WebView 要等布局稳定后才可用，延迟一拍再加载（同 WebViewDialog）
    QTimer::singleShot(300, this, [this, url]() {
        if (QQuickItem *r = m_view->rootObject())
            QMetaObject::invokeMethod(r, "load", Q_ARG(QVariant, QVariant(url)));
    });
#else
    // 页面从 CDN 加载 three.js（ESM），file:// 页面默认不允许访问远程地址
    m_view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    m_view->setUrl(url);
#endif
}

void BusinessTestPanel::onLoadStatus(int status, const QString &url, const QString &error)
{
    // 0 开始 / 1 停止 / 2 成功 / 3 失败
    switch (status) {
    case 0: setStatus(QStringLiteral("加载中… %1").arg(url)); break;
    case 2: setStatus(QString()); break;
    case 3: setStatus(QStringLiteral("加载失败：%1（%2）").arg(error, url)); break;
    default: break;
    }
}

void BusinessTestPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
#ifdef Q_OS_ANDROID
    if (m_view && m_view->rootObject())
        m_view->rootObject()->setVisible(true);
#endif
}

void BusinessTestPanel::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
#ifdef Q_OS_ANDROID
    // Android 的 WebView 是盖在 Qt 窗口上的原生控件，切换到别的标签页时
    // 不会自动跟着隐藏，需要通过 QML 根节点的 visible 通知它
    if (m_view && m_view->rootObject())
        m_view->rootObject()->setVisible(false);
#endif
}
