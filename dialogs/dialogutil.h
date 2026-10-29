#ifndef DIALOGS_DIALOGUTIL_H
#define DIALOGS_DIALOGUTIL_H

#include <QDialog>
#include <QEvent>
#include <QKeyEvent>
#include <QLayout>
#include <QObject>

// 移动端（Android）对话框适配：
//  1. 对话框以最大化窗口显示，避免固定尺寸超出屏幕导致“关闭”按钮不可见；
//  2. 响应系统返回键（Qt::Key_Back），QDialog 默认只处理 Esc。
// 桌面平台不做任何改动。
namespace DialogUtil {

class BackKeyFilter : public QObject
{
public:
    explicit BackKeyFilter(QDialog *dlg) : QObject(dlg), m_dlg(dlg) {}

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (obj == m_dlg && ev->type() == QEvent::KeyPress
            && static_cast<QKeyEvent *>(ev)->key() == Qt::Key_Back) {
            m_dlg->close();   // QDialog::closeEvent -> reject()
            return true;
        }
        return QObject::eventFilter(obj, ev);
    }

private:
    QDialog *m_dlg;
};

inline void adaptForMobile(QDialog *dlg)
{
#ifdef Q_OS_ANDROID
    // 须在 dlg 的布局建立之后调用：布局的最小尺寸提示会撑大窗口，
    // 解除约束后窗口可以被限制在屏幕范围内
    dlg->setMinimumSize(0, 0);
    if (dlg->layout())
        dlg->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    dlg->setWindowState(dlg->windowState() | Qt::WindowMaximized);
    dlg->installEventFilter(new BackKeyFilter(dlg));
#else
    Q_UNUSED(dlg);
#endif
}

} // namespace DialogUtil

#endif // DIALOGS_DIALOGUTIL_H
