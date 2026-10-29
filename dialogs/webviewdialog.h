#ifndef WEBVIEWDIALOG_H
#define WEBVIEWDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>

// 桌面：QWebEngineView（Qt WebEngine）
// Android：Qt WebView（Qt 6 仅提供 QML 类型），通过 QQuickWidget 内嵌
#ifdef Q_OS_ANDROID
class QQuickWidget;
#else
class QWebEngineView;
#endif

class WebViewDialog : public QDialog
{
    Q_OBJECT

public:
    explicit WebViewDialog(const QString &url = QString(),
                          const QString &title = QString("WebView"),
                          QWidget *parent = nullptr);
    ~WebViewDialog() = default;

    void loadUrl(const QString &url);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onLoadStatus(int status, const QString &url, const QString &error);
    void onUrlEdited();
    void onBack();
    void onForward();
    void onReload();
    void onViewUrlChanged(const QString &url);
    void onViewTitleChanged(const QString &title);

private:
    void setupUi();

#ifdef Q_OS_ANDROID
    QQuickWidget   *m_view = nullptr;
#else
    QWebEngineView *m_view = nullptr;
#endif
    QLabel         *m_statusLabel = nullptr;
    QString         m_pendingUrl;   // 窗口首次显示前请求的地址，显示后再真正加载
    bool            m_shown = false;
    QLineEdit      *m_urlEdit = nullptr;
    QPushButton    *m_btnBack = nullptr;
    QPushButton    *m_btnForward = nullptr;
    QPushButton    *m_btnReload = nullptr;
    QString         m_initialUrl;
};

#endif // WEBVIEWDIALOG_H
