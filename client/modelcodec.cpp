#include "modelcodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

QString modelcodec::deviceParamsToJson(const QMap<QString, DeviceParams> &params)
{
    QJsonArray arr;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        const DeviceParams &dp = it.value();
        QJsonObject obj;
        // key
        obj["instance_key"]        = it.key();
        // 1. 基础公共属性
        obj["instance_id"]         = dp.instanceId;
        obj["instance_name"]       = dp.instanceName;
        obj["device_role"]         = dp.deviceRole;
        obj["device_category"]     = dp.deviceCategory;
        obj["io_role"]             = dp.ioRole;
        obj["max_connections"]     = dp.maxConnections;
        obj["device_height_m"]     = dp.deviceHeightM;
        obj["max_bandwidth_bps"]   = dp.maxBandwidthBps;
        // 2. 无线参数
        obj["freq_hz"]             = dp.freqHz;
        obj["tx_power_dbm"]        = dp.txPowerDbm;
        obj["rx_sensitivity_dbm"]  = dp.rxSensitivityDbm;
        obj["tx_antenna_gain_dbi"] = dp.txAntennaGainDbi;
        obj["rx_antenna_gain_dbi"] = dp.rxAntennaGainDbi;
        obj["noise_figure_db"]     = dp.noiseFigureDb;
        obj["snr_threshold_db"]    = dp.snrThresholdDb;
        obj["path_loss_exponent"]  = dp.pathLossExponent;
        obj["additional_loss_db"]  = dp.additionalLossDb;
        // 3. 有线参数
        obj["fiber_attenuation_db_per_km"] = dp.fiberAttenuationDbPerKm;
        obj["connector_loss_db"]   = dp.connectorLossDb;
        obj["comm_protocol"]       = dp.commProtocol;
        obj["device_type"]         = dp.deviceType;
        // 4. 交换机参数
        obj["backplane_bandwidth_bps"] = dp.backplaneBandwidthBps;
        obj["processing_delay_us"]     = dp.processingDelayUs;
        arr.append(obj);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QMap<QString, DeviceParams> modelcodec::jsonToDeviceParams(const QString &json)
{
    QMap<QString, DeviceParams> result;
    if (json.isEmpty()) return result;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());

    // 支持新格式（数组）和旧格式（对象）两种讻入，保证向前兼容
    if (doc.isArray()) {
        for (const QJsonValue &v : doc.array()) {
            QJsonObject obj = v.toObject();
            QString key = obj["instance_key"].toString();
            if (key.isEmpty()) continue;
            DeviceParams dp;
            dp.instanceId            = obj["instance_id"].toInt(0);
            dp.instanceName          = obj["instance_name"].toString();
            dp.deviceRole            = obj["device_role"].toString("communication");
            dp.deviceCategory        = obj["device_category"].toString("wireless");
            dp.ioRole                = obj["io_role"].toString("normal");
            dp.maxConnections        = obj["max_connections"].toInt(4);
            dp.deviceHeightM         = obj["device_height_m"].toDouble(0.0);
            dp.maxBandwidthBps       = obj["max_bandwidth_bps"].toDouble(100e6);
            dp.freqHz                = obj["freq_hz"].toDouble(3e9);
            dp.txPowerDbm            = obj["tx_power_dbm"].toDouble(30.0);
            dp.rxSensitivityDbm      = obj["rx_sensitivity_dbm"].toDouble(-90.0);
            dp.txAntennaGainDbi      = obj["tx_antenna_gain_dbi"].toDouble(0.0);
            dp.rxAntennaGainDbi      = obj["rx_antenna_gain_dbi"].toDouble(0.0);
            dp.noiseFigureDb         = obj["noise_figure_db"].toDouble(7.0);
            dp.snrThresholdDb        = obj["snr_threshold_db"].toDouble(10.0);
            dp.pathLossExponent      = obj["path_loss_exponent"].toDouble(2.0);
            dp.additionalLossDb      = obj["additional_loss_db"].toDouble(0.0);
            dp.fiberAttenuationDbPerKm = obj["fiber_attenuation_db_per_km"].toDouble(0.2);
            dp.connectorLossDb       = obj["connector_loss_db"].toDouble(1.0);
            dp.commProtocol          = obj["comm_protocol"].toString();
            dp.deviceType            = obj["device_type"].toString();
            dp.backplaneBandwidthBps = obj["backplane_bandwidth_bps"].toDouble(1000e6);
            dp.processingDelayUs     = obj["processing_delay_us"].toDouble(50.0);
            result.insert(key, dp);
        }
    } else if (doc.isObject()) {
        // 旧格式兼容读入
        QJsonObject root = doc.object();
        for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
            QJsonObject obj = it.value().toObject();
            DeviceParams dp;
            dp.instanceId       = 0;
            dp.instanceName     = it.key();
            dp.deviceRole       = "communication";
            dp.deviceCategory   = obj.contains("freq_hz") ? "wireless" : "wired";
            dp.maxBandwidthBps  = 100e6;
            dp.freqHz             = obj["freq_hz"].toDouble(3e9);
            dp.txPowerDbm         = obj["tx_power_dbm"].toDouble(30.0);
            dp.rxSensitivityDbm   = obj["rx_sensitivity_dbm"].toDouble(-90.0);
            dp.txAntennaGainDbi   = obj["tx_antenna_gain_dbi"].toDouble(0.0);
            dp.rxAntennaGainDbi   = obj["rx_antenna_gain_dbi"].toDouble(0.0);
            dp.noiseFigureDb      = obj["noise_figure_db"].toDouble(7.0);
            dp.snrThresholdDb     = obj["snr_threshold_db"].toDouble(10.0);
            dp.pathLossExponent   = obj["path_loss_exponent"].toDouble(2.0);
            dp.additionalLossDb   = obj["additional_loss_db"].toDouble(0.0);
            dp.commProtocol       = obj["comm_protocol"].toString();
            dp.deviceType         = obj["device_type"].toString();
            result.insert(it.key(), dp);
        }
    }
    return result;
}

