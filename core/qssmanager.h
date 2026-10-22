#ifndef CORE_QSSMANAGER_H
#define CORE_QSSMANAGER_H

#include <QObject>
#include <QString>
#include <QHash>
#include <QWidget>

/**
 * @brief QSS 样式管理器 (单例)
 *
 * 负责加载全局 QSS 以及按模块加载样式。
 * 加载顺序: theme -> global -> common -> 模块特定样式
 */
class QssManager : public QObject
{
    Q_OBJECT
public:
    static QssManager* instance();

    /// 加载全局基础样式 (theme + global + common/*)
    void loadGlobalStyle();

    /// 加载模块特定样式 (会覆盖该 widget 上的全局样式)
    void loadStyleForWidget(QWidget *widget, const QString &moduleName);

    /// 切换主题 (dark / light)
    void setTheme(const QString &themeName);

    /// 当前主题名
    QString currentTheme() const;

    /// 重新加载所有样式
    void reloadAllStyles();

    /// 读取 QSS 文件内容
    QString getStyleContent(const QString &resourcePath) const;

signals:
    /// 主题或样式发生变化
    void styleChanged();

private:
    explicit QssManager(QObject *parent = nullptr);
    QssManager(const QssManager&) = delete;
    QssManager& operator=(const QssManager&) = delete;

    QString resolveResourcePath(const QString &moduleName) const;

    QString m_currentTheme = QStringLiteral("light");
    QHash<QWidget*, QString> m_widgetStyles;  // widget -> moduleName
};

#endif // CORE_QSSMANAGER_H
