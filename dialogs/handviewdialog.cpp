#include "dialogs/handviewdialog.h"
#include "communication/revo2hand.h"
#include "dialogs/dialogutil.h"
#include "core/qssmanager.h"

#include <QAbstractSlider>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QtConcurrent>
#include <QVBoxLayout>
#include <QMutex>

namespace {
QString fmtHex(const QByteArray &ba)
{
    QString out;
    for (uint8_t b : ba) out += QString::asprintf("%02X ", b);
    return out.trimmed();
}

QString sideCn(const QString &side)
{
    return (side == "left") ? QStringLiteral("左手") : QStringLiteral("右手");
}
}

// =====================================================================
// Tab 1: MicroCard — 单个 DOF 微调卡
// =====================================================================
class MicroCard : public QFrame
{
    Q_OBJECT
public:
    MicroCard(int dofIdx, QWidget *parent = nullptr);

    void setCurDeg(float deg);
    float getDeg() const;
    void setEnabled(bool enabled);

signals:
    void sendRequested(int dofIdx, float deg);

private slots:
    void onSendSliderChanged(int valTenths);
    void onSendSpinChanged(double deg);
    void onSendReleased();

private:
    void syncSendTo(double deg);
    void syncCurFrom(float deg);

    int m_dofIdx;
    bool m_touched = false;
    bool m_suppress = false;

    QSlider *m_sendSlider = nullptr;
    QDoubleSpinBox *m_sendSpin = nullptr;
    QSlider *m_curSlider = nullptr;
    QLabel *m_curLabel = nullptr;

    int m_maxDeg = 81; // will be set from DOF_INFO
    double m_lastCurDeg = 0.0;
};

// =====================================================================
// Tab 1: FingerMicroTab — 6 DOF 微调 Tab
// =====================================================================
class FingerMicroTab : public QWidget
{
    Q_OBJECT
public:
    explicit FingerMicroTab(Revo2HandClient *client, QWidget *parent = nullptr);
    ~FingerMicroTab();

signals:
    void stateUpdated(); // emitted on main thread after state changes

private slots:
    void onCardSendRequested(int dofIdx, float deg);
    void onReadClicked();
    void onLiveToggled(bool checked);
    void onViewPollTick();
    void onStateReceived(const HandState &state);

private:
    void applyState(const HandState &st);
    void threadWriteDof(int dofIdx, float deg, int timeMs);
    void afterWrite(int dofIdx, int raw, const QString &err);
    void threadReadOnce();
    void afterRead(const QString &err);

    Revo2HandClient *m_client = nullptr;
    QList<MicroCard *> m_cards;
    QComboBox *m_cmbBus = nullptr;
    QSpinBox *m_spinSid = nullptr;
    QSpinBox *m_spinTime = nullptr;
    QPushButton *m_btnRead = nullptr;
    QCheckBox *m_chkLive = nullptr;
    QLabel *m_lblStat = nullptr;
    QLabel *m_lblCounter = nullptr;
    QTimer *m_pollTimer = nullptr;
    QMutex m_writeMutex;
    bool m_pollInflight = false;
};

// =====================================================================
// Tab 2: PresetTab — 预设手势
// =====================================================================
class PresetTab : public QWidget
{
    Q_OBJECT
public:
    explicit PresetTab(Revo2HandClient *client, QWidget *parent = nullptr);

private slots:
    void onPreset(const QString &name);

private:
    Revo2HandClient *m_client = nullptr;
};

// =====================================================================
// Tab 3: ModbusDebugTab — Modbus RTU 调试
// =====================================================================
class ModbusDebugTab : public QWidget
{
    Q_OBJECT
public:
    explicit ModbusDebugTab(Revo2HandClient *client, QWidget *parent = nullptr);

private slots:
    void onSendClicked();
    void onGenReadFrame();
    void onGenAllFrame();
    void onRefreshStatus();

private:
    void log(const QString &line);
    QString timestamp();

    Revo2HandClient *m_client = nullptr;
    QLabel *m_lblStatus = nullptr;
    QComboBox *m_cmbFc = nullptr;
    QSpinBox *m_spinReg = nullptr;
    QSpinBox *m_spinQty = nullptr;
    QLineEdit *m_txtData = nullptr;
    QTextEdit *m_txtLog = nullptr;
};

// =====================================================================
// Tab 4: ParamConfigTab — 参数配置
// =====================================================================
class ParamConfigTab : public QWidget
{
    Q_OBJECT
public:
    explicit ParamConfigTab(Revo2HandClient *client, QWidget *parent = nullptr);

private slots:
    void onSave();

private:
    Revo2HandClient *m_client = nullptr;
    QSpinBox *m_spinSlaveAddr = nullptr;
    QComboBox *m_cmbEcSlave = nullptr;
    QSpinBox *m_spinTimeMs = nullptr;
};

