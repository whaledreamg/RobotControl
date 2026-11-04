// 回归测试：ArmWebSocket 超时后收到迟到响应导致 use-after-free 崩溃
//
// 复现路径（对应真实故障「连接后就闪退」）：
//   1. connectToArm 成功 -> subscribeAll/fetchInitialStatus -> onPollTick
//   2. onPollTick 每秒 3 次 sendAndWait(..., 2000ms)，控制器对 project/status
//      之类回包稍慢即超时
//   3. 旧实现：sendAndWaitRaw 把「捕获栈变量引用」的 lambda 存进 m_pending，
//      超时后**不摘除**该条目 -> 栈帧销毁后成为悬垂引用
//   4. 迟到的响应命中 completePending -> cb(QVariant(obj)) 写已销毁的栈内存
//      -> use-after-free（崩溃点 armwebsocket.cpp:294）
//
// 验证点：
//   - 超时后 m_pending 必须归零（迟到响应不得再回调到已失效的等待者）
//   - 迟到响应到达时不得崩溃
//   - 未连接时 sendAndWait 必须立刻返回（不得白等一个完整超时）
//   - 轮询不得重入堆积（pending 数不得无限增长）

#include "armwebsocket.h"

#include <QCoreApplication>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

static int g_failures = 0;

static void check(bool cond, const QString &what)
{
    if (cond) {
        qInfo().noquote() << "  [PASS] " << what;
    } else {
        qWarning().noquote() << "  [FAIL] " << what;
        ++g_failures;
    }
}

static void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// ===========================================================================
// 测试服务端
//   - 普通动作立即回包
//   - kHeldAction 收到后扣住不回复，由测试手动 flushHeld() 制造「迟到响应」
// ===========================================================================
class TestServer : public QObject
{
public:
    static constexpr const char *kHeldAction = "lateTest";

    explicit TestServer(QObject *parent = nullptr) : QObject(parent)
    {
        m_server = new QWebSocketServer("test", QWebSocketServer::NonSecureMode, this);
        m_server->listen(QHostAddress::LocalHost, 0);
        connect(m_server, &QWebSocketServer::newConnection, this, &TestServer::onNew);
    }

    quint16 port() const { return m_server->serverPort(); }

    /// 控制器真实臂名（文档强调主题名必须取自 robot/list）
    void setRealArmNames(const QStringList &names) { m_realArms = names; }
    QStringList realArmNames() const { return m_realArms; }

    /// 释放被扣住的迟到响应 —— 此刻 sendAndWaitRaw 的栈帧早已销毁
    void flushHeld()
    {
        for (QWebSocket *s : m_sockets) {
            if (m_heldId >= 0) {
                s->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(reply(m_heldId)).toJson(QJsonDocument::Compact)));
            }
        }
        m_heldId = -1;
    }

    /// 主动下发一条订阅推送（ty 形如 "publish/robot/LeftArm/status"，无 id）
    void push(const QString &topic, const QJsonObject &db)
    {
        QJsonObject out;
        out.insert("id", -1);
        out.insert("ty", QStringLiteral("publish/") + topic);
        out.insert("db", db);
        const QByteArray raw =
            QJsonDocument(out).toJson(QJsonDocument::Compact);
        for (QWebSocket *s : m_sockets)
            s->sendTextMessage(QString::fromUtf8(raw));
    }

