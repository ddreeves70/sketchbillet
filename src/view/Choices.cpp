#include "sb/view/Choices.h"

#include <cmath>
#include <limits>

namespace sb::view {

namespace {

kernel::Vec3 at(const ViewChoice& c, double u, double v) {
    return {c.origin.x + c.xAxis.x * u + c.yAxis.x * v, c.origin.y + c.xAxis.y * u + c.yAxis.y * v,
            c.origin.z + c.xAxis.z * u + c.yAxis.z * v};
}

// Whether a point is inside a polygon on the screen (even-odd rule).
bool inside(const std::array<ScreenPoint, 4>& poly, ScreenPoint p) {
    bool in = false;
    for (std::size_t i = 0, j = 3; i < 4; j = i++) {
        const auto& a = poly[i];
        const auto& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

// How far a point is from a line segment on the screen, in pixels.
double distance(ScreenPoint a, ScreenPoint b, ScreenPoint p) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
    t = std::fmax(0.0, std::fmin(1.0, t));
    return std::hypot(a.x + t * dx - p.x, a.y + t * dy - p.y);
}

// How far along the line of sight it meets the choice's plane, or infinity.
double alongSight(const ViewChoice& c, const Ray& sight) {
    const kernel::Vec3 n{c.xAxis.y * c.yAxis.z - c.xAxis.z * c.yAxis.y,
                         c.xAxis.z * c.yAxis.x - c.xAxis.x * c.yAxis.z,
                         c.xAxis.x * c.yAxis.y - c.xAxis.y * c.yAxis.x};
    const double facing = n.x * sight.direction.x + n.y * sight.direction.y + n.z * sight.direction.z;
    if (std::abs(facing) < 1e-12) return std::numeric_limits<double>::infinity();
    const double t = (n.x * (c.origin.x - sight.from.x) + n.y * (c.origin.y - sight.from.y) +
                      n.z * (c.origin.z - sight.from.z)) /
                     facing;
    return t;
}

}  // namespace

std::array<kernel::Vec3, 4> choiceCorners(const ViewChoice& c) {
    return {at(c, c.from, c.from), at(c, c.to, c.from), at(c, c.to, c.to), at(c, c.from, c.to)};
}

kernel::Vec3 choiceLabelAt(const ViewChoice& c) {
    // A little in from the middle of its far side, so the words of squares that
    // meet at one point (the base planes) don't land on each other.
    const double size = c.to - c.from;
    return at(c, c.from + 0.3 * size, c.to - 0.12 * size);
}

std::optional<std::size_t> choiceAt(const std::vector<ViewChoice>& choices, const Projection& project,
                                    ScreenPoint mouse, double reach, const std::optional<Ray>& sight) {
    std::optional<std::size_t> best;
    double bestDepth = std::numeric_limits<double>::infinity();
    bool bestInside = false;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        std::array<ScreenPoint, 4> poly{};
        bool shown = true;
        const auto corners = choiceCorners(choices[i]);
        for (std::size_t k = 0; k < 4; ++k) {
            const auto s = project(corners[k]);
            if (!s) {
                shown = false;
                break;
            }
            poly[k] = *s;
        }
        if (!shown) continue;
        const bool in = inside(poly, mouse);
        bool near = false;
        for (std::size_t k = 0; k < 4 && !near; ++k)
            near = distance(poly[k], poly[(k + 1) % 4], mouse) <= reach;
        if (!in && !near) continue;
        // Inside beats only near a side; then the nearest along the line of sight.
        const double depth = sight ? alongSight(choices[i], *sight) : static_cast<double>(i);
        if (!best || (in && !bestInside) || (in == bestInside && depth < bestDepth)) {
            best = i;
            bestDepth = depth;
            bestInside = in;
        }
    }
    return best;
}

}  // namespace sb::view
