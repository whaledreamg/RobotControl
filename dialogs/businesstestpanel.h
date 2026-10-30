#ifndef DIALOGS_BUSINESSTESTPANEL_H
#define DIALOGS_BUSINESSTESTPANEL_H

#include <QWidget>

class QLabel;
#ifdef Q_OS_ANDROID
class QQuickWidget;
#else
class QWebEngineView;
#endif

/**
 * @brief 业务调试面板：内嵌 business-test.html / business-friction.js
 *
 * 两个文件通过 resources.qrc 打包进程序。运行时先释放到可写目录
 * （AppDataLocation/web），再以 file:// 加载：
 *   - Qt WebView（Android）不支持 qrc 地址；
 *   - 页面里的 <script src="./business-friction.js"> 需要和 html 处于同一目录。
 *
 * 桌面用 QWebEngineView，Android 用 Qt WebView（经 QQuickWidget 内嵌）。
 * 首次调用 ensureLoaded() 时才创建网页内容，避免打开设置页就加载 1MB 的页面。
 * 需要在宿主窗口已显示之后调用（Android 的原生 WebView 依赖窗口句柄）。
 */
class BusinessTestPanel : public QWidget
{
    Q_OBJECT
public:
    explicit BusinessTestPanel(QWidget *parent = nullptr);

    /// 首次调用时释放资源并加载页面，之后调用无效果
    void ensureLoaded();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private slots:
    void onLoadStatus(int status, const QString &url, const QString &error);

private:
    QString extractWebFiles(QString *error) const;
    void setStatus(const QString &text);

#ifdef Q_OS_ANDROID
    QQuickWidget   *m_view = nullptr;
#else
    QWebEngineView *m_view = nullptr;
#endif
    QLabel *m_status = nullptr;
    bool    m_loaded = false;
};

#endif // DIALOGS_BUSINESSTESTPANEL_H
