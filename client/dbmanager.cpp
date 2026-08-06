#include "dbmanager.h"

#include "modelcodec.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QVariant>
#include <QDebug>
#include <QtMath>
#include <QDateTime>
#include <QCoreApplication>

int DbManager::s_instanceCount = 0;

DbManager::DbManager(QObject *parent)
    : QObject(parent)
{
}

DbManager::~DbManager()
{
    if (m_db.isOpen())
        m_db.close();
    QString connName = m_db.connectionName();
    m_db = QSqlDatabase();
    if (!connName.isEmpty())
        QSqlDatabase::removeDatabase(connName);
}

// ─── 初始化 ───────────────────────────────────────────────────────────────────
bool DbManager::initDB(const QString &path)
{
    // SQLite path: if path is empty, default to applicationDirPath() + "/sim_data.db"
    if (!path.isEmpty())
        m_dbPath = path.trimmed();
    
    // Default SQLite database file path if empty or if it contains the legacy host:port string
    if (m_dbPath.isEmpty() || m_dbPath == "127.0.0.1:5236") {
        m_dbPath = QCoreApplication::applicationDirPath() + "/sim_data.db";
    }

    // 关闭旧连接
    if (m_db.isOpen()) {
        m_db.close();
        QString oldConn = m_db.connectionName();
        m_db = QSqlDatabase();
        if (!oldConn.isEmpty())
            QSqlDatabase::removeDatabase(oldConn);
    }

    /*
    // ======= 达梦数据库连接代码（Windows，按需启用，已注释）=======
    // 前置条件：已安装 DM8 客户端，且在 ODBC 数据源管理器中注册了 "DM8 ODBC DRIVER"
    // （32/64 位需与本程序位宽一致）。或通过本程序 main.cpp 的 PATH 把
    // C:\dmdbms\bin 加入搜索路径，让 QODBC 找到 dpodbc.dll。
    QString host = "127.0.0.1";
    QString port = "5236";
    const QStringList parts = m_dbPath.split(":");
    if (parts.size() >= 1 && !parts[0].isEmpty()) host = parts[0].trimmed();
    if (parts.size() >= 2 && !parts[1].isEmpty()) port = parts[1].trimmed();

    QString connName = QString("txgh_db_%1").arg(s_instanceCount++);
    m_db = QSqlDatabase::addDatabase("QODBC", connName);

    m_db.setDatabaseName(
        QString("DRIVER={DM8 ODBC DRIVER};"
                "SERVER=%1;PORT=%2;"
                "DATABASE=DAMENG;"
                "UID=TXGH_USER;PWD=Txgh_Pwd_2026;").arg(host, port)
    );

    if (!m_db.open()) {
        qWarning() << "DbManager: cannot open DM database:" << m_db.lastError().text();
        return false;
    }
    // ===============================================

    // ======= 达梦数据库连接代码（Linux / 麒麟银河，按需启用，已注释）=======
    // 前置条件：
    //   1) 安装 DM8 Linux 客户端（含 libdodbc.so）到 /opt/dmdbms
    //   2) 安装 unixODBC：apt/yum install unixodbc unixodbc-dev
    //   3) 在 /etc/odbcinst.ini 注册 DM8 驱动：
    //        [DM8 ODBC DRIVER]
    //        Description = DM ODBC Driver
    //        Driver = /opt/dmdbms/bin/libdodbc.so
    //   4) main.cpp 已把 /opt/dmdbms/bin 加到 LD_LIBRARY_PATH（同样已注释）
    //
    // 连接串与 Windows 完全一致，由 unixODBC 转发到 libdodbc.so
    //
    // QString host = "127.0.0.1";
    // QString port = "5236";
    // const QStringList parts = m_dbPath.split(":");
    // if (parts.size() >= 1 && !parts[0].isEmpty()) host = parts[0].trimmed();
    // if (parts.size() >= 2 && !parts[1].isEmpty()) port = parts[1].trimmed();
    //
    // QString connName = QString("txgh_db_%1").arg(s_instanceCount++);
    // m_db = QSqlDatabase::addDatabase("QODBC", connName);
    // m_db.setDatabaseName(
    //     QString("DRIVER={DM8 ODBC DRIVER};"
    //             "SERVER=%1;PORT=%2;"
    //             "DATABASE=DAMENG;"
    //             "UID=TXGH_USER;PWD=Txgh_Pwd_2026;").arg(host, port)
    // );
    // if (!m_db.open()) {
    //     qWarning() << "DbManager: cannot open DM database (Linux):"
    //                << m_db.lastError().text();
    //     return false;
    // }
    // ===============================================
    */

    // ======= SQLite 数据库连接代码 =======
    QString connName = QString("txgh_db_%1").arg(s_instanceCount++);
    m_db = QSqlDatabase::addDatabase("QSQLITE", connName);
    m_db.setDatabaseName(m_dbPath);

    if (!m_db.open()) {
        qWarning() << "DbManager: cannot open SQLite database:" << m_db.lastError().text();
        return false;
    }

    // 启用 SQLite 外键约束
    execSQL("PRAGMA foreign_keys = ON;");

    bool ok = true;

    /*
    // ======= 达梦数据库建表语句 (只注释不删除) =======
    ok &= execSQL(
        "CREATE TABLE IF NOT EXISTS scenes ("
        " id INT IDENTITY(1,1) NOT NULL PRIMARY KEY,"
        " name VARCHAR(200) NOT NULL,"
        " description VARCHAR(2000),"
        " scene_type VARCHAR(100),"
        " create_time VARCHAR(50))"
    );
    ok &= execSQL(
        "CREATE TABLE IF NOT EXISTS node_templates ("
        " id INT IDENTITY(1,1) NOT NULL PRIMARY KEY,"
        " template_name VARCHAR(200) NOT NULL,"
        " node_type VARCHAR(100),"
        " comm_methods VARCHAR(500),"
        " interference_db DOUBLE DEFAULT 0.0,"
        " description VARCHAR(2000))"
    );
    ok &= execSQL(
        "CREATE TABLE IF NOT EXISTS link_templates ("
        " id INT IDENTITY(1,1) NOT NULL PRIMARY KEY,"
        " template_name VARCHAR(200) NOT NULL,"
        " link_type VARCHAR(100),"
        " wireless_type VARCHAR(100),"
        " bandwidth_bps DOUBLE DEFAULT 50000000,"
        " freq_hz DOUBLE DEFAULT 3000000000,"
        " tx_power_dbm DOUBLE DEFAULT 30,"
        " rx_sensitivity_dbm DOUBLE DEFAULT -90,"
        " tx_antenna_gain_dbi DOUBLE DEFAULT 0,"
        " rx_antenna_gain_dbi DOUBLE DEFAULT 0,"
        " noise_figure_db DOUBLE DEFAULT 7,"
        " snr_threshold_db DOUBLE DEFAULT 10,"
        " path_loss_exponent DOUBLE DEFAULT 2.0,"
        " additional_loss_db DOUBLE DEFAULT 0,"
        " description VARCHAR(2000))"
    );
    ok &= execSQL(
        "CREATE TABLE IF NOT EXISTS scene_nodes ("
        " id INT IDENTITY(1,1) NOT NULL PRIMARY KEY,"
        " scene_id INT NOT NULL REFERENCES scenes(id) ON DELETE CASCADE,"
        " node_id INT NOT NULL,"
        " from_template INT,"
        " name VARCHAR(200),"
        " node_type VARCHAR(100),"
        " status VARCHAR(50) DEFAULT '在线',"
        " longitude DOUBLE NOT NULL,"
        " latitude DOUBLE NOT NULL,"
        " altitude DOUBLE DEFAULT 0.0,"
        " interference_db DOUBLE DEFAULT 0.0,"
        " comm_methods VARCHAR(500),"
        " device_params CLOB)"
    );
    // 兼容旧数据库：若 device_params 列不存在，尝试添加（忽略错误）
    execSQL("ALTER TABLE scene_nodes ADD device_params CLOB");
    ok &= execSQL(
        "CREATE TABLE IF NOT EXISTS scene_links ("
        " id INT IDENTITY(1,1) NOT NULL PRIMARY KEY,"
        " scene_id INT NOT NULL REFERENCES scenes(id) ON DELETE CASCADE,"
        " src INT NOT NULL,"
        " dst INT NOT NULL,"
        " from_template INT,"
        " link_type VARCHAR(100),"
        " wireless_type VARCHAR(100),"
        " bandwidth_bps DOUBLE,"
        " prop_delay_s DOUBLE,"
        " freq_hz DOUBLE,"
        " tx_power_dbm DOUBLE,"
        " rx_sensitivity_dbm DOUBLE,"
        " tx_antenna_gain_dbi DOUBLE,"
        " rx_antenna_gain_dbi DOUBLE,"
        " noise_figure_db DOUBLE,"
        " snr_threshold_db DOUBLE,"
        " path_loss_exponent DOUBLE,"
        " additional_loss_db DOUBLE,"
        " comm_protocol VARCHAR(200),"
        " device_type VARCHAR(200),"
        " flows CLOB)"
    );
    // 兼容旧数据库：若 flows 列不存在，尝试添加（忽略错误）
    execSQL("ALTER TABLE scene_links ADD flows CLOB");
    // ===============================================
    */

    // ======= SQLite 建表语句 =======
    ok &= execSQL(
        "CREATE TABLE IF NOT EXISTS scenes ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name VARCHAR(200) NOT NULL,"
        " description VARCHAR(2000),"
        " scene_type VARCHAR(100),"
        " create_time VARCHAR(50))"
    );
    ok &= execSQL(
        "CREATE TABLE IF NOT EXISTS node_templates ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " template_name VARCHAR(200) NOT NULL,"
        " node_type VARCHAR(100),"
        " comm_methods VARCHAR(500),"
        " interference_db DOUBLE DEFAULT 0.0,"
        " description VARCHAR(2000))"
    );
    ok &= execSQL(
        "CREATE TABLE IF NOT EXISTS link_templates ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " template_name VARCHAR(200) NOT NULL,"
        " link_type VARCHAR(100),"
        " wireless_type VARCHAR(100),"
        " bandwidth_bps DOUBLE DEFAULT 50000000,"
        " freq_hz DOUBLE DEFAULT 3000000000,"
        " tx_power_dbm DOUBLE DEFAULT 30,"
        " rx_sensitivity_dbm DOUBLE DEFAULT -90,"
        " tx_antenna_gain_dbi DOUBLE DEFAULT 0,"
        " rx_antenna_gain_dbi DOUBLE DEFAULT 0,"
        " noise_figure_db DOUBLE DEFAULT 7,"
        " snr_threshold_db DOUBLE DEFAULT 10,"
        " path_loss_exponent DOUBLE DEFAULT 2.0,"
        " additional_loss_db DOUBLE DEFAULT 0,"
        " description VARCHAR(2000))"
    );
    ok &= execSQL(
        "CREATE TABLE IF NOT EXISTS scene_nodes ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " scene_id INTEGER NOT NULL REFERENCES scenes(id) ON DELETE CASCADE,"
        " node_id INT NOT NULL,"
        " from_template INTEGER,"
        " name VARCHAR(200),"
        " node_type VARCHAR(100),"
        " status VARCHAR(50) DEFAULT '在线',"
        " longitude DOUBLE NOT NULL,"
        " latitude DOUBLE NOT NULL,"
        " altitude DOUBLE DEFAULT 0.0,"
        " interference_db DOUBLE DEFAULT 0.0,"
        " comm_methods VARCHAR(500),"
        " device_params TEXT)"
    );
    // 兼容旧数据库：若新列不存在，尝试添加（忽略错误）
    execSQL("ALTER TABLE scene_nodes ADD COLUMN device_params TEXT");
    execSQL("ALTER TABLE scene_nodes ADD COLUMN device_connections TEXT");
    execSQL("ALTER TABLE node_templates ADD COLUMN device_params TEXT");
    ok &= execSQL(
        "CREATE TABLE IF NOT EXISTS scene_links ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " scene_id INTEGER NOT NULL REFERENCES scenes(id) ON DELETE CASCADE,"
        " src INT NOT NULL,"
        " dst INT NOT NULL,"
        " from_template INTEGER,"
        " link_type VARCHAR(100),"
        " wireless_type VARCHAR(100),"
        " bandwidth_bps DOUBLE,"
        " prop_delay_s DOUBLE,"
        " freq_hz DOUBLE,"
        " tx_power_dbm DOUBLE,"
        " rx_sensitivity_dbm DOUBLE,"
        " tx_antenna_gain_dbi DOUBLE,"
        " rx_antenna_gain_dbi DOUBLE,"
        " noise_figure_db DOUBLE,"
        " snr_threshold_db DOUBLE,"
        " path_loss_exponent DOUBLE,"
        " additional_loss_db DOUBLE,"
        " comm_protocol VARCHAR(200),"
        " device_type VARCHAR(200),"
        " flows TEXT)"
    );
    execSQL("ALTER TABLE scene_links ADD COLUMN flows TEXT");

    return ok;
}

