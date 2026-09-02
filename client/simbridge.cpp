#include "simbridge.h"

#include "thgh/message.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

namespace {

// 协议常量一律取自共享层，不在这里写字面量。
// message.h 里的 string_view 转成 QString 只发生在比较/构造的那一刻，
// 都是常量长度的小串，开销可以忽略。
inline QString protoStr(std::string_view sv)
{
    return QString::fromUtf8(sv.data(), static_cast<int>(sv.size()));
}

inline QString typeName(thgh::MessageType t)
{
    return protoStr(thgh::toString(t));
}

}  // namespace

// ─── constructor / destructor ─────────────────────────────────────────────────

SimBridge::SimBridge(QObject *parent) : QObject(parent)
{
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected,
            this, &SimBridge::onSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected,
            this, &SimBridge::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,
            this, &SimBridge::onSocketReadyRead);
    // Qt5 里 QAbstractSocket::error 既是信号又是同名 getter，必须用 QOverload
    // 消歧义；Qt6 把信号改名为 errorOccurred，歧义没了，QOverload 也就不需要了。
    connect(m_socket, &QTcpSocket::errorOccurred,
            this, &SimBridge::onSocketError);

    m_retryTimer = new QTimer(this);
    m_retryTimer->setInterval(3000);   // 3 秒后重连
    connect(m_retryTimer, &QTimer::timeout, this, &SimBridge::onRetryConnect);
}

SimBridge::~SimBridge()
{
    m_userDisconnected = true;
    m_retryTimer->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
        m_socket->waitForDisconnected(500);
    }
}

// ─── public API ──────────────────────────────────────────────────────────────

void SimBridge::setServer(const QString &host, quint16 port)
{
    m_host = host;
    m_port = port;
}

void SimBridge::connectToServer()
{
    m_userDisconnected = false;
    m_retryTimer->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
    m_socket->connectToHost(m_host, m_port);
}

void SimBridge::disconnectFromServer()
{
    m_userDisconnected = true;
    m_retryTimer->stop();
    m_socket->disconnectFromHost();
}

bool SimBridge::isConnected() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

void SimBridge::startPlan(const QJsonObject &payload)
{
    if (!isConnected()) {
        emit simError(QString("未连接到规划服务端（%1:%2）").arg(m_host).arg(m_port));
        return;
    }
    // 发新任务前清掉旧任务 id：旧任务若还有消息在路上，
    // 会因为对不上当前 id 而被丢弃，不会污染新任务的界面状态。
    // 真正的新 id 要等服务端在 ack 里给。
    m_currentTask = 0;
    m_lineBuf.clear();
    m_readPos = m_searchPos = 0;

    QJsonObject msg;
    msg[protoStr(thgh::field::kType)]    = typeName(thgh::MessageType::StartPlan);
    msg[protoStr(thgh::field::kPayload)] = payload;
    QByteArray data = QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
    m_socket->write(data);
    m_socket->flush();
    qDebug() << "[SimBridge] startPlan sent, payload size=" << data.size();
}

// ─── private slots ────────────────────────────────────────────────────────────

void SimBridge::onSocketConnected()
{
    qDebug() << "[SimBridge] TCP 连接成功:" << m_host << m_port;
    m_retryTimer->stop();
    m_lineBuf.clear();
    m_readPos = m_searchPos = 0;
    m_currentTask = 0;
    emit connected();
}

void SimBridge::onSocketDisconnected()
{
    qDebug() << "[SimBridge] TCP 连接断开";
    emit disconnected();
    if (!m_userDisconnected)
        m_retryTimer->start();
}