// =====================================================================
// Tab 5: SequenceEditTab — 关节序列
// =====================================================================
class SequenceEditTab : public QWidget
{
    Q_OBJECT
public:
    explicit SequenceEditTab(Revo2HandClient *client, QWidget *parent = nullptr);

private slots:
    void onAddRow();
    void onDelRow();
    void onClear();
    void onCapture();
    void onPlay();

private:
    Revo2HandClient *m_client = nullptr;
    QTableWidget *m_table = nullptr;
};

// =====================================================================
// MicroCard
// =====================================================================
MicroCard::MicroCard(int dofIdx, QWidget *parent)
    : QFrame(parent), m_dofIdx(dofIdx)
{
    m_maxDeg = dofMaxDeg(dofIdx);

    setObjectName(QStringLiteral("microCard"));   // 样式见 dialogs/handviewdialog.qss

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    // 名称
    QString nameCn = handDofNameCnList().value(dofIdx,
                                           QStringLiteral("DOF%1").arg(dofIdx));
    auto *nameLabel = new QLabel(QStringLiteral("%1\n0~%2°").arg(nameCn).arg(m_maxDeg));
    nameLabel->setProperty("role", QStringLiteral("cardTitle"));
    nameLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(nameLabel);

    // 发送行
    auto *sendRow = new QHBoxLayout();
    sendRow->setSpacing(2);
    sendRow->setContentsMargins(0, 0, 0, 0);
    auto *tag = new QLabel(QStringLiteral("发送"));
    tag->setFixedWidth(28);
    tag->setProperty("role", QStringLiteral("tag"));
    sendRow->addWidget(tag);

    m_sendSlider = new QSlider(Qt::Horizontal);
    m_sendSlider->setMinimum(0);
    m_sendSlider->setMaximum(m_maxDeg * 10);
    m_sendSlider->setValue(0);
    m_sendSlider->setEnabled(true);
    connect(m_sendSlider, &QAbstractSlider::valueChanged,
            this, &MicroCard::onSendSliderChanged);
    connect(m_sendSlider, &QAbstractSlider::sliderReleased,
            this, &MicroCard::onSendReleased);
    sendRow->addWidget(m_sendSlider, 1);

    m_sendSpin = new QDoubleSpinBox();
    m_sendSpin->setDecimals(1);
    m_sendSpin->setMinimum(0);
    m_sendSpin->setMaximum(m_maxDeg);
    m_sendSpin->setSingleStep(0.1);
    m_sendSpin->setValue(0);
    m_sendSpin->setFixedWidth(58);
    m_sendSpin->setEnabled(true);
    connect(m_sendSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MicroCard::onSendSpinChanged);
    connect(m_sendSpin, &QDoubleSpinBox::editingFinished,
            this, &MicroCard::onSendReleased);
    sendRow->addWidget(m_sendSpin);
    layout->addLayout(sendRow);

    // 当前行
    auto *curRow = new QHBoxLayout();
    curRow->setSpacing(2);
    curRow->setContentsMargins(0, 0, 0, 0);
    auto *tag2 = new QLabel(QStringLiteral("当前"));
    tag2->setFixedWidth(28);
    tag2->setProperty("role", QStringLiteral("tag"));
    curRow->addWidget(tag2);

    m_curSlider = new QSlider(Qt::Horizontal);
    m_curSlider->setObjectName(QStringLiteral("curSlider"));   // 只读“当前位置”条，QSS 里配成绿色
    m_curSlider->setMinimum(0);
    m_curSlider->setMaximum(m_maxDeg * 10);
    m_curSlider->setValue(0);
    m_curSlider->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_curSlider->setFocusPolicy(Qt::NoFocus);
    curRow->addWidget(m_curSlider, 1);

    m_curLabel = new QLabel(QStringLiteral("—"));
    m_curLabel->setProperty("role", QStringLiteral("mono"));
    m_curLabel->setFixedWidth(58);
    m_curLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    curRow->addWidget(m_curLabel);
    layout->addLayout(curRow);
}

void MicroCard::syncSendTo(double deg)
{
    m_suppress = true;
    m_sendSpin->setValue(deg);
    m_sendSlider->setValue(int(qRound(deg * 10)));
    m_suppress = false;
}

void MicroCard::syncCurFrom(float deg)
{
    m_suppress = true;
    m_curSlider->setValue(int(qRound(deg * 10)));
    m_curLabel->setText(QString::asprintf("%.1f°", deg));
    m_suppress = false;
}

void MicroCard::onSendSliderChanged(int valTenths)
{
    if (m_suppress) return;
    double deg = valTenths / 10.0;
    m_suppress = true;
    m_sendSpin->setValue(deg);
    m_suppress = false;
    m_touched = true;
}

void MicroCard::onSendSpinChanged(double deg)
{
    if (m_suppress) return;
    m_suppress = true;
    m_sendSlider->setValue(int(qRound(deg * 10)));
    m_suppress = false;
    m_touched = true;
}

void MicroCard::onSendReleased()
{
    emit sendRequested(m_dofIdx, m_sendSpin->value());
}