bool DbManager::execSQL(const QString &sql)
{
    QSqlQuery q(m_db);
    if (!q.exec(sql)) {
        qWarning() << "SQL error:" << q.lastError().text();
        return false;
    }
    return true;
}

// SQLite 获取自增主键
int DbManager::lastInsertedId()
{
    /*
    // ======= 达梦数据库获取自增ID代码 (只注释不删除) =======
    QSqlQuery q(m_db);
    if (q.exec("SELECT SCOPE_IDENTITY()") && q.next()) {
        QVariant v = q.value(0);
        if (!v.isNull()) return v.toInt();
    }
    return -1;
    // ====================================================
    */

    QSqlQuery q(m_db);
    if (q.exec("SELECT last_insert_rowid()") && q.next()) {
        QVariant v = q.value(0);
        if (!v.isNull()) return v.toInt();
    }
    return -1;
}


// ─── 传播时延 ─────────────────────────────────────────────────────────────────
double DbManager::calcPropDelay(double lon1, double lat1, double alt1,
                                double lon2, double lat2, double alt2)
{
    const double a  = 6378137.0;
    const double e2 = 0.00669437999014;

    auto toEcef = [&](double lon, double lat, double alt,
                      double &x, double &y, double &z) {
        double lonR = qDegreesToRadians(lon);
        double latR = qDegreesToRadians(lat);
        double N = a / qSqrt(1.0 - e2 * qSin(latR) * qSin(latR));
        x = (N + alt) * qCos(latR) * qCos(lonR);
        y = (N + alt) * qCos(latR) * qSin(lonR);
        z = (N * (1.0 - e2) + alt) * qSin(latR);
    };

    double x1, y1, z1, x2, y2, z2;
    toEcef(lon1, lat1, alt1, x1, y1, z1);
    toEcef(lon2, lat2, alt2, x2, y2, z2);
    double dist = qSqrt((x2-x1)*(x2-x1) + (y2-y1)*(y2-y1) + (z2-z1)*(z2-z1));
    return dist / 3.0e8;
}

