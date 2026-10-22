#ifndef COMMON_GLOBAL_H
#define COMMON_GLOBAL_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaEnum>
#include "types.h"

namespace Global {

// JSON 字段名集中管理 (避免拼写错误)
namespace JsonKey {
inline constexpr const char* Action       = "ty";
inline constexpr const char* Id           = "id";
inline constexpr const char* Data         = "db";
inline constexpr const char* Status       = "status";
inline constexpr const char* Error        = "error";
inline constexpr const char* Message      = "message";
}

// 字符串转换辅助
QString connectStatusToString(ConnectStatus status);
QString directionToString(DirectionCommand dir);
QString projectCommandToString(ProjectCommand cmd);
QString gripperCommandToString(GripperCommand cmd);
QString liftingColumnCommandToString(LiftingColumnCommand cmd);

} // namespace Global

// 类型注册辅助
void registerCommonMetaTypes();

#endif // COMMON_GLOBAL_H