void MicroCard::setEnabled(bool enabled)
{
    m_sendSlider->setEnabled(enabled);
    m_sendSpin->setEnabled(enabled);
}

void MicroCard::setCurDeg(float deg)
{
    syncCurFrom(deg);
    m_lastCurDeg = deg;
    if (!m_touched) {
        syncSendTo(deg);
    }
}

float MicroCard::getDeg() const
{
    return float(m_sendSpin->value());
}

// =====================================================================
// FingerMicroTab
// =====================================================================
FingerMicroTab::FingerMicroTab(Revo2HandClient *client, QWidget *parent)
    : QWidget(parent), m_client(client)
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(6);

    // 顶部 meta
    auto *meta = new QHBoxLayout();
    meta->setSpacing(10);
    meta->setContentsMargins(0, 0, 0, 0);

    meta->addWidget(new QLabel(QStringLiteral("通道:")));
    m_cmbBus = new QComboBox();
    m_cmbBus->setEditable(true);
    m_cmbBus->addItems({"EtherCAT485L", "EtherCAT485R"});
    m_cmbBus->setCurrentText(client->ec2rsSlave());
    meta->addWidget(m_cmbBus);

    meta->addWidget(new QLabel(QStringLiteral("从站:")));
    m_spinSid = new QSpinBox();
    m_spinSid->setRange(1, 254);
    m_spinSid->setValue(client->modbusAddr());
    meta->addWidget(m_spinSid);

    meta->addWidget(new QLabel(QStringLiteral("时间 ms:")));
    m_spinTime = new QSpinBox();
    m_spinTime->setRange(1, 2000);
    m_spinTime->setValue(WRITE_TIME_MS_DEFAULT);
    meta->addWidget(m_spinTime);

    meta->addStretch(1);

    m_btnRead = new QPushButton(QStringLiteral("读取当前位置"));
    m_btnRead->setProperty("role", QStringLiteral("primary"));
    m_btnRead->setCursor(Qt::PointingHandCursor);
    connect(m_btnRead, &QPushButton::clicked, this, &FingerMicroTab::onReadClicked);
    meta->addWidget(m_btnRead);

    m_chkLive = new QCheckBox(QStringLiteral("实时读取 (200ms)"));
    connect(m_chkLive, &QCheckBox::toggled, this, &FingerMicroTab::onLiveToggled);
    meta->addWidget(m_chkLive);

    layout->addLayout(meta);

    // 6 DOF 卡片网格
    auto *grid = new QGridLayout();
    grid->setSpacing(6);
    m_cards.reserve(DOF_COUNT);
    for (int i = 0; i < DOF_COUNT; ++i) {
        MicroCard *card = new MicroCard(i);
        connect(card, &MicroCard::sendRequested,
                this, &FingerMicroTab::onCardSendRequested);
        m_cards.append(card);
        int r = i / 3, c = i % 3;
        grid->addWidget(card, r, c);
    }
    layout->addLayout(grid);

    // 状态行
    auto *infoRow = new QHBoxLayout();
    m_lblStat = new QLabel(QStringLiteral("先读取当前位置，发送条对齐后再拖动"));
    m_lblStat->setProperty("role", QStringLiteral("hint"));
    infoRow->addWidget(m_lblStat, 1);
    m_lblCounter = new QLabel(QStringLiteral("TX:0 RX:0 ERR:0"));
    m_lblCounter->setProperty("role", QStringLiteral("mono"));
    infoRow->addWidget(m_lblCounter);
    layout->addLayout(infoRow);

    // 注册状态回调
    m_client->registerStateCallback([this](const HandState &st) {
        QTimer::singleShot(0, [this, st]() { onStateReceived(st); });
    });

    // View 弹窗轮询定时器（200ms）
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(200);
    connect(m_pollTimer, &QTimer::timeout, this, &FingerMicroTab::onViewPollTick);
    m_pollTimer->start();

    // 初始化显示
    applyState(m_client->state());
}

FingerMicroTab::~FingerMicroTab()
{
    if (m_pollTimer) m_pollTimer->stop();
}

void FingerMicroTab::applyState(const HandState &st)
{
    for (int i = 0; i < DOF_COUNT && i < m_cards.size(); ++i) {
        m_cards[i]->setCurDeg(rawToDeg(st.positions.value(i, 0), i));
    }
    m_lblCounter->setText(QString::asprintf(
        "TX:%llu RX:%llu ERR:%llu",
        (unsigned long long)m_client->txCount(),
        (unsigned long long)m_client->rxCount(),
        (unsigned long long)m_client->errCount()));
    if (!st.lastError.isEmpty()) {
        m_lblStat->setText(QStringLiteral("⚠ %1").arg(st.lastError));
    } else if (st.inited) {
        if (m_chkLive->isChecked()) {
            m_lblStat->setText(QStringLiteral("● 通道已 init · 实时模式（200ms）"));
        } else {
            m_lblStat->setText(QStringLiteral("● 通道已 init · 后台轮询（200ms）"));
        }
    } else {
        m_lblStat->setText(
            QStringLiteral("未 init · 拖动将自动 init 并写入（请先到主页确保连接）"));
    }
}