// ─── 场景 CRUD ────────────────────────────────────────────────────────────────
int DbManager::createScene(const SceneInfo &s)
{
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO scenes(name,description,scene_type,create_time)"
              " VALUES(:n,:d,:t,:c)");
    q.bindValue(":n", s.name);
    q.bindValue(":d", s.description);
    q.bindValue(":t", s.sceneType);
    q.bindValue(":c", s.createTime.isEmpty()
                      ? QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
                      : s.createTime);
    if (!q.exec()) { qWarning() << "createScene:" << q.lastError().text(); return -1; }
    return lastInsertedId();
}

QList<SceneInfo> DbManager::listScenes()
{
    QList<SceneInfo> result;
    QSqlQuery q(m_db);
    q.exec("SELECT id,name,description,scene_type,create_time FROM scenes ORDER BY id");
    while (q.next()) {
        SceneInfo s;
        s.id          = q.value(0).toInt();
        s.name        = q.value(1).toString();
        s.description = q.value(2).toString();
        s.sceneType   = q.value(3).toString();
        s.createTime  = q.value(4).toString();
        result << s;
    }
    return result;
}

bool DbManager::updateScene(const SceneInfo &s)
{
    QSqlQuery q(m_db);
    q.prepare("UPDATE scenes SET name=:n,description=:d,scene_type=:t WHERE id=:id");
    q.bindValue(":n",  s.name);
    q.bindValue(":d",  s.description);
    q.bindValue(":t",  s.sceneType);
    q.bindValue(":id", s.id);
    if (!q.exec()) { qWarning() << "updateScene:" << q.lastError().text(); return false; }
    return true;
}