QString modelcodec::deviceConnsToJson(const QList<QPair<QString,QString>> &conns)
{
    QJsonArray arr;
    for (const QPair<QString,QString> &p : conns) {
        QJsonArray pair;
        pair.append(p.first);
        pair.append(p.second);
        arr.append(pair);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QList<QPair<QString,QString>> modelcodec::jsonToDeviceConns(const QString &json)
{
    QList<QPair<QString,QString>> result;
    if (json.isEmpty()) return result;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray()) return result;
    for (const QJsonValue &v : doc.array()) {
        QJsonArray pair = v.toArray();
        if (pair.size() >= 2)
            result.append({pair[0].toString(), pair[1].toString()});
    }
    return result;
}

QString modelcodec::methodsToJson(const QStringList &methods)
{
    QJsonArray arr;
    for (const QString &m : methods) arr.append(m);
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QStringList modelcodec::jsonToMethods(const QString &json)
{
    QStringList result;
    if (json.isEmpty()) return result;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isArray())
        for (const QJsonValue &v : doc.array()) result << v.toString();
    return result;
}

QString modelcodec::flowsToJson(const QList<LinkFlow> &flows)
{
    QJsonArray arr;
    for (const LinkFlow &f : flows) {
        QJsonObject obj;
        obj["flow_id"] = f.flowId;
        obj["bandwidth_bps"] = f.bandwidthBps;
        obj["description"] = f.description;
        arr.append(obj);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QList<LinkFlow> modelcodec::jsonToFlows(const QString &json)
{
    QList<LinkFlow> result;
    if (json.isEmpty()) return result;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isArray()) {
        for (const QJsonValue &v : doc.array()) {
            QJsonObject obj = v.toObject();
            LinkFlow f;
            f.flowId = obj["flow_id"].toInt();
            f.bandwidthBps = obj["bandwidth_bps"].toDouble();
            f.description = obj["description"].toString();
            result << f;
        }
    }
    return result;
}