void FingerMicroTab::onCardSendRequested(int dofIdx, float deg)
{
    int timeMs = m_spinTime->value();
    QtConcurrent::run([this, dofIdx, deg, timeMs]() {
        threadWriteDof(dofIdx, deg, timeMs);
    });
}

void FingerMicroTab::threadWriteDof(int dofIdx, float deg, int timeMs)
{
    QMutexLocker locker(&m_writeMutex);
    m_client->setWriteTimeMs(timeMs);
    int raw = degToRaw(deg, dofIdx);
    QString err = m_client->writeDof(dofIdx, raw);
    QTimer::singleShot(0, [this, dofIdx, raw, err]() {
        afterWrite(dofIdx, raw, err);
    });
}

void FingerMicroTab::afterWrite(int dofIdx, int raw, const QString &err)
{
    const QString nameCn = handDofNameCnList().value(dofIdx);
    if (!err.isEmpty()) {
        m_lblStat->setText(QString::asprintf(
            "⚠ DOF%d (%s) 失败: %s", dofIdx,
            qPrintable(nameCn), qPrintable(err)));
    } else {
        m_lblStat->setText(QString::asprintf(
            "✓ DOF%d (%s) = %.1f°", dofIdx,
            qPrintable(nameCn), rawToDeg(raw, dofIdx)));
    }
    m_lblCounter->setText(QString::asprintf(
        "TX:%llu RX:%llu ERR:%llu",
        (unsigned long long)m_client->txCount(),
        (unsigned long long)m_client->rxCount(),
        (unsigned long long)m_client->errCount()));
}

void FingerMicroTab::onReadClicked()
{
    QtConcurrent::run([this]() { threadReadOnce(); });
}

void FingerMicroTab::threadReadOnce()
{
    QString err = m_client->readPositions();
    QTimer::singleShot(0, [this, err]() { afterRead(err); });
}

void FingerMicroTab::afterRead(const QString &err)
{
    if (!err.isEmpty()) {
        m_lblStat->setText(QStringLiteral("⚠ 读失败: %1").arg(err));
    } else {
        m_lblStat->setText(QStringLiteral("✓ 当前位置已更新"));
    }
    m_lblCounter->setText(QString::asprintf(
        "TX:%llu RX:%llu ERR:%llu",
        (unsigned long long)m_client->txCount(),
        (unsigned long long)m_client->rxCount(),
        (unsigned long long)m_client->errCount()));
}

void FingerMicroTab::onLiveToggled(bool checked)
{
    if (checked) {
        m_client->setPollInterval(50);
        m_lblStat->setText(QStringLiteral("● 实时模式（50ms 轮询）"));
    } else {
        m_client->setPollInterval(200);
        HandState st = m_client->state();
        if (st.inited) {
            m_lblStat->setText(QStringLiteral("● 通道已 init · 后台轮询（200ms）"));
        } else {
            m_lblStat->setText(QStringLiteral("未 init"));
        }
    }
}

void FingerMicroTab::onViewPollTick()
{
    if (m_pollInflight) return;
    m_pollInflight = true;
    QtConcurrent::run([this]() {
        m_client->readPositions();
        m_pollInflight = false;
    });
}

void FingerMicroTab::onStateReceived(const HandState &st)
{
    applyState(st);
}

// =====================================================================
// PresetTab
// =====================================================================
PresetTab::PresetTab(Revo2HandClient *client, QWidget *parent)
    : QWidget(parent), m_client(client)
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("点击按钮执行预设手势："));
    layout->addWidget(title);

    const QMap<QString, QString> presets = {
        {QStringLiteral("一把抓"),   QStringLiteral("所有 DOF = 1000（满握）")},
        {QStringLiteral("释放"),     QStringLiteral("所有 DOF = 0（张开）")},
        {QStringLiteral("半抓"),     QStringLiteral("所有 DOF = 500")},
        {QStringLiteral("1010"),     QStringLiteral("食指+无名指伸开，其他握紧")},
        {QStringLiteral("两指捏"),   QStringLiteral("拇指弯曲+侧摆，食指半弯")},
        {QStringLiteral("点赞"),     QStringLiteral("拇指全伸，其他全握")},
        {QStringLiteral("指点"),     QStringLiteral("仅食指伸开")},
    };

    for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
        auto *row = new QHBoxLayout();
        row->setSpacing(8);

        QPushButton *btn = new QPushButton(QStringLiteral("✋  %1").arg(it.key()));
        btn->setProperty("role", QStringLiteral("preset"));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setMinimumHeight(36);
        connect(btn, &QPushButton::clicked, [this, name = it.key()]() {
            onPreset(name);
        });
        row->addWidget(btn, 1);

        QLabel *desc = new QLabel(it.value());
        desc->setProperty("role", QStringLiteral("hint"));
        row->addWidget(desc, 2);
        layout->addLayout(row);
    }
    layout->addStretch(1);
}

