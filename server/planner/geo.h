#ifndef THGH_SERVER_PLANNER_GEO_H
#define THGH_SERVER_PLANNER_GEO_H

// ─────────────────────────────────────────────────────────────────────────────
// 大地坐标与距离计算
//
// 规划要按"通信距离"决定两个节点能否建链，而节点位置是经纬高（WGS84 大地坐标）。
// 经纬度是**角度**不是长度，不能直接做欧氏距离 ——
// 同样是 1 度经度差，在赤道约 111km，在北纬 60 度只有约 55km。
//
// 做法：先转成 ECEF（地心地固直角坐标系，单位米），再算三维欧氏距离。
// ECEF 的原点在地心、Z 轴指向北极、X 轴指向本初子午线与赤道交点，
// 三个轴都是长度量纲，可以直接做减法。
//
// 为什么不用 Haversine：Haversine 把地球当正球体算**地表大圆弧长**，
// 适合算航程。而通信链路走的是直线（视距传播），且节点有高度差
// （地面站与中继塔可能差几百米），必须用三维直线距离。
// ─────────────────────────────────────────────────────────────────────────────

namespace thgh {

// WGS84 椭球参数
namespace wgs84 {
inline constexpr double kSemiMajorAxis = 6378137.0;        // 长半轴 a，米
inline constexpr double kFlattening = 1.0 / 298.257223563; // 扁率 f
// 第一偏心率平方 e² = 2f - f²
inline constexpr double kEccentricitySquared =
    2.0 * kFlattening - kFlattening * kFlattening;
}  // namespace wgs84

// 真空光速，用于由距离推算传播时延
inline constexpr double kSpeedOfLight = 3e8;  // m/s

struct EcefPoint {
    double x = 0.0;  // 米
    double y = 0.0;
    double z = 0.0;
};

// WGS84 大地坐标 → ECEF 地心地固直角坐标。
// 输入经纬度为度，高度为米；输出三个分量均为米。
EcefPoint geodeticToEcef(double lonDeg, double latDeg, double altM);

// 两点间三维直线距离（米）
double distanceMeters(const EcefPoint& a, const EcefPoint& b);

// 直接由两组经纬高算距离（米）
double distanceMeters(double lon1, double lat1, double alt1, double lon2,
                      double lat2, double alt2);

// 由距离推算自由空间传播时延（秒）
inline double propagationDelaySec(double distanceM) {
    return distanceM / kSpeedOfLight;
}

}  // namespace thgh

#endif  // THGH_SERVER_PLANNER_GEO_H
