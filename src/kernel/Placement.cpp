#include "sb/kernel/Placement.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sb::kernel {

namespace {

Vec3 add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 mul(Vec3 a, double k) {
    return {a.x * k, a.y * k, a.z * k};
}
double dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double length(Vec3 a) {
    return std::sqrt(dot(a, a));
}

// Turns v by `angle` about the unit direction `axis` (Rodrigues' formula).
Vec3 turn(Vec3 v, Vec3 axis, double angle) {
    const double c = std::cos(angle), s = std::sin(angle);
    return add(add(mul(v, c), mul(cross(axis, v), s)), mul(axis, dot(axis, v) * (1 - c)));
}

}  // namespace

Vec3 yAxisOf(const Placement& p) {
    return cross(p.zAxis, p.xAxis);
}

std::optional<Placement> tidyPlacement(const Placement& p) {
    const double zl = length(p.zAxis);
    if (zl < 1e-12) return std::nullopt;
    const Vec3 z = mul(p.zAxis, 1 / zl);
    const Vec3 x = sub(p.xAxis, mul(z, dot(p.xAxis, z)));  // square to z
    const double xl = length(x);
    if (xl < 1e-9 * std::max(1.0, length(p.xAxis))) return std::nullopt;
    return Placement{p.origin, mul(x, 1 / xl), z};
}

Point3 apply(const Placement& p, const Point3& point) {
    return add(p.origin, applyDirection(p, point));
}

Vec3 applyDirection(const Placement& p, const Vec3& d) {
    return add(add(mul(p.xAxis, d.x), mul(yAxisOf(p), d.y)), mul(p.zAxis, d.z));
}

Placement compose(const Placement& outer, const Placement& inner) {
    return {apply(outer, inner.origin), applyDirection(outer, inner.xAxis),
            applyDirection(outer, inner.zAxis)};
}

Placement inverse(const Placement& p) {
    // The transpose of the turn, then the origin moved back.
    const Vec3 x = p.xAxis, y = yAxisOf(p), z = p.zAxis;
    const Vec3 newX{x.x, y.x, z.x};
    const Vec3 newZ{x.z, y.z, z.z};
    const Vec3 o = p.origin;
    return {{-dot(x, o), -dot(y, o), -dot(z, o)}, newX, newZ};
}

Motion toMotion(const Placement& p) {
    // The turn's matrix has the placement's directions as its columns.
    const Vec3 x = p.xAxis, y = yAxisOf(p), z = p.zAxis;
    const double m[3][3] = {{x.x, y.x, z.x}, {x.y, y.y, z.y}, {x.z, y.z, z.z}};
    const double trace = m[0][0] + m[1][1] + m[2][2];
    const double angle = std::acos(std::clamp((trace - 1) / 2, -1.0, 1.0));
    Motion out;
    out.offset = p.origin;
    out.center = {};
    if (angle < 1e-12) {
        out.axis = {0, 0, 1};
        out.angle = 0;
        return out;
    }
    Vec3 axis{m[2][1] - m[1][2], m[0][2] - m[2][0], m[1][0] - m[0][1]};
    if (std::numbers::pi - angle < 1e-6) {
        // Half a turn: the axis is the matrix's biggest column of (M + I) / 2.
        const double xx = (m[0][0] + 1) / 2, yy = (m[1][1] + 1) / 2, zz = (m[2][2] + 1) / 2;
        if (xx >= yy && xx >= zz) {
            const double ax = std::sqrt(std::max(xx, 0.0));
            axis = {ax, m[0][1] / (2 * ax), m[0][2] / (2 * ax)};
        } else if (yy >= zz) {
            const double ay = std::sqrt(std::max(yy, 0.0));
            axis = {m[0][1] / (2 * ay), ay, m[1][2] / (2 * ay)};
        } else {
            const double az = std::sqrt(std::max(zz, 0.0));
            axis = {m[0][2] / (2 * az), m[1][2] / (2 * az), az};
        }
    }
    const double l = length(axis);
    out.axis = mul(axis, 1 / l);
    out.angle = angle;
    return out;
}

Placement fromMotion(const Motion& m) {
    const double l = length(m.axis);
    auto turned = [&](Vec3 v) {
        return l < 1e-12 || m.angle == 0 ? v : turn(v, mul(m.axis, 1 / l), m.angle);
    };
    // A point q goes to turned(q - center) + center + offset.
    const Point3 origin = add(add(turned(mul(m.center, -1)), m.center), m.offset);
    return {origin, turned({1, 0, 0}), turned({0, 0, 1})};
}

bool samePlacement(const Placement& a, const Placement& b, double tolerance) {
    return length(sub(a.origin, b.origin)) <= tolerance && length(sub(a.xAxis, b.xAxis)) <= tolerance &&
           length(sub(a.zAxis, b.zAxis)) <= tolerance;
}

}  // namespace sb::kernel