bool DbManager::deleteScene(int sceneId)
{
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM scenes WHERE id=:id");
    q.bindValue(":id", sceneId);
    if (!q.exec()) { qWarning() << "deleteScene:" << q.lastError().text(); return false; }
    return true;
}

// ─── 节点 CRUD ────────────────────────────────────────────────────────────────
int DbManager::addNode(const NodeInfo &n)
{
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO scene_nodes"
        "(scene_id,node_id,from_template,name,node_type,status,"
        " longitude,latitude,altitude,interference_db,comm_methods,device_params,device_connections)"
        " VALUES(:sid,:nid,:ftpl,:nm,:tp,:st,:lon,:lat,:alt,:idb,:cm,:dp,:dc)"
    );
    q.bindValue(":sid",  n.sceneId);
    q.bindValue(":nid",  n.nodeId);
    q.bindValue(":ftpl", n.fromTemplate < 0 ? QVariant() : QVariant(n.fromTemplate));
    q.bindValue(":nm",   n.name);
    q.bindValue(":tp",   n.nodeType);
    q.bindValue(":st",   n.status.isEmpty() ? QString::fromUtf8("\u5728\u7ebf") : n.status);
    q.bindValue(":lon",  n.longitude);
    q.bindValue(":lat",  n.latitude);
    q.bindValue(":alt",  n.altitude);
    q.bindValue(":idb",  n.interferenceDb);
    q.bindValue(":cm",   modelcodec::methodsToJson(n.commMethods));
    q.bindValue(":dp",   modelcodec::deviceParamsToJson(n.deviceParams));
    q.bindValue(":dc",   modelcodec::deviceConnsToJson(n.deviceConnections));
    if (!q.exec()) { qWarning() << "addNode:" << q.lastError().text(); return -1; }
    return lastInsertedId();
}

