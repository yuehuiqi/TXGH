#include "mainwindow.h"

#include <QApplication>
#include <QByteArray>
#include <QMessageBox>
#include <cstdlib>

#include <QAbstractButton>

// 全局过滤器：所有 QMessageBox 去掉系统标题栏，并翻译标准按钮为中文
class FramelessMsgFilter : public QObject
{
public:
    explicit FramelessMsgFilter(QObject *parent = nullptr) : QObject(parent) {}
protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == QEvent::Polish) {
            if (QMessageBox *box = qobject_cast<QMessageBox*>(obj)) {
                box->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
                for (QAbstractButton *btn : box->buttons()) {
                    QMessageBox::StandardButton stdBtn = box->standardButton(btn);
                    switch (stdBtn) {
                        case QMessageBox::Ok:       btn->setText("确定"); break;
                        case QMessageBox::Cancel:   btn->setText("取消"); break;
                        case QMessageBox::Yes:      btn->setText("是"); break;
                        case QMessageBox::No:       btn->setText("否"); break;
                        case QMessageBox::Save:     btn->setText("保存"); break;
                        case QMessageBox::Discard:  btn->setText("丢弃"); break;
                        case QMessageBox::Close:    btn->setText("关闭"); break;
                        case QMessageBox::Apply:    btn->setText("应用"); break;
                        case QMessageBox::Reset:    btn->setText("重置"); break;
                        case QMessageBox::Help:     btn->setText("帮助"); break;
                        case QMessageBox::Abort:    btn->setText("中止"); break;
                        case QMessageBox::Retry:    btn->setText("重试"); break;
                        case QMessageBox::Ignore:   btn->setText("忽略"); break;
                        default: break;
                    }
                }
            }
        }
        return false;
    }
};

int main(int argc, char *argv[])
{

//    // 1. 关键：禁止 Qt 自动查询系统缩放倍数
//    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, false);
//    // 2. 关键：手动设置全局缩放因子为 1.5
//    qputenv("QT_SCALE_FACTOR", "1.5");
//    QApplication a(argc, argv);
//    // 解决图标在高分屏下的模糊问题
//    a.setAttribute(Qt::AA_UseHighDpiPixmaps);


    // ── 达梦 ODBC 驱动加载路径设置 ───────────────────────────────────────────
    // 项目当前已切换为 QSQLITE，故下面整段都已注释。如需切回达梦，按平台启用：
#ifdef Q_OS_WIN
    /*
    // Windows：把达梦 DM8 客户端 bin 目录加入 PATH，确保 QODBC 能找到 dpodbc.dll
    {
        QByteArray cur = qgetenv("PATH");
        QByteArray dmBin = "C:\\dmdbms\\bin";
        if (!cur.contains(dmBin))
            qputenv("PATH", dmBin + ";" + cur);
    }
    */
#endif
#ifdef Q_OS_LINUX
    /*
    // Linux（含麒麟银河）：把达梦客户端 lib 目录加入 LD_LIBRARY_PATH，
    // 并指向 odbcinst.ini 中注册的"DM8 ODBC DRIVER"。
    // 安装包通常落在 /opt/dmdbms/bin（含 libdodbc.so）
    {
        QByteArray cur = qgetenv("LD_LIBRARY_PATH");
        QByteArray dmLib = "/opt/dmdbms/bin";
        if (!cur.contains(dmLib))
            qputenv("LD_LIBRARY_PATH",
                    cur.isEmpty() ? dmLib : (dmLib + ":" + cur));
    }
    */
#endif

    // Qt6 起高 DPI 缩放**始终开启**，AA_EnableHighDpiScaling 已弃用且不再有任何效果，
    // 所以这行删掉（留着只会产生弃用告警）。
    //
    // 需要控制的是**缩放因子的舍入策略**，这个在 Qt6 里仍然有效且必须在
    // QApplication 构造之前设置。PassThrough 保留小数缩放（125% 就是 1.25），
    // 相比默认的 Round（四舍五入到整数倍）在 125%/150% 这类常见 Windows 缩放下
    // 布局更贴合设计稿，不会出现控件被撑开或字体错位。
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication a(argc, argv);
    a.installEventFilter(new FramelessMsgFilter(&a));
    QFile styleFile(":/resources/style/style_light.qss");
    if (styleFile.open(QFile::ReadOnly)) {
        a.setStyleSheet(styleFile.readAll());
        styleFile.close();
    }
    MainWindow w;
    w.show();
    return a.exec();
}
