#include "mapinfodialog.h"
#include "core/qssmanager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QGraphicsScene>
#include <QStyleOption>
#include <QtMath>

using std::nullopt;

// =====================================================================
// LaserScanItem
// =====================================================================
LaserScanItem::LaserScanItem(QGraphicsItem *parent)
    : QGraphicsItem(parent)
{
    setZValue(8);
}

void LaserScanItem::setData(const QVariantList &lasers)
{
    m_beams.clear();
    m_polys.clear();

    for (const QVariant &lv : lasers) {
        QVariantMap lm = lv.toMap();
        QVariantMap install = lm.value("install_info").toMap();

        double instX   = install.value("x").toDouble();
        double instY   = install.value("y").toDouble();
        double instYaw = qDegreesToRadians(install.value("yaw").toDouble());

        QVariantList beams = lm.value("beams").toList();
        for (const QVariant &bv : beams) {
            QVariantMap b = bv.toMap();
            if (!b.value("valid", true).toBool()) continue;
            double dist = b.value("dist", 0.0).toDouble();
            if (dist <= 0) continue;

            Beam beam;
            beam.angle       = qDegreesToRadians(b.value("angle", 0.0).toDouble());
            beam.dist        = dist;
            beam.valid       = b.value("valid", true).toBool();
            beam.installX    = instX;
            beam.installY    = instY;
            beam.installYaw  = instYaw;
            m_beams.push_back(beam);
        }
    }
    update();
}

void LaserScanItem::setLaserAngle(double deg)
{
    m_laserAngle = deg;
    update();
}

QRectF LaserScanItem::boundingRect() const
{
    return QRectF(-30 * SCALE_DISTANCE, -30 * SCALE_DISTANCE,
                  60 * SCALE_DISTANCE, 60 * SCALE_DISTANCE);
}

void LaserScanItem::rebuildPolys()
{
    m_polys.clear();
    if (m_beams.empty()) return;

    // 按 install 分组
    struct Key { double x, y, yaw; };
    std::map<std::tuple<double,double,double>, std::vector<Beam*>> groups;
    for (Beam &b : m_beams) {
        auto key = std::make_tuple(b.installX, b.installY, b.installYaw);
        groups[key].push_back(&b);
    }

    for (auto &[key, group] : groups) {
        auto [instX, instY, instYaw] = key;
        QPolygonF poly;
        int gap = 0;

        // 按角度排序
        std::sort(group.begin(), group.end(),
                  [](const Beam *a, const Beam *b){ return a->angle < b->angle; });

        for (Beam *b : group) {
            double a = b->angle + b->installYaw;
            double lx = b->dist * std::cos(a) + b->installX;
            double ly = b->dist * std::sin(a) + b->installY;

            if (b->valid && b->dist > 0) {
                poly.append(QPointF(lx * SCALE_DISTANCE, ly * SCALE_DISTANCE));
                gap = 0;
            } else {
                gap++;
                if (gap > 6) {
                    poly.append(QPointF(instX * SCALE_DISTANCE, instY * SCALE_DISTANCE));
                    if (poly.size() >= 3)
                        m_polys.append(poly);
                    poly.clear();
                    gap = 0;
                }
            }
        }

        if (!poly.empty()) {
            poly.append(QPointF(instX * SCALE_DISTANCE, instY * SCALE_DISTANCE));
            if (poly.size() >= 3)
                m_polys.append(poly);
        }
    }
}

void LaserScanItem::paint(QPainter *painter,
                         const QStyleOptionGraphicsItem *option,
                         QWidget *widget)
{
    Q_UNUSED(option); Q_UNUSED(widget);
    if (m_beams.empty()) return;
    if (m_polys.empty()) rebuildPolys();

    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->rotate(m_laserAngle);

    // 多边形填充（浅青色）
    QColor fillColor(217, 255, 255, 120);
    painter->setPen(QPen(fillColor, 1));
    painter->setBrush(QBrush(fillColor));
    for (const QPolygonF &poly : m_polys) {
        painter->drawPolygon(poly, Qt::OddEvenFill);
    }

    // 绿色小圆点
    QColor dotColor(0, 200, 0, 200);
    painter->setPen(QPen(dotColor, 1));
    painter->setBrush(QBrush(dotColor));
    for (const Beam &b : m_beams) {
        if (!b.valid || b.dist <= 0) continue;
        double a = b.angle + b.installYaw;
        double lx = b.dist * std::cos(a) + b.installX;
        double ly = b.dist * std::sin(a) + b.installY;
        painter->drawEllipse(QPointF(lx * SCALE_DISTANCE, ly * SCALE_DISTANCE), 3, 3);
    }
}