void SimBridge::onSocketReadyRead()
{
    m_lineBuf += m_socket->readAll();

    // ── 分帧。这段原来有三个真实缺陷，随 v3.0 一并修掉 ──────────────────
    //  ① 每次都从头 indexOf('\n')：一条 3.5 万字节的报文分 20 个 TCP 段到达时，
    //     前 19 次都要把已扫过的部分重扫一遍 —— O(n²)。用 searchPos 记住扫到哪了。
    //  ② 每取出一条就 remove(0, n)：QByteArray::remove 会把后面的全部前移，
    //     同样是 O(n²)。改成用 readPos 游标标记，攒够了再一次性压缩。
    //  ③ 缓冲区无上限：对端（或中间人）持续发不含 '\n' 的字节，
    //     m_lineBuf 会一直涨到内存耗尽。这是真实可触发的 DoS 面。
    //  服务端侧的 LineFramer 在 P1 就是按这三条写的，客户端这次对齐。
    // ★ 两个游标必须是**成员**而不是局部变量。
    //   最典型的场景正是"一条 35KB 的报文分 20 个 TCP 段到达"：
    //   游标若每次调用都归零，这 20 次 readyRead 会把同一段数据反复重扫，
    //   O(n²) 依然存在，只是从"每条消息"变成了"每个 TCP 段"。
    static const int kMaxLineBytes     = 16 * 1024 * 1024;  // 单条消息上限
    static const int kCompactThreshold = 64 * 1024;         // 废弃前缀超过它才压缩

    while (true) {
        const int nl = m_lineBuf.indexOf('\n', m_searchPos);
        if (nl < 0) {
            // 没找到分隔符：记下已扫到的位置，下次从这里继续
            m_searchPos = m_lineBuf.size();
            if (m_lineBuf.size() - m_readPos > kMaxLineBytes) {
                qWarning() << "[SimBridge] 单条消息超过上限，断开连接";
                m_lineBuf.clear();
                m_readPos = m_searchPos = 0;
                emit simError("服务端返回的消息超长，已断开");
                m_socket->abort();
                return;
            }
            break;
        }
        const QByteArray line = m_lineBuf.mid(m_readPos, nl - m_readPos).trimmed();
        m_readPos   = nl + 1;
        m_searchPos = m_readPos;
        if (!line.isEmpty())
            parseLine(line);
    }

    // 压缩：全部消费完就直接 clear（O(1)）；否则攒够一批再搬一次，
    // 避免每条消息都触发一次整体前移。
    if (m_readPos >= m_lineBuf.size()) {
        m_lineBuf.clear();
        m_readPos = m_searchPos = 0;
    } else if (m_readPos >= kCompactThreshold) {
        m_lineBuf.remove(0, m_readPos);
        m_searchPos -= m_readPos;
        m_readPos = 0;
    }
}

void SimBridge::onSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)
    qDebug() << "[SimBridge] 连接错误:" << m_socket->errorString();
    if (!m_userDisconnected) {
        emit connectionError(m_socket->errorString());
        if (m_socket->state() == QAbstractSocket::UnconnectedState)
            m_retryTimer->start();
    }
}

void SimBridge::onRetryConnect()
{
    if (m_socket->state() == QAbstractSocket::UnconnectedState)
        m_socket->connectToHost(m_host, m_port);
}

// ─── 任务归属判定 ─────────────────────────────────────────────────────────────

bool SimBridge::belongsToCurrentTask(const QJsonObject &obj) const
{
    // 不带 task_id 的服务端（改造前的 Python 版本）一律放行 ——
    // 否则升级客户端就必须同时升级服务端，联调时很难受。
    if (!obj.contains(protoStr(thgh::field::kTaskId)))
        return true;
    const quint64 id = static_cast<quint64>(obj.value(protoStr(thgh::field::kTaskId)).toDouble());
    return m_currentTask == 0 || id == m_currentTask;
}

// ─── parseLine（v3.0 协议：ack / progress / plan_result / error）─────────────

