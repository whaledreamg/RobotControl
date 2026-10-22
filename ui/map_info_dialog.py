"""
地图绘制弹窗
参考 SeerAgv/graphic_interface/ 的实现：
  mapdrawerform.cpp / view.cpp / locationmark.cpp / currentagv.cpp
  / coordinateaxis.cpp / chip.cpp / straightpath.cpp / bezierpath.cpp / global_data.h

关键约定（来自 SeerAgv global_data.h）：
  SCALE_DISTANCE = 200   地图坐标(米) * 200 = 像素
  SCALE_LM = 2           LocationMark(站点)图元尺寸的额外倍数
  SCALE_AGV = 2          CurrentAGV 图元尺寸的额外倍数
  RESOLUTION = 0.1       栅格分辨率（米）

我们这里 SCALE_DISTANCE 改用 30（200 太大会让几十米的地图爆屏），
SCALE_LM / SCALE_AGV 沿用 2。
"""
import math
from PySide6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QGridLayout,
    QLabel, QPushButton, QGraphicsView, QGraphicsScene,
    QGraphicsItem, QGraphicsTextItem, QSlider, QSizePolicy, QStyle,
)
from PySide6.QtCore import Qt, Signal, QRectF, QPointF
from PySide6.QtGui import (
    QPen, QBrush, QColor, QPainter, QPolygonF, QFont, QTransform,
    QPainterPath,
)

# ----- 缩放常量（参考 SeerAgv global_data.h） -----
# SCALE_DISTANCE = 200: 1 米地图坐标 = 200 像素（SeerAgv 原始值）
#   真实 AGV 约 0.8m 宽 → 站体 160px ≈ 0.8m ✓
#   10m 场景 → 2000px，fitInView 自动缩到屏幕
# SCALE_LM = 2, SCALE_AGV = 2: 站点/AGV 图元尺寸用 SCALE_LM/SCALE_AGV
#   倍数（与 SCALE_DISTANCE 无关，是图元局部像素尺寸）
SCALE_DISTANCE = 200.0
SCALE_LM = 2.0
SCALE_AGV = 2.0

# 兼容旧代码里 PIXEL_SCALE 的引用（保留语义）
PIXEL_SCALE = SCALE_DISTANCE

# ----- "自适应"按钮的聚焦视野 -----
#   以 AGV 为中心,显示 AGV 周围 ±FIT_VIEW_RADIUS_M 米 (5m × 5m)
#   zoom slider 可独立调整,不会被这个值限制
FIT_VIEW_RADIUS_M = 2.5


# ----- 缩放 ↔ slider 互转 -----
#   与 SeerAgv view.cpp:setupMatrix 一致: scale = 2^((val - 70) / 15)
#   val=70 → scale=1 (中点); val=30 → scale≈0.157 (SeerAgv 默认)
def _slider_to_scale(slider_value: int) -> float:
    return math.pow(2.0, (slider_value - 70) / 15.0)


def _scale_to_slider(scale: float) -> int:
    if scale <= 0:
        return 0
    return int(70 + 15 * math.log2(scale))


# =====================================================================
# 自定义图元（QGraphicsItem 子类）—— 与 SeerAgv 的 LocationMark/CurrentAGV
# 风格保持一致：Z 值、颜色、形状、绘制方式
# =====================================================================