QList<NodeInfo> DbManager::getNodesByScene(int sceneId)
{
    QList<NodeInfo> result;
    QSqlQuery q(m_db);
    q.prepare(
        "SELECT id,scene_id,node_id,from_template,name,node_type,status,"
        "       longitude,latitude,altitude,interference_db,comm_methods,device_params,device_connections"
        " FROM scene_nodes WHERE scene_id=:sid ORDER BY node_id"
    );
    q.bindValue(":sid", sceneId);
    q.exec();
    while (q.next()) {
        NodeInfo n;
        n.id               = q.value(0).toInt();
        n.sceneId          = q.value(1).toInt();
        n.nodeId           = q.value(2).toInt();
        n.fromTemplate     = q.value(3).isNull() ? -1 : q.value(3).toInt();
        n.name             = q.value(4).toString();
        n.nodeType         = q.value(5).toString();
        n.status           = q.value(6).toString();
        n.longitude        = q.value(7).toDouble();
        n.latitude         = q.value(8).toDouble();
        n.altitude         = q.value(9).toDouble();
        n.interferenceDb   = q.value(10).toDouble();
        n.commMethods      = modelcodec::jsonToMethods(q.value(11).toString());
        n.deviceParams     = modelcodec::jsonToDeviceParams(q.value(12).toString());
        n.deviceConnections = modelcodec::jsonToDeviceConns(q.value(13).toString());
        result << n;
    }
    return result;
}

bool DbManager::updateNode(const NodeInfo &n)
{
    QSqlQuery q(m_db);
    q.prepare(
        "UPDATE scene_nodes SET"
        " node_id=:nid,name=:nm,node_type=:tp,status=:st,"
        " longitude=:lon,latitude=:lat,altitude=:alt,"
        " interference_db=:idb,comm_methods=:cm,device_params=:dp,device_connections=:dc"
        " WHERE id=:id"
    );
    q.bindValue(":nid", n.nodeId);
    q.bindValue(":nm",  n.name);
    q.bindValue(":tp",  n.nodeType);
    q.bindValue(":st",  n.status);
    q.bindValue(":lon", n.longitude);
    q.bindValue(":lat", n.latitude);
    q.bindValue(":alt", n.altitude);
    q.bindValue(":idb", n.interferenceDb);
    q.bindValue(":cm",  modelcodec::methodsToJson(n.commMethods));
    q.bindValue(":dp",  modelcodec::deviceParamsToJson(n.deviceParams));
    q.bindValue(":dc",  modelcodec::deviceConnsToJson(n.deviceConnections));
    q.bindValue(":id",  n.id);
    if (!q.exec()) { qWarning() << "updateNode:" << q.lastError().text(); return false; }
    return true;
}