private:
    static QJsonObject reply(int id)
    {
        QJsonObject out;
        out.insert("id", id);
        out.insert("ty", "resp");
        out.insert("db", QJsonObject{{"ok", true}});
        return out;
    }

    /// project/listProjects 返回数组 db，其余动作用 reply()
    static QJsonObject listReply(int id)
    {
        QJsonObject p1;
        p1.insert("name", QStringLiteral("demo_project"));
        p1.insert("note", QStringLiteral("演示"));
        QJsonObject out;
        out.insert("id", id);
        out.insert("ty", "resp");
        out.insert("db", QJsonArray{p1});
        return out;
    }

    /// robot/list：返回控制器实际注册的机器人
    QJsonObject robotListReply(int id) const
    {
        QJsonArray arr;
        for (int i = 0; i < m_realArms.size(); ++i) {
            QJsonObject r;
            r.insert("id", i);
            r.insert("name", m_realArms.at(i));
            r.insert("jointCount", 6);
            arr.append(r);
        }
        QJsonObject out;
        out.insert("id", id);
        out.insert("ty", "resp");
        out.insert("db", arr);
        return out;
    }

    /// 订阅 ack：未知主题按文档 §4.3 返回 err 25004
    static QJsonObject subAck(int id, bool ok, const QString &msg = {})
    {
        QJsonObject out;
        out.insert("id", id);
        out.insert("ty", "resp");
        if (ok) {
            out.insert("db", true);
        } else {
            out.insert("db", QJsonValue());
            QJsonObject e;
            e.insert("code", 25004);
            e.insert("message", msg.isEmpty() ? QStringLiteral("请求的订阅主题不存在。") : msg);
            out.insert("err", e);
        }
        return out;
    }

    void onNew()
    {
        while (m_server->hasPendingConnections()) {
            QWebSocket *sock = m_server->nextPendingConnection();
            m_sockets << sock;
            connect(sock, &QWebSocket::textMessageReceived, this,
                    [this, sock](const QString &msg) {
                const QJsonObject in =
                    QJsonDocument::fromJson(msg.toUtf8()).object();
                const int id = in.value("id").toInt(-1);
                const QString ty = in.value("ty").toString();

                const auto send = [sock](const QJsonObject &o) {
                    sock->sendTextMessage(
                        QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
                };

                if (ty == QLatin1String(kHeldAction)) {
                    m_heldId = id;          // 扣住，制造迟到
                    return;
                }
                if (ty == QLatin1String("robot/list")) {
                    send(robotListReply(id));
                    return;
                }
                if (ty.endsWith(QStringLiteral("/sub"))) {
                    // 顶层 topics：只有真实臂名的主题才被接受
                    QStringList topics;
                    const QJsonValue tv = in.value(QStringLiteral("topics"));
                    if (tv.isArray()) {
                        for (const QJsonValue &v : tv.toArray())
                            topics << v.toString();
                    } else if (tv.isString()) {
                        topics << tv.toString();
                    }
                    bool allKnown = true;
                    for (const QString &t : topics) {
                        if (t.startsWith(QStringLiteral("robot/"))) {
                            const QString arm = t.mid(6, t.size() - 6 - 7);
                            if (!m_realArms.contains(arm))
                                allKnown = false;
                        } else if (t != QStringLiteral("project/status")) {
                            allKnown = false;
                        }
                    }
                    send(subAck(id, allKnown));
                    return;
                }
                if (ty == QLatin1String("project/listProjects")) {
                    send(listReply(id));
                    return;
                }
                send(reply(id));
            });
        }
    }

    QWebSocketServer *m_server = nullptr;
    QList<QWebSocket *> m_sockets;
    int m_heldId = -1;
    QStringList m_realArms{QStringLiteral("LeftArm"), QStringLiteral("RightArm")};
};

// ===========================================================================
// 用例 1：超时 -> 栈帧销毁 -> 迟到响应（旧代码在此 use-after-free 崩溃）
// ===========================================================================
static void testLateResponseAfterTimeout()
{
    qInfo() << "\n=== 用例 1: 超时后到达的迟到响应 ===";
    TestServer server;
    ArmWebSocket arm;

    check(arm.connectToArm(QStringLiteral("127.0.0.1"), server.port()),
          "connectToArm 成功");

    // 先让 connectToArm 内置的 singleShot(500ms)->subscribeAll->
    // fetchInitialStatus 跑完，把嵌套事件循环的影响排除干净
    pump(800);

    // 服务端扣住 kHeldAction 不回包 -> 这次等待必然走满超时
    QElapsedTimer t;
    t.start();
    const QVariant r =
        arm.sendAndWait(QString::fromLatin1(TestServer::kHeldAction), QJsonObject(), 300);
    const qint64 elapsed = t.elapsed();

    check(!r.isValid(), "超时应返回空 QVariant");
    check(elapsed >= 250,   // 容忍定时器精度
          QStringLiteral("确实等满了超时（实际 %1ms）").arg(elapsed));
    check(arm.pendingCount() == 0,
          QStringLiteral("超时后 pending 必须归零（实际 %1）").arg(arm.pendingCount()));

    // >>> sendAndWaitRaw 的栈帧此刻已销毁 <<<
    // 迟到的响应送达：旧实现在 completePending 里回调悬垂 lambda 而崩溃
    server.flushHeld();
    pump(500);

    check(true, "迟到响应到达后未崩溃");
    check(arm.pendingCount() == 0, "迟到响应不应复活 pending 条目");
}

