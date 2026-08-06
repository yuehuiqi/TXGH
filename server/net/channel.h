#ifndef THGH_SERVER_NET_CHANNEL_H
#define THGH_SERVER_NET_CHANNEL_H

// ─────────────────────────────────────────────────────────────────────────────
// Channel —— 一个 fd 的事件注册与分发单元
//
// 职责：把"这个 fd 关心哪些事件"和"事件发生时调用谁"绑在一起。
// EventLoop 从 epoll_wait 拿到就绪的 Channel 列表，逐个调用它的 handleEvent。
//
// Channel **不拥有 fd**：fd 的生命周期由持有它的对象负责
//   （Acceptor 持有监听 fd、TcpConnection 持有连接 fd）。
// 这样职责清晰：Channel 只管事件，不管资源。
//
// 一个 Channel 只属于一个 EventLoop，所有操作都在该 loop 所在线程执行，
// 因此内部**不需要任何锁**。
// ─────────────────────────────────────────────────────────────────────────────

#include <sys/epoll.h>

#include <functional>
#include <memory>

namespace thgh {

class EventLoop;

class Channel {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    // 事件回调
    void setReadCallback(EventCallback cb) { m_readCallback = std::move(cb); }
    void setWriteCallback(EventCallback cb) { m_writeCallback = std::move(cb); }
    void setCloseCallback(EventCallback cb) { m_closeCallback = std::move(cb); }
    void setErrorCallback(EventCallback cb) { m_errorCallback = std::move(cb); }

    // 由 EventLoop 在 epoll_wait 返回后调用
    void handleEvent();

    int fd() const { return m_fd; }
    int events() const { return m_events; }
    void setRevents(int revents) { m_revents = revents; }

    // ── 关注的事件 ──────────────────────────────────────────────────────
    // 全部使用**边缘触发(ET)**：只在状态发生变化时通知一次。
    //
    // ET vs LT：
    //   LT（水平触发）：只要缓冲区还有数据就持续通知。容错性好，
    //     没读完下次还会提醒，但同一批数据会反复唤醒 epoll_wait。
    //   ET（边缘触发）：只在"从无到有"时通知一次。唤醒次数少、效率高，
    //     但**必须一次性把数据读干净**（循环读到 EAGAIN），
    //     否则剩余数据不会再触发，那条连接就"卡死"了。
    //
    // ET 的硬性前提是 **fd 必须非阻塞**：循环读到最后一次必然遇到"没数据"，
    // 阻塞 fd 会永久卡在那次 read 上，整个 EventLoop 随之死掉。
    void enableReading();
    void disableReading();
    void enableWriting();
    void disableWriting();
    void disableAll();

    bool isWriting() const { return (m_events & EPOLLOUT) != 0; }
    bool isReading() const { return (m_events & EPOLLIN) != 0; }
    bool isNoneEvent() const { return m_events == 0; }

    // epoll 中的注册状态，由 EventLoop 维护
    enum class State { New, Added, Deleted };
    State state() const { return m_state; }
    void setState(State s) { m_state = s; }

    EventLoop* ownerLoop() const { return m_loop; }

    // 把 Channel 的生命周期绑定到持有者（TcpConnection）的 shared_ptr 上。
    // 场景：handleEvent 执行到一半时回调里销毁了 TcpConnection，
    // 回调返回后继续访问 Channel 成员就是 use-after-free。
    // tie 之后 handleEvent 期间会临时提升一个 shared_ptr，把对象续命到本次
    // 事件处理结束。
    void tie(const std::shared_ptr<void>& obj);

private:
    void update();
    void handleEventWithGuard();

    EventLoop* m_loop;
    const int m_fd;
    int m_events = 0;   // 关注的事件
    int m_revents = 0;  // epoll 返回的实际事件
    State m_state = State::New;

    std::weak_ptr<void> m_tie;
    bool m_tied = false;
    bool m_handlingEvent = false;

    EventCallback m_readCallback;
    EventCallback m_writeCallback;
    EventCallback m_closeCallback;
    EventCallback m_errorCallback;
};

}  // namespace thgh

#endif  // THGH_SERVER_NET_CHANNEL_H