bool DbManager::deleteNode(int nodeDbId)
{
    // 先查出该节点的 scene_id 和 node_id，以便级联删除关联链路
    QSqlQuery qn(m_db);
    qn.prepare("SELECT scene_id, node_id FROM scene_nodes WHERE id=:id");
    qn.bindValue(":id", nodeDbId);
    if (qn.exec() && qn.next()) {
        int sceneId = qn.value(0).toInt();
        int nodeId  = qn.value(1).toInt();
        // 删除以该节点为端点的所有链路（DB无FK约束，需手动清理）
        QSqlQuery ql(m_db);
        ql.prepare("DELETE FROM scene_links WHERE scene_id=:sid AND (src=:nid OR dst=:nid)");
        ql.bindValue(":sid", sceneId);
        ql.bindValue(":nid", nodeId);
        ql.exec();
    }
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM scene_nodes WHERE id=:id");
    q.bindValue(":id", nodeDbId);
    if (!q.exec()) { qWarning() << "deleteNode:" << q.lastError().text(); return false; }
    return true;
}

// ─── 链路 CRUD ────────────────────────────────────────────────────────────────
// ── 批量新增（P6 新增）─────────────────────────────────────────────────────
// 本地实现用事务包住：一是原子性（导入一半失败不该留残缺场景），
// 二是性能——SQLite/ODBC 每条 INSERT 各自提交时开销极大。
// 远端实现（RemoteDataStore）把这件事变成一次请求，服务端侧再合并成
// 多值 INSERT，收益更大。