// =====================================================================
// LocationMarkItem
// =====================================================================
LocationMarkItem::LocationMarkItem(const QString &name, double angleDeg, QGraphicsItem *parent)
    : QGraphicsItem(parent), m_name(name), m_lmAngle(angleDeg)
{
    setZValue(2);
    setAcceptHoverEvents(true);
}

QString LocationMarkItem::labelOffsetY() const
{
    return QString::number(-(28 * SCALE_LM + 8));
}

void LocationMarkItem::setLmAngle(double deg)
{
    m_lmAngle = deg;
    update();
}

QRectF LocationMarkItem::boundingRect() const
{
    double s = 49 * SCALE_LM;
    return QRectF(-s, -s, 2 * s, 2 * s);
}

void LocationMarkItem::paint(QPainter *painter,
                             const QStyleOptionGraphicsItem *option,
                             QWidget *widget)
{
    Q_UNUSED(widget);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->rotate(m_lmAngle);

    // 站体矩形（80×56，填充浅橙色）
    double w = 40 * SCALE_LM;
    double h = 28 * SCALE_LM;

    QColor fillColor(245, 179, 138, 200);
    if (option->state & QStyle::State_MouseOver)
        fillColor = fillColor.lighter(120);

    QColor boundColor(0, 128, 0, 200);
    QPen pen(boundColor, 2, Qt::DotLine);
    painter->setPen(pen);
    painter->setBrush(QBrush(fillColor));
    painter->drawRect(QRectF(-w, -h, 2 * w, 2 * h));

    // 朝向三角（红色）
    QColor dirColor(200, 0, 0, 150);
    QPolygonF tri;
    tri << QPointF( 25 * SCALE_LM,  8 * SCALE_LM)
        << QPointF( 25 * SCALE_LM, -8 * SCALE_LM)
        << QPointF( 35 * SCALE_LM,  0);
    painter->setPen(QPen(dirColor));
    painter->setBrush(QBrush(dirColor));
    painter->drawPolygon(tri);

    // 转弯圆（淡橙，虚线）
    QPen cirPen(QColor(245, 184, 148, 80));
    cirPen.setStyle(Qt::DotLine);
    painter->setPen(cirPen);
    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(QPointF(0, 0), 49 * SCALE_LM, 49 * SCALE_LM);

    // 中心十字
    QPen cp(QColor(200, 0, 0, 100));
    cp.setWidth(5);
    painter->setPen(cp);
    painter->setBrush(QBrush(QColor(200, 0, 0, 100)));
    painter->drawLine(QPointF(-10 * SCALE_LM, 0), QPointF(10 * SCALE_LM, 0));
    painter->drawLine(QPointF(0, -10 * SCALE_LM), QPointF(0, 10 * SCALE_LM));
}

// =====================================================================
// CurrentAGVItem
// =====================================================================
CurrentAGVItem::CurrentAGVItem(QGraphicsItem *parent)
    : QGraphicsItem(parent)
{
    setZValue(3);
    setAcceptHoverEvents(true);
}

void CurrentAGVItem::setAgvAngle(double deg)
{
    m_agvAngle = deg;
    update();
}

QRectF CurrentAGVItem::boundingRect() const
{
    return QRectF(-49 * SCALE_AGV, -49 * SCALE_AGV,
                  98 * SCALE_AGV, 98 * SCALE_AGV);
}

