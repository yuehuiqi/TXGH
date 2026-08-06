-- ─────────────────────────────────────────────────────────────────────────────
-- 演示数据
--
-- 用途：让"起容器 → 打开客户端"直接能看到一个完整场景，
--       不用先手工画一遍节点和链路。演示、联调、录屏都用得上。
--
-- ⚠️ 这是**演示数据不是生产数据**。全部坐标是华北地区的示意位置，
--    不对应任何真实部署。
--
-- 幂等：先按名字删掉同名场景再插，重复执行不会产生副本。
--       子表靠外键 ON DELETE CASCADE 自动清理。
--
-- 用法：
--   本地/裸机   mysql -u thgh -p thgh < server/db/seed.sql
--   容器        docker exec -i thgh-mysql mysql -uthgh -p密码 thgh < server/db/seed.sql
-- ─────────────────────────────────────────────────────────────────────────────

SET NAMES utf8mb4;

-- ── 清理旧的同名演示数据（幂等）──────────────────────────────────────────
DELETE FROM scenes WHERE name = '演示场景·华北通信网';

-- ── 场景 ────────────────────────────────────────────────────────────────────
INSERT INTO scenes (name, description, scene_type, create_time)
VALUES ('演示场景·华北通信网',
        '3 个干线节点 + 4 个支线节点，覆盖光缆/微波/卫星三种通信方式，用于演示与联调',
        '演示',
        '2026-08-06 10:00:00');

SET @sid = LAST_INSERT_ID();

-- ── 节点 ────────────────────────────────────────────────────────────────────
-- node_id 是用户可见的编号（1..N），id 是数据库主键。
-- comm_methods 存的是 JSON 文本数组 —— 客户端 modelcodec 就是按这个格式解析的。
--
-- 布局：3 个干线沿东西向铺开（相距约 40km，在默认 150km 通信距离内），
--       4 个支线各自靠近某个干线。这样规划算法能算出有意义的多跳路径。
INSERT INTO scene_nodes
    (scene_id, node_id, from_template, name, node_type, status,
     longitude, latitude, altitude, interference_db,
     comm_methods, device_params, device_connections)
VALUES
    -- 干线：两两互联，构成骨干
    (@sid, 1, -1, '干线节点·西', '干线', '在线', 116.10, 39.90,  50.0, 0.0,
     '["fiber","microwave"]', '[]', '[]'),
    (@sid, 2, -1, '干线节点·中', '干线', '在线', 116.50, 39.92, 120.0, 0.0,
     '["fiber","microwave","satellite"]', '[]', '[]'),
    (@sid, 3, -1, '干线节点·东', '干线', '在线', 116.90, 39.88,  80.0, 0.0,
     '["fiber","microwave"]', '[]', '[]'),
    -- 支线：只接入干线，不互联（分层组网的典型结构）
    (@sid, 4, -1, '支线节点·甲', '支线', '在线', 116.15, 40.05,  20.0, 1.5,
     '["microwave","manet"]', '[]', '[]'),
    (@sid, 5, -1, '支线节点·乙', '支线', '在线', 116.45, 40.08,  30.0, 2.0,
     '["microwave","manet"]', '[]', '[]'),
    (@sid, 6, -1, '支线节点·丙', '支线', '在线', 116.55, 39.75,  15.0, 0.8,
     '["microwave","satellite"]', '[]', '[]'),
    (@sid, 7, -1, '支线节点·丁', '支线', '离线', 116.95, 40.02,  25.0, 3.2,
     '["manet"]', '[]', '[]');

-- ── 链路 ────────────────────────────────────────────────────────────────────
-- prop_delay_s 这里给的是示意值；实际规划时服务端会按 WGS84→ECEF 距离重算
-- （见 server/planner/geo.cpp），所以这里的值只影响"打开场景时看到的初始状态"。
INSERT INTO scene_links
    (scene_id, src, dst, from_template, link_type, wireless_type,
     bandwidth_bps, prop_delay_s, comm_protocol, device_type, flows)
VALUES
    -- 干线骨干：光缆
    (@sid, 1, 2, -1, 'wired',    '',           10000000000, 0.000115, 'IP', '光缆', '[]'),
    (@sid, 2, 3, -1, 'wired',    '',           10000000000, 0.000115, 'IP', '光缆', '[]'),
    (@sid, 1, 3, -1, 'wireless', 'microwave',     50000000, 0.000230, '',   '',    '[]'),
    -- 支线接入：微波
    (@sid, 4, 1, -1, 'wireless', 'microwave',     50000000, 0.000060, '',   '',    '[]'),
    (@sid, 5, 2, -1, 'wireless', 'microwave',     50000000, 0.000065, '',   '',    '[]'),
    (@sid, 6, 2, -1, 'wireless', 'satellite',      2000000, 0.120000, '',   '',    '[]'),
    (@sid, 7, 3, -1, 'wireless', 'manet',         25000000, 0.000055, '',   '',    '[]');

-- ── 模板 ────────────────────────────────────────────────────────────────────
-- 幂等：先删同名再插
DELETE FROM node_templates WHERE template_name IN ('演示·干线站', '演示·支线站');
INSERT INTO node_templates
    (template_name, node_type, comm_methods, interference_db, description, device_params)
VALUES
    ('演示·干线站', '干线', '["fiber","microwave"]',  0.0, '骨干节点，光缆 + 微波双备份', '[]'),
    ('演示·支线站', '支线', '["microwave","manet"]',  1.5, '接入节点，微波回传 + 自组网', '[]');

DELETE FROM link_templates WHERE template_name IN ('演示·光缆干线', '演示·微波接入', '演示·卫星备份');
INSERT INTO link_templates
    (template_name, link_type, wireless_type, bandwidth_bps, description)
VALUES
    ('演示·光缆干线', 'wired',    '',          10000000000, '骨干光缆，10Gbps'),
    ('演示·微波接入', 'wireless', 'microwave',    50000000, '支线接入微波，50Mbps'),
    ('演示·卫星备份', 'wireless', 'satellite',     2000000, '卫星备份链路，2Mbps 但时延 120ms');

-- ── 结果 ────────────────────────────────────────────────────────────────────
SELECT CONCAT('演示数据已写入：场景 id=', @sid,
              '，节点 ', (SELECT COUNT(*) FROM scene_nodes WHERE scene_id = @sid),
              ' 个，链路 ', (SELECT COUNT(*) FROM scene_links WHERE scene_id = @sid),
              ' 条') AS 结果;
