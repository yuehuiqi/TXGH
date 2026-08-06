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
        connect(model, &QAbstractItemModel::modelReset, this, [this]() { triggerAdjust(); }, Qt::UniqueConnection);
        connect(model, &QAbstractItemModel::rowsInserted, this, [this]() { triggerAdjust(); }, Qt::UniqueConnection);
        connect(model, &QAbstractItemModel::layoutChanged, this, [this]() { triggerAdjust(); }, Qt::UniqueConnection);
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

private:
    void triggerAdjust()
    {
        QTimer::singleShot(0, this, [this]() { adjustColumns(); });
    }

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
