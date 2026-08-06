#ifndef SIMBRIDGE_H
#define SIMBRIDGE_H

#include "datamodel.h"
#include <QObject>
#include <QTcpSocket>
#include <QJsonObject>
#include <QTimer>

// ─────────────────────────────────────────────────────────────────────────────
// SimBridge：Qt ↔ C++ 服务端通信网关（v3.0 协议）
//
// ── v2.0 → v3.0 的变化 ────────────────────────────────────────────────────
// v2.0 是"一次请求一次响应"：发出 start_plan 之后，界面除了把按钮置灰之外
// 什么也做不了，直到 plan_result 回来。规模大时这个空窗期能到几百毫秒甚至更久，
// 用户看到的是一个"卡住的程序"，分不清是在算还是已经死了。
//
// v3.0 拆成三阶段：
//     ack       → 服务端已受理，附带 task_id 与规模确认
//     progress  → 阶段性进度（0~100），可多条
//     plan_result → 最终结果
//
// ── 为什么要 taskId ───────────────────────────────────────────────────────
// 用户可能连点两次"启动"。没有 taskId 的话，第一次的结果回来时客户端
// 无法判断它是不是当前这次的，界面会先闪一下旧方案再被新方案覆盖。
// 客户端记住"当前 taskId"，非当前任务的消息一律丢弃。
// 服务端侧也做了同样的判断（见 plan_service.cpp），两边都拦一道。
// ─────────────────────────────────────────────────────────────────────────────
class SimBridge : public QObject
{
    Q_OBJECT
public:
    explicit SimBridge(QObject *parent = nullptr);
    ~SimBridge();

    // 服务器地址配置（默认 127.0.0.1:9000）
    void setServer(const QString &host, quint16 port);
    QString host() const { return m_host; }
    quint16 port() const { return m_port; }

    // 连接管理
    void connectToServer();
    void disconnectFromServer();
    bool isConnected() const;

    // v2.0：发送综合规划请求（场景 + 节点 + 设备 + 规划流，一次性打包）
    void startPlan(const QJsonObject &payload);

signals:
    // 连接状态
    void connected();
    void disconnected();
    void connectionError(QString msg);

    // ── v3.0 分阶段推送 ──────────────────────────────────────────────
    // 服务端已受理请求。到这一步就可以把进度条从"不确定"切成 0%，
    // 用户立刻知道请求发出去了、对面收到了。
    void planAcknowledged(quint64 taskId, int nodeCount, int flowCount);
    // 阶段性进度。percent 0~100，stage 是阶段名（如"分配业务流带宽"）
    void planProgress(int percent, QString stage);

    // 最终综合规划结果（链路 + 路径 + 告警）
    void planResultReceived(PlanResultPacket packet);

    // 错误 / 结束
    void simError(QString msg);
    void simFinished();

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void onRetryConnect();

private:
    QTcpSocket *m_socket        = nullptr;
    QTimer     *m_retryTimer    = nullptr;
    QByteArray  m_lineBuf;
    // 分帧游标。必须是成员：一条大报文分多个 TCP 段到达时，
    // 局部变量每次归零会导致反复重扫同一段数据（见 onSocketReadyRead 注释）。
    int         m_readPos       = 0;   // 已消费到哪
    int         m_searchPos     = 0;   // 已搜索到哪
    QString     m_host          = "127.0.0.1";
    quint16     m_port          = 9000;
    bool        m_userDisconnected = false;

    // 当前任务 id。0 表示没有进行中的任务。
    // 服务端在 ack 里给出，后续 progress / plan_result 都带着它。
    quint64     m_currentTask   = 0;

    void parseLine(const QByteArray &line);
    // 消息是否属于当前任务。不带 task_id 的旧版服务端一律放行，
    // 否则连老服务端都用不了了。
    bool belongsToCurrentTask(const QJsonObject &obj) const;
};

#endif // SIMBRIDGE_H