void SimBridge::parseLine(const QByteArray &line)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[SimBridge] 无法解析行 (size=" << line.size() << "):"
                   << err.errorString();
        return;
    }
    QJsonObject obj  = doc.object();
    const QString type = obj.value(protoStr(thgh::field::kType)).toString();

    // 过期任务的消息直接丢弃：用户连点两次"启动"时，
    // 第一次的结果不能覆盖第二次的界面状态。
    if (!belongsToCurrentTask(obj)) {
        qDebug() << "[SimBridge] 丢弃过期任务消息:" << type
                 << "task_id=" << obj.value(protoStr(thgh::field::kTaskId)).toDouble()
                 << "当前=" << m_currentTask;
        return;
    }

    if (type == typeName(thgh::MessageType::Ack)) {
        m_currentTask = static_cast<quint64>(obj.value(protoStr(thgh::field::kTaskId)).toDouble());
        const QJsonObject pl = obj.value(protoStr(thgh::field::kPayload)).toObject();
        qDebug() << "[SimBridge] 服务端已受理 task_id=" << m_currentTask;
        emit planAcknowledged(m_currentTask,
                              pl.value(protoStr(thgh::field::ack::kNodeCount)).toInt(),
                              pl.value(protoStr(thgh::field::ack::kFlowCount)).toInt());

    } else if (type == typeName(thgh::MessageType::Progress)) {
        const QJsonObject pl = obj.value(protoStr(thgh::field::kPayload)).toObject();
        emit planProgress(pl.value(protoStr(thgh::field::progress::kPercent)).toInt(),
                          pl.value(protoStr(thgh::field::progress::kStage)).toString());

    } else if (type == typeName(thgh::MessageType::Heartbeat)) {
        // 服务端探活，原样应答。不走业务逻辑。
        QJsonObject ackMsg;
        ackMsg[protoStr(thgh::field::kType)] = typeName(thgh::MessageType::HeartbeatAck);
        m_socket->write(QJsonDocument(ackMsg).toJson(QJsonDocument::Compact) + "\n");

    } else if (type == typeName(thgh::MessageType::HeartbeatAck)) {
        // 对端应答了我们的心跳，无需处理

    } else if (type == typeName(thgh::MessageType::PlanResult)) {
        QJsonObject pl = obj.value(protoStr(thgh::field::kPayload)).toObject();
        PlanResultPacket packet;

        // 解析 links[]
        for (const QJsonValue &lv : pl.value(protoStr(thgh::field::result::kLinks)).toArray()) {
            QJsonObject lo = lv.toObject();
            LinkResult lr;
            lr.linkId       = lo.value(protoStr(thgh::field::link::kLinkId)).toInt();
            lr.srcNodeId    = lo.value(protoStr(thgh::field::link::kSrcNodeId)).toInt();
            lr.dstNodeId    = lo.value(protoStr(thgh::field::link::kDstNodeId)).toInt();
            lr.linkType     = lo.value(protoStr(thgh::field::link::kLinkType)).toString();
            lr.wirelessType = lo.value(protoStr(thgh::field::link::kWirelessType)).toString();
            lr.bandwidthBps = lo.value(protoStr(thgh::field::link::kBandwidthBps)).toDouble();
            lr.propDelayS   = lo.value(protoStr(thgh::field::link::kPropDelayS)).toDouble();
            for (const QJsonValue &fv : lo.value(protoStr(thgh::field::link::kFlows)).toArray()) {
                QJsonObject fo = fv.toObject();
                LinkResultFlow lrf;
                lrf.fid          = fo.value(protoStr(thgh::field::linkflow::kFid)).toInt();
                lrf.bandwidthBps = fo.value(protoStr(thgh::field::linkflow::kBandwidthBps)).toDouble();
                lrf.description  = fo.value(protoStr(thgh::field::linkflow::kDescription)).toString();
                lr.flows.append(lrf);
            }
            packet.links.append(lr);
        }

        // 解析 plan_results[]
        for (const QJsonValue &pv : pl.value(protoStr(thgh::field::result::kPlanResults)).toArray()) {
            QJsonObject po = pv.toObject();
            PlanResult pr;
            pr.fid                = po.value(protoStr(thgh::field::planresult::kFid)).toInt();
            pr.srcNodeId          = po.value(protoStr(thgh::field::planresult::kSrcNodeId)).toInt();
            pr.dstNodeId          = po.value(protoStr(thgh::field::planresult::kDstNodeId)).toInt();
            pr.isSatisfied        = po.value(protoStr(thgh::field::planresult::kIsSatisfied)).toBool();
            pr.hops               = po.value(protoStr(thgh::field::planresult::kHops)).toInt(-1);
            pr.actualBandwidthBps = po.value(protoStr(thgh::field::planresult::kActualBandwidthBps)).toDouble();
            pr.unsatisfiedReason  = po.value(protoStr(thgh::field::planresult::kUnsatisfiedReason)).toString();
            for (const QJsonValue &nv : po.value(protoStr(thgh::field::planresult::kPathNodes)).toArray())
                pr.pathNodes.append(nv.toInt());
            for (const QJsonValue &lv : po.value(protoStr(thgh::field::planresult::kPathLinks)).toArray())
                pr.pathLinks.append(lv.toInt());
            packet.planResults.append(pr);
        }

        // 解析 warns[]
        for (const QJsonValue &wv : pl.value(protoStr(thgh::field::result::kWarns)).toArray()) {
            QJsonObject wo = wv.toObject();
            WarnItem wi;
            wi.level      = wo.value(protoStr(thgh::field::warn::kLevel)).toString("info");
            wi.message    = wo.value(protoStr(thgh::field::warn::kMessage)).toString();
            wi.warnType   = wo.value(protoStr(thgh::field::warn::kWarnType)).toString("general");
            wi.relatedFid = wo.value(protoStr(thgh::field::warn::kRelatedFid)).toInt(-1);
            packet.warns.append(wi);
        }

        qDebug() << "[SimBridge] plan_result: links=" << packet.links.size()
                 << "plan_results=" << packet.planResults.size()
                 << "warns=" << packet.warns.size();
        m_currentTask = 0;   // 任务结束，后续同 id 的消息（若有）不再接受
        emit planResultReceived(packet);
        emit simFinished();

    } else if (type == typeName(thgh::MessageType::Error)) {
        m_currentTask = 0;
        emit simError(obj.value(protoStr(thgh::field::kMessage)).toString());
        emit simFinished();   // 让界面把按钮和进度条恢复，否则会永远卡在"规划中"

    } else {
        qWarning() << "SimBridge: 未知消息类型:" << type;
    }
}