void PresetTab::onPreset(const QString &name)
{
    m_client->executePreset(name);
}

// =====================================================================
// ModbusDebugTab
// =====================================================================
ModbusDebugTab::ModbusDebugTab(Revo2HandClient *client, QWidget *parent)
    : QWidget(parent), m_client(client)
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(6);

    // 状态
    auto *statusRow = new QHBoxLayout();
    m_lblStatus = new QLabel(QStringLiteral("通道状态: —"));
    m_lblStatus->setProperty("role", QStringLiteral("hint"));
    statusRow->addWidget(m_lblStatus);
    statusRow->addStretch(1);

    QPushButton *btnRefresh = new QPushButton(QStringLiteral("↻ 刷新状态"));
    btnRefresh->setCursor(Qt::PointingHandCursor);
    connect(btnRefresh, &QPushButton::clicked, this, &ModbusDebugTab::onRefreshStatus);
    statusRow->addWidget(btnRefresh);
    layout->addLayout(statusRow);

    // 发送
    auto *sendBox = new QGroupBox(QStringLiteral("发送（Modbus RTU）"));
    auto *sf = new QFormLayout(sendBox);

    m_cmbFc = new QComboBox();
    m_cmbFc->addItems({
        QStringLiteral("0x04 读输入寄存器 (Read Input)"),
        QStringLiteral("0x10 写多寄存器 (Write Multiple)"),
    });
    sf->addRow(QStringLiteral("功能码:"), m_cmbFc);

    m_spinReg = new QSpinBox();
    m_spinReg->setRange(0, 0xFFFF);
    m_spinReg->setValue(REG_READ_POS);
    m_spinReg->setDisplayIntegerBase(16);
    sf->addRow(QStringLiteral("起始寄存器:"), m_spinReg);

    m_spinQty = new QSpinBox();
    m_spinQty->setRange(1, 32);
    m_spinQty->setValue(DOF_COUNT);
    sf->addRow(QStringLiteral("数量:"), m_spinQty);

    m_txtData = new QLineEdit();
    m_txtData->setPlaceholderText(
        QStringLiteral("HEX 字节，空格分隔，例如：00 00 03 E8 00 64"));
    sf->addRow(QStringLiteral("数据(HEX):"), m_txtData);

    auto *quick = new QHBoxLayout();
    QPushButton *btnSend = new QPushButton(QStringLiteral("发送"));
    btnSend->setProperty("role", QStringLiteral("primary"));
    btnSend->setCursor(Qt::PointingHandCursor);
    connect(btnSend, &QPushButton::clicked, this, &ModbusDebugTab::onSendClicked);
    quick->addWidget(btnSend);

    QPushButton *btnGenRead = new QPushButton(QStringLiteral("生成读位置帧"));
    btnGenRead->setCursor(Qt::PointingHandCursor);
    connect(btnGenRead, &QPushButton::clicked, this, &ModbusDebugTab::onGenReadFrame);
    quick->addWidget(btnGenRead);

    QPushButton *btnGenAll = new QPushButton(QStringLiteral("生成多指帧 (1010)"));
    btnGenAll->setCursor(Qt::PointingHandCursor);
    connect(btnGenAll, &QPushButton::clicked, this, &ModbusDebugTab::onGenAllFrame);
    quick->addWidget(btnGenAll);

    QPushButton *btnClear = new QPushButton(QStringLiteral("清空记录"));
    btnClear->setCursor(Qt::PointingHandCursor);
    connect(btnClear, &QPushButton::clicked, [this]() { m_txtLog->clear(); });
    quick->addWidget(btnClear);

    sf->addRow(quick);
    layout->addWidget(sendBox);

    // 收发记录
    auto *logBox = new QGroupBox(QStringLiteral("收发记录"));
    auto *ll = new QVBoxLayout(logBox);
    m_txtLog = new QTextEdit();
    m_txtLog->setReadOnly(true);
    m_txtLog->setFontFamily(QStringLiteral("Consolas, monospace"));
    m_txtLog->setMinimumHeight(180);
    ll->addWidget(m_txtLog);
    layout->addWidget(logBox, 1);
}

void ModbusDebugTab::onRefreshStatus()
{
    QJsonObject resp = m_client->ec2rsGetStatus();
    if (resp.isEmpty()) {
        m_lblStatus->setText(QStringLiteral("通道状态: 无响应"));
        return;
    }
    if (resp.contains("err") && !resp["err"].toString().isEmpty()) {
        m_lblStatus->setText(QStringLiteral("通道状态: 错误 %1").arg(resp["err"].toString()));
        return;
    }
    QJsonObject db = resp["db"].toObject();
    m_lblStatus->setText(QString::asprintf(
        "ready=%d configured=%d busy=%d occupiedBy='%s' rx=%dB",
        db["ready"].toInt(), db["configured"].toInt(), db["busy"].toInt(),
        qPrintable(db["occupiedBy"].toString()),
        db["receivedLength"].toInt()));
}

