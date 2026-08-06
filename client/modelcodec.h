#ifndef MODELCODEC_H
#define MODELCODEC_H

// ─────────────────────────────────────────────────────────────────────────────
// modelcodec —— 数据模型 ↔ JSON 字符串
//
// 几个复合字段（设备参数、设备间连线、通信方式、流量）在库里是以 JSON 文本
// 存在单列里的，读写两侧都要做同样的编解码。
//
// P6 之前这些函数是 DbManager 的私有静态成员。加了 RemoteDataStore 之后
// 两个实现都要用：让 RemoteDataStore 去 include DbManager 只为了调几个
// 纯函数是不合理的依赖 —— 它根本不需要知道有个直连数据库的实现存在。
// 所以抽成自由函数，两边平等地依赖它。
//
// 编码格式没有变，与改造前的库里数据完全兼容。
// ─────────────────────────────────────────────────────────────────────────────

#include "datamodel.h"

#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>

namespace modelcodec {

QString     methodsToJson(const QStringList &methods);
QStringList jsonToMethods(const QString &json);

QString                     deviceParamsToJson(const QMap<QString, DeviceParams> &params);
QMap<QString, DeviceParams> jsonToDeviceParams(const QString &json);

QString                        deviceConnsToJson(const QList<QPair<QString,QString>> &conns);
QList<QPair<QString,QString>>  jsonToDeviceConns(const QString &json);

QString         flowsToJson(const QList<LinkFlow> &flows);
QList<LinkFlow> jsonToFlows(const QString &json);

}  // namespace modelcodec

#endif // MODELCODEC_H
