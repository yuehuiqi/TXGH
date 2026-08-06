#include "framelessdialog.h"

#include <QMouseEvent>
#include <QCursor>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

FramelessDialog::FramelessDialog(QWidget *parent, Qt::WindowFlags f)
    : QDialog(parent, f | Qt::FramelessWindowHint)
{
    // 启用鼠标跟踪：非 Windows 平台需要它来在边缘悬停时切换光标形状
    setMouseTracking(true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Windows 平台：通过 WM_NCHITTEST 把"在边缘/标题栏"的命中告诉 OS，
// 由 Windows 系统自身完成拖拽与拉伸（体验最好，支持窗口贴边等系统特性）。
// ─────────────────────────────────────────────────────────────────────────────
bool FramelessDialog::nativeEvent(const QByteArray &eventType,
                                  void *message, long *result)
{
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG") {
        MSG *msg = static_cast<MSG *>(message);

        if (msg->message == WM_NCHITTEST) {
            RECT wr;
            GetWindowRect(msg->hwnd, &wr);
            int x = static_cast<int>(static_cast<short>(LOWORD(msg->lParam)))
                    - static_cast<int>(wr.left);
            int y = static_cast<int>(static_cast<short>(HIWORD(msg->lParam)))
                    - static_cast<int>(wr.top);
            int w = static_cast<int>(wr.right  - wr.left);
            int h = static_cast<int>(wr.bottom - wr.top);

            bool L = x < kBorderWidth;
            bool R = x >= w - kBorderWidth;
            bool T = y < kBorderWidth;
            bool B = y >= h - kBorderWidth;

            if (T && L) { *result = HTTOPLEFT;     return true; }
            if (T && R) { *result = HTTOPRIGHT;    return true; }
            if (B && L) { *result = HTBOTTOMLEFT;  return true; }
            if (B && R) { *result = HTBOTTOMRIGHT; return true; }
            if (T) { *result = HTTOP;    return true; }
            if (B) { *result = HTBOTTOM; return true; }
            if (L) { *result = HTLEFT;   return true; }
            if (R) { *result = HTRIGHT;  return true; }

            if (y < kCaptionHeight) { *result = HTCAPTION; return true; }

            *result = HTCLIENT;
            return true;
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return QDialog::nativeEvent(eventType, message, result);
}

// ─────────────────────────────────────────────────────────────────────────────
// 非 Windows 平台（Linux/macOS）后备路径：通过 Qt 鼠标事件实现拖拽与拉伸。
// 注：在 Windows 上，边缘/标题区域被 OS 直接处理，下方鼠标事件不会被触发；
//     客户区的鼠标事件由 Qt 派发给具体子控件，不影响本对话框。
// ─────────────────────────────────────────────────────────────────────────────
int FramelessDialog::hitEdges(const QPoint &p) const
{
    int edges = EdgeNone;
    if (p.x() < kBorderWidth)               edges |= EdgeLeft;
    if (p.x() >= width()  - kBorderWidth)   edges |= EdgeRight;
    if (p.y() < kBorderWidth)               edges |= EdgeTop;
    if (p.y() >= height() - kBorderWidth)   edges |= EdgeBottom;
    return edges;
}

void FramelessDialog::updateCursorForEdges(int edges)
{
    Qt::CursorShape shape = Qt::ArrowCursor;
    const bool L = edges & EdgeLeft;
    const bool R = edges & EdgeRight;
    const bool T = edges & EdgeTop;
    const bool B = edges & EdgeBottom;

    if      ((T && L) || (B && R)) shape = Qt::SizeFDiagCursor;
    else if ((T && R) || (B && L)) shape = Qt::SizeBDiagCursor;
    else if (T || B)               shape = Qt::SizeVerCursor;
    else if (L || R)               shape = Qt::SizeHorCursor;

    setCursor(shape);
}

void FramelessDialog::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) { QDialog::mousePressEvent(e); return; }

    const QPoint pos = e->pos();
    m_resizeEdges = hitEdges(pos);

    if (m_resizeEdges != EdgeNone) {
        // 进入拉伸
        m_pressGlobal = e->globalPos();
        m_pressGeom   = geometry();
        e->accept();
        return;
    }

    if (pos.y() < kCaptionHeight) {
        // 进入拖拽（仅顶部 40px 内）
        m_dragging    = true;
        m_pressGlobal = e->globalPos();
        m_pressGeom   = geometry();
        e->accept();
        return;
    }

    QDialog::mousePressEvent(e);
}

void FramelessDialog::mouseMoveEvent(QMouseEvent *e)
{
    // 没有按键按下：实时根据光标位置切换光标形状（提示拉伸方向）
    if (!(e->buttons() & Qt::LeftButton)) {
        updateCursorForEdges(hitEdges(e->pos()));
        QDialog::mouseMoveEvent(e);
        return;
    }

    // 正在拉伸
    if (m_resizeEdges != EdgeNone) {
        QPoint delta = e->globalPos() - m_pressGlobal;
        QRect g      = m_pressGeom;
        const int minW = qMax(minimumWidth(),  80);
        const int minH = qMax(minimumHeight(), 60);

        if (m_resizeEdges & EdgeLeft) {
            int newLeft = qMin(g.left() + delta.x(), g.right() - minW);
            g.setLeft(newLeft);
        }
        if (m_resizeEdges & EdgeRight) {
            int newRight = qMax(g.right() + delta.x(), g.left() + minW);
            g.setRight(newRight);
        }
        if (m_resizeEdges & EdgeTop) {
            int newTop = qMin(g.top() + delta.y(), g.bottom() - minH);
            g.setTop(newTop);
        }
        if (m_resizeEdges & EdgeBottom) {
            int newBottom = qMax(g.bottom() + delta.y(), g.top() + minH);
            g.setBottom(newBottom);
        }
        setGeometry(g);
        e->accept();
        return;
    }

    // 正在拖拽
    if (m_dragging) {
        QPoint delta = e->globalPos() - m_pressGlobal;
        move(m_pressGeom.topLeft() + delta);
        e->accept();
        return;
    }

    QDialog::mouseMoveEvent(e);
}

void FramelessDialog::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging    = false;
        m_resizeEdges = EdgeNone;
        setCursor(Qt::ArrowCursor);
        e->accept();
        return;
    }
    QDialog::mouseReleaseEvent(e);
}

void FramelessDialog::leaveEvent(QEvent *e)
{
    if (!m_dragging && m_resizeEdges == EdgeNone)
        setCursor(Qt::ArrowCursor);
    QDialog::leaveEvent(e);
}