void CurrentAGVItem::paint(QPainter *painter,
                           const QStyleOptionGraphicsItem *option,
                           QWidget *widget)
{
    Q_UNUSED(widget);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->rotate(m_agvAngle);

    // 车体矩形（半透明绿）
    double w = 40 * SCALE_AGV;
    double h = 28 * SCALE_AGV;
    QColor fillColor(0, 128, 0, 100);
    QColor frameColor(0, 128, 0, 200);

    QPen pen(frameColor, 2, Qt::DashLine);
    painter->setPen(pen);
    painter->setBrush(QBrush(fillColor));
    painter->drawRect(QRectF(-w, -h, 2 * w, 2 * h));

    // 朝向三角
    QColor dirColor(200, 0, 0, 150);
    QPolygonF tri;
    tri << QPointF( 25 * SCALE_AGV,  8 * SCALE_AGV)
        << QPointF( 25 * SCALE_AGV, -8 * SCALE_AGV)
        << QPointF( 35 * SCALE_AGV,  0);
    painter->setPen(QPen(dirColor));
    painter->setBrush(QBrush(dirColor));
    painter->drawPolygon(tri);

    // 转弯圆（淡蓝，虚线）
    QPen cirPen(QColor(0, 0, 150, 80));
    cirPen.setStyle(Qt::DotLine);
    painter->setPen(cirPen);
    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(QPointF(0, 0), 49 * SCALE_AGV, 49 * SCALE_AGV);

    // 中心小红点
    QPen cp(QColor(200, 0, 0, 150));
    cp.setWidth(2);
    painter->setPen(cp);
    painter->setBrush(QBrush(QColor(200, 0, 0, 150)));
    painter->drawEllipse(QPointF(0, 0), 2 * SCALE_AGV, 2 * SCALE_AGV);
}

// =====================================================================
// CoordinateAxisItem
// =====================================================================
double CoordinateAxisItem::scaleAxis = 1.0;

CoordinateAxisItem::CoordinateAxisItem(QGraphicsItem *parent)
    : QGraphicsItem(parent)
{
    setZValue(6);

    // 文字标签作为子 item
    struct LabelInfo { QString text; QColor color; double x, y; };
    QList<LabelInfo> labels = {
        { QStringLiteral("0,0"), QColor(255, 0, 0),   -10, -10 },
        { QStringLiteral("x"),   QColor(255, 0, 0),    60,  -10 },
        { QStringLiteral("y"),   QColor(0,   87, 0),  -10,  60 },
    };

    for (const LabelInfo &info : labels) {
        QGraphicsTextItem *t = new QGraphicsTextItem(info.text, this);
        t->setZValue(7);
        t->setDefaultTextColor(info.color);
        QFont font = t->font();
        font.setFamily("Microsoft YaHei");
        font.setPointSize(6);
        t->setFont(font);
        t->setPos(info.x, info.y);
        // 反转 view Y 翻转
        t->setTransform(QTransform().scale(-1, 1).rotate(-180));
    }
}

QRectF CoordinateAxisItem::boundingRect() const
{
    return QRectF(-30, -80, 110, 160);
}

void CoordinateAxisItem::paint(QPainter *painter,
                               const QStyleOptionGraphicsItem *option,
                               QWidget *widget)
{
    Q_UNUSED(option); Q_UNUSED(widget);
    double s = scaleAxis;
    if (s > 0) painter->scale(1.0 / s, 1.0 / s);

    // X 轴（红）
    QPen penX(QColor(255, 0, 0), 1, Qt::SolidLine);
    painter->setPen(penX);
    painter->drawLine(QPointF(0, 0), QPointF(60, 0));
    painter->setBrush(QBrush(QColor(255, 0, 0)));
    painter->drawPolygon(QPolygonF() << QPointF(60, 5) << QPointF(60, -5) << QPointF(65, 0));

    // Y 轴（绿）
    QPen penY(QColor(0, 87, 0), 1, Qt::SolidLine);
    painter->setPen(penY);
    painter->drawLine(QPointF(0, 0), QPointF(0, 60));
    painter->setBrush(QBrush(QColor(0, 87, 0)));
    painter->drawPolygon(QPolygonF() << QPointF(-5, 60) << QPointF(5, 60) << QPointF(0, 65));

    // 中心点
    QPen penC(QColor(255, 0, 0), 2);
    painter->setPen(penC);
    painter->setBrush(QBrush(QColor(255, 0, 0)));
    painter->drawEllipse(QPointF(0, 0), 4, 4);
}

// =====================================================================
// MapView
// =====================================================================
MapView::MapView(QGraphicsScene *scene, QWidget *parent)
    : QGraphicsView(scene, parent)
{
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorUnderMouse);
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    scale(1, -1); // 翻 Y，与 SeerAgv 一致
}

