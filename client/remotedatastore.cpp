#include "remotedatastore.h"

#include "modelcodec.h"
#include "thgh/message.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>

namespace {

// 协议常量一律取自共享层，不写字面量（理由见 protocol/thgh/message.h）
inline QString protoStr(std::string_view sv)
{
    return QString::fromUtf8(sv.data(), static_cast<int>(sv.size()));
}
inline QString typeName(thgh::MessageType t)
{
    return protoStr(thgh::toString(t));
}

} // namespace

RemoteDataStore::RemoteDataStore(QObject *parent) : QObject(parent)
{
    m_socket = new QTcpSocket(this);
}

RemoteDataStore::~RemoteDataStore()
{
    disconnectFromServer();
}

void RemoteDataStore::setServer(const QString &host, quint16 port)
{
    m_host = host;
    m_port = port;
}

bool RemoteDataStore::connectToServer(int timeoutMs)
{
    if (m_socket->state() == QAbstractSocket::ConnectedState)
        return true;

    m_buf.clear();
    m_socket->abort();
    m_socket->connectToHost(m_host, m_port);
    if (!m_socket->waitForConnected(timeoutMs)) {
        m_lastError = QString("连接数据服务失败（%1:%2）：%3")
                          .arg(m_host).arg(m_port).arg(m_socket->errorString());
        return false;
    }
    // 数据请求都是小报文、要求低延迟，关掉 Nagle 避免内核攒批
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_lastError.clear();
    return true;
}

void RemoteDataStore::disconnectFromServer()
{
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
        if (m_socket->state() != QAbstractSocket::UnconnectedState)
            m_socket->waitForDisconnected(500);
    }
    m_buf.clear();
}

bool RemoteDataStore::isReady() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

QString RemoteDataStore::sourceDescription() const
{
    return QString("%1:%2").arg(m_host).arg(m_port);
}

bool RemoteDataStore::ensureConnected()
{
    if (isReady())
        return true;
    // 服务端重启或网络瞬断之后自动重连一次，不该要求用户手动点。
    // 只试一次：连不上就是真连不上，反复重试只会把超时叠加起来。
    return connectToServer(m_timeoutMs);
}

// ─────────────────────────────────────────────────────────────────────────────
// 同步请求-应答
// ─────────────────────────────────────────────────────────────────────────────

bool RemoteDataStore::request(const QString &op, const QJsonObject &args,
                              QJsonObject &dataOut)
{
    if (!ensureConnected())
        return false;   // m_lastError 已由 connectToServer 填好

    const quint64 taskId = ++m_nextTaskId;

    QJsonObject payload;
    payload[protoStr(thgh::field::data::kOp)]   = op;
    payload[protoStr(thgh::field::data::kArgs)] = args;

    QJsonObject msg;
    msg[protoStr(thgh::field::kType)]    = typeName(thgh::MessageType::DataRequest);
    msg[protoStr(thgh::field::kTaskId)]  = static_cast<qint64>(taskId);
    msg[protoStr(thgh::field::kPayload)] = payload;

    const QByteArray line =
        QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
    if (m_socket->write(line) != line.size() || !m_socket->waitForBytesWritten(m_timeoutMs)) {
        m_lastError = QString("发送请求失败：%1").arg(m_socket->errorString());
        disconnectFromServer();
        return false;
    }

    // ── 等回复 ──────────────────────────────────────────────────────────
    // 整体超时用 QElapsedTimer 计，而不是每次 waitForReadyRead 都传满超时值 ——
    // 否则对端每隔 4.9 秒吐一个字节就能让我们永远等下去。
    QElapsedTimer clock;
    clock.start();

    for (;;) {
        int nl = m_buf.indexOf('\n');
        while (nl >= 0) {
            const QByteArray oneLine = m_buf.left(nl).trimmed();
            m_buf.remove(0, nl + 1);

            if (!oneLine.isEmpty()) {
                QJsonParseError perr;
                const QJsonDocument doc = QJsonDocument::fromJson(oneLine, &perr);
                if (perr.error == QJsonParseError::NoError && doc.isObject()) {
                    const QJsonObject obj = doc.object();
                    const QString type =
                        obj.value(protoStr(thgh::field::kType)).toString();
                    const quint64 rid = static_cast<quint64>(
                        obj.value(protoStr(thgh::field::kTaskId)).toDouble());

                    // 服务端可能主动发心跳，随手应答，不影响本次等待
                    if (type == typeName(thgh::MessageType::Heartbeat)) {
                        QJsonObject ack;
                        ack[protoStr(thgh::field::kType)] =
                            typeName(thgh::MessageType::HeartbeatAck);
                        m_socket->write(
                            QJsonDocument(ack).toJson(QJsonDocument::Compact) + "\n");
                    } else if (type == typeName(thgh::MessageType::Error)) {
                        m_lastError =
                            obj.value(protoStr(thgh::field::kMessage)).toString();
                        return false;
                    } else if (type == typeName(thgh::MessageType::DataReply)) {
                        // 只认本次请求的回复。id 对不上说明是上一次超时后
                        // 迟到的回复，丢掉继续等 —— 用它会张冠李戴。
                        if (rid != taskId) {
                            qWarning() << "[RemoteDataStore] 丢弃迟到回复 task_id="
                                       << rid << "当前=" << taskId;
                        } else {
                            const QJsonObject pl =
                                obj.value(protoStr(thgh::field::kPayload)).toObject();
                            if (!pl.value(protoStr(thgh::field::data::kOk)).toBool()) {
                                m_lastError =
                                    pl.value(protoStr(thgh::field::data::kError))
                                        .toString();
                                if (m_lastError.isEmpty())
                                    m_lastError = "服务端未说明失败原因";
                                return false;
                            }
                            dataOut = pl.value(protoStr(thgh::field::data::kData))
                                          .toObject();
                            m_lastError.clear();
                            return true;
                        }
                    }
                    // 其它类型（规划相关）不会出现在这条连接上，忽略
                }
            }
            nl = m_buf.indexOf('\n');
        }

        const qint64 left = m_timeoutMs - clock.elapsed();
        if (left <= 0) {
            m_lastError = QString("等待服务端响应超时（%1 ms），操作：%2")
                              .arg(m_timeoutMs).arg(op);
            // 连接上还挂着一个未完成的请求，留着它下次会读到错位的回复。
            // 直接断开，下次调用重连。
            disconnectFromServer();
            return false;
        }
        if (!m_socket->waitForReadyRead(static_cast<int>(left))) {
            if (m_socket->state() != QAbstractSocket::ConnectedState) {
                m_lastError = QString("连接已断开：%1").arg(m_socket->errorString());
                disconnectFromServer();
                return false;
            }
            continue;   // 只是这一轮没数据，还没到整体超时
        }
        m_buf += m_socket->readAll();
    }
}

