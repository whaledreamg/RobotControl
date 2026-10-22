#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QMetaType>

// AGV 状态
struct AgvState
{
    double x = 0.0;          // x 坐标 (m)
    double y = 0.0;          // y 坐标 (m)
    double theta = 0.0;      // 朝向 (rad)
    double linearVel = 0.0;  // 线速度
    double angularVel = 0.0; // 角速度
    int battery = 0;         // 电量百分比 0-100
    QString status;          // 状态描述
    QString currentStation;  // 当前站点
    QString targetStation;   // 目标站点

    QJsonObject toJson() const;
    static AgvState fromJson(const QJsonObject &obj);
};

// 机械臂状态
struct ArmState
{
    QString armName;                       // LeftArm / RightArm
    bool enabled = false;                  // 使能状态
    double speedOverride = 0.0;            // 速度倍率 0-1
    QVector<double> jointAngles;           // 关节角 (6 个)
    QVector<double> endPosition;           // 末端位姿 (x,y,z,rx,ry,rz)
    QString projectName;                   // 当前工程
    QString projectStatus;                 // 工程状态
    QString errorMessage;                  // 错误信息

    QJsonObject toJson() const;
    static ArmState fromJson(const QString &armName, const QJsonObject &obj);
};

// 夹爪状态
struct GripperState
{
    QString armName;
    int force = 0;                // 力度 0-100
    QString status;               // open / closed / stopped / moving
    double position = 0.0;        // 开度 0-100 (%)

    QJsonObject toJson() const;
};

// 升降柱状态
struct LiftingColumnState
{
    int height = 0;               // 高度 (mm)
    int upperLimit = 0;           // 上限位
    int lowerLimit = 0;           // 下限位
    bool atUpperLimit = false;
    bool atLowerLimit = false;
    QString status;               // idle / moving / fault

    QJsonObject toJson() const;
};

// 连接状态
enum class ConnectStatus {
    Disconnected,
    Connecting,
    Connected,
    Error
};

// 设备类型
enum class DeviceType {
    AGV,
    LeftArm,
    RightArm
};

// 工程控制命令
enum class ProjectCommand {
    Run,
    Pause,
    Resume,
    Stop
};

// 夹爪控制命令
enum class GripperCommand {
    Open,
    Close,
    Stop
};

// 升降柱控制命令
enum class LiftingColumnCommand {
    Up,
    Down,
    Stop,
    Reset
};

// 方向控制
enum class DirectionCommand {
    Stop,
    Forward,
    Backward,
    TurnLeft,
    TurnRight
};

Q_DECLARE_METATYPE(AgvState)
Q_DECLARE_METATYPE(ArmState)
Q_DECLARE_METATYPE(GripperState)
Q_DECLARE_METATYPE(LiftingColumnState)

#endif // COMMON_TYPES_H