class LaserScanItem(QGraphicsItem):
    """激光点云（参考 SeerAgv graphic_interface/laserdata.cpp）

    坐标系：item 局部坐标 = AGV 局部坐标
    - 外部 setPos/setRotation 同步 AGV 位姿到世界
    - 内部 paint() 算每个 beam 在 AGV 局部系下的点，再 *SCALE_DISTANCE
    - 多 beam 端点 + 安装位置 = 凸多边形，浅蓝色填充（半透明）
    - 每个有效 beam 端点画绿色小圆点
    - 连续 6° 以上的"无效 beam 间隔"会断开多边形（避免一片大块）

    Z 值 = 8（参考 SeerAgv，比 path/station 高，比 AGV 低）
    """

    # 单个 laser 雷达的安装偏移在每个 beam 里存（避免多 laser 干扰）
    def __init__(self, beams: list = None):
        super().__init__()
        self._beams = beams or []  # [{angle_rad, dist, is_obstacle, install_x, install_y, install_yaw_rad}, ...]
        self.setZValue(8)
        # 缓存多边形点（避免每次 paint 重复算）
        self._polys: list = []  # [[QPointF, ...], ...]  每段一段多边形
        self._laser_angle = 0.0  # 不用 setRotation（PySide6 6.x 有 bug），用 setTransform

    def setData(self, beams: list):
        """更新激光数据并标记重绘"""
        self._beams = beams or []
        self._polys = []  # 失效缓存
        self.update()

    def set_laser_angle(self, deg: float):
        self._laser_angle = deg
        self.update()  # 触发重绘，paint 里手动 painter.rotate

    def boundingRect(self) -> QRectF:
        """激光覆盖范围 = 整张图最大射程（30m）"""
        r = 30.0 * SCALE_DISTANCE
        return QRectF(-r, -r, 2 * r, 2 * r)

    def _build_polys(self):
        """按 (install_x, install_y, install_yaw) 分组 + 间断拆分（参考 SeerAgv laserdata.cpp 第 40-85 行）"""
        self._polys = []
        groups: dict = {}
        for b in self._beams:
            key = (b["install_x"], b["install_y"], b["install_yaw"])
            groups.setdefault(key, []).append(b)

        for key, group_beams in groups.items():
            inst_x, inst_y, inst_yaw = key
            poly_points = []
            gap = 0
            for b in sorted(group_beams, key=lambda x: x["angle"]):
                a = b["angle"] + inst_yaw
                lx = b["dist"] * math.cos(a) + inst_x
                ly = b["dist"] * math.sin(a) + inst_y
                if b.get("valid", True) and b["dist"] > 0:
                    # view 已 scale(1,-1)，item 局部不翻 Y
                    poly_points.append(
                        QPointF(lx * SCALE_DISTANCE, ly * SCALE_DISTANCE)
                    )
                    gap = 0
                else:
                    gap += 1
                    if gap > 6:
                        poly_points.append(
                            QPointF(inst_x * SCALE_DISTANCE, inst_y * SCALE_DISTANCE)
                        )
                        if len(poly_points) >= 3:
                            self._polys.append(poly_points)
                        poly_points = []
                        gap = 0
            if poly_points:
                poly_points.append(
                    QPointF(inst_x * SCALE_DISTANCE, inst_y * SCALE_DISTANCE)
                )
                if len(poly_points) >= 3:
                    self._polys.append(poly_points)

    def paint(self, painter: QPainter, option, widget=None):
        if not self._beams:
            return
        if not self._polys:
            self._build_polys()

        painter.setRenderHint(QPainter.Antialiasing, True)
        # 与 SeerAgv mapdrawerform.cpp:242-249 一致:
        #   laserdata_->setTransformOriginPoint(QPointF(0,0));
        #   laserdata_->setRotation(current_angle_);   // 正度数,无负号
        # view 的 scale(1,-1) 只翻屏幕渲染的 Y,不影响 item 局部旋转方向,
        # 所以这里 painter.rotate 用正数。
        painter.rotate(self._laser_angle)

        # 1) 多边形填充（浅蓝色半透明 —— 参考 SeerAgv laserdata.cpp 第 95-105 行）
        fill_color = QColor(217, 255, 255, 120)  # 浅青
        painter.setPen(QPen(fill_color, 1))
        painter.setBrush(QBrush(fill_color))
        for poly in self._polys:
            painter.drawPolygon(QPolygonF(poly), Qt.OddEvenFill)

        # 2) 每个有效 beam 端点画绿色圆点（参考 SeerAgv laserdata.cpp 第 113-121 行）
        dot_color = QColor(0, 200, 0, 200)
        painter.setPen(QPen(dot_color, 1))
        painter.setBrush(QBrush(dot_color))
        for b in self._beams:
            if not b.get("valid", True) or b["dist"] <= 0:
                continue
            a = b["angle"] + b["install_yaw"]
            lx = b["dist"] * math.cos(a) + b["install_x"]
            ly = b["dist"] * math.sin(a) + b["install_y"]
            # view 已 scale(1,-1)，item 局部不翻 Y
            painter.drawEllipse(
                QPointF(lx * SCALE_DISTANCE, ly * SCALE_DISTANCE),
                3, 3,  # 半径 3px
            )

class LocationMarkItem(QGraphicsItem):
    """导航站点（LocationMark）—— 站体矩形 + 朝向三角 + 转弯圆 + 中心十字

    文字标签作为独立 QGraphicsTextItem 由外部加到 scene，名称通过
    name() 方法取。避免子 item 跟随父 rotation 翻转。
    Z 值 = 2（参考 SeerAgv）
    """

    def __init__(self, name: str, angle_deg: float = 0.0):
        super().__init__()
        self._name = name
        self._lm_angle = angle_deg
        self.setZValue(2)
        self.setAcceptHoverEvents(True)
        # 不调 setRotation（PySide6 6.x + view scale(1,-1) 组合 bug）
        # paint 里手动 painter.rotate(self._lm_angle) —— 与 SeerAgv 的
        # lm->setRotation(angle*180/M_PI) 等价（正度数，无负号）

    def set_lm_angle(self, deg: float):
        self._lm_angle = deg
        self.update()

    def name(self) -> str:
        return self._name

    def label_offset_y(self) -> float:
        """文字标签相对 item 中心 Y 偏移（站体上方）"""
        return -(28 * SCALE_LM + 8)

    def boundingRect(self) -> QRectF:
        # 49 * SCALE_LM 包含转弯圆
        s = 49 * SCALE_LM
        return QRectF(-s, -s, 2 * s, 2 * s)

    def paint(self, painter: QPainter, option, widget=None):
        painter.setRenderHint(QPainter.Antialiasing, True)
        # 与 SeerAgv mapdrawerform.cpp:158-163 一致：
        #   lm->setTransformOriginPoint(QPointF(0,0));
        #   lm->setRotation(angle * 180 / M_PI);   // 正度数,无负号
        # view 的 scale(1,-1) 只翻屏幕渲染的 Y,不影响 item 局部旋转方向,
        # 所以这里 painter.rotate 用正数。
        painter.rotate(self._lm_angle)

        # 站体填充：浅橙色，hover 变亮
        color = QColor(245, 179, 138, 200)
        if option.state & QStyle.State_MouseOver:
            color = color.lighter(120)
        color_bound = QColor(0, 128, 0, 200)

        # 站体矩形（80 × 56，* SCALE_LM）
        pen = QPen(color_bound, 2, Qt.DotLine)
        painter.setPen(pen)
        painter.setBrush(QBrush(color))
        w = 80 * SCALE_LM / 2
        h = 56 * SCALE_LM / 2
        painter.drawRect(QRectF(-w, -h, 2 * w, 2 * h))

        # 朝向三角（红）
        color_dir = QColor(200, 0, 0, 150)
        pen_dir = QPen(color_dir)
        pen_dir.setColor(color_dir)
        painter.setPen(pen_dir)
        tri = QPolygonF([
            QPointF(25 * SCALE_LM,  8 * SCALE_LM),
            QPointF(25 * SCALE_LM, -8 * SCALE_LM),
            QPointF(35 * SCALE_LM,  0),
        ])
        painter.setBrush(QBrush(color_dir))
        painter.drawPolygon(tri)

        # 转弯圆（淡橙，虚线，49 * SCALE_LM 半径）
        pen_cir = QPen(QColor(245, 184, 148, 80))
        pen_cir.setStyle(Qt.DotLine)
        painter.setPen(pen_cir)
        painter.setBrush(QBrush(QColor(255, 255, 255, 0)))
        painter.drawEllipse(QPointF(0, 0), 49 * SCALE_LM, 49 * SCALE_LM)

        # 中心十字（红）
        pen_c = QPen(QColor(200, 0, 0, 100))
        pen_c.setWidth(5)
        painter.setPen(pen_c)
        painter.setBrush(QBrush(QColor(200, 0, 0, 100)))
        painter.drawLine(QPointF(-10 * SCALE_LM, 0), QPointF(10 * SCALE_LM, 0))
        painter.drawLine(QPointF(0, -10 * SCALE_LM), QPointF(0, 10 * SCALE_LM))
        # 文字标签用 self._label（QGraphicsTextItem），不放在这里画


