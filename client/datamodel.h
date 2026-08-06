#ifndef DATAMODEL_H
#define DATAMODEL_H

#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QJsonArray>

// ─── 场景 ────────────────────────────────────────────────────────────────────
struct SceneInfo {
    int     id          = -1;
    QString name;
    QString description;
    QString sceneType;
    QString createTime;
};

// ─── 通信设备参数（每个节点每个设备独立存储参数）────────────────────────────
struct DeviceParams {
    // ── 1. 基础公共属性 ───────────────────────────────────────────────────────
    int     instanceId         = 0;                // 设备在该节点内的实例编号，从 0 开始递增
    QString instanceName;                          // 设备实例显示名称（如"光缆_01"），可由用户修改
    QString deviceRole         = "communication";  // "communication"(通信设备) / "switch"(交换设备)
    QString deviceCategory     = "wireless";       // "wireless"(无线) / "wired"(有线)
    QString ioRole             = "normal";         // "normal"(普通) / "input"(输入端) / "output"(输出端)，决定节点内设备流向
    int     maxConnections     = 4;                // 该设备最大连接数（限制节点内设备间连线数量）
    double  deviceHeightM      = 0.0;              // 设备架设高度 (m)，所有设备通用
    double  maxBandwidthBps    = 100e6;            // 物理层速率上限 (bps)，通信设备专用

    // ── 2. 无线通信设备专用参数 (deviceRole=="communication" && deviceCategory=="wireless") ──
    double  freqHz             = 3e9;
    double  txPowerDbm         = 30.0;
    double  rxSensitivityDbm   = -90.0;
    double  txAntennaGainDbi   = 0.0;
    double  rxAntennaGainDbi   = 0.0;
    double  noiseFigureDb      = 7.0;
    double  snrThresholdDb     = 10.0;
    double  pathLossExponent   = 2.0;
    double  additionalLossDb   = 0.0;

    // ── 3. 有线通信设备专用参数 (deviceCategory=="wired" && deviceRole=="communication") ──
    // txPowerDbm / rxSensitivityDbm 复用上述字段（光发射功率 / 光接收灵敏度）
    double  fiberAttenuationDbPerKm = 0.2;         // 光纤每公里衰减系数 (dB/km)
    double  connectorLossDb        = 1.0;          // 接头/熔接总损耗 (dB)
    QString commProtocol;                          // 通信体制/协议（如以太网）
    QString deviceType;                            // 设备型号

    // ── 4. 交换设备专用参数 (deviceRole=="switch") ──────────────────────────
    double  backplaneBandwidthBps  = 1000e6;       // 背板带宽 (bps)
    double  processingDelayUs      = 50.0;         // 固定处理时延 (微秒)
};

// ─── 节点实例 ─────────────────────────────────────────────────────────────────
struct NodeInfo {
    int     id           = -1;   // 数据库主键 scene_nodes.id
    int     sceneId      = -1;
    int     nodeId       = 0;    // 用户设定的仿真节点编号
    int     fromTemplate = -1;   // -1 表示未从模板创建
    QString name;
    QString nodeType;            // 支线 / 干线
    QString status;              // 在线 / 离线 / 告警
    double  longitude    = 0.0;
    double  latitude     = 0.0;
    double  altitude     = 0.0;
    double  interferenceDb = 0.0;
    QStringList commMethods;                         // 设备实例 key 列表（如 ["fiber_00","switch_00"]）
    QMap<QString, DeviceParams> deviceParams;        // 设备实例 key → 设备参数
    QList<QPair<QString, QString>> deviceConnections; // 节点内设备间连接对 [("fiber_00","switch_00"), ...]
};

// ─── 设备实例 key 辅助函数 ───────────────────────────────────────────────────
// 设备实例 key 形如 "fiber_00"、"switch_01"。以下辅助函数在“实例 key”与
// “设备类型前缀”之间转换，并兼容旧数据（直接是类型名，如 "fiber"）。

// 取实例 key 的类型前缀：仅当 "_" 后是纯数字时才剥离（"fiber_00"→"fiber"；"fiber"→"fiber"）
inline QString deviceTypeOfKey(const QString &key)
{
    int idx = key.lastIndexOf('_');
    if (idx > 0) {
        bool ok = false;
        key.mid(idx + 1).toInt(&ok);
        if (ok) return key.left(idx);
    }
    return key;
}