bool RemoteDataStore::requestVoid(const QString &op, const QJsonObject &args)
{
    QJsonObject data;
    return request(op, args, data);
}

int RemoteDataStore::requestId(const QString &op, const QJsonObject &args)
{
    QJsonObject data;
    if (!request(op, args, data))
        return -1;
    return data.value(protoStr(thgh::field::data::kId)).toInt(-1);
}

bool RemoteDataStore::requestItems(const QString &op, const QJsonObject &args,
                                   QJsonArray &itemsOut)
{
    QJsonObject data;
    if (!request(op, args, data))
        return false;
    itemsOut = data.value(protoStr(thgh::field::data::kItems)).toArray();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 模型 ↔ JSON
// 字段名与服务端 data_service.cpp 一一对应。复合字段沿用库里原有的
// JSON 文本格式（modelcodec），这样服务端存进去的和改造前完全一致，
// 老数据不用迁移。
// ─────────────────────────────────────────────────────────────────────────────

QJsonObject RemoteDataStore::toJson(const SceneInfo &s)
{
    QJsonObject o;
    o["id"]          = s.id;
    o["name"]        = s.name;
    o["description"] = s.description;
    o["scene_type"]  = s.sceneType;
    o["create_time"] = s.createTime;
    return o;
}

SceneInfo RemoteDataStore::sceneFromJson(const QJsonObject &o)
{
    SceneInfo s;
    s.id          = o.value("id").toInt(-1);
    s.name        = o.value("name").toString();
    s.description = o.value("description").toString();
    s.sceneType   = o.value("scene_type").toString();
    s.createTime  = o.value("create_time").toString();
    return s;
}

QJsonObject RemoteDataStore::toJson(const NodeInfo &n)
{
    QJsonObject o;
    o["id"]                 = n.id;
    o["scene_id"]           = n.sceneId;
    o["node_id"]            = n.nodeId;
    o["from_template"]      = n.fromTemplate;
    o["name"]               = n.name;
    o["node_type"]          = n.nodeType;
    o["status"]             = n.status;
    o["longitude"]          = n.longitude;
    o["latitude"]           = n.latitude;
    o["altitude"]           = n.altitude;
    o["interference_db"]    = n.interferenceDb;
    o["comm_methods"]       = modelcodec::methodsToJson(n.commMethods);
    o["device_params"]      = modelcodec::deviceParamsToJson(n.deviceParams);
    o["device_connections"] = modelcodec::deviceConnsToJson(n.deviceConnections);
    return o;
}

NodeInfo RemoteDataStore::nodeFromJson(const QJsonObject &o)
{
    NodeInfo n;
    n.id             = o.value("id").toInt(-1);
    n.sceneId        = o.value("scene_id").toInt(-1);
    n.nodeId         = o.value("node_id").toInt(0);
    n.fromTemplate   = o.value("from_template").toInt(-1);
    n.name           = o.value("name").toString();
    n.nodeType       = o.value("node_type").toString();
    n.status         = o.value("status").toString();
    n.longitude      = o.value("longitude").toDouble();
    n.latitude       = o.value("latitude").toDouble();
    n.altitude       = o.value("altitude").toDouble();
    n.interferenceDb = o.value("interference_db").toDouble();
    n.commMethods    = modelcodec::jsonToMethods(o.value("comm_methods").toString());
    n.deviceParams   = modelcodec::jsonToDeviceParams(o.value("device_params").toString());
    n.deviceConnections =
        modelcodec::jsonToDeviceConns(o.value("device_connections").toString());
    return n;
}

QJsonObject RemoteDataStore::toJson(const LinkInfo &l)
{
    QJsonObject o;
    o["id"]            = l.id;
    o["scene_id"]      = l.sceneId;
    o["src"]           = l.src;
    o["dst"]           = l.dst;
    o["from_template"] = l.fromTemplate;
    o["link_type"]     = l.linkType;
    o["wireless_type"] = l.wirelessType;
    o["bandwidth_bps"] = l.bandwidthBps;
    o["prop_delay_s"]  = l.propDelayS;
    o["comm_protocol"] = l.commProtocol;
    o["device_type"]   = l.deviceType;
    o["flows"]         = modelcodec::flowsToJson(l.flows);
    return o;
}

LinkInfo RemoteDataStore::linkFromJson(const QJsonObject &o)
{
    LinkInfo l;
    l.id           = o.value("id").toInt(-1);
    l.sceneId      = o.value("scene_id").toInt(-1);
    l.src          = o.value("src").toInt(0);
    l.dst          = o.value("dst").toInt(0);
    l.fromTemplate = o.value("from_template").toInt(-1);
    l.linkType     = o.value("link_type").toString();
    l.wirelessType = o.value("wireless_type").toString();
    l.bandwidthBps = o.value("bandwidth_bps").toDouble();
    l.propDelayS   = o.value("prop_delay_s").toDouble();
    l.commProtocol = o.value("comm_protocol").toString();
    l.deviceType   = o.value("device_type").toString();
    l.flows        = modelcodec::jsonToFlows(o.value("flows").toString());
    return l;
}

QJsonObject RemoteDataStore::toJson(const NodeTemplate &t)
{
    QJsonObject o;
    o["template_name"]   = t.name;
    o["node_type"]       = t.nodeType;
    o["comm_methods"]    = modelcodec::methodsToJson(t.commMethods);
    o["interference_db"] = t.defaultInterferenceDb;
    o["description"]     = t.description;
    o["device_params"]   = modelcodec::deviceParamsToJson(t.deviceParams);
    return o;
}

NodeTemplate RemoteDataStore::nodeTemplateFromJson(const QJsonObject &o)
{
    NodeTemplate t;
    t.id                    = o.value("id").toInt(-1);
    t.name                  = o.value("template_name").toString();
    t.nodeType              = o.value("node_type").toString();
    t.description           = o.value("description").toString();
    t.commMethods           = modelcodec::jsonToMethods(o.value("comm_methods").toString());
    t.deviceParams          = modelcodec::jsonToDeviceParams(o.value("device_params").toString());
    t.defaultInterferenceDb = o.value("interference_db").toDouble();
    return t;
}

QJsonObject RemoteDataStore::toJson(const LinkTemplate &t)
{
    QJsonObject o;
    o["template_name"] = t.name;
    o["link_type"]     = t.linkType;
    o["wireless_type"] = t.wirelessType;
    o["bandwidth_bps"] = t.bandwidthBps;
    o["description"]   = t.description;
    return o;
}

LinkTemplate RemoteDataStore::linkTemplateFromJson(const QJsonObject &o)
{
    LinkTemplate t;
    t.id           = o.value("id").toInt(-1);
    t.name         = o.value("template_name").toString();
    t.linkType     = o.value("link_type").toString();
    t.wirelessType = o.value("wireless_type").toString();
    t.bandwidthBps = o.value("bandwidth_bps").toDouble(50e6);
    t.description  = o.value("description").toString();
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// IDataStore 实现
// ─────────────────────────────────────────────────────────────────────────────

int RemoteDataStore::createScene(const SceneInfo &s)
{
    return requestId(protoStr(thgh::field::op::kSceneCreate), toJson(s));
}

QList<SceneInfo> RemoteDataStore::listScenes()
{
    QList<SceneInfo> out;
    QJsonArray items;
    if (!requestItems(protoStr(thgh::field::op::kSceneList), QJsonObject(), items))
        return out;
    for (const QJsonValue &v : items)
        out.append(sceneFromJson(v.toObject()));
    return out;
}

bool RemoteDataStore::updateScene(const SceneInfo &s)
{
    return requestVoid(protoStr(thgh::field::op::kSceneUpdate), toJson(s));
}

bool RemoteDataStore::deleteScene(int sceneId)
{
    QJsonObject a;
    a["id"] = sceneId;
    return requestVoid(protoStr(thgh::field::op::kSceneDelete), a);
}

int RemoteDataStore::addNode(const NodeInfo &n)
{
    return requestId(protoStr(thgh::field::op::kNodeAdd), toJson(n));
}

bool RemoteDataStore::addNodes(int sceneId, const QList<NodeInfo> &nodes)
{
    if (nodes.isEmpty())
        return true;
    // ★ 一次请求装下全部节点。逐个发的话 500 个节点就是 500 次往返，
    //   即使每次只要 1ms 也要半秒，跨网络更是不可用。
    QJsonArray items;
    for (const NodeInfo &n : nodes)
        items.append(toJson(n));
    QJsonObject a;
    a["scene_id"] = sceneId;
    a[protoStr(thgh::field::data::kItems)] = items;
    return requestVoid(protoStr(thgh::field::op::kNodeAddBatch), a);
}

QList<NodeInfo> RemoteDataStore::getNodesByScene(int sceneId)
{
    QList<NodeInfo> out;
    QJsonObject a;
    a["scene_id"] = sceneId;
    QJsonArray items;
    if (!requestItems(protoStr(thgh::field::op::kNodeList), a, items))
        return out;
    for (const QJsonValue &v : items)
        out.append(nodeFromJson(v.toObject()));
    return out;
}

bool RemoteDataStore::updateNode(const NodeInfo &n)
{
    return requestVoid(protoStr(thgh::field::op::kNodeUpdate), toJson(n));
}

bool RemoteDataStore::deleteNode(int nodeDbId)
{
    QJsonObject a;
    a["id"] = nodeDbId;
    return requestVoid(protoStr(thgh::field::op::kNodeDelete), a);
}

int RemoteDataStore::addLink(const LinkInfo &l)
{
    return requestId(protoStr(thgh::field::op::kLinkAdd), toJson(l));
}

bool RemoteDataStore::addLinks(int sceneId, const QList<LinkInfo> &links)
{
    if (links.isEmpty())
        return true;
    QJsonArray items;
    for (const LinkInfo &l : links)
        items.append(toJson(l));
    QJsonObject a;
    a["scene_id"] = sceneId;
    a[protoStr(thgh::field::data::kItems)] = items;
    return requestVoid(protoStr(thgh::field::op::kLinkAddBatch), a);
}

QList<LinkInfo> RemoteDataStore::getLinksByScene(int sceneId)
{
    QList<LinkInfo> out;
    QJsonObject a;
    a["scene_id"] = sceneId;
    QJsonArray items;
    if (!requestItems(protoStr(thgh::field::op::kLinkList), a, items))
        return out;
    for (const QJsonValue &v : items)
        out.append(linkFromJson(v.toObject()));
    return out;
}

bool RemoteDataStore::updateLink(const LinkInfo &l)
{
    return requestVoid(protoStr(thgh::field::op::kLinkUpdate), toJson(l));
}

bool RemoteDataStore::deleteLink(int linkDbId)
{
    QJsonObject a;
    a["id"] = linkDbId;
    return requestVoid(protoStr(thgh::field::op::kLinkDelete), a);
}

bool RemoteDataStore::clearLinksByScene(int sceneId)
{
    QJsonObject a;
    a["scene_id"] = sceneId;
    return requestVoid(protoStr(thgh::field::op::kLinkClearByScene), a);
}

int RemoteDataStore::saveNodeTemplate(const NodeTemplate &t)
{
    return requestId(protoStr(thgh::field::op::kNodeTemplateSave), toJson(t));
}

QList<NodeTemplate> RemoteDataStore::listNodeTemplates()
{
    QList<NodeTemplate> out;
    QJsonArray items;
    if (!requestItems(protoStr(thgh::field::op::kNodeTemplateList), QJsonObject(),
                      items))
        return out;
    for (const QJsonValue &v : items)
        out.append(nodeTemplateFromJson(v.toObject()));
    return out;
}

int RemoteDataStore::saveLinkTemplate(const LinkTemplate &t)
{
    return requestId(protoStr(thgh::field::op::kLinkTemplateSave), toJson(t));
}

QList<LinkTemplate> RemoteDataStore::listLinkTemplates()
{
    QList<LinkTemplate> out;
    QJsonArray items;
    if (!requestItems(protoStr(thgh::field::op::kLinkTemplateList), QJsonObject(),
                      items))
        return out;
    for (const QJsonValue &v : items)
        out.append(linkTemplateFromJson(v.toObject()));
    return out;
}