void ModbusDebugTab::onGenReadFrame()
{
    QByteArray frame = handReadPositionsFrame(m_client->modbusAddr());
    m_txtData->setText(fmtHex(frame));
    m_spinReg->setValue(HAND_REG_READ_POS);
    m_spinQty->setValue(DOF_COUNT);
    m_cmbFc->setCurrentIndex(0);
}

void ModbusDebugTab::onGenAllFrame()
{
    HandState st = m_client->state();
    QList<int> positions;
    if (st.positions.size() == DOF_COUNT) {
        positions = st.positions;
    } else {
        positions = QList<int>() << 0 << 0 << 0 << 0 << 0 << 0;
    }
    QByteArray frame = handWriteAllDofsFrame(m_client->modbusAddr(),
                                             QVector<int>(positions.begin(), positions.end()),
                                             st.writeTimeMs);
    m_txtData->setText(fmtHex(frame));
    m_spinReg->setValue(HAND_REG_WRITE_ALL);
    m_spinQty->setValue(DOF_COUNT * 2);
    m_cmbFc->setCurrentIndex(1);
    log(QStringLiteral("[生成] 多指帧 1010（%1×(pos,time)=%2 寄存器）")
            .arg(DOF_COUNT).arg(DOF_COUNT * 2));
}

void ModbusDebugTab::onSendClicked()
{
    QString dataHex = m_txtData->text().trimmed();
    if (dataHex.isEmpty()) {
        log(QStringLiteral("✗ 数据为空"));
        return;
    }
    QStringList parts = dataHex.split(' ', Qt::SkipEmptyParts);
    QList<uint8_t> dataBytes;
    for (const QString &p : parts) {
        bool ok;
        int v = p.toInt(&ok, 16);
        if (!ok) { log(QStringLiteral("✗ HEX 格式错误: %1").arg(p)); return; }
        dataBytes.append(uint8_t(v & 0xFF));
    }

    int fcIdx = m_cmbFc->currentIndex();
    int reg = m_spinReg->value();
    int qty = m_spinQty->value();
    uint8_t slave = uint8_t(m_client->modbusAddr());
    QByteArray frame;
    int readLen = 0;

    if (fcIdx == 0) {
        // 0x04 读输入寄存器
        frame.append(char(slave));
        frame.append(char(0x04));
        frame.append(char((reg >> 8) & 0xFF));
        frame.append(char(reg & 0xFF));
        frame.append(char((qty >> 8) & 0xFF));
        frame.append(char(qty & 0xFF));
        frame.append(handCrcBytes(frame));
        readLen = 5 + 2 * qty;
    } else {
        // 0x10 写多个保持寄存器
        QByteArray payload;
        payload.append(char((reg >> 8) & 0xFF));
        payload.append(char(reg & 0xFF));
        payload.append(char((qty >> 8) & 0xFF));
        payload.append(char(qty & 0xFF));
        payload.append(char(dataBytes.size()));
        for (uint8_t b : dataBytes) payload.append(char(b));
        frame.append(char(slave));
        frame.append(char(0x10));
        frame.append(payload);
        frame.append(handCrcBytes(frame));
        readLen = 8;
    }

    log(QStringLiteral("[%1] TX → %2").arg(timestamp()).arg(fmtHex(frame)));

    const QString rxHex = m_client->rawExchange(frame, readLen);
    if (rxHex.isEmpty()) {
        log(QStringLiteral("[%1] ✗ 无应答").arg(timestamp()));
        return;
    }
    // rawExchange 返回十六进制串；失败时以中文错误前缀返回
    if (rxHex.startsWith(QLatin1String("写")) || rxHex.startsWith(QLatin1String("读"))
        || rxHex.startsWith(QLatin1String("未连接"))) {
        log(QStringLiteral("[%1] ✗ %2").arg(timestamp()).arg(rxHex));
        return;
    }

    const QByteArray rx =
        QByteArray::fromHex(QByteArray(rxHex.toLatin1()).replace(' ', ""));
    log(QStringLiteral("[%1] RX ← %2  CRC=%3")
            .arg(timestamp()).arg(rxHex)
            .arg(handVerifyCrc(rx) ? QStringLiteral("OK") : QStringLiteral("FAIL")));
}

void ModbusDebugTab::log(const QString &line)
{
    m_txtLog->append(line);
}

QString ModbusDebugTab::timestamp()
{
    auto now = QDateTime::currentDateTime();
    return now.toString(QStringLiteral("HH:mm:ss.zzz"));
}

