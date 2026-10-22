#include "qssmanager.h"
#include <QFile>
#include <QApplication>
#include <QDir>
#include <QDebug>

QssManager* QssManager::instance()
{
    static QssManager inst;
    return &inst;
}

QssManager::QssManager(QObject *parent) : QObject(parent) {}

void QssManager::loadGlobalStyle()
{
    // 加载顺序: theme -> global -> common/*
    QStringList styleFiles = {
        QStringLiteral(":/resources/styles/theme_%1.qss").arg(m_currentTheme),
        QStringLiteral(":/resources/styles/global.qss"),
        QStringLiteral(":/resources/styles/common/common_button.qss"),
        QStringLiteral(":/resources/styles/common/common_input.qss"),
        QStringLiteral(":/resources/styles/common/common_label.qss"),
        QStringLiteral(":/resources/styles/common/common_combobox.qss"),
        QStringLiteral(":/resources/styles/common/common_slider.qss"),
        QStringLiteral(":/resources/styles/common/common_groupbox.qss"),
        QStringLiteral(":/resources/styles/common/common_scrollbar.qss"),
    };

    QString combined;
    for (const QString &path : styleFiles) {
        combined += getStyleContent(path) + QStringLiteral("\n");
    }

    qApp->setStyleSheet(combined);
    qDebug() << "[QssManager] Global style loaded. Theme:" << m_currentTheme;
}

void QssManager::loadStyleForWidget(QWidget *widget, const QString &moduleName)
{
    if (!widget) {
        return;
    }

    const QString path = resolveResourcePath(moduleName);
    const QString content = getStyleContent(path);
    if (content.isEmpty()) {
        qWarning() << "[QssManager] No style content for module:" << moduleName;
        return;
    }

    widget->setStyleSheet(content);
    m_widgetStyles[widget] = moduleName;
}

void QssManager::setTheme(const QString &themeName)
{
    if (m_currentTheme == themeName) {
        return;
    }
    m_currentTheme = themeName;
    loadGlobalStyle();
    emit styleChanged();
}

QString QssManager::currentTheme() const
{
    return m_currentTheme;
}

void QssManager::reloadAllStyles()
{
    loadGlobalStyle();

    // 重新应用模块样式
    const QList<QWidget*> widgets = m_widgetStyles.keys();
    for (QWidget *w : widgets) {
        if (w) {
            loadStyleForWidget(w, m_widgetStyles.value(w));
        } else {
            m_widgetStyles.remove(w);
        }
    }

    emit styleChanged();
}

QString QssManager::getStyleContent(const QString &resourcePath) const
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

QString QssManager::resolveResourcePath(const QString &moduleName) const
{
    // 支持: "controls/agvcontrol" -> ":/resources/styles/controls/agvcontrol.qss"
    if (moduleName.startsWith(QLatin1String(":/"))) {
        return moduleName;
    }
    return QStringLiteral(":/resources/styles/%1.qss").arg(moduleName);
}