// 判断节点是否具备某通信类型能力（按类型前缀匹配，fieldwire 兼容大小写）
inline bool nodeHasCapability(const QStringList &methods, const QString &type)
{
    for (const QString &k : methods) {
        QString t = deviceTypeOfKey(k);
        if (t.compare(type, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

// 将实例 key 列表去重折叠为类型名列表（["fiber_00","fiber_01","switch_00"]→["fiber","switch"]）
inline QStringList deviceTypesOf(const QStringList &methods)
{
    QStringList out;
    for (const QString &k : methods) {
        QString t = deviceTypeOfKey(k);
        if (!out.contains(t)) out << t;
    }
    return out;
}

// ─── 链路流信息（Python网络推演返回的流信息）────────────────────────────────
struct LinkFlow {
    int     flowId       = 0;
    double  bandwidthBps = 0.0;
    QString description;
};

// ─── 链路实例 ─────────────────────────────────────────────────────────────────
struct LinkInfo {
    int     id           = -1;   // 数据库主键 scene_links.id
    int     sceneId      = -1;
    int     fromTemplate = -1;
    int     src          = 0;    // 对应 scene_nodes.node_id
    int     dst          = 0;
    QString linkType;            // wired / wireless
    QString wirelessType;        // microwave/scatter/adhoc/satellite/4G5G/shortwave
    double  bandwidthBps = 50e6;
    double  propDelayS   = 0.0;  // 0 表示需自动计算
    // ── 无线专属 ──
    double  freqHz             = 3e9;
    double  txPowerDbm         = 30.0;
    double  rxSensitivityDbm   = -90.0;
    double  txAntennaGainDbi   = 0.0;
    double  rxAntennaGainDbi   = 0.0;
    double  noiseFigureDb      = 7.0;
    double  snrThresholdDb     = 10.0;
    double  pathLossExponent   = 2.0;
    double  additionalLossDb   = 0.0;
    // ── 有线专属 ──
    QString commProtocol;
    QString deviceType;
    // ── Python网络推演返回的流信息 ──
    QList<LinkFlow> flows;
};

// ─── 节点模板 ─────────────────────────────────────────────────────────────────
struct NodeTemplate {
    int     id           = -1;
    QString name;
    QString nodeType;
    QString description;
    QStringList commMethods;                       // 设备实例 key 列表（多实例格式）
    QMap<QString, DeviceParams> deviceParams;      // 设备实例 key → 设备参数（与 NodeInfo 一致）
    double  defaultInterferenceDb = 0.0;
};

// ─── 链路模板 ─────────────────────────────────────────────────────────────────
struct LinkTemplate {
    int     id             = -1;
    QString name;
    QString linkType;
    QString wirelessType;
    QString description;
    double  bandwidthBps       = 50e6;
    double  freqHz             = 3e9;
    double  txPowerDbm         = 30.0;
    double  rxSensitivityDbm   = -90.0;
    double  txAntennaGainDbi   = 0.0;
    double  rxAntennaGainDbi   = 0.0;
    double  noiseFigureDb      = 7.0;
    double  snrThresholdDb     = 10.0;
    double  pathLossExponent   = 2.0;
    double  additionalLossDb   = 0.0;
};

// ─── 规划流（纯内存，不入库）────────────────────────────────────────────────
struct FlowInfo {
    int     fid          = 0;    // 流编号
    int     src          = 0;    // 仿真 node_id
    int     dst          = 0;
    double  rateBps      = 8e6;  // 带宽需求（bps）
    double  arrivalT     = 0.0;
    double  durT         = 30.0;
    int     qosLevel     = 0;
    int     kPaths       = 3;
    QString optTarget    = "performance";  // "performance" / "resource" / "min_change"
};

// ─── v2.0 新增：告警条目（plan_result.warns[]）────────────────────────────────
struct WarnItem {
    QString level;        // "info" / "warning" / "error"
    QString message;
    QString warnType;     // "unsatisfied_plan" / "link_overload" / "general"
    int     relatedFid = -1;
};

// ─── v2.0 新增：规划路径结果（plan_result.plan_results[]）────────────────────
struct PlanResult {
    int         fid                = 0;
    int         srcNodeId          = 0;
    int         dstNodeId          = 0;
    bool        isSatisfied        = false;
    QList<int>  pathNodes;          // 路由节点 node_id 序列
    QList<int>  pathLinks;          // 路由链路 link_id 序列
    int         hops               = -1;
    double      actualBandwidthBps = 0.0;
    QString     unsatisfiedReason;
};

// ─── v2.0 新增：链路承载流（plan_result.links[i].flows[]）────────────────────
struct LinkResultFlow {
    int     fid          = 0;
    double  bandwidthBps = 0.0;
    QString description;
};

// ─── v2.0 新增：Python 返回的拓扑链路（plan_result.links[]）─────────────────
struct LinkResult {
    int     linkId       = 0;
    int     srcNodeId    = 0;
    int     dstNodeId    = 0;
    QString linkType;            // "wired" / "wireless"
    QString wirelessType;        // "microwave" / "adhoc" 等，有线时为 ""
    double  bandwidthBps = 0.0;
    double  propDelayS   = 0.0;
    QList<LinkResultFlow> flows;
};

// ─── v2.0 新增：plan_result 顶层数据包 ───────────────────────────────────────
struct PlanResultPacket {
    QList<LinkResult>  links;
    QList<PlanResult>  planResults;
    QList<WarnItem>    warns;
};

#endif // DATAMODEL_H
