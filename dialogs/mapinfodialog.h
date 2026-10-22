#ifndef MAPINFODIALOG_H
#define MAPINFODIALOG_H

#include <QDialog>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QGraphicsTextItem>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QTransform>
#include <QWheelEvent>
#include <QPointF>
#include <QRectF>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QDebug>
#include <cmath>
#include <vector>
#include <map>

// =====================================================================
// 常量（参考 SeerAgv global_data.h）
// =====================================================================
inline constexpr double SCALE_DISTANCE = 200.0;  // 地图坐标(米) * 200 = 像素
inline constexpr double SCALE_LM      = 2.0;   // LocationMark 图元尺寸倍数
inline constexpr double SCALE_AGV     = 2.0;   // CurrentAGV 图元尺寸倍数
inline constexpr double FIT_VIEW_RADIUS_M = 2.5; // 自适应视野半径（米）

// =====================================================================
// 缩放 ↔ slider 互转（参考 SeerAgv view.cpp）
// =====================================================================
inline double sliderToScale(int sliderValue) {
    return std::pow(2.0, (sliderValue - 70) / 15.0);
}
inline int scaleToSlider(double scale) {
    if (scale <= 0) return 0;
    return static_cast<int>(70 + 15 * std::log2(scale));
}

// =====================================================================
// 激光点云图元（Z=8）
// =====================================================================
class LaserScanItem : public QGraphicsItem {
public:
    explicit LaserScanItem(QGraphicsItem *parent = nullptr);

    void setData(const QVariantList &lasers);
    void setLaserAngle(double deg);

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
               QWidget *widget = nullptr) override;

private:
    struct Beam {
        double angle;      // 弧度
        double dist;       // 米
        bool   valid;
        double installX;
        double installY;
        double installYaw; // 弧度
    };
    void rebuildPolys();

    std::vector<Beam> m_beams;
    QList<QPolygonF>  m_polys;
    double             m_laserAngle = 0.0; // 度
};

// =====================================================================
// 导航站点图元（Z=2）
// =====================================================================
class LocationMarkItem : public QGraphicsItem {
public:
    explicit LocationMarkItem(const QString &name, double angleDeg, QGraphicsItem *parent = nullptr);

    QString labelOffsetY() const;
    void setLmAngle(double deg);

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
               QWidget *widget = nullptr) override;

private:
    QString m_name;
    double  m_lmAngle = 0.0; // 度
};

// =====================================================================
// AGV 图元（Z=3）
// =====================================================================
class CurrentAGVItem : public QGraphicsItem {
public:
    explicit CurrentAGVItem(QGraphicsItem *parent = nullptr);

    void setAgvAngle(double deg);

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
               QWidget *widget = nullptr) override;

private:
    double m_agvAngle = 0.0; // 度
};

// =====================================================================
// 坐标轴图元（Z=6）
// =====================================================================
class CoordinateAxisItem : public QGraphicsItem {
public:
    explicit CoordinateAxisItem(QGraphicsItem *parent = nullptr);

    static double scaleAxis;

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
               QWidget *widget = nullptr) override;
};

// =====================================================================
// 地图视图（支持 Ctrl+滚轮缩放、拖拽平移）
// =====================================================================
class MapView : public QGraphicsView {
    Q_OBJECT
public:
    explicit MapView(QGraphicsScene *scene, QWidget *parent = nullptr);

protected:
    void wheelEvent(QWheelEvent *event) override;
};

// =====================================================================
// 地图信息弹窗
// =====================================================================
class MapInfoDialog : public QDialog {
    Q_OBJECT
public:
    explicit MapInfoDialog(QWidget *parent = nullptr);
    ~MapInfoDialog() = default;

    // 设置地图数据
    void setData(const QVariantList &maps, const QString &currentMap,
                 const QVariantMap &mapData);

    // 更新 AGV 位姿
    void updateAgvPose(double x, double y, double angleDeg);

    // 更新激光点云
    void updateLasers(const QVariantList &lasers);

signals:
    void refreshClicked();

private slots:
    void onApplyZoom(int value);
    void onFit();
    void onRefresh();

private:
    void initUi();
    void clearMapItems();
    void redrawMap();
    void createAgvItem(double x, double y, double angleDeg);

    // ---- UI ----
    QLabel           *m_lblMap       = nullptr;
    QLabel           *m_lblPoints    = nullptr;
    QSlider          *m_zoomSlider   = nullptr;
    QGraphicsScene   *m_scene        = nullptr;
    MapView          *m_view         = nullptr;
    QLabel           *m_lblStatus    = nullptr;

    // ---- 地图数据 ----
    QVariantList    m_maps;
    QString         m_currentMap;
    QVariantMap     m_mapData;

    // ---- AGV 状态 ----
    struct AGVState { double x, y, angleDeg; };
    std::optional<AGVState> m_lastAgv;

    // ---- 图元 ----
    QList<QGraphicsItem*> m_normalItems;
    QList<QGraphicsItem*> m_stationItems;
    QList<QGraphicsItem*> m_pathItems;
    CurrentAGVItem   *m_agvItem      = nullptr;
    LaserScanItem    *m_laserItem     = nullptr;
    CoordinateAxisItem *m_axisItem    = nullptr;
};

#endif // MAPINFODIALOG_H