// ===========================================================================
// 用例 2：轮询重入保护
//   onPollTick 内部是阻塞式 sendAndWait，嵌套事件循环会让 1s 定时器在
//   2s 等待期间再次触发本槽 -> 轮询递归堆积
// ===========================================================================
static void testPollDoesNotReenter()
{
    qInfo() << "\n=== 用例 2: 轮询重入保护 ===";
    ArmWebSocket arm;

    // 故意连一个不存在的地址：握手超时后 isConnected() 为 false，
    // 直接验证 startPolling 后反复触发不会把 pending 堆起来
    arm.startPolling(1000);
    pump(2500);

    check(arm.pendingCount() <= 1,
          QStringLiteral("未连接时轮询不应产生 pending（实际 %1）").arg(arm.pendingCount()));
}

// ===========================================================================
// 用例 3：未连接时 sendAndWait 必须立即返回，而不是白等一个完整超时
// ===========================================================================
static void testDisconnectedReturnsImmediately()
{
    qInfo() << "\n=== 用例 3: 未连接时 sendAndWait 立即返回 ===";
    ArmWebSocket arm;   // 从未连接

    QElapsedTimer timer;
    timer.start();
    const QVariant r =
        arm.sendAndWait(QStringLiteral("project/status"), QJsonObject(), 5000);
    const qint64 elapsed = timer.elapsed();

    check(!r.isValid(), "未连接时返回空 QVariant");
    check(elapsed < 1000,
          QStringLiteral("未连接时应立即返回而非等满超时（实际 %1ms）").arg(elapsed));
}

// ===========================================================================
// 用例 4：推送必须翻译成 UI 实际连接的 Qt 信号
//   此前 dispatchPush 只 emit 了 topicPushReceived，而 MainWindow 连的是
//   robotStatusReceived / projectStatusReceived / projectListReceived，
//   导致数据收到即丢弃 -> 关节角/位姿/工程状态/下拉框都不刷新。
//   另：带 id>=0 的推送（控制器可能回 id=0）也必须走推送分支，不能被丢弃。
// ===========================================================================
static void testPushDispatchesUiSignals()
{
    qInfo() << "\n=== 用例 4: 推送翻译为 UI 信号（状态刷新） ===";
    TestServer server;
    ArmWebSocket arm;

    check(arm.connectToArm(QStringLiteral("127.0.0.1"), server.port()),
          "connectToArm 成功");
    pump(800);   // 等 connectToArm 内置的 subscribeAll 跑完

    QString gotArm;
    QJsonObject gotArmDb;
    int armCount = 0, projCount = 0, listCount = 0;
    QJsonObject gotProjDb;

    QObject::connect(&arm, &ArmWebSocket::robotStatusReceived,
                     [&](const QString &name, const QJsonObject &db) {
                         gotArm = name; gotArmDb = db; ++armCount;
                     });
    QObject::connect(&arm, &ArmWebSocket::projectStatusReceived,
                     [&](const QJsonObject &db) { gotProjDb = db; ++projCount; });
    QObject::connect(&arm, &ArmWebSocket::projectListReceived,
                     [&](const QVariantList &) { ++listCount; });

    // 机械臂状态推送（带 id=0，模拟控制器推送夹带非负 id）
    QJsonObject armDb;
    armDb.insert("enabled", true);
    QJsonArray joints;
    for (int i = 0; i < 6; ++i) joints.append(0.5);
    armDb.insert("joints", joints);
    armDb.insert("cartesian", QJsonObject{{"x", 0.1}, {"y", 0.2}, {"z", 0.3},
                                         {"a", 0.0}, {"b", 0.0}, {"c", 0.0}});
    server.push(QStringLiteral("robot/LeftArm/status"), armDb);
    pump(400);

    check(armCount == 1,
          QStringLiteral("robotStatusReceived 应被触发（实际 %1 次）").arg(armCount));
    check(gotArm == QStringLiteral("LeftArm"),
          QStringLiteral("臂名解析正确（实际 '%1'）").arg(gotArm));
    check(gotArmDb.value(QStringLiteral("joints")).toArray().size() == 6,
          "关节数据完整传递");
    check(gotArmDb.value(QStringLiteral("cartesian")).toObject()
              .value(QStringLiteral("z")).toDouble() > 0.29,
          "位姿数据完整传递");

    // 工程状态推送
    QJsonObject projDb;
    projDb.insert("state", QStringLiteral("running"));
    projDb.insert("project", QStringLiteral("demo"));
    server.push(QStringLiteral("project/status"), projDb);
    pump(400);

    check(projCount == 1,
          QStringLiteral("projectStatusReceived 应被触发（实际 %1 次）").arg(projCount));
    check(gotProjDb.value(QStringLiteral("state")).toString()
              == QStringLiteral("running"),
          "工程状态数据传递正确");

    // listProjects() 必须 emit，否则工程下拉框永远为空
    arm.listProjects();
    pump(200);
    check(listCount >= 1,
          QStringLiteral("projectListReceived 应被触发（实际 %1 次）").arg(listCount));
}