// =====================================================================
// ParamConfigTab
// =====================================================================
ParamConfigTab::ParamConfigTab(Revo2HandClient *client, QWidget *parent)
    : QWidget(parent), m_client(client)
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(6);

    // 串口参数
    auto *uartBox = new QGroupBox(QStringLiteral("串口参数"));
    auto *uf = new QFormLayout(uartBox);

    m_spinSlaveAddr = new QSpinBox();
    m_spinSlaveAddr->setRange(1, 254);
    m_spinSlaveAddr->setValue(client->modbusAddr());
    uf->addRow(QStringLiteral("从站地址 (1~254):"), m_spinSlaveAddr);

    m_cmbEcSlave = new QComboBox();
    m_cmbEcSlave->setEditable(true);
    m_cmbEcSlave->addItems({"EtherCAT485L", "EtherCAT485R"});
    m_cmbEcSlave->setCurrentText(client->ec2rsSlave());
    uf->addRow(QStringLiteral("ec2rs485 通道:"), m_cmbEcSlave);

    m_spinTimeMs = new QSpinBox();
    m_spinTimeMs->setRange(1, 2000);
    m_spinTimeMs->setValue(client->writeTimeMs());
    uf->addRow(QStringLiteral("写时间 ms (1~2000):"), m_spinTimeMs);

    layout->addWidget(uartBox);

    // 寄存器
    auto *regBox = new QGroupBox(QStringLiteral("寄存器（Revo2 协议固定）"));
    auto *rf = new QFormLayout(regBox);
    struct RegDef { const char *label; int val; };
    RegDef regs[] = {
        {"读当前位置 (0x07D0)", REG_READ_POS},
        {"写单个 DOF (0x041C)", REG_WRITE_DOF},
        {"写所有 DOF (0x03F2)", REG_WRITE_ALL},
    };
    for (auto &rd : regs) {
        QSpinBox *sp = new QSpinBox();
        sp->setRange(0, 0xFFFF);
        sp->setValue(rd.val);
        sp->setDisplayIntegerBase(16);
        sp->setReadOnly(true);
        rf->addRow(QString::fromUtf8(rd.label) + ":", sp);
    }
    layout->addWidget(regBox);

    layout->addStretch(1);

    // 保存
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    QPushButton *btnSave = new QPushButton(QStringLiteral("保存（立即生效）"));
    btnSave->setProperty("role", QStringLiteral("success"));
    btnSave->setCursor(Qt::PointingHandCursor);
    connect(btnSave, &QPushButton::clicked, this, &ParamConfigTab::onSave);
    btnRow->addWidget(btnSave);
    layout->addLayout(btnRow);
}

void ParamConfigTab::onSave()
{
    m_client->setModbusAddr(m_spinSlaveAddr->value());
    m_client->setEc2rsSlave(m_cmbEcSlave->currentText().trimmed());
    m_client->setWriteTimeMs(m_spinTimeMs->value());
    m_client->resetInited();
    QMessageBox::information(this, QStringLiteral("已保存"),
        QStringLiteral("参数已生效。下次写/读时若通道变化会重新 init。"));
}

// =====================================================================
// SequenceEditTab
// =====================================================================
SequenceEditTab::SequenceEditTab(Revo2HandClient *client, QWidget *parent)
    : QWidget(parent), m_client(client)
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(6);

    m_table = new QTableWidget();
    QStringList cols;
    cols << "#";
    for (int i = 0; i < DOF_COUNT; ++i) {
        cols << QStringLiteral("%1.%2").arg(handDofNameList().value(i)).arg(i + 1);
    }
    cols << QStringLiteral("时间(ms)");
    m_table->setColumnCount(cols.size());
    m_table->setHorizontalHeaderLabels(cols);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table);

    auto *btnRow = new QHBoxLayout();

    QPushButton *btnAdd = new QPushButton(QStringLiteral("+ 添加步骤"));
    btnAdd->setCursor(Qt::PointingHandCursor);
    connect(btnAdd, &QPushButton::clicked, this, &SequenceEditTab::onAddRow);
    btnRow->addWidget(btnAdd);

    QPushButton *btnDel = new QPushButton(QStringLiteral("- 删除选中"));
    btnDel->setProperty("role", QStringLiteral("danger"));
    btnDel->setCursor(Qt::PointingHandCursor);
    connect(btnDel, &QPushButton::clicked, this, &SequenceEditTab::onDelRow);
    btnRow->addWidget(btnDel);

    QPushButton *btnClear = new QPushButton(QStringLiteral("清空"));
    btnClear->setProperty("role", QStringLiteral("danger"));
    btnClear->setCursor(Qt::PointingHandCursor);
    connect(btnClear, &QPushButton::clicked, this, &SequenceEditTab::onClear);
    btnRow->addWidget(btnClear);

    btnRow->addStretch(1);

    QPushButton *btnCapture = new QPushButton(QStringLiteral("捕获当前位置"));
    btnCapture->setProperty("role", QStringLiteral("secondary"));
    btnCapture->setCursor(Qt::PointingHandCursor);
    connect(btnCapture, &QPushButton::clicked, this, &SequenceEditTab::onCapture);
    btnRow->addWidget(btnCapture);

    QPushButton *btnPlay = new QPushButton(QStringLiteral("▶ 播放"));
    btnPlay->setProperty("role", QStringLiteral("success"));
    btnPlay->setCursor(Qt::PointingHandCursor);
    connect(btnPlay, &QPushButton::clicked, this, &SequenceEditTab::onPlay);
    btnRow->addWidget(btnPlay);

    layout->addLayout(btnRow);

    onAddRow(); // 添加第一行
}

