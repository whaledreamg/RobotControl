#ifndef WEBVIEWDIALOG_H
#define WEBVIEWDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QWebEngineView>

class WebViewDialog : public QDialog
{
    Q_OBJECT

public:
    explicit WebViewDialog(const QString &url = QString(),
                          const QString &title = QString("WebView"),
                          QWidget *parent = nullptr);
    ~WebViewDialog() = default;

    void loadUrl(const QString &url);

private slots:
    void onUrlEdited();
    void onBack();
    void onForward();
    void onReload();
    void onViewUrlChanged(const QUrl &url);
    void onViewTitleChanged(const QString &title);

private:
    void setupUi();

    QWebEngineView *m_view = nullptr;
    QLineEdit      *m_urlEdit = nullptr;
    QPushButton    *m_btnBack = nullptr;
    QPushButton    *m_btnForward = nullptr;
    QPushButton    *m_btnReload = nullptr;
    QString         m_initialUrl;
};

#endif // WEBVIEWDIALOG_H
