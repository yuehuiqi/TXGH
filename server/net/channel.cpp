#include "net/channel.h"

#include "net/event_loop.h"

#include <cstdio>

namespace thgh {

Channel::Channel(EventLoop* loop, int fd) : m_loop(loop), m_fd(fd) {}

Channel::~Channel() = default;

void Channel::tie(const std::shared_ptr<void>& obj) {
    m_tie = obj;
    m_tied = true;
}

void Channel::update() {
    m_loop->updateChannel(this);
}

void Channel::enableReading() {
    // EPOLLET：边缘触发。配套要求是 fd 非阻塞 + 循环读到 EAGAIN。
    m_events |= (EPOLLIN | EPOLLPRI | EPOLLET);
    update();
}

void Channel::disableReading() {
    m_events &= ~(EPOLLIN | EPOLLPRI);
    update();
}

void Channel::enableWriting() {
    m_events |= (EPOLLOUT | EPOLLET);
    update();
}

void Channel::disableWriting() {
    m_events &= ~EPOLLOUT;
    update();
}

void Channel::disableAll() {
    m_events = 0;
    update();
}

void Channel::handleEvent() {
    if (m_tied) {
        // 提升 weak_ptr：把持有者（TcpConnection）续命到本次事件处理结束。
        // 否则回调里若销毁了连接，回调返回后再访问 Channel 成员就是 UAF。
        std::shared_ptr<void> guard = m_tie.lock();
        if (guard) {
            handleEventWithGuard();
        }
        // 提升失败说明持有者已销毁，直接放弃本次事件
    } else {
        handleEventWithGuard();
    }
}

void Channel::handleEventWithGuard() {
    m_handlingEvent = true;

    // 判定顺序有讲究：先处理关闭，再处理错误，最后才是读写。
    //
    // EPOLLHUP 且没有 EPOLLIN：对端完全关闭且没有待读数据 → 连接结束。
    // 必须先判，否则会先走进 read 分支做无谓的系统调用。
    if ((m_revents & EPOLLHUP) && !(m_revents & EPOLLIN)) {
        if (m_closeCallback) {
            m_closeCallback();
        }
        m_handlingEvent = false;
        return;
    }

    if (m_revents & EPOLLERR) {
        if (m_errorCallback) {
            m_errorCallback();
        }
        m_handlingEvent = false;
        return;
    }

    // EPOLLRDHUP 表示对端关闭了写端（半关闭）。
    // 这时可能还有数据在路上，所以归到读事件里处理 ——
    // 读到 0 字节才是真正的结束信号。
    if (m_revents & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (m_readCallback) {
            m_readCallback();
        }
    }

    if (m_revents & EPOLLOUT) {
        if (m_writeCallback) {
            m_writeCallback();
        }
    }

    m_handlingEvent = false;
}

}  // namespace thgh