// ===========================================================================
// 用例 5：臂名必须取自 robot/list
//   文档 §2.1：订阅主题要用真实机器人名，样例 "LeftArm"/"arm0" 是占位。
//   写错主题 -> 整条订阅被拒（25004）-> 连接成功但永远收不到推送，
//   正是「连接成功后状态不刷新」的根因。
// ===========================================================================
static void testArmNamesResolvedFromRobotList()
{
    qInfo() << "\n=== 用例 5: 臂名取自 robot/list ===";
    TestServer server;
    // 控制器真实臂名是 arm0/arm1，而配置里写的是 LeftArm/RightArm
    server.setRealArmNames({QStringLiteral("arm0"), QStringLiteral("arm1")});

    ArmWebSocket arm;
    arm.setArmNames(QStringLiteral("LeftArm"), QStringLiteral("RightArm"));

    QString resolvedL, resolvedR;
    QObject::connect(&arm, &ArmWebSocket::armNamesResolved,
                     [&](const QString &l, const QString &r) { resolvedL = l; resolvedR = r; });

    QString errText_;
    QObject::connect(&arm, &ArmWebSocket::errorOccurred,
                     [&](const QString &e) { errText_ = e; });

    check(arm.connectToArm(QStringLiteral("127.0.0.1"), server.port()),
          "connectToArm 成功");
    pump(1500);   // 等 singleShot(500) -> subscribeAll -> resolveArmNames

    check(resolvedL == QStringLiteral("arm0"),
          QStringLiteral("左臂名应解析为 arm0（实际 '%1'）").arg(resolvedL));
    check(resolvedR == QStringLiteral("arm1"),
          QStringLiteral("右臂名应解析为 arm1（实际 '%1'）").arg(resolvedR));
    check(arm.leftArmName() == QStringLiteral("arm0"),
          "内部 m_leftArm 已同步为真实臂名");
    check(errText_.isEmpty(),
          QStringLiteral("订阅不应被拒（实际错误: %1）").arg(errText_));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testLateResponseAfterTimeout();
    testPollDoesNotReenter();
    testDisconnectedReturnsImmediately();
    testPushDispatchesUiSignals();
    testArmNamesResolvedFromRobotList();

    qInfo() << "\n========================================";
    if (g_failures == 0)
        qInfo() << "全部通过";
    else
        qWarning() << QStringLiteral("失败 %1 项").arg(g_failures);
    qInfo() << "========================================";
    return g_failures == 0 ? 0 : 1;
}