class CurrentAGVItem(QGraphicsItem):
    """当前 AGV 位置（CurrentAGV）—— 车体矩形 + 朝向三角 + 转弯圆 + 中心点

    Z 值 = 3（参考 SeerAgv，比站点高，渲染在站点之上）
    """

    def __init__(self):
        super().__init__()
        self.setZValue(3)
        self.setAcceptHoverEvents(True)
        self._agv_angle = 0.0  # 不依赖 setRotation/setTransform（PySide6 6.x 有 bug）
        # 自定义 paint 直接 painter.rotate，避免 Qt 6 item transform bug

    def set_agv_angle(self, deg: float):
        self._agv_angle = deg
        self.update()  # 触发重绘

    def boundingRect(self) -> QRectF:
        s = 49 * SCALE_AGV
        return QRectF(-s, -s, 2 * s, 2 * s)

    def paint(self, painter: QPainter, option, widget=None):
        painter.setRenderHint(QPainter.Antialiasing, True)
        # 与 SeerAgv mapdrawerform.cpp:35-40 一致：
        #   current_agv_->setTransformOriginPoint(QPointF(0,0));
        #   current_agv_->setRotation(current_angle_);   // 正度数,无负号
        # view 的 scale(1,-1) 只翻屏幕渲染的 Y,不影响 item 局部旋转方向,
        # 所以这里 painter.rotate 用正数,不要取负号(否则车体方向与站点方向
        # 会同时反,看起来仍像"没旋转",但单独看站点/AGV 角度都是错的)。
        painter.rotate(self._agv_angle)

        # 车体填充（半透明绿）
        color = QColor(0, 128, 0, 100)
        frame_color = QColor(0, 128, 0, 200)

        # 车体矩形（80 × 56，* SCALE_AGV）
        pen = QPen(frame_color, 2, Qt.DashLine)
        painter.setPen(pen)
        painter.setBrush(QBrush(color))
        w = 40 * SCALE_AGV
        h = 28 * SCALE_AGV
        painter.drawRect(QRectF(-w, -h, 2 * w, 2 * h))

        # 朝向三角（红）
        pen_dir = QPen(QColor(200, 0, 0, 150))
        painter.setPen(pen_dir)
        tri = QPolygonF([
            QPointF(25 * SCALE_AGV,  8 * SCALE_AGV),
            QPointF(25 * SCALE_AGV, -8 * SCALE_AGV),
            QPointF(35 * SCALE_AGV,  0),
        ])
        painter.setBrush(QBrush(QColor(200, 0, 0, 150)))
        painter.drawPolygon(tri)

        # 转弯圆（淡蓝，虚线，49 * SCALE_AGV）
        pen_cir = QPen(QColor(0, 0, 150, 80))
        pen_cir.setStyle(Qt.DotLine)
        painter.setPen(pen_cir)
        painter.setBrush(QBrush(QColor(255, 255, 255, 0)))
        painter.drawEllipse(QPointF(0, 0), 49 * SCALE_AGV, 49 * SCALE_AGV)

        # 中心点（小红圆点）
        pen_c = QPen(QColor(200, 0, 0, 150))
        pen_c.setWidth(2)
        painter.setPen(pen_c)
        painter.setBrush(QBrush(QColor(200, 0, 0, 150)))
        painter.drawEllipse(QPointF(0, 0), 2 * SCALE_AGV, 2 * SCALE_AGV)


