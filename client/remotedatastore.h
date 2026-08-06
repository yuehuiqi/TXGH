#ifndef REMOTEDATASTORE_H
#define REMOTEDATASTORE_H

// ─────────────────────────────────────────────────────────────────────────────
// RemoteDataStore —— 经协议访问服务端数据（P6）
//
// ── 为什么用一条独立连接，而不是复用 SimBridge 那条 ★ ────────────────────
// SimBridge 那条连接是**推送式**的：发一次 start_plan，服务端会陆续推回
// ack / progress×N / plan_result，什么时候来、来几条都不确定。
// 本类是**请求-应答式**的：发一条问一条，同步等回复。
//
// 两者混在一条连接上会互相打架 —— 同步等待数据回复时会先读到规划进度，
// 得把它缓存起来再转交给 SimBridge，而 SimBridge 又是靠 Qt 信号驱动的，
// 在阻塞等待里转交等于重入。用两条连接，这个问题根本不存在。
// 代价是多一条 TCP 连接，对服务端来说微不足道。
//
// ── 同步等待怎么做才安全 ★ ────────────────────────────────────────────────
// 用 `QTcpSocket::waitForReadyRead()` 而**不是** `QEventLoop::exec()`。
// 后者会继续派发 GUI 事件：用户在等待期间还能点按钮、关窗口，
// 于是同一个函数被重入，或者对象在自己的方法执行中途被析构。
// waitForReadyRead 只驱动 socket 自身，不派发 GUI 事件，界面表现为"忙"，
// 这是我们想要的语义。
//
// 每次请求都带硬超时。超时即返回失败并把连接标记为需重连 ——
// 半死不活的连接留着只会让后续每次调用都再等一个超时。
// ─────────────────────────────────────────────────────────────────────────────

#include "idatastore.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTcpSocket>

class RemoteDataStore : public QObject, public IDataStore
{
    Q_OBJECT
public:
    explicit RemoteDataStore(QObject *parent = nullptr);
    ~RemoteDataStore() override;

    void setServer(const QString &host, quint16 port);
    // 建立连接。失败返回 false，原因见 lastError()。
    bool connectToServer(int timeoutMs = 5000);
    void disconnectFromServer();

    // 单次请求的最长等待。默认 5 秒：够慢查询跑完，又不至于让用户
    // 在服务端已经挂掉的情况下盯着冻结的界面太久。
    void setTimeout(int ms) { m_timeoutMs = ms; }

    // ── IDataStore ─────────────────────────────────────────────────────
    bool    isReady() const override;
    QString sourceDescription() const override;
    QString lastError() const override { return m_lastError; }

    int              createScene(const SceneInfo &s) override;
    QList<SceneInfo> listScenes() override;
    bool             updateScene(const SceneInfo &s) override;
    bool             deleteScene(int sceneId) override;

    int              addNode(const NodeInfo &n) override;
    bool             addNodes(int sceneId, const QList<NodeInfo> &nodes) override;
    QList<NodeInfo>  getNodesByScene(int sceneId) override;
    bool             updateNode(const NodeInfo &n) override;
    bool             deleteNode(int nodeDbId) override;

    int              addLink(const LinkInfo &l) override;
    bool             addLinks(int sceneId, const QList<LinkInfo> &links) override;
    QList<LinkInfo>  getLinksByScene(int sceneId) override;
    bool             updateLink(const LinkInfo &l) override;
    bool             deleteLink(int linkDbId) override;
    bool             clearLinksByScene(int sceneId) override;

    int                 saveNodeTemplate(const NodeTemplate &t) override;
    QList<NodeTemplate> listNodeTemplates() override;
    int                 saveLinkTemplate(const LinkTemplate &t) override;
    QList<LinkTemplate> listLinkTemplates() override;

private:
    // 发一条请求并同步等回复。ok=false 时 m_lastError 已填好。
    // 返回的是 payload.data 对象。
    bool request(const QString &op, const QJsonObject &args, QJsonObject &dataOut);
    // 只关心成功与否的写操作
    bool requestVoid(const QString &op, const QJsonObject &args);
    // 返回新插入行 id 的写操作，失败返回 -1
    int  requestId(const QString &op, const QJsonObject &args);
    // 读列表
    bool requestItems(const QString &op, const QJsonObject &args,
                      QJsonArray &itemsOut);

    // 断线自愈：请求前若发现连接已断，尝试重连一次再发。
    // 服务端重启、网络瞬断之后不该要求用户手动重连。
    bool ensureConnected();

    // ── 模型 ↔ JSON（与服务端 data_service.cpp 的字段名一一对应）────
    static QJsonObject toJson(const SceneInfo &s);
    static SceneInfo   sceneFromJson(const QJsonObject &o);
    static QJsonObject toJson(const NodeInfo &n);
    static NodeInfo    nodeFromJson(const QJsonObject &o);
    static QJsonObject toJson(const LinkInfo &l);
    static LinkInfo    linkFromJson(const QJsonObject &o);
    static QJsonObject toJson(const NodeTemplate &t);
    static NodeTemplate nodeTemplateFromJson(const QJsonObject &o);
    static QJsonObject toJson(const LinkTemplate &t);
    static LinkTemplate linkTemplateFromJson(const QJsonObject &o);

    QTcpSocket *m_socket = nullptr;
    QString     m_host   = "127.0.0.1";
    quint16     m_port   = 9000;
    int         m_timeoutMs = 5000;
    quint64     m_nextTaskId = 0;
    QString     m_lastError;
    QByteArray  m_buf;    // 跨请求残留的字节（正常情况下为空）
};

#endif // REMOTEDATASTORE_H
