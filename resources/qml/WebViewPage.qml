import QtQuick
import QtWebView

// Android 上 WebViewDialog 通过 QQuickWidget 加载本页面。
// Qt 6 的 Qt WebView 只提供 QML 类型，没有 C++ 的 QWebView 控件。
Item {
    id: root

    signal urlUpdated(string url)
    signal titleUpdated(string title)
    // status: 0 开始加载 / 1 停止 / 2 成功 / 3 失败（对应 WebView.Load*Status）
    signal loadStatus(int status, string url, string error)

    // 直接调用而不是靠属性变化触发：同一地址重复加载也会生效
    function load(u)     { web.url = u }
    function goBack()    { web.goBack() }
    function goForward() { web.goForward() }
    function reload()    { web.reload() }

    WebView {
        id: web
        anchors.fill: parent
        onUrlChanged: root.urlUpdated(web.url.toString())
        onTitleChanged: root.titleUpdated(web.title)
        onLoadingChanged: function(req) {
            root.loadStatus(req.status, req.url.toString(), req.errorString)
        }
    }
}