class CoordinateAxisItem(QGraphicsItem):
    """0,0 处的 X/Y 坐标轴（参考 SeerAgv CoordinateAxis）

    文字用 QGraphicsTextItem 子 item，避免 view scale(1,-1) 翻转导致文字颠倒。
    """
    # 全局缩放因子（CoordinateAxis 的 line 粗细反缩放用）
    scale_axis = 1.0

    def __init__(self):
        super().__init__()
        self.setZValue(6)
        # 文字标签作为子 item
        for txt, color, x, y in [
            ("0,0", QColor(255, 0, 0), -10, -10),
            ("x",  QColor(255, 0, 0),  60, -10),
            ("y",  QColor(0, 87, 0),  -10,  60),
        ]:
            t = QGraphicsTextItem(txt, self)
            t.setZValue(7)
            t.setDefaultTextColor(color)
            font = QFont("Microsoft YaHei", 6)
            t.setFont(font)
            t.setPos(x, y)
            # 反转 view 的 Y 翻转：scale(-1, 1) + rotate(-180)
            # → view scale(1,-1) 与 M(翻 X + 旋转 180) 复合后恒等，文字正立
            t.setTransform(QTransform().scale(-1, 1).rotate(-180))

    def boundingRect(self) -> QRectF:
        return QRectF(-30, -80, 110, 160)

    def paint(self, painter: QPainter, option, widget=None):
        # 反缩放：让坐标轴线宽不随 view 缩放变化
        s = CoordinateAxisItem.scale_axis
        if s > 0:
            painter.scale(1.0 / s, 1.0 / s)

        color_x = QColor(255, 0, 0, 255)
        color_y = QColor(0, 87, 0, 255)

        # X 轴（红）
        pen_x = QPen(color_x, 1, Qt.SolidLine)
        painter.setPen(pen_x)
        painter.drawLine(QPointF(0, 0), QPointF(60, 0))
        painter.setBrush(QBrush(color_x))
        painter.drawPolygon(QPolygonF([
            QPointF(60,  5), QPointF(60, -5), QPointF(65, 0),
        ]))

        # Y 轴（绿）
        pen_y = QPen(color_y, 1, Qt.SolidLine)
        painter.setPen(pen_y)
        painter.drawLine(QPointF(0, 0), QPointF(0, 60))
        painter.setBrush(QBrush(color_y))
        painter.drawPolygon(QPolygonF([
            QPointF(-5, 60), QPointF(5, 60), QPointF(0, 65),
        ]))

        # 中心点
        pen_c = QPen(QColor(255, 0, 0, 255), 2)
        painter.setPen(pen_c)
        painter.setBrush(QBrush(QColor(255, 0, 0, 255)))
        painter.drawEllipse(QPointF(0, 0), 4, 4)
        # 文字标签用 self 的子 item (QGraphicsTextItem)，不放在这里画


# =====================================================================
# 视图：支持滚轮缩放 + 拖拽平移
# =====================================================================

class MapView(QGraphicsView):
    """支持 Ctrl+滚轮缩放（精细）、普通滚轮平移、拖拽平移

    参考 SeerAgv view.cpp:208：view 初始化时 scale(1, -1) 翻 Y，
    这样 item setPos / setRotation 直接对应地图坐标/角度，
    文字 label 反向旋转即可保持水平。
    缩放用 Ctrl+滚轮（精细），普通滚轮走默认滚动。
    """

    def __init__(self, scene, parent=None):
        super().__init__(scene, parent)
        self.setRenderHint(QPainter.Antialiasing, True)
        self.setRenderHint(QPainter.SmoothPixmapTransform, True)
        self.setDragMode(QGraphicsView.ScrollHandDrag)
        self.setTransformationAnchor(QGraphicsView.AnchorUnderMouse)
        self.setResizeAnchor(QGraphicsView.AnchorUnderMouse)
        self.setViewportUpdateMode(QGraphicsView.SmartViewportUpdate)
        # 参考 SeerAgv view.cpp:208：view 初始化时 scale(1, -1) 翻 Y，
        # 这样 item setPos / setRotation 直接对应地图坐标/角度，
        # 文字 label 反向旋转即可保持水平。
        self.scale(1, -1)
        self._zoom_step = 1.15

    def wheelEvent(self, event):
        if event.modifiers() & Qt.ControlModifier:
            if event.angleDelta().y() > 0:
                self.scale(self._zoom_step, self._zoom_step)
            else:
                self.scale(1 / self._zoom_step, 1 / self._zoom_step)
            event.accept()
        else:
            super().wheelEvent(event)


# =====================================================================
# 主弹窗
# =====================================================================

