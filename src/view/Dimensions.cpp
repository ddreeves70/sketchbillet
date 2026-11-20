#include "sb/view/Dimensions.h"

#include <cmath>

namespace sb::view {

namespace {

using kernel::Vec3;

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
Vec3 unit(Vec3 a) {
    const double l = length(a);
    return l > 1e-12 ? mul(a, 1 / l) : Vec3{};
}

void line(std::vector<Vec3>& into, Vec3 a, Vec3 b) {
    into.push_back(a);
    into.push_back(b);
}

}  // namespace

DimensionDrawing dimensionDrawing(const ViewOverlay& o, double mmPerPixel, const ScreenAxes& screen,
                                  const DimensionSizes& sizes) {
    DimensionDrawing out;
    if (o.points.size() < 3 || mmPerPixel <= 0) return out;
    const Vec3 from = o.points[0], to = o.points[1], through = o.points[2];
    Vec3 u = unit(o.dimension.along);
    if (length(u) == 0) u = unit(sub(to, from));
    if (length(u) == 0) return out;  // nothing to measure along
    // Square to its line, on its plane.
    Vec3 side = unit(cross(unit(o.dimension.normal), u));
    if (length(side) == 0) side = unit(cross(Vec3{0, 0, 1}, u));
    if (length(side) == 0) side = unit(cross(Vec3{0, 1, 0}, u));
    const double px = mmPerPixel;

    // Its line: through `through`, level with each measured point.
    auto onLine = [&](Vec3 p) { return add(p, mul(side, dot(sub(through, p), side))); };
    const Vec3 a = onLine(from), b = onLine(to);
    out.lineFrom = a;
    out.lineTo = b;
    // Outwards: from what it measures towards its line.
    const double outwards = dot(sub(through, mul(add(from, to), 0.5)), side);
    const Vec3 outside = mul(side, outwards < 0 ? -1.0 : 1.0);

    // Extension lines, from just off each point to just past the line.
    for (const auto& [p, q] : {std::pair{from, a}, std::pair{to, b}}) {
        const double far = dot(sub(q, p), side);
        if (std::abs(far) <= sizes.gap * px) continue;  // the line passes (nearly) through it
        const Vec3 out1 = mul(side, far > 0 ? 1.0 : -1.0);
        line(out.lines, add(p, mul(out1, sizes.gap * px)), add(q, mul(out1, sizes.overshoot * px)));
    }

    // How far its number reaches along a direction, from its middle, in pixels.
    const double letters = o.texts.empty() ? 0 : static_cast<double>(o.texts[0].size());
    const double halfWide = 0.5 * sizes.letterWidth * o.size * letters, halfTall = 0.5 * o.size;
    auto reach = [&](Vec3 dir) {
        return std::abs(dot(dir, screen.right)) * halfWide + std::abs(dot(dir, screen.up)) * halfTall;
    };

    // The line itself, with a gap for its number, and arrowheads pointing out at each end.
    const Vec3 toB = unit(sub(b, a));
    const Vec3 middle = mul(add(a, b), 0.5);
    const double lineLength = length(sub(b, a)) / px;  // pixels
    const bool across = std::abs(dot(u, screen.right)) >= std::abs(dot(u, screen.up));
    const double hole = reach(u) + sizes.textGap;  // half the gap, in pixels
    const bool inGap =
        !o.dimension.textAtEnd && letters > 0 && across && lineLength >= 2 * (hole + sizes.arrowLength);
    if (inGap) {
        line(out.lines, a, sub(middle, mul(toB, hole * px)));
        line(out.lines, add(middle, mul(toB, hole * px)), b);
    } else {
        line(out.lines, a, b);
    }
    auto arrow = [&](Vec3 tip, Vec3 back) {
        const Vec3 base = add(tip, mul(back, sizes.arrowLength * px));
        line(out.lines, tip, add(base, mul(side, sizes.arrowWidth * px)));
        line(out.lines, tip, add(base, mul(side, -sizes.arrowWidth * px)));
    };
    if (length(toB) > 0) {
        if (o.dimension.arrowAtFrom) arrow(a, toB);
        if (o.dimension.arrowAtTo) arrow(b, mul(toB, -1));
    }

    // The number: past the end of a leader, in the gap, or beside the middle, outside.
    if (o.dimension.textAtEnd && length(toB) > 0) {
        out.textAt = add(b, mul(toB, (sizes.textGap + reach(toB)) * px));
    } else if (inGap) {
        out.textAt = middle;
    } else {
        out.textBeside = true;
        out.textAt = add(middle, mul(outside, (sizes.textGap + reach(outside)) * px));
    }
    return out;
}

std::vector<ViewOverlay> expandDimensions(const std::vector<ViewOverlay>& overlays, double mmPerPixel,
                                          const ScreenAxes& screen) {
    std::vector<ViewOverlay> out;
    out.reserve(overlays.size());
    for (const auto& o : overlays) {
        if (o.shape != OverlayShape::Dimension) {
            out.push_back(o);
            continue;
        }
        const auto drawn = dimensionDrawing(o, mmPerPixel, screen);
        ViewOverlay lines = o;
        lines.shape = OverlayShape::Lines;
        lines.points = drawn.lines;
        lines.texts.clear();
        lines.size = 1.5;
        out.push_back(std::move(lines));
        if (!o.texts.empty() && !o.texts[0].empty() && !drawn.lines.empty()) {
            ViewOverlay words = o;
            words.shape = OverlayShape::Labels;
            words.points = {drawn.textAt};
            words.texts = {o.texts[0]};
            words.boxed = true;
            out.push_back(std::move(words));
        }
    }
    return out;
}

}  // namespace sb::view