void MapView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const double step = 1.15;
        if (event->angleDelta().y() > 0) {
            scale(step, step);
        } else {
            scale(1.0 / step, 1.0 / step);
        }
        event->accept();
    } else {
        QGraphicsView::wheelEvent(event);
    }
}

// =====================================================================
// MapInfoDialog
// =====================================================================
MapInfoDialog::MapInfoDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("地图绘制"));
    setMinimumSize(1000, 700);
    initUi();

    // 加载对话框专用样式
    QssManager::instance()->loadStyleForWidget(this, QStringLiteral("dialogs/mapinfodialog"));
}

void MapInfoDialog::initUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(8);

    // ---- 顶部控制条 ----
    QGridLayout *top = new QGridLayout();
    top->setHorizontalSpacing(8);
    top->setVerticalSpacing(4);

    QLabel *lblMapTitle = new QLabel(QStringLiteral("地图:"));
    lblMapTitle->setProperty("role", "title");
    top->addWidget(lblMapTitle, 0, 0);

    m_lblMap = new QLabel(QStringLiteral("-"));
    m_lblMap->setProperty("role", "mapName");
    top->addWidget(m_lblMap, 0, 1);

    m_lblPoints = new QLabel(QStringLiteral("普通点: 0  |  站点: 0"));
    m_lblPoints->setProperty("role", "points");
    top->addWidget(m_lblPoints, 0, 2);

    QLabel *lblHint = new QLabel(QStringLiteral("Ctrl+滚轮: 缩放  |  拖拽: 平移"));
    lblHint->setProperty("role", "hint");
    top->addWidget(lblHint, 0, 3);

    // 缩放 slider
    QLabel *lblZoom = new QLabel(QStringLiteral("缩放"));
    lblZoom->setProperty("role", "zoomLabel");
    top->addWidget(lblZoom, 0, 4);

    m_zoomSlider = new QSlider(Qt::Horizontal);
    m_zoomSlider->setMinimum(0);
    m_zoomSlider->setMaximum(100);
    m_zoomSlider->setValue(30);
    m_zoomSlider->setFixedWidth(140);
    m_zoomSlider->setToolTip(QStringLiteral("缩放（左=缩小，右=放大）"));
    connect(m_zoomSlider, &QSlider::valueChanged, this, &MapInfoDialog::onApplyZoom);
    top->addWidget(m_zoomSlider, 0, 5);

    QPushButton *btnFit = new QPushButton(QStringLiteral("自适应"));
    btnFit->setProperty("role", "primary");
    connect(btnFit, &QPushButton::clicked, this, &MapInfoDialog::onFit);
    top->addWidget(btnFit, 0, 6);

    QPushButton *btnRefresh = new QPushButton(QStringLiteral("刷新"));
    btnRefresh->setProperty("role", "success");
    connect(btnRefresh, &QPushButton::clicked, this, &MapInfoDialog::onRefresh);
    top->addWidget(btnRefresh, 0, 7);

    QPushButton *btnClose = new QPushButton(QStringLiteral("关闭"));
    btnClose->setProperty("role", "close");
    // 注意：MapInfoDialog 是通过 show() 显示（非 exec()），因此
    // QDialog::accept()/done() 在 show() 模式下不会隐藏窗口，
    // 表现为"点关闭关不掉"。这里直接调用 close() 关闭窗口。
    connect(btnClose, &QPushButton::clicked, this, &QWidget::close);
    top->addWidget(btnClose, 0, 8);

    // 设置列拉伸
    top->setColumnStretch(1, 0);
    top->setColumnStretch(2, 0);
    top->setColumnStretch(3, 1);

    mainLayout->addLayout(top);

    // ---- 图形视图 ----
    m_scene = new QGraphicsScene(this);
    m_scene->setBackgroundBrush(QBrush(QColor(250, 250, 250)));
    m_view = new MapView(m_scene, this);
    m_view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mainLayout->addWidget(m_view, 1);

    // ---- 底部状态条 ----
    m_lblStatus = new QLabel(QStringLiteral("提示：连接 AGV 并加载地图后显示"));
    m_lblStatus->setObjectName("lblStatus");
    mainLayout->addWidget(m_lblStatus);
}

