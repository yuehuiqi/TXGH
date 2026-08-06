#include "planner/geo.h"

#include <cmath>

namespace thgh {
namespace {

constexpr double kPi = 3.14159265358979323846;

inline double toRadians(double deg) { return deg * kPi / 180.0; }

}  // namespace

EcefPoint geodeticToEcef(double lonDeg, double latDeg, double altM) {
    const double lon = toRadians(lonDeg);
    const double lat = toRadians(latDeg);

    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);

    // N 是卯酉圈曲率半径：从当前点沿法线到 Z 轴的距离。
    // 地球是扁的（极半径比赤道半径短约 21km），所以这个值随纬度变化，
    // 不能用一个固定的地球半径代替 —— 那正是把地球当正球体的近似做法，
    // 在高纬度会有公里级误差。
    const double N = wgs84::kSemiMajorAxis /
                     std::sqrt(1.0 - wgs84::kEccentricitySquared * sinLat * sinLat);

    EcefPoint p;
    p.x = (N + altM) * cosLat * std::cos(lon);
    p.y = (N + altM) * cosLat * std::sin(lon);
    // Z 分量要乘 (1 - e²)：椭球在极方向被压扁，
    // 同一个纬度对应的 Z 比正球体模型小
    p.z = (N * (1.0 - wgs84::kEccentricitySquared) + altM) * sinLat;
    return p;
}

double distanceMeters(const EcefPoint& a, const EcefPoint& b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double dz = b.z - a.z;
    // 用 std::hypot 的三参数重载：它内部做了防溢出/防下溢处理，
    // 比手写 sqrt(dx*dx+dy*dy+dz*dz) 在极端量级下更稳。
    // 这里的量级（1e6~1e7 米）平方后是 1e14，double 完全够用，
    // 但用标准设施没有额外成本。
    return std::hypot(std::hypot(dx, dy), dz);
}

double distanceMeters(double lon1, double lat1, double alt1, double lon2,
                      double lat2, double alt2) {
    return distanceMeters(geodeticToEcef(lon1, lat1, alt1),
                          geodeticToEcef(lon2, lat2, alt2));
}

}  // namespace thgh
