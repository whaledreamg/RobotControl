#include "mainwindow.h"
#include "core/settingsmanager.h"
#include "core/qssmanager.h"
#include "common/global.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("RobotControl"));
    app.setOrganizationName(QStringLiteral("RobotControl"));

    // 注册元类型 (用于跨线程信号槽)
    registerCommonMetaTypes();

    // 加载设置
    SettingsManager::instance()->loadSettings();

    // 加载全局样式
    QssManager::instance()->loadGlobalStyle();

    MainWindow w;
    w.show();

    return app.exec();
}