void MapInfoDialog::setData(const QVariantList &maps, const QString &currentMap,
                            const QVariantMap &mapData)
{
    m_maps = maps;
    m_currentMap = currentMap;
    m_mapData = mapData;
    m_lblMap->setText(m_currentMap.isEmpty() ? QStringLiteral("-") : m_currentMap);
    redrawMap();
}

void MapInfoDialog::updateAgvPose(double x, double y, double angleDeg)
{
    m_lastAgv = { x, y, angleDeg };

    if (m_agvItem) {
        m_agvItem->setPos(QPointF(x * SCALE_DISTANCE, y * SCALE_DISTANCE));
        m_agvItem->setAgvAngle(angleDeg);
    }

    if (m_laserItem) {
        m_laserItem->setPos(QPointF(x * SCALE_DISTANCE, y * SCALE_DISTANCE));
        m_laserItem->setLaserAngle(angleDeg);
    }
}

void MapInfoDialog::updateLasers(const QVariantList &lasers)
{
    if (!m_laserItem) {
        m_laserItem = new LaserScanItem();
        m_laserItem->setZValue(8);
        m_scene->addItem(m_laserItem);
    }
    m_laserItem->setData(lasers);

    if (m_lastAgv.has_value()) {
        m_laserItem->setPos(QPointF(m_lastAgv->x * SCALE_DISTANCE,
                                    m_lastAgv->y * SCALE_DISTANCE));
        m_laserItem->setLaserAngle(m_lastAgv->angleDeg);
    }
}

void MapInfoDialog::clearMapItems()
{
    for (QGraphicsItem *it : m_normalItems)
        m_scene->removeItem(it);
    m_normalItems.clear();

    for (QGraphicsItem *it : m_stationItems)
        m_scene->removeItem(it);
    m_stationItems.clear();

    for (QGraphicsItem *it : m_pathItems)
        m_scene->removeItem(it);
    m_pathItems.clear();

    if (m_agvItem) {
        m_scene->removeItem(m_agvItem);
        m_agvItem = nullptr;
    }
    if (m_laserItem) {
        m_scene->removeItem(m_laserItem);
        m_laserItem = nullptr;
    }
    if (m_axisItem) {
        m_scene->removeItem(m_axisItem);
        m_axisItem = nullptr;
    }
}

void MapInfoDialog::createAgvItem(double x, double y, double angleDeg)
{
    m_agvItem = new CurrentAGVItem();
    m_agvItem->setPos(QPointF(x * SCALE_DISTANCE, y * SCALE_DISTANCE));
    m_agvItem->setAgvAngle(angleDeg);
    m_scene->addItem(m_agvItem);
}