bool DbManager::addNodes(int sceneId, const QList<NodeInfo> &nodes)
{
    if (nodes.isEmpty()) return true;
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    for (NodeInfo n : nodes) {
        n.sceneId = sceneId;
        if (addNode(n) < 0) {
            m_db.rollback();
            return false;
        }
    }
    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

bool DbManager::addLinks(int sceneId, const QList<LinkInfo> &links)
{
    if (links.isEmpty()) return true;
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    for (LinkInfo l : links) {
        l.sceneId = sceneId;
        if (addLink(l) < 0) {
            m_db.rollback();
            return false;
        }
    }
    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

int DbManager::addLink(const LinkInfo &l)
{
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO scene_links"
        "(scene_id,src,dst,from_template,link_type,wireless_type,"
        " bandwidth_bps,prop_delay_s,"
        " freq_hz,tx_power_dbm,rx_sensitivity_dbm,"
        " tx_antenna_gain_dbi,rx_antenna_gain_dbi,"
        " noise_figure_db,snr_threshold_db,"
        " path_loss_exponent,additional_loss_db,"
        " comm_protocol,device_type,flows)"
        " VALUES(:sid,:src,:dst,:ftpl,:lt,:wt,"
        ":bw,:pd,:freq,:txp,:rxs,:txg,:rxg,:nf,:snr,:ple,:al,:cp,:dt,:flows)"
    );
    q.bindValue(":sid",  l.sceneId);
    q.bindValue(":src",  l.src);
    q.bindValue(":dst",  l.dst);
    q.bindValue(":ftpl", l.fromTemplate < 0 ? QVariant() : QVariant(l.fromTemplate));
    q.bindValue(":lt",   l.linkType);
    q.bindValue(":wt",   l.wirelessType);
    q.bindValue(":bw",   l.bandwidthBps);
    q.bindValue(":pd",   l.propDelayS);
    q.bindValue(":freq", l.freqHz);
    q.bindValue(":txp",  l.txPowerDbm);
    q.bindValue(":rxs",  l.rxSensitivityDbm);
    q.bindValue(":txg",  l.txAntennaGainDbi);
    q.bindValue(":rxg",  l.rxAntennaGainDbi);
    q.bindValue(":nf",   l.noiseFigureDb);
    q.bindValue(":snr",  l.snrThresholdDb);
    q.bindValue(":ple",  l.pathLossExponent);
    q.bindValue(":al",   l.additionalLossDb);
    q.bindValue(":cp",   l.commProtocol);
    q.bindValue(":dt",   l.deviceType);
    q.bindValue(":flows", modelcodec::flowsToJson(l.flows));
    if (!q.exec()) { qWarning() << "addLink:" << q.lastError().text(); return -1; }
    return lastInsertedId();
}

QList<LinkInfo> DbManager::getLinksByScene(int sceneId)
{
    QList<LinkInfo> result;
    QSqlQuery q(m_db);
    q.prepare(
        "SELECT id,scene_id,src,dst,from_template,link_type,wireless_type,"
        "       bandwidth_bps,prop_delay_s,"
        "       freq_hz,tx_power_dbm,rx_sensitivity_dbm,"
        "       tx_antenna_gain_dbi,rx_antenna_gain_dbi,"
        "       noise_figure_db,snr_threshold_db,"
        "       path_loss_exponent,additional_loss_db,"
        "       comm_protocol,device_type,flows"
        " FROM scene_links WHERE scene_id=:sid ORDER BY id"
    );
    q.bindValue(":sid", sceneId);
    q.exec();
    while (q.next()) {
        LinkInfo l;
        l.id               = q.value(0).toInt();
        l.sceneId          = q.value(1).toInt();
        l.src              = q.value(2).toInt();
        l.dst              = q.value(3).toInt();
        l.fromTemplate     = q.value(4).isNull() ? -1 : q.value(4).toInt();
        l.linkType         = q.value(5).toString();
        l.wirelessType     = q.value(6).toString();
        l.bandwidthBps     = q.value(7).toDouble();
        l.propDelayS       = q.value(8).toDouble();
        l.freqHz           = q.value(9).toDouble();
        l.txPowerDbm       = q.value(10).toDouble();
        l.rxSensitivityDbm = q.value(11).toDouble();
        l.txAntennaGainDbi = q.value(12).toDouble();
        l.rxAntennaGainDbi = q.value(13).toDouble();
        l.noiseFigureDb    = q.value(14).toDouble();
        l.snrThresholdDb   = q.value(15).toDouble();
        l.pathLossExponent = q.value(16).toDouble();
        l.additionalLossDb = q.value(17).toDouble();
        l.commProtocol     = q.value(18).toString();
        l.deviceType       = q.value(19).toString();
        l.flows            = modelcodec::jsonToFlows(q.value(20).toString());
        result << l;
    }
    return result;
}

bool DbManager::updateLink(const LinkInfo &l)
{
    QSqlQuery q(m_db);
    q.prepare(
        "UPDATE scene_links SET"
        " src=:src,dst=:dst,link_type=:lt,wireless_type=:wt,"
        " bandwidth_bps=:bw,prop_delay_s=:pd,"
        " freq_hz=:freq,tx_power_dbm=:txp,rx_sensitivity_dbm=:rxs,"
        " tx_antenna_gain_dbi=:txg,rx_antenna_gain_dbi=:rxg,"
        " noise_figure_db=:nf,snr_threshold_db=:snr,"
        " path_loss_exponent=:ple,additional_loss_db=:al,"
        " comm_protocol=:cp,device_type=:dt,flows=:flows"
        " WHERE id=:id"
    );
    q.bindValue(":src",  l.src);
    q.bindValue(":dst",  l.dst);
    q.bindValue(":lt",   l.linkType);
    q.bindValue(":wt",   l.wirelessType);
    q.bindValue(":bw",   l.bandwidthBps);
    q.bindValue(":pd",   l.propDelayS);
    q.bindValue(":freq", l.freqHz);
    q.bindValue(":txp",  l.txPowerDbm);
    q.bindValue(":rxs",  l.rxSensitivityDbm);
    q.bindValue(":txg",  l.txAntennaGainDbi);
    q.bindValue(":rxg",  l.rxAntennaGainDbi);
    q.bindValue(":nf",   l.noiseFigureDb);
    q.bindValue(":snr",  l.snrThresholdDb);
    q.bindValue(":ple",  l.pathLossExponent);
    q.bindValue(":al",   l.additionalLossDb);
    q.bindValue(":cp",   l.commProtocol);
    q.bindValue(":dt",   l.deviceType);
    q.bindValue(":flows", modelcodec::flowsToJson(l.flows));
    q.bindValue(":id",   l.id);
    if (!q.exec()) { qWarning() << "updateLink:" << q.lastError().text(); return false; }
    return true;
}

bool DbManager::deleteLink(int linkDbId)
{
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM scene_links WHERE id=:id");
    q.bindValue(":id", linkDbId);
    if (!q.exec()) { qWarning() << "deleteLink:" << q.lastError().text(); return false; }
    return true;
}

bool DbManager::clearLinksByScene(int sceneId)
{
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM scene_links WHERE scene_id=:sid");
    q.bindValue(":sid", sceneId);
    if (!q.exec()) { qWarning() << "clearLinksByScene:" << q.lastError().text(); return false; }
    return true;
}

// ─── 节点模板 ─────────────────────────────────────────────────────────────────
int DbManager::saveNodeTemplate(const NodeTemplate &t)
{
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO node_templates(template_name,node_type,comm_methods,"
        "  interference_db,description,device_params) VALUES(:n,:tp,:cm,:idb,:desc,:dp)"
    );
    q.bindValue(":n",    t.name);
    q.bindValue(":tp",   t.nodeType);
    q.bindValue(":cm",   modelcodec::methodsToJson(t.commMethods));
    q.bindValue(":idb",  t.defaultInterferenceDb);
    q.bindValue(":desc", t.description);
    q.bindValue(":dp",   modelcodec::deviceParamsToJson(t.deviceParams));
    if (!q.exec()) { qWarning() << "saveNodeTemplate:" << q.lastError().text(); return -1; }
    return lastInsertedId();
}

QList<NodeTemplate> DbManager::listNodeTemplates()
{
    QList<NodeTemplate> result;
    QSqlQuery q(m_db);
    q.exec("SELECT id,template_name,node_type,comm_methods,interference_db,description,device_params"
           " FROM node_templates ORDER BY id");
    while (q.next()) {
        NodeTemplate t;
        t.id                    = q.value(0).toInt();
        t.name                  = q.value(1).toString();
        t.nodeType              = q.value(2).toString();
        t.commMethods           = modelcodec::jsonToMethods(q.value(3).toString());
        t.defaultInterferenceDb = q.value(4).toDouble();
        t.description           = q.value(5).toString();
        t.deviceParams          = modelcodec::jsonToDeviceParams(q.value(6).toString());
        result << t;
    }
    return result;
}

// ─── 链路模板 ─────────────────────────────────────────────────────────────────
int DbManager::saveLinkTemplate(const LinkTemplate &t)
{
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO link_templates"
        "(template_name,link_type,wireless_type,"
        " bandwidth_bps,freq_hz,tx_power_dbm,rx_sensitivity_dbm,"
        " tx_antenna_gain_dbi,rx_antenna_gain_dbi,"
        " noise_figure_db,snr_threshold_db,"
        " path_loss_exponent,additional_loss_db,description)"
        " VALUES(:n,:lt,:wt,:bw,:freq,:txp,:rxs,:txg,:rxg,:nf,:snr,:ple,:al,:desc)"
    );
    q.bindValue(":n",    t.name);
    q.bindValue(":lt",   t.linkType);
    q.bindValue(":wt",   t.wirelessType);
    q.bindValue(":bw",   t.bandwidthBps);
    q.bindValue(":freq", t.freqHz);
    q.bindValue(":txp",  t.txPowerDbm);
    q.bindValue(":rxs",  t.rxSensitivityDbm);
    q.bindValue(":txg",  t.txAntennaGainDbi);
    q.bindValue(":rxg",  t.rxAntennaGainDbi);
    q.bindValue(":nf",   t.noiseFigureDb);
    q.bindValue(":snr",  t.snrThresholdDb);
    q.bindValue(":ple",  t.pathLossExponent);
    q.bindValue(":al",   t.additionalLossDb);
    q.bindValue(":desc", t.description);
    if (!q.exec()) { qWarning() << "saveLinkTemplate:" << q.lastError().text(); return -1; }
    return lastInsertedId();
}