void SequenceEditTab::onAddRow()
{
    int r = m_table->rowCount();
    m_table->insertRow(r);
    m_table->setItem(r, 0, new QTableWidgetItem(QString::number(r + 1)));
    m_table->item(r, 0)->setFlags(m_table->item(r, 0)->flags() & ~Qt::ItemIsEditable);
    for (int c = 1; c <= DOF_COUNT; ++c) {
        m_table->setItem(r, c, new QTableWidgetItem(QStringLiteral("0")));
    }
    m_table->setItem(r, DOF_COUNT + 1, new QTableWidgetItem(QStringLiteral("100")));
}

void SequenceEditTab::onDelRow()
{
    int r = m_table->currentRow();
    if (r >= 0) m_table->removeRow(r);
}

void SequenceEditTab::onClear()
{
    m_table->setRowCount(0);
}

void SequenceEditTab::onCapture()
{
    HandState st = m_client->state();
    int r = m_table->rowCount() - 1;
    if (r < 0) { onAddRow(); r = 0; }
    for (int c = 0; c < DOF_COUNT && c < st.positions.size(); ++c) {
        m_table->item(r, c + 1)->setText(QString::number(st.positions.at(c)));
    }
}

void SequenceEditTab::onPlay()
{
    HandState st = m_client->state();
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QList<int> positions;
        for (int c = 1; c <= DOF_COUNT; ++c) {
            QString txt = m_table->item(r, c) ? m_table->item(r, c)->text() : QStringLiteral("0");
            bool ok;
            int v = txt.toInt(&ok);
            positions.append(ok ? qBound(0, v, 1000) : 0);
        }
        QString durTxt = m_table->item(r, DOF_COUNT + 1)
                             ? m_table->item(r, DOF_COUNT + 1)->text()
                             : QStringLiteral("100");
        bool ok;
        int durationMs = durTxt.toInt(&ok);
        if (!ok) durationMs = 100;
        durationMs = qMax(10, durationMs);

        // 延迟播放
        QTimer::singleShot(0, [this, positions]() {
            m_client->writeAll(positions);
        });
        QThread::sleep(uint(durationMs / 1000));
    }
}

// =====================================================================
// HandViewDialog
// =====================================================================
HandViewDialog::HandViewDialog(const QString &side,
                               Revo2HandClient *client,
                               QWidget *parent)
    : QDialog(parent), m_side(side), m_client(client)
{
    setWindowTitle(QStringLiteral("🤖 Revo2 View — %1  6 自由度微调")
                       .arg(sideCn(side)));
    resize(900, 620);
    buildUi();
    QssManager::instance()->loadStyleForWidget(this, QStringLiteral("dialogs/handviewdialog"));
    DialogUtil::adaptForMobile(this);
}

HandViewDialog::~HandViewDialog() = default;

void HandViewDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *title = new QLabel(QString::asprintf(
        "🤖 Revo2 灵巧手 — %s · 从站 %d · %s",
        qPrintable(sideCn(m_side)),
        m_client->modbusAddr(),
        qPrintable(m_client->ec2rsSlave())));
    title->setObjectName(QStringLiteral("dialogTitle"));
    layout->addWidget(title);

    m_tabs = new QTabWidget();
    m_tabs->addTab(new FingerMicroTab(m_client), QStringLiteral("✋ 6 自由度微调"));
    m_tabs->addTab(new PresetTab(m_client), QStringLiteral("⭐ 预设手势"));
    m_tabs->addTab(new ModbusDebugTab(m_client), QStringLiteral("🔧 Modbus 调试"));
    m_tabs->addTab(new ParamConfigTab(m_client), QStringLiteral("⚙ 参数配置"));
    m_tabs->addTab(new SequenceEditTab(m_client), QStringLiteral("▶ 关节序列"));
    layout->addWidget(m_tabs, 1);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    QPushButton *btnClose = new QPushButton(QStringLiteral("关闭"));
    btnClose->setProperty("role", QStringLiteral("close"));
    btnClose->setCursor(Qt::PointingHandCursor);
    // 通过 show() 显示且设置了 WA_DeleteOnClose，用 close() 才能真正关闭并释放
    connect(btnClose, &QPushButton::clicked, this, &QWidget::close);
    btnRow->addWidget(btnClose);
    layout->addLayout(btnRow);
}

#include "handviewdialog.moc"
