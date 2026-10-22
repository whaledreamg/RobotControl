#include "global.h"
#include "types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonDocument>

namespace Global {

QString connectStatusToString(ConnectStatus status)
{
    switch (status) {
    case ConnectStatus::Disconnected: return QStringLiteral("未连接");
    case ConnectStatus::Connecting:   return QStringLiteral("连接中");
    case ConnectStatus::Connected:    return QStringLiteral("已连接");
    case ConnectStatus::Error:        return QStringLiteral("连接错误");
    }
    return QStringLiteral("未知");
}

QString directionToString(DirectionCommand dir)
{
    switch (dir) {
    case DirectionCommand::Stop:      return QStringLiteral("停止");
    case DirectionCommand::Forward:   return QStringLiteral("前进");
    case DirectionCommand::Backward:  return QStringLiteral("后退");
    case DirectionCommand::TurnLeft:  return QStringLiteral("左转");
    case DirectionCommand::TurnRight: return QStringLiteral("右转");
    }
    return QStringLiteral("未知");
}

QString projectCommandToString(ProjectCommand cmd)
{
    switch (cmd) {
    case ProjectCommand::Run:    return QStringLiteral("运行");
    case ProjectCommand::Pause:  return QStringLiteral("暂停");
    case ProjectCommand::Resume: return QStringLiteral("继续");
    case ProjectCommand::Stop:   return QStringLiteral("停止");
    }
    return QStringLiteral("未知");
}

QString gripperCommandToString(GripperCommand cmd)
{
    switch (cmd) {
    case GripperCommand::Open:  return QStringLiteral("打开");
    case GripperCommand::Close: return QStringLiteral("闭合");
    case GripperCommand::Stop:  return QStringLiteral("停止");
    }
    return QStringLiteral("未知");
}

QString liftingColumnCommandToString(LiftingColumnCommand cmd)
{
    switch (cmd) {
    case LiftingColumnCommand::Up:    return QStringLiteral("上升");
    case LiftingColumnCommand::Down:  return QStringLiteral("下降");
    case LiftingColumnCommand::Stop:  return QStringLiteral("停止");
    case LiftingColumnCommand::Reset: return QStringLiteral("复位");
    }
    return QStringLiteral("未知");
}

} // namespace Global

// 类型实现
QJsonObject AgvState::toJson() const
{
    QJsonObject obj;
    obj["x"] = x;
    obj["y"] = y;
    obj["theta"] = theta;
    obj["linearVel"] = linearVel;
    obj["angularVel"] = angularVel;
    obj["battery"] = battery;
    obj["status"] = status;
    obj["currentStation"] = currentStation;
    obj["targetStation"] = targetStation;
    return obj;
}

AgvState AgvState::fromJson(const QJsonObject &obj)
{
    AgvState s;
    s.x = obj.value("x").toDouble();
    s.y = obj.value("y").toDouble();
    s.theta = obj.value("theta").toDouble();
    s.linearVel = obj.value("linearVel").toDouble();
    s.angularVel = obj.value("angularVel").toDouble();
    s.battery = obj.value("battery").toInt();
    s.status = obj.value("status").toString();
    s.currentStation = obj.value("currentStation").toString();
    s.targetStation = obj.value("targetStation").toString();
    return s;
}

QJsonObject ArmState::toJson() const
{
    QJsonObject obj;
    obj["armName"] = armName;
    obj["enabled"] = enabled;
    obj["speedOverride"] = speedOverride;

    QJsonArray jointArr;
    for (double v : jointAngles) {
        jointArr.append(v);
    }
    obj["jointAngles"] = jointArr;

    QJsonArray posArr;
    for (double v : endPosition) {
        posArr.append(v);
    }
    obj["endPosition"] = posArr;

    obj["projectName"] = projectName;
    obj["projectStatus"] = projectStatus;
    obj["errorMessage"] = errorMessage;
    return obj;
}

ArmState ArmState::fromJson(const QString &armName, const QJsonObject &obj)
{
    ArmState s;
    s.armName = armName;
    s.enabled = obj.value("enabled").toBool();
    s.speedOverride = obj.value("speedOverride").toDouble();

    QJsonArray joints = obj.value("jointAngles").toArray();
    for (const QJsonValue &v : joints) {
        s.jointAngles.append(v.toDouble());
    }

    QJsonArray pos = obj.value("endPosition").toArray();
    for (const QJsonValue &v : pos) {
        s.endPosition.append(v.toDouble());
    }

    s.projectName = obj.value("projectName").toString();
    s.projectStatus = obj.value("projectStatus").toString();
    s.errorMessage = obj.value("errorMessage").toString();
    return s;
}

QJsonObject GripperState::toJson() const
{
    QJsonObject obj;
    obj["armName"] = armName;
    obj["force"] = force;
    obj["status"] = status;
    obj["position"] = position;
    return obj;
}

QJsonObject LiftingColumnState::toJson() const
{
    QJsonObject obj;
    obj["height"] = height;
    obj["upperLimit"] = upperLimit;
    obj["lowerLimit"] = lowerLimit;
    obj["atUpperLimit"] = atUpperLimit;
    obj["atLowerLimit"] = atLowerLimit;
    obj["status"] = status;
    return obj;
}

void registerCommonMetaTypes()
{
    qRegisterMetaType<AgvState>("AgvState");
    qRegisterMetaType<ArmState>("ArmState");
    qRegisterMetaType<GripperState>("GripperState");
    qRegisterMetaType<LiftingColumnState>("LiftingColumnState");
    qRegisterMetaType<QVector<double>>("QVector<double>");
}
