-- ─────────────────────────────────────────────────────────────────────────────
-- THGH 服务端数据库 schema（MySQL 8.0）
--
-- 由客户端原有的 SQLite schema 迁移而来。迁移中的几个刻意选择：
--
--   1. **引擎必须是 InnoDB**：MyISAM 不支持事务和外键，而本项目的
--      "创建场景 + 批量插入节点 + 批量插入链路" 必须是原子的。
--      MySQL 8.0 默认就是 InnoDB，这里显式写出来避免依赖默认值。
--
--   2. **字符集 utf8mb4**：MySQL 的 "utf8" 是个历史遗留的坑，它只支持
--      最多 3 字节的字符，存不下 emoji 和部分生僻字。utf8mb4 才是真正的 UTF-8。
--
--   3. **索引先不建**：4.2 要求"用 EXPLAIN 对比索引前后耗时"，
--      所以基线表结构里只有主键和外键（外键会自动带索引）。
--      业务查询用的索引由 indexes.sql 单独添加，便于做前后对比实验。
--
--   4. 保留 SQLite 版的字段名与类型语义，让客户端的数据结构不用改。
-- ─────────────────────────────────────────────────────────────────────────────

CREATE DATABASE IF NOT EXISTS thgh
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_unicode_ci;

USE thgh;

-- ── 场景 ────────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS scenes (
    id           INT AUTO_INCREMENT PRIMARY KEY,
    name         VARCHAR(200)  NOT NULL,
    description  VARCHAR(2000),
    scene_type   VARCHAR(100),
    create_time  VARCHAR(50)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ── 节点模板 ────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS node_templates (
    id               INT AUTO_INCREMENT PRIMARY KEY,
    template_name    VARCHAR(200) NOT NULL,
    node_type        VARCHAR(100),
    comm_methods     VARCHAR(500),
    interference_db  DOUBLE DEFAULT 0.0,
    description      VARCHAR(2000),
    device_params    TEXT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ── 链路模板 ────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS link_templates (
    id                   INT AUTO_INCREMENT PRIMARY KEY,
    template_name        VARCHAR(200) NOT NULL,
    link_type            VARCHAR(100),
    wireless_type        VARCHAR(100),
    bandwidth_bps        DOUBLE DEFAULT 50000000,
    freq_hz              DOUBLE DEFAULT 3000000000,
    tx_power_dbm         DOUBLE DEFAULT 30,
    rx_sensitivity_dbm   DOUBLE DEFAULT -90,
    tx_antenna_gain_dbi  DOUBLE DEFAULT 0,
    rx_antenna_gain_dbi  DOUBLE DEFAULT 0,
    noise_figure_db      DOUBLE DEFAULT 7,
    snr_threshold_db     DOUBLE DEFAULT 10,
    path_loss_exponent   DOUBLE DEFAULT 2.0,
    additional_loss_db   DOUBLE DEFAULT 0,
    description          VARCHAR(2000)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ── 场景内的节点 ────────────────────────────────────────────────────────────
-- ON DELETE CASCADE：删除场景时自动清掉它的节点，避免孤儿数据。
-- 这是数据库层面保证一致性，比在应用层写"先删子表再删主表"可靠——
-- 应用层那种写法一旦中途崩溃就会留下垃圾。
CREATE TABLE IF NOT EXISTS scene_nodes (
    id                  INT AUTO_INCREMENT PRIMARY KEY,
    scene_id            INT NOT NULL,
    node_id             INT NOT NULL,
    from_template       INT,
    name                VARCHAR(200),
    node_type           VARCHAR(100),
    status              VARCHAR(50) DEFAULT '在线',
    longitude           DOUBLE NOT NULL,
    latitude            DOUBLE NOT NULL,
    altitude            DOUBLE DEFAULT 0.0,
    interference_db     DOUBLE DEFAULT 0.0,
    comm_methods        VARCHAR(500),
    device_params       TEXT,
    device_connections  TEXT,
    CONSTRAINT fk_scene_nodes_scene
        FOREIGN KEY (scene_id) REFERENCES scenes(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ── 场景内的链路 ────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS scene_links (
    id                   INT AUTO_INCREMENT PRIMARY KEY,
    scene_id             INT NOT NULL,
    src                  INT NOT NULL,
    dst                  INT NOT NULL,
    from_template        INT,
    link_type            VARCHAR(100),
    wireless_type        VARCHAR(100),
    bandwidth_bps        DOUBLE,
    prop_delay_s         DOUBLE,
    freq_hz              DOUBLE,
    tx_power_dbm         DOUBLE,
    rx_sensitivity_dbm   DOUBLE,
    tx_antenna_gain_dbi  DOUBLE,
    rx_antenna_gain_dbi  DOUBLE,
    noise_figure_db      DOUBLE,
    snr_threshold_db     DOUBLE,
    path_loss_exponent   DOUBLE,
    additional_loss_db   DOUBLE,
    comm_protocol        VARCHAR(200),
    device_type          VARCHAR(200),
    flows                TEXT,
    CONSTRAINT fk_scene_links_scene
        FOREIGN KEY (scene_id) REFERENCES scenes(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