void MapInfoDialog::redrawMap()
{
    clearMapItems();

    if (m_mapData.isEmpty()) {
        m_lblStatus->setText(QStringLiteral("暂无地图数据（连接 AGV 后会自动拉取）"));
        m_lblPoints->setText(QStringLiteral("普通点: 0  |  站点: 0"));
        return;
    }

    // 坐标轴
    m_axisItem = new CoordinateAxisItem();
    m_scene->addItem(m_axisItem);

    // 地图边界
    QVariantMap header = m_mapData.value("header").toMap();
    QVariantMap minPos = header.value("minPos").toMap();
    QVariantMap maxPos = header.value("maxPos").toMap();

    double minX = minPos.value("x").toDouble();
    double minY = minPos.value("y").toDouble();
    double maxX = maxPos.value("x").toDouble();
    double maxY = maxPos.value("y").toDouble();

    if (maxX - minX < 1 || maxY - minY < 1) {
        minX = minY = -5;
        maxX = maxY = 5;
    }

    QRectF borderRect(QPointF(minX * SCALE_DISTANCE, minY * SCALE_DISTANCE),
                      QPointF(maxX * SCALE_DISTANCE, maxY * SCALE_DISTANCE));
    QGraphicsItem *border = m_scene->addRect(
        borderRect,
        QPen(QColor(127, 140, 141), 1, Qt::DashLine),
        Qt::NoBrush
    );
    border->setZValue(0);
    m_pathItems.append(border);

    // 普通点（墙/障碍）
    QVariantList normalList = m_mapData.value("normalPosList").toList();
    QPen penGray(QColor(44, 62, 80), 0);
    QBrush brushGray(QColor(52, 73, 94));

    for (const QVariant &pv : normalList) {
        QVariantMap p = pv.toMap();
        double x = 0, y = 0;
        if (p.contains("pos")) {
            QVariantMap pos = p.value("pos").toMap();
            x = pos.value("x").toDouble();
            y = pos.value("y").toDouble();
        } else {
            x = p.value("x").toDouble();
            y = p.value("y").toDouble();
        }
        double px = x * SCALE_DISTANCE;
        double py = y * SCALE_DISTANCE;
        QGraphicsItem *item = m_scene->addRect(px - 0.3, py - 0.3, 0.6, 0.6, penGray, brushGray);
        item->setZValue(0);
        m_normalItems.append(item);
    }

    // 站点
    QVariantList advanced = m_mapData.value("advancedPointList").toList();
    for (const QVariant &pv : advanced) {
        QVariantMap p = pv.toMap();
        QVariantMap pos = p.value("pos").toMap();
        if (pos.isEmpty()) continue;

        QString name = p.value("instanceName").toString();
        if (name.isEmpty()) name = p.value("className").toString();
        double dirRad = p.value("dir").toDouble(0);
        double angleDeg = dirRad * 180.0 / M_PI;

        double ix = pos.value("x").toDouble() * SCALE_DISTANCE;
        double iy = pos.value("y").toDouble() * SCALE_DISTANCE;

        LocationMarkItem *lm = new LocationMarkItem(name, angleDeg);
        lm->setPos(QPointF(ix, iy));
        lm->setLmAngle(angleDeg);
        lm->setToolTip(QStringLiteral("%1\n(%2, %3)\ndir=%4°")
                           .arg(name)
                           .arg(pos.value("x").toDouble(), 0, 'f', 2)
                           .arg(pos.value("y").toDouble(), 0, 'f', 2)
                           .arg(angleDeg, 0, 'f', 1));
        m_scene->addItem(lm);
        m_stationItems.append(lm);

        // 文字标签
        QGraphicsTextItem *lbl = new QGraphicsTextItem(name);
        lbl->setZValue(8);
        QFont font = lbl->font();
        font.setFamily("Microsoft YaHei");
        font.setPointSize(8);
        font.setBold(true);
        lbl->setFont(font);
        lbl->setDefaultTextColor(QColor(50, 50, 50));
        lbl->setPos(ix - name.length() * 4, iy - (28 * SCALE_LM + 8));
        lbl->setTransform(QTransform().scale(-1, 1).rotate(-180));
        m_scene->addItem(lbl);
        m_stationItems.append(lbl);
    }

    // 路径
    QVariantList curves = m_mapData.value("advancedCurveList").toList();
    for (const QVariant &cv : curves) {
        QVariantMap path = cv.toMap();
        QString className = path.value("className").toString();

        QVariant spVar = path.value("startPos");
        QVariant epVar = path.value("endPos");
        QVariantMap sp = spVar.isValid() ? spVar.toMap() : QVariantMap();
        QVariantMap ep = epVar.isValid() ? epVar.toMap() : QVariantMap();

        QVariantMap spPos = sp.value("pos").toMap();
        QVariantMap epPos = ep.value("pos").toMap();
        if (spPos.isEmpty() || epPos.isEmpty()) continue;

        double spX = spPos.value("x").toDouble() * SCALE_DISTANCE;
        double spY = spPos.value("y").toDouble() * SCALE_DISTANCE;
        double epX = epPos.value("x").toDouble() * SCALE_DISTANCE;
        double epY = epPos.value("y").toDouble() * SCALE_DISTANCE;

        if (className == QStringLiteral("StraightPath")) {
            QGraphicsLineItem *line = m_scene->addLine(
                spX, spY, epX, epY,
                QPen(QColor(52, 152, 219), 3, Qt::SolidLine)
            );
            line->setZValue(4);
            line->setToolTip(QStringLiteral("%1 (直线)")
                                 .arg(path.value("instanceName").toString()));
            m_pathItems.append(line);
        } else if (className == QStringLiteral("BezierPath") ||
                   className == QStringLiteral("DegenerateBezier")) {
            QVariantMap cp1 = path.value("controlPos1").toMap();
            QVariantMap cp2 = path.value("controlPos2").toMap();

            QPainterPath pp(QPointF(spX, spY));
            pp.cubicTo(
                QPointF(cp1.value("x").toDouble() * SCALE_DISTANCE,
                        cp1.value("y").toDouble() * SCALE_DISTANCE),
                QPointF(cp2.value("x").toDouble() * SCALE_DISTANCE,
                        cp2.value("y").toDouble() * SCALE_DISTANCE),
                QPointF(epX, epY)
            );
            QGraphicsPathItem *item = m_scene->addPath(
                pp,
                QPen(QColor(153, 77, 77), 3, Qt::SolidLine),
                Qt::NoBrush
            );
            item->setZValue(5);
            item->setToolTip(QStringLiteral("%1 (%2)")
                                 .arg(path.value("instanceName").toString())
                                 .arg(className));
            m_pathItems.append(item);
        }
    }

    // AGV
    if (m_lastAgv.has_value()) {
        createAgvItem(m_lastAgv->x, m_lastAgv->y, m_lastAgv->angleDeg);
    } else {
        createAgvItem(0, 0, 0);
    }

    // 状态
    m_lblPoints->setText(
        QStringLiteral("普通点: %1  |  站点: %2")
            .arg(m_normalItems.count())
            .arg(m_stationItems.count() / 2) // 每个站点2个item(lm+lbl)
    );
    m_lblStatus->setText(
        QStringLiteral("地图: %1  |  Ctrl+滚轮: 缩放  |  拖拽: 平移")
            .arg(m_currentMap)
    );
}

