#ifndef TABLEUTILS_H
#define TABLEUTILS_H

#include <QTableView>
#include <QHeaderView>
#include <QAbstractItemModel>
#include <QSignalBlocker>
#include <QTimer>
#include <QEvent>
#include <QVector>

class TableResizeEventFilter : public QObject
{
    // ★ 必须有 Q_OBJECT：下面的 connect 用了 Qt::UniqueConnection，
    //   而 UniqueConnection **只支持成员函数指针**，不支持 lambda/仿函数。
    //   要用成员函数指针做信号槽连接，这个类就得有元对象。
    Q_OBJECT
public:
    explicit TableResizeEventFilter(QTableView *tv) : QObject(tv), m_tv(tv), m_lastModel(nullptr) {
        tv->installEventFilter(this);
        if (tv->model()) {
            m_lastModel = tv->model();
            connectModel(m_lastModel);
        }
    }

    void connectModel(QAbstractItemModel *model) {
        if (!model) return;
        // ⚠️ 这里原来把 lambda 和 Qt::UniqueConnection 一起用了。
        //    Qt 文档明确写着 UniqueConnection **不能用于 lambda/仿函数**，
        //    只能用于 QObject 子类的成员函数指针。
        //    Qt5 下这是静默无效（连接照建，但去重不生效，模型换来换去会重复连）；
        //    **Qt6 直接拒绝建立连接**并打印
        //      "unique connections require a pointer to member function of a QObject subclass"
        //    ——也就是说表格列宽在模型重置/插入行时根本不会再自适应了，
        //    而且没有任何报错指向业务代码，只有一行 qt.core.qobject.connect 警告。
        //    改成成员函数指针后，UniqueConnection 才真正生效。
        connect(model, &QAbstractItemModel::modelReset,
                this, &TableResizeEventFilter::triggerAdjust, Qt::UniqueConnection);
        connect(model, &QAbstractItemModel::rowsInserted,
                this, &TableResizeEventFilter::triggerAdjust, Qt::UniqueConnection);
        connect(model, &QAbstractItemModel::layoutChanged,
                this, &TableResizeEventFilter::triggerAdjust, Qt::UniqueConnection);
    }

protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (obj == m_tv) {
            // Check if model has changed
            QAbstractItemModel *currentModel = m_tv->model();
            if (currentModel != m_lastModel) {
                m_lastModel = currentModel;
                connectModel(currentModel);
                triggerAdjust();
            }

            if (event->type() == QEvent::Resize) {
                triggerAdjust();
            } else if (event->type() == QEvent::Show) {
                triggerAdjust();
            }
        }
        return QObject::eventFilter(obj, event);
    }

public slots:
    // 作为槽被三个信号连接。三个信号的参数各不相同
    // （rowsInserted 带 3 个参数、layoutChanged 带 2 个），
    // 槽的参数比信号少是允许的，多出来的会被丢弃。
    void triggerAdjust()
    {
        QTimer::singleShot(0, this, [this]() { adjustColumns(); });
    }

private:

    void adjustColumns()
    {
        if (!m_tv) return;
        QHeaderView *header = m_tv->horizontalHeader();
        int columnCount = header->count();
        if (columnCount <= 0) return;

        QSignalBlocker blocker(header);

        // Temporarily set to ResizeToContents to get contents widths
        for (int i = 0; i < columnCount; ++i) {
            header->setSectionResizeMode(i, QHeaderView::ResizeToContents);
        }
        
        m_tv->doItemsLayout();

        QVector<int> widths(columnCount);
        int totalWidth = 0;
        for (int i = 0; i < columnCount; ++i) {
            int w = m_tv->columnWidth(i) + 25; // 25px extra padding
            widths[i] = w;
            totalWidth += w;
        }

        int viewportWidth = m_tv->viewport()->width();
        if (viewportWidth > totalWidth) {
            int extra = viewportWidth - totalWidth;
            int addPerCol = extra / columnCount;
            int remainder = extra % columnCount;
            for (int i = 0; i < columnCount; ++i) {
                widths[i] += addPerCol + (i < remainder ? 1 : 0);
            }
        }

        for (int i = 0; i < columnCount; ++i) {
            header->setSectionResizeMode(i, QHeaderView::Interactive);
            m_tv->setColumnWidth(i, widths[i]);
        }
    }

private:
    QTableView *m_tv;
    QAbstractItemModel *m_lastModel;
};

// Unified light-theme stylesheet for all table views
static const char* kDataTableStyleSheet =
    "QTableView, QTableWidget {"
    "  font-size: 13px;"
    "  gridline-color: #E2E8F0;"
    "  border: 1px solid #D0DCE8;"
    "  border-radius: 4px;"
    "  outline: none;"
    "  background-color: #FFFFFF;"
    "}"
    "QHeaderView::section {"
    "  background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
    "    stop:0 #EBF4FF, stop:1 #D6EAFF);"
    "  color: #1565C0;"
    "  font-size: 13px !important;"
    "  font-weight: 600 !important;"
    "  padding: 4px 8px !important;"
    "  border: none;"
    "  border-bottom: 2px solid #90CAF9;"
    "}"
    "QHeaderView::section:last {"
    "  border-right: none;"
    "}"
    "QTableView::item, QTableWidget::item {"
    "  padding: 3px 8px;"
    "  border: none;"
    "  color: #2C3E50;"
    "}"
    "QTableView::item:selected, QTableWidget::item:selected {"
    "  background: #BBDEFB;"
    "  color: #0D47A1;"
    "  font-size: 13px !important;"
    "  font-weight: normal !important;"
    "}"
    "QTableView::item:alternate, QTableWidget::item:alternate {"
    "  background: #F0F7FF;"
    "}";

inline void styleDataTable(QTableView *tv)
{
    tv->setSelectionBehavior(QAbstractItemView::SelectRows);
    tv->setSelectionMode(QAbstractItemView::SingleSelection);
    tv->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tv->setAlternatingRowColors(true);
    tv->verticalHeader()->setVisible(false);
    tv->horizontalHeader()->setHighlightSections(false);
    
    // Install the event filter
    new TableResizeEventFilter(tv);

    tv->horizontalHeader()->setFixedHeight(36);
    tv->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    tv->setStyleSheet(kDataTableStyleSheet);
}

#endif // TABLEUTILS_H
