#ifndef COMMUNICATION_UTIL_H
#define COMMUNICATION_UTIL_H

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>
#include <QVector>

/// 安全转换 QVariant → QJsonObject
static inline QJsonObject varToObj(const QVariant &v)
{
    if (!v.isValid()) return {};
    if (v.canConvert<QJsonObject>()) return v.value<QJsonObject>();
    return v.toJsonValue().toObject();
}

/// 安全转换 QVariant → QJsonArray
static inline QJsonArray varToArr(const QVariant &v)
{
    if (!v.isValid()) return {};
    if (v.canConvert<QJsonArray>()) return v.value<QJsonArray>();
    return v.toJsonValue().toArray();
}

/// 安全转换 QJsonArray → QVector<QJsonObject>
static inline QVector<QJsonObject> jsonArrayToObjectVector(const QJsonArray &arr)
{
    QVector<QJsonObject> out;
    out.reserve(arr.size());
    for (const QJsonValue &v : arr)
        out.append(v.toObject());
    return out;
}

#endif // COMMUNICATION_UTIL_H