void MapInfoDialog::onApplyZoom(int sliderValue)
{
    double newScale = sliderToScale(sliderValue);
    QSize viewport = m_view->viewport()->size();
    if (viewport.width() <= 0 || viewport.height() <= 0) return;

    QTransform oldT = m_view->transform();
    double oldS = oldT.m11();
    double oldDx = oldT.dx();
    double oldDy = oldT.dy();
    int vw = viewport.width();
    int vh = viewport.height();

    double cx, cy;
    if (qAbs(oldS) < 1e-9) {
        cx = cy = 0.0;
    } else {
        cx = (vw / 2.0 - oldDx) / oldS;
        cy = -(vh / 2.0 - oldDy) / oldS;
    }

    double newDx = vw / 2.0 - newScale * cx;
    double newDy = vh / 2.0 + newScale * cy;
    QTransform t(newScale, 0, 0, -newScale, newDx, newDy);
    m_view->setTransform(t);
    CoordinateAxisItem::scaleAxis = newScale;
}

void MapInfoDialog::onFit()
{
    QSize viewport = m_view->viewport()->size();
    int vw = viewport.width();
    int vh = viewport.height();
    if (vw <= 0 || vh <= 0) return;

    double viewDiameterM = 2.0 * FIT_VIEW_RADIUS_M;
    double scaleW = vw / (viewDiameterM * SCALE_DISTANCE);
    double scaleH = vh / (viewDiameterM * SCALE_DISTANCE);
    double scale = qMin(scaleW, scaleH);

    double agvX, agvY;
    if (m_lastAgv.has_value()) {
        agvX = m_lastAgv->x * SCALE_DISTANCE;
        agvY = m_lastAgv->y * SCALE_DISTANCE;
    } else {
        QRectF rect = m_scene->itemsBoundingRect();
        if (rect.isEmpty()) {
            agvX = agvY = 0.0;
        } else {
            agvX = rect.center().x();
            agvY = rect.center().y();
        }
    }

    QTransform t(scale, 0, 0, -scale,
                vw / 2.0 - scale * agvX,
                vh / 2.0 + scale * agvY);
    m_view->setTransform(t);

    int sliderVal = scaleToSlider(scale);
    sliderVal = qMax(0, qMin(100, sliderVal));
    m_zoomSlider->blockSignals(true);
    m_zoomSlider->setValue(sliderVal);
    m_zoomSlider->blockSignals(false);
    CoordinateAxisItem::scaleAxis = scale;
}

void MapInfoDialog::onRefresh()
{
    emit refreshClicked();
}