QList<LinkTemplate> DbManager::listLinkTemplates()
{
    QList<LinkTemplate> result;
    QSqlQuery q(m_db);
    q.exec(
        "SELECT id,template_name,link_type,wireless_type,"
        "       bandwidth_bps,freq_hz,tx_power_dbm,rx_sensitivity_dbm,"
        "       tx_antenna_gain_dbi,rx_antenna_gain_dbi,"
        "       noise_figure_db,snr_threshold_db,"
        "       path_loss_exponent,additional_loss_db,description"
        " FROM link_templates ORDER BY id"
    );
    while (q.next()) {
        LinkTemplate t;
        t.id               = q.value(0).toInt();
        t.name             = q.value(1).toString();
        t.linkType         = q.value(2).toString();
        t.wirelessType     = q.value(3).toString();
        t.bandwidthBps     = q.value(4).toDouble();
        t.freqHz           = q.value(5).toDouble();
        t.txPowerDbm       = q.value(6).toDouble();
        t.rxSensitivityDbm = q.value(7).toDouble();
        t.txAntennaGainDbi = q.value(8).toDouble();
        t.rxAntennaGainDbi = q.value(9).toDouble();
        t.noiseFigureDb    = q.value(10).toDouble();
        t.snrThresholdDb   = q.value(11).toDouble();
        t.pathLossExponent = q.value(12).toDouble();
        t.additionalLossDb = q.value(13).toDouble();
        t.description      = q.value(14).toString();
        result << t;
    }
    return result;
}