class MapInfoDialog(QDialog):
    """地图信息 + 地图绘制弹窗（参考 SeerAgv mapdrawerform）"""

    refresh_clicked = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("地图绘制")
        self.setMinimumSize(1000, 700)
        self._init_ui()

        # 运行时状态
        self._maps = []
        self._current_map = ""
        self._map_data = {}
        self._last_agv = None  # (x, y, angle_deg)
        self._agv_item: CurrentAGVItem = None
        self._laser_item: "LaserScanItem" = None  # 激光点云 item

        # 标记
        self._normal_items = []
        self._station_items = []   # [(name, LocationMarkItem)]
        self._path_items = []
        self._axis_item: CoordinateAxisItem = None

    def _init_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(4)

        # --- 顶部控制条 ---
        top = QGridLayout()
        top.addWidget(QLabel("地图:"), 0, 0)
        self.map_combo = QLabel("-")
        self.map_combo.setStyleSheet("font-weight: bold; color: #2c3e50;")
        top.addWidget(self.map_combo, 0, 1)

        self.lbl_points = QLabel("普通点: 0  |  站点: 0")
        top.addWidget(self.lbl_points, 0, 2)

        top.addWidget(QLabel("Ctrl+滚轮: 缩放  |  拖拽: 平移"), 0, 3)

        # 缩放 slider（参考 SeerAgv view.cpp：0-100，默认 30，
        # 公式 2^((val-70)/15)，默认 30 → scale=0.157 → 1m=31.4px）
        self.zoom_slider = QSlider(Qt.Horizontal)
        self.zoom_slider.setMinimum(0)
        self.zoom_slider.setMaximum(100)
        self.zoom_slider.setValue(30)
        self.zoom_slider.setFixedWidth(120)
        self.zoom_slider.setToolTip("缩放（左=缩小，右=放大）")
        self.zoom_slider.valueChanged.connect(self._apply_zoom)
        top.addWidget(QLabel("缩放"), 0, 4)
        top.addWidget(self.zoom_slider, 0, 5)

        self.btn_fit = QPushButton("自适应")
        self.btn_fit.clicked.connect(self._on_fit)
        top.addWidget(self.btn_fit, 0, 6)

        self.btn_refresh = QPushButton("刷新")
        self.btn_refresh.clicked.connect(self._on_refresh)
        top.addWidget(self.btn_refresh, 0, 7)

        self.btn_close = QPushButton("关闭")
        self.btn_close.clicked.connect(self.accept)
        top.addWidget(self.btn_close, 0, 8)

        layout.addLayout(top)

        # --- 图形视图 ---
        self._scene = QGraphicsScene()
        self._scene.setBackgroundBrush(QBrush(QColor("#fafafa")))
        self._view = MapView(self._scene)
        self._view.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        layout.addWidget(self._view, 1)

        # --- 底部状态条 ---
        self.lbl_status = QLabel("提示：连接 AGV 并加载地图后显示")
        self.lbl_status.setStyleSheet("color: #7f8c8d;")
        layout.addWidget(self.lbl_status)

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def set_data(self, maps: list, current_map: str, map_data: dict):
        """设置地图元数据 + 绘制地图"""
        self._maps = list(maps or [])
        self._current_map = current_map or ""
        self._map_data = map_data or {}
        self.map_combo.setText(self._current_map or "-")
        self._redraw_map()

    def update_agv_pose(self, x: float, y: float, angle_deg: float):
        """AGV 实时位置/朝向更新（外部 AGV 状态推送时调用）
        Y 翻转改在 item paint 里处理，setRotation 不再使用（PySide6 6.x 有 bug）
        """
        self._last_agv = (x, y, angle_deg)
        if self._agv_item is None:
            return
        self._agv_item.setPos(QPointF(x * SCALE_DISTANCE, y * SCALE_DISTANCE))
        self._agv_item.set_agv_angle(angle_deg)
        # 同步激光点云：位置/朝向跟随 AGV（参考 SeerAgv mapdrawerform.cpp:242-249）
        if self._laser_item is not None:
            self._laser_item.setPos(QPointF(x * SCALE_DISTANCE, y * SCALE_DISTANCE))
            self._laser_item.set_laser_angle(angle_deg)

    def update_lasers(self, lasers: list):
        """激光点云更新（AGV 推送的 lasers 字段）
        lasers: [{"beams": [{"angle", "dist", "is_obstacle", "valid", ...}],
                  "install_info": {"x", "y", "yaw"}, ...}, ...]
        角度单位：度（与 SeerAgv 协议一致：beam.angle 是度，install.yaw 也是度）
        距离单位：米
        坐标系：item 局部 = AGV 局部（setPos/setRotation 跟随 AGV 到世界）
        参考 SeerAgv graphic_interface/laserdata.cpp
        """
        # 解析并按 (is_obstacle, valid) 分类
        # valid==false 或 dist<=0 的 beam 视为无效
        parsed = []
        for laser in lasers or []:
            install = laser.get("install_info", {}) or {}
            inst_x = install.get("x", 0.0)           # 米，相对 AGV
            inst_y = install.get("y", 0.0)           # 米
            inst_yaw = install.get("yaw", 0.0)       # 度
            for beam in laser.get("beams", []) or []:
                if not beam.get("valid", True):
                    continue
                dist = beam.get("dist", 0.0)
                if dist <= 0:
                    continue
                angle = beam.get("angle", 0.0)  # 度
                parsed.append({
                    "angle": math.radians(angle),
                    "dist": dist,
                    "is_obstacle": beam.get("is_obstacle", True),
                    "install_x": inst_x,
                    "install_y": inst_y,
                    "install_yaw": math.radians(inst_yaw),
                })

        if self._laser_item is None:
            self._laser_item = LaserScanItem(parsed)
            # z 高于 path / station，低于 AGV（AGV 在最上层）
            self._laser_item.setZValue(8)
            # 不设父 item，独立 + 手动同步 pos/rotation
            self._scene.addItem(self._laser_item)
        else:
            self._laser_item.setData(parsed)
        # 立刻同步到当前 AGV 位置/朝向
        if self._last_agv is not None:
            x, y, ang = self._last_agv
            self._laser_item.setPos(QPointF(x * SCALE_DISTANCE, y * SCALE_DISTANCE))
            self._laser_item.set_laser_angle(ang)

    # ------------------------------------------------------------------
    # 绘制
    # ------------------------------------------------------------------

    def _clear_map_items(self):
        for it in self._normal_items:
            self._scene.removeItem(it)
        self._normal_items.clear()
        for _, it in self._station_items:
            self._scene.removeItem(it)
        self._station_items.clear()
        for it in self._path_items:
            self._scene.removeItem(it)
        self._path_items.clear()
        if self._agv_item is not None:
            self._scene.removeItem(self._agv_item)
            self._agv_item = None
        if self._laser_item is not None:
            self._scene.removeItem(self._laser_item)
            self._laser_item = None
        if self._axis_item is not None:
            self._scene.removeItem(self._axis_item)
            self._axis_item = None

    def _redraw_map(self):
        """重画地图（参考 SeerAgv MapDrawerForm::drawMap）"""
        self._clear_map_items()

        data = self._map_data or {}
        if not data:
            self.lbl_status.setText("暂无地图数据（连接 AGV 后会自动拉取）")
            self.lbl_points.setText("普通点: 0  |  站点: 0")
            return

        # 0) 坐标轴（永远在 0,0）—— 参考 SeerAgv drawAxis()
        self._axis_item = CoordinateAxisItem()
        self._scene.addItem(self._axis_item)

        # 1) 地图边界矩形（minPos / maxPos）
        header = data.get("header", {})
        min_pos = header.get("minPos", {}) or {}
        max_pos = header.get("maxPos", {}) or {}
        min_pt = QPointF(min_pos.get("x", 0) * SCALE_DISTANCE,
                          min_pos.get("y", 0) * SCALE_DISTANCE)
        max_pt = QPointF(max_pos.get("x", 0) * SCALE_DISTANCE,
                          max_pos.get("y", 0) * SCALE_DISTANCE)
        rect = QRectF(min_pt, max_pt).normalized()
        if rect.width() < 1 or rect.height() < 1:
            # 兜底：10m × 10m
            rect = QRectF(-5 * SCALE_DISTANCE, -5 * SCALE_DISTANCE,
                          10 * SCALE_DISTANCE, 10 * SCALE_DISTANCE)
        border = self._scene.addRect(
            rect,
            QPen(QColor("#7f8c8d"), 1, Qt.DashLine),
            QBrush(QColor(0, 0, 0, 0)),
        )
        border.setZValue(0)
        self._path_items.append(border)

        # 2) 普通点（墙/障碍，灰色小点）—— SeerAgv Chip 类，1×1 像素黑色
        # 我们用 0.6×0.6 像素（更明显些，且点很多时不会糊成一坨）
        normal_list = data.get("normalPosList", [])
        pen_gray = QPen(QColor("#2c3e50"), 0)
        brush_gray = QBrush(QColor("#34495e"))
        for p in normal_list:
            if not isinstance(p, dict):
                continue
            if "pos" in p and isinstance(p["pos"], dict):
                x = p["pos"].get("x", 0)
                y = p["pos"].get("y", 0)
            else:
                x = p.get("x", 0)
                y = p.get("y", 0)
            px = x * SCALE_DISTANCE
            py = y * SCALE_DISTANCE  # view 已 scale(1,-1)，不翻 Y
            item = self._scene.addRect(
                px - 0.3, py - 0.3, 0.6, 0.6,
                pen_gray, brush_gray,
            )
            item.setZValue(0)  # SeerAgv Chip Z=0
            self._normal_items.append(item)

        # 3) 高级点（站点）—— LocationMarkItem，Z=2
        advanced = data.get("advancedPointList", [])
        for p in advanced:
            if not isinstance(p, dict):
                continue
            pos = p.get("pos", {})
            if not pos:
                continue
            name = p.get("instanceName") or p.get("className") or ""
            dir_rad = p.get("dir", 0) or 0
            angle_deg = dir_rad * 180.0 / math.pi

            x = pos.get("x", 0) * SCALE_DISTANCE
            y = pos.get("y", 0) * SCALE_DISTANCE  # view 已 scale(1,-1)，不翻 Y

            lm = LocationMarkItem(name, angle_deg)
            lm.setPos(QPointF(x, y))
            # 不调 setRotation（PySide6 6.x bug），改用 set_lm_angle 让 paint 里手动 rotate
            lm.set_lm_angle(angle_deg)
            lm.setToolTip(f"{name}\n({pos.get('x', 0):.2f}, {pos.get('y', 0):.2f})\n"
                          f"dir={angle_deg:.1f}°")
            self._scene.addItem(lm)

            # 文字标签（独立 QGraphicsTextItem，跟随站体）
            lbl = QGraphicsTextItem(name)
            lbl.setZValue(8)
            font = QFont("Microsoft YaHei", 8)
            font.setBold(True)
            lbl.setFont(font)
            lbl.setDefaultTextColor(QColor(50, 50, 50))
            lbl.setPos(x - len(name) * 4, y - (28 * SCALE_LM + 8))
            # 参考 SeerAgv locationmark.cpp:90：
            #   painter.scale(-1, 1); painter.rotate(rotation - 180);
            # 净效果是矩阵 M = scale(-1, 1) · rotate(-180) = scale(1, -1) 翻 Y
            # 叠加 view scale(1, -1) 后恒等，文字正立
            lbl.setTransform(QTransform().scale(-1, 1).rotate(-180))
            self._scene.addItem(lbl)

            self._station_items.append((name, lm, lbl))

        # 4) 路径（SeerAgv 真实协议：所有路径在 advancedCurveList 里，
        #     按 className 区分 StraightPath / BezierPath / DegenerateBezier）
        #     字段：startPos={instanceName, pos:{x,y}}, endPos=...
        #           controlPos1={x,y}, controlPos2={x,y}（贝塞尔有）
        for path in data.get("advancedCurveList", []) or []:
            class_name = path.get("className", "")
            sp = path.get("startPos", {}) or {}
            ep = path.get("endPos", {}) or {}
            # startPos 嵌套结构：{instanceName, pos:{x,y}}
            sp_pos = sp.get("pos", {}) if isinstance(sp, dict) else {}
            ep_pos = ep.get("pos", {}) if isinstance(ep, dict) else {}
            if not sp_pos or not ep_pos:
                continue
            sp_x = sp_pos.get("x", 0) * SCALE_DISTANCE
            sp_y = sp_pos.get("y", 0) * SCALE_DISTANCE  # view 已 scale(1,-1)
            ep_x = ep_pos.get("x", 0) * SCALE_DISTANCE
            ep_y = ep_pos.get("y", 0) * SCALE_DISTANCE  # view 已 scale(1,-1)

            if class_name == "StraightPath":
                # 直线：蓝色实线
                line = self._scene.addLine(
                    sp_x, sp_y, ep_x, ep_y,
                    QPen(QColor("#3498db"), 3, Qt.SolidLine),
                )
                line.setZValue(4)
                line.setToolTip(f"{path.get('instanceName', '')} (直线)\n"
                                 f"({sp_pos.get('x', 0):.2f}, {sp_pos.get('y', 0):.2f})"
                                 f" → ({ep_pos.get('x', 0):.2f}, {ep_pos.get('y', 0):.2f})")
                self._path_items.append(line)
            elif class_name in ("BezierPath", "DegenerateBezier"):
                # 三次贝塞尔：红褐色曲线
                cp1 = path.get("controlPos1", {}) or {}
                cp2 = path.get("controlPos2", {}) or {}
                pp = QPainterPath(QPointF(sp_x, sp_y))
                pp.cubicTo(
                    QPointF(cp1.get("x", 0) * SCALE_DISTANCE, cp1.get("y", 0) * SCALE_DISTANCE),
                    QPointF(cp2.get("x", 0) * SCALE_DISTANCE, cp2.get("y", 0) * SCALE_DISTANCE),
                    QPointF(ep_x, ep_y),
                )
                item = self._scene.addPath(
                    pp,
                    QPen(QColor("#994d4d"), 3, Qt.SolidLine),
                    QBrush(QColor(255, 255, 255, 0)),
                )
                item.setZValue(5)
                item.setToolTip(f"{path.get('instanceName', '')} ({class_name})\n"
                                 f"({sp_pos.get('x', 0):.2f}, {sp_pos.get('y', 0):.2f})"
                                 f" → ({ep_pos.get('x', 0):.2f}, {ep_pos.get('y', 0):.2f})")
                self._path_items.append(item)

        # 6) AGV 当前位置（Z=3，会画在普通点 Z=0 之上、站点 Z=2 之上）
        agv = self._last_agv
        if agv:
            self._create_agv_item(agv[0], agv[1], agv[2])
        else:
            self._create_agv_item(0, 0, 0)

        # 状态信息
        self.lbl_points.setText(
            f"普通点: {len(self._normal_items)}  |  站点: {len(self._station_items)}"
        )
        self.lbl_status.setText(
            f"地图: {self._current_map}  |  Ctrl+滚轮: 缩放  |  拖拽: 平移"
        )

        # 注意：不在这里调 _on_fit()，因为 _agv_item 此时还在 (0,0) 占位，
        # 居中计算会拿到错误结果。应在外部 update_agv_pose(...) 之后再 fit。

    def _create_agv_item(self, x: float, y: float, angle_deg: float):
        """创建 AGV 图元（CurrentAGVItem，Z=3）"""
        agv = CurrentAGVItem()
        agv.setPos(QPointF(x * SCALE_DISTANCE, y * SCALE_DISTANCE))
        agv.set_agv_angle(angle_deg)  # 用 setTransform 绕 PySide6 setRotation bug
        self._scene.addItem(agv)
        self._agv_item = agv

    # ------------------------------------------------------------------
    # Slots
    # ------------------------------------------------------------------

    def _apply_zoom(self, slider_value: int):
        """slider 缩放 —— 公式参考 SeerAgv view.cpp: qPow(2, (val-70)/15)
        70 是中点 → scale=1；向左 scale<1 缩小，向右 scale>1 放大
        默认 slider=30 → scale=2^((30-70)/15)=0.157（与 SeerAgv 一致）

        Hybrid 模式:
          - 用户拖 slider 只改 scale,保持当前视图中心不变
          - "自适应"按钮才把 AGV 拉到屏幕中心 + 5m×5m 视野
          - 这样用户可以先按"自适应"看 AGV,再拖 slider 自由缩放不重置位置

        公式:旧 transform 是 (old_s, 0, 0, -old_s, old_dx, old_dy),
              屏幕 x = old_s*x + old_dx, 屏幕 y = -old_s*y + old_dy
              视图中心 (vw/2, vh/2) 对应世界点 (cx, cy):
                cx = (vw/2 - old_dx) / old_s
                cy = -(vh/2 - old_dy) / old_s
              缩放后保持 (cx, cy) 仍在屏幕中心:
                new_dx = vw/2 - new_s * cx
                new_dy = vh/2 + new_s * cy
        """
        new_scale = _slider_to_scale(slider_value)

        viewport = self._view.viewport().size()
        vw, vh = viewport.width(), viewport.height()
        if vw <= 0 or vh <= 0:
            return

        # 取当前 transform,从中读出 (old_dx, old_dy, old_s)
        old_t = self._view.transform()
        old_s = old_t.m11()  # m11 = 水平 scale
        old_dx = old_t.dx()
        old_dy = old_t.dy()

        # 视图中心对应的世界坐标 (注意:view scale(1,-1) 翻 Y,cy 计算时也要翻)
        if abs(old_s) < 1e-9:
            cx, cy = 0.0, 0.0
        else:
            cx = (vw / 2.0 - old_dx) / old_s
            cy = -(vh / 2.0 - old_dy) / old_s

        # 缩放后保持 (cx, cy) 仍在屏幕中心
        new_dx = vw / 2.0 - new_scale * cx
        new_dy = vh / 2.0 + new_scale * cy
        t = QTransform(new_scale, 0, 0, -new_scale, new_dx, new_dy)
        self._view.setTransform(t)
        # 同步给 CoordinateAxisItem 用于反缩放
        CoordinateAxisItem.scale_axis = new_scale

    def _on_fit(self):
        """以 AGV 为中心聚焦（视野 = AGV 周围 ±FIT_VIEW_RADIUS_M 米）

        需求（来自用户）：
          - AGV 永远在视图中心
          - 视野大小固定（5m × 5m = 周围 ±2.5m）
          - zoom slider 仍可独立调整（用户手动缩放不会被重置）

        与之前的"装下整张地图"算法的差别：
          - 之前用 scene.itemsBoundingRect() 算 scale → 整张地图挤进屏幕
          - 现在用 FIT_VIEW_RADIUS_M * 2 算 scale → 永远聚焦 AGV 周围固定范围

        屏幕坐标公式（view 保持 scale(1,-1) 翻 Y，SeerAgv view.cpp:208）：
          屏幕 x = scale*x + dx
          屏幕 y = -scale*y + dy
        目标：scale*agv.x + dx = vw/2, -scale*agv.y + dy = vh/2
        """
        viewport = self._view.viewport().size()
        vw, vh = viewport.width(), viewport.height()
        if vw <= 0 or vh <= 0:
            return

        # === 1. scale：让 FIT_VIEW_RADIUS_M 范围刚好覆盖屏幕短边 ===
        #   视野直径 = 2 * FIT_VIEW_RADIUS_M 米
        #   scale = vw / (2 * R * SCALE_DISTANCE) 或 vh / (...) 取小,
        #   保证 5m × 5m 完全可见（短边不超界）
        view_diameter_m = 2.0 * FIT_VIEW_RADIUS_M
        scale_w = vw / (view_diameter_m * SCALE_DISTANCE)
        scale_h = vh / (view_diameter_m * SCALE_DISTANCE)
        scale = min(scale_w, scale_h)

        # === 2. AGV 中心（无 AGV 数据时退回地图中心）===
        if self._last_agv is not None:
            ax, ay, _ = self._last_agv
            agv_x, agv_y = ax * SCALE_DISTANCE, ay * SCALE_DISTANCE
        else:
            rect = self._scene.itemsBoundingRect()
            if rect.isEmpty():
                agv_x, agv_y = 0.0, 0.0
            else:
                agv_x, agv_y = rect.center().x(), rect.center().y()

        # === 3. 构造 transform：scale(1,-1) 翻 Y + 平移让 AGV 居中 ===
        t = QTransform(scale, 0, 0, -scale,
                       vw / 2.0 - scale * agv_x,
                       vh / 2.0 + scale * agv_y)
        self._view.setTransform(t)

        # === 4. 同步 zoom slider 到当前 scale（hybrid 模式：用户拖 slider 后
        #       调 _on_fit 会重置 slider,这是符合预期的）===
        slider_val = _scale_to_slider(scale)
        self.zoom_slider.blockSignals(True)
        self.zoom_slider.setValue(max(0, min(100, slider_val)))
        self.zoom_slider.blockSignals(False)
        CoordinateAxisItem.scale_axis = scale

    def _on_refresh(self):
        """刷新按钮 → 让外部重新拉地图数据"""
        self.refresh_clicked.emit()
