#ifndef FRAMELESSDIALOG_H
#define FRAMELESSDIALOG_H

#include <QDialog>
#include <QPoint>
#include <QRect>

/**
 * 无边框可拖拽/拉伸对话框基类。继承此类即可获得：
 *  - 无系统标题栏（Qt::FramelessWindowHint）
 *  - 实心背景（无透明）
 *  - 上边缘 40px 区域拖拽移动
 *  - 四边及四角拉伸调整大小
 *
 * 平台兼容：
 *  - Windows：走 nativeEvent (WM_NCHITTEST)，由 OS 处理拖拽与拉伸，体验最好。
 *  - Linux/macOS：用 Qt 事件 (mousePress/Move/Release) 实现拖拽与拉伸，
 *    保证在麒麟银河等系统上能正常使用。
 */
class FramelessDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FramelessDialog(QWidget *parent = nullptr,
                             Qt::WindowFlags f = Qt::Dialog);

protected:
    // Qt6：第三个参数由 long* 改为 qintptr*，见 mainwindow.h 同名函数的注释
    bool nativeEvent(const QByteArray &eventType,
                     void *message, qintptr *result) override;

    // 非 Windows 平台（或 Windows 客户区）下用 Qt 事件做拖拽/拉伸
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    // 命中边缘组合标志
    enum Edge { EdgeNone = 0, EdgeLeft = 1, EdgeRight = 2, EdgeTop = 4, EdgeBottom = 8 };
    int  hitEdges(const QPoint &localPos) const;
    void updateCursorForEdges(int edges);

    bool   m_dragging      = false;
    int    m_resizeEdges   = 0;
    QPoint m_pressGlobal;
    QRect  m_pressGeom;

    static const int kBorderWidth   = 5;   // 拉伸响应边宽（px）
    static const int kCaptionHeight = 40;  // 顶部拖拽带高度（px）
};

#endif // FRAMELESSDIALOG_H
