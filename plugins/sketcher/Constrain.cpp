#include "Constrain.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace sb::plugins::sketcher {

namespace {

using sketch::ConstraintKind;
using sketch::Point2;
using sketch::PointRef;
using sketch::PointRole;
constexpr double kPi = std::numbers::pi;

sketch::Constraint make(ConstraintKind kind, std::vector<PointRef> points, std::vector<EntityId> entities,
                        double value = 0) {
    sketch::Constraint c;
    c.kind = kind;
    c.points = std::move(points);
    c.entities = std::move(entities);
    c.value = value;
    return c;
}

const sketch::LineShape* lineOf(const sketch::Sketch& s, const EntityId& id) {
    const auto* e = s.find(id);
    return e ? std::get_if<sketch::LineShape>(&e->shape) : nullptr;
}

double direction(const sketch::LineShape& l) {
    return std::atan2(l.end.y - l.start.y, l.end.x - l.start.x);
}

// An angle wrapped into (-180, 180] degrees, in radians.
double wrap(double a) {
    while (a <= -kPi) a += 2 * kPi;
    while (a > kPi) a -= 2 * kPi;
    return a;
}

Point2 mid(Point2 a, Point2 b) {
    return {(a.x + b.x) / 2, (a.y + b.y) / 2};
}

// "40", "12.5", "3.33": up to 2 decimals, no trailing zeros.
std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

}  // namespace

const std::vector<Rule>& allRules() {
    static const std::vector<Rule> rules{Rule::Coincident,
                                         Rule::Horizontal,
                                         Rule::Vertical,
                                         Rule::Parallel,
                                         Rule::Perpendicular,
                                         Rule::Equal,
                                         Rule::Tangent,
                                         Rule::Smooth,
                                         Rule::On,
                                         Rule::Midpoint,
                                         Rule::Symmetric,
                                         Rule::Fix,
                                         Rule::Dimension,
                                         Rule::HorizontalDimension,
                                         Rule::VerticalDimension};
    return rules;
}

RuleInfo ruleInfo(Rule rule) {
    switch (rule) {
        case Rule::Coincident:
            return {"coincident", "Coincident",
                    "Join two points into one. Pick two points (ends or centers).", "two points"};
        case Rule::Horizontal:
            return {"horizontal", "Horizontal", "Make the picked lines horizontal. Pick one or more lines.",
                    "one or more lines"};
        case Rule::Vertical:
            return {"vertical", "Vertical", "Make the picked lines vertical. Pick one or more lines.",
                    "one or more lines"};
        case Rule::Parallel:
            return {"parallel", "Parallel", "Make lines point the same way. Pick two or more lines.",
                    "two or more lines"};
        case Rule::Perpendicular:
            return {"perpendicular", "Perpendicular", "Make two lines meet at 90 degrees. Pick two lines.",
                    "two lines"};
        case Rule::Equal:
            return {"equal", "Equal",
                    "Make lines the same length, or circles and arcs the same size. Pick two or more of one "
                    "kind.",
                    "two or more lines, or two or more circles and arcs"};
        case Rule::Tangent:
            return {
                "tangent", "Tangent",
                "Make a line or circle touch a circle or arc without crossing it, or make a spline leave the "
                "end of a line, arc or spline it's joined to without a corner. Pick the two of them.",
                "a line and a circle or arc, two circles or arcs, or a spline and a curve joined to its end"};
        case Rule::Smooth:
            return {
                "smooth", "Smooth",
                "Make a spline join a line, arc or another spline with no seam you can see: no corner, and "
                "bending just as much on both sides. Pick the two of them; their ends must be joined.",
                "a spline and a line, arc or spline joined to its end"};
        case Rule::On:
            return {
                "on", "On",
                "Put a point on a line, circle or spline. Pick the point, then the line, circle or spline.",
                "a point and a line, circle, arc or spline"};
        case Rule::Midpoint:
            return {"midpoint", "Midpoint",
                    "Put a point in the middle of a line. Pick the point and the line.",
                    "a point and a line"};
        case Rule::Symmetric:
            return {"symmetric", "Symmetric",
                    "Make two points mirror images across a line, like the two sides of a bracket across its "
                    "center "
                    "line. Pick the two points, then the line.",
                    "two points and a line"};
        case Rule::Fix:
            return {"fix", "Fix", "Pin points where they are now. Pick one or more points.",
                    "one or more points"};
        case Rule::Dimension:
            return {"dimension", "Dimension",
                    "Set a size: a line's length, a circle's diameter, an arc's radius, how far apart two "
                    "points are, or the angle between two lines. Then type the number in the Sketch panel.",
                    "a line, a circle, an arc, two points or two lines"};
        case Rule::HorizontalDimension:
            return {"horizontal_dimension", "Width (horizontal dimension)",
                    "Set how far apart two points are from left to right. Pick two points, or a line.",
                    "two points or a line"};
        case Rule::VerticalDimension:
            return {"vertical_dimension", "Height (vertical dimension)",
                    "Set how far apart two points are from bottom to top. Pick two points, or a line.",
                    "two points or a line"};
    }
    return {"", "", "", ""};
}

std::vector<sketch::Constraint> plan(Rule rule, const Picks& p, const sketch::Sketch& s) {
    const std::size_t n = p.count();
    const auto P = p.points.size(), L = p.lines.size(), R = p.rounds.size(), S = p.splines.size();
    // A spline and a curve joined end to end (a line, an arc or another spline),
    // for Tangent and Smooth: the spline first.
    auto joined = [&]() -> std::optional<std::pair<EntityId, EntityId>> {
        if (n != 2 || S < 1) return std::nullopt;
        std::optional<EntityId> other;
        if (S == 2) other = p.splines[1];
        if (L == 1) other = p.lines[0];
        if (R == 1) {
            const auto* e = s.find(p.rounds[0]);
            if (e && std::holds_alternative<sketch::ArcShape>(e->shape))
                other = p.rounds[0];  // a circle has no ends
        }
        if (!other || !sketch::jointOf(s, p.splines[0], *other)) return std::nullopt;
        return std::pair{p.splines[0], *other};
    };
    std::vector<sketch::Constraint> out;
    auto each = [&](ConstraintKind kind) {
        for (const auto& l : p.lines) out.push_back(make(kind, {}, {l}));
    };
    auto pairs = [&](ConstraintKind kind, const std::vector<EntityId>& ids) {
        for (std::size_t i = 1; i < ids.size(); ++i) out.push_back(make(kind, {}, {ids[0], ids[i]}));
    };
    switch (rule) {
        case Rule::Coincident:
            if (P == 2 && n == 2)
                out.push_back(make(ConstraintKind::Coincident, {p.points[0], p.points[1]}, {}));
            break;
        case Rule::Horizontal:
            if (L >= 1 && n == L) each(ConstraintKind::Horizontal);
            break;
        case Rule::Vertical:
            if (L >= 1 && n == L) each(ConstraintKind::Vertical);
            break;
        case Rule::Parallel:
            if (L >= 2 && n == L) pairs(ConstraintKind::Parallel, p.lines);
            break;
        case Rule::Perpendicular:
            if (L == 2 && n == 2)
                out.push_back(make(ConstraintKind::Perpendicular, {}, {p.lines[0], p.lines[1]}));
            break;
        case Rule::Equal:
            if (L >= 2 && n == L) pairs(ConstraintKind::Equal, p.lines);
            if (R >= 2 && n == R) pairs(ConstraintKind::Equal, p.rounds);
            break;
        case Rule::Tangent:
            if (n == 2 && L == 1 && R == 1)
                out.push_back(make(ConstraintKind::Tangent, {}, {p.lines[0], p.rounds[0]}));
            if (n == 2 && R == 2)
                out.push_back(make(ConstraintKind::Tangent, {}, {p.rounds[0], p.rounds[1]}));
            if (const auto j = joined())
                out.push_back(make(ConstraintKind::Tangent, {}, {j->first, j->second}));
            break;
        case Rule::Smooth:
            if (const auto j = joined())
                out.push_back(make(ConstraintKind::Smooth, {}, {j->first, j->second}));
            break;
        case Rule::On:
            if (n == 2 && P == 1 && L == 1)
                out.push_back(make(ConstraintKind::PointOnLine, {p.points[0]}, {p.lines[0]}));
            if (n == 2 && P == 1 && R == 1)
                out.push_back(make(ConstraintKind::PointOnCircle, {p.points[0]}, {p.rounds[0]}));
            // Not one of the spline's own control points: those pull the curve, they aren't on it.
            if (n == 2 && P == 1 && S == 1 && p.points[0].entity != p.splines[0])
                out.push_back(make(ConstraintKind::PointOnSpline, {p.points[0]}, {p.splines[0]}));
            break;
        case Rule::Midpoint:
            if (n == 2 && P == 1 && L == 1)
                out.push_back(make(ConstraintKind::Midpoint, {p.points[0]}, {p.lines[0]}));
            break;
        case Rule::Symmetric:
            // Not the line's own ends: a line can't mirror its own points across itself.
            if (n == 3 && P == 2 && L == 1 && p.points[0].entity != p.lines[0] &&
                p.points[1].entity != p.lines[0])
                out.push_back(make(ConstraintKind::Symmetric, {p.points[0], p.points[1]}, {p.lines[0]}));
            break;
        case Rule::Fix:
            if (P >= 1 && n == P) {
                for (const auto& pt : p.points) {
                    auto c = make(ConstraintKind::Fix, {pt}, {});
                    if (const auto at = sketch::pointOf(s, pt)) c.at = *at;
                    out.push_back(c);
                }
            }
            break;
        case Rule::Dimension:
            if (n == 1 && L == 1) {
                if (const auto* l = lineOf(s, p.lines[0]))
                    out.push_back(make(ConstraintKind::Length, {}, {p.lines[0]},
                                       std::hypot(l->end.x - l->start.x, l->end.y - l->start.y)));
            } else if (n == 1 && R == 1) {
                const auto* e = s.find(p.rounds[0]);
                if (const auto* c = e ? std::get_if<sketch::CircleShape>(&e->shape) : nullptr)
                    out.push_back(make(ConstraintKind::Diameter, {}, {e->id}, 2 * c->radius));
                if (const auto* a = e ? std::get_if<sketch::ArcShape>(&e->shape) : nullptr)
                    out.push_back(make(ConstraintKind::Radius, {}, {e->id}, a->radius));
            } else if (n == 2 && P == 2) {
                const auto a = sketch::pointOf(s, p.points[0]), b = sketch::pointOf(s, p.points[1]);
                if (a && b)
                    out.push_back(make(ConstraintKind::Distance, {p.points[0], p.points[1]}, {},
                                       std::hypot(b->x - a->x, b->y - a->y)));
            } else if (n == 2 && L == 2) {
                const auto* a = lineOf(s, p.lines[0]);
                const auto* b = lineOf(s, p.lines[1]);
                if (a && b)
                    out.push_back(make(ConstraintKind::Angle, {}, {p.lines[0], p.lines[1]},
                                       wrap(direction(*b) - direction(*a))));
            }
            break;
        case Rule::HorizontalDimension:
        case Rule::VerticalDimension: {
            const auto kind = rule == Rule::HorizontalDimension ? ConstraintKind::HorizontalDistance
                                                                : ConstraintKind::VerticalDistance;
            std::optional<std::pair<PointRef, PointRef>> ends;
            if (n == 2 && P == 2) ends.emplace(p.points[0], p.points[1]);
            if (n == 1 && L == 1)
                ends.emplace(PointRef{p.lines[0], PointRole::Start}, PointRef{p.lines[0], PointRole::End});
            if (!ends) break;
            const auto a = sketch::pointOf(s, ends->first), b = sketch::pointOf(s, ends->second);
            if (a && b)
                out.push_back(make(kind, {ends->first, ends->second}, {},
                                   rule == Rule::HorizontalDimension ? b->x - a->x : b->y - a->y));
            break;
        }
    }
    return out;
}

bool sameConstraint(const sketch::Constraint& a, const sketch::Constraint& b) {
    if (a.kind != b.kind) return false;
    auto samePoints = [](const std::vector<PointRef>& x, const std::vector<PointRef>& y) {
        if (x.size() != y.size()) return false;
        auto eq = [](const PointRef& p, const PointRef& q) {
            return p.entity == q.entity && p.role == q.role;
        };
        if (std::equal(x.begin(), x.end(), y.begin(), eq)) return true;
        return x.size() == 2 && eq(x[0], y[1]) && eq(x[1], y[0]);  // either order
    };
    auto sameEntities = [](const std::vector<EntityId>& x, const std::vector<EntityId>& y) {
        if (x.size() != y.size()) return false;
        if (std::equal(x.begin(), x.end(), y.begin())) return true;
        return x.size() == 2 && x[0] == y[1] && x[1] == y[0];
    };
    return samePoints(a.points, b.points) && sameEntities(a.entities, b.entities);
}

std::string labelText(const sketch::Constraint& c, LengthUnit unit) {
    switch (c.kind) {
        case ConstraintKind::Coincident:
            return "";
        case ConstraintKind::Fix:
            return "Fix";
        case ConstraintKind::Horizontal:
            return "H";
        case ConstraintKind::Vertical:
            return "V";
        case ConstraintKind::Parallel:
            return "//";
        case ConstraintKind::Perpendicular:
            return "90°";
        case ConstraintKind::Equal:
            return "=";
        case ConstraintKind::PointOnLine:
        case ConstraintKind::PointOnCircle:
        case ConstraintKind::PointOnSpline:
            return "On";
        case ConstraintKind::Midpoint:
            return "Mid";
        case ConstraintKind::Symmetric:
            return "Sym";
        case ConstraintKind::Tangent:
            return "T";
        case ConstraintKind::Smooth:
            return "S";
        case ConstraintKind::Distance:
        case ConstraintKind::Length:
            return lengthText(c.value, unit);
        case ConstraintKind::HorizontalDistance:
            return "x " + lengthText(c.value, unit);
        case ConstraintKind::VerticalDistance:
            return "y " + lengthText(c.value, unit);
        case ConstraintKind::Radius:
            return "R" + lengthText(c.value, unit);
        case ConstraintKind::Diameter:
            return "Ø" + lengthText(c.value, unit);
        case ConstraintKind::Angle:
            return number(c.value * 180 / kPi) + "°";
    }
    return "";
}

std::optional<DimensionPlace> dimensionPlace(const sketch::Constraint& c, const sketch::Sketch& s, double gap,
                                             Point2 middle) {
    auto unit = [](Point2 v, Point2 fallback) {
        const double n = std::hypot(v.x, v.y);
        return n > 1e-12 ? Point2{v.x / n, v.y / n} : fallback;
    };
    const double out = 2 * gap;  // how far its line is from what it measures
    // Square to a segment, on its outside, `out` from it.
    auto aligned = [&](Point2 a, Point2 b) -> std::optional<DimensionPlace> {
        if (std::hypot(b.x - a.x, b.y - a.y) < 1e-9) return std::nullopt;
        const Point2 m = mid(a, b);
        Point2 n = unit({-(b.y - a.y), b.x - a.x}, {0, 1});
        if ((m.x - middle.x) * n.x + (m.y - middle.y) * n.y < 0) n = {-n.x, -n.y};
        DimensionPlace d;
        d.from = a;
        d.to = b;
        d.through = {m.x + n.x * out, m.y + n.y * out};
        d.textAt = d.through;
        return d;
    };
    auto twoPoints = [&]() -> std::optional<std::pair<Point2, Point2>> {
        if (c.points.size() < 2) return std::nullopt;
        const auto a = sketch::pointOf(s, c.points[0]), b = sketch::pointOf(s, c.points[1]);
        if (!a || !b) return std::nullopt;
        return std::pair{*a, *b};
    };
    // A leader from the edge of a circle or arc, outwards, away from the middle.
    auto leader = [&](const EntityId& id) -> std::optional<DimensionPlace> {
        const auto* e = s.find(id);
        if (!e) return std::nullopt;
        Point2 center, dir;
        double r = 0;
        if (const auto* ci = std::get_if<sketch::CircleShape>(&e->shape)) {
            center = ci->center;
            r = ci->radius;
            // Up along a diagonal, to the side away from the middle: clear of
            // lengths and distances (which run across and up) and of the corner
            // labels below (Fix), as Fusion draws them.
            const double h = std::sqrt(0.5);
            dir = {center.x >= middle.x ? h : -h, h};
        } else if (const auto* a = std::get_if<sketch::ArcShape>(&e->shape)) {
            center = a->center;
            r = a->radius;
            const double t = (a->startAngle + a->endAngle) / 2;
            dir = {std::cos(t), std::sin(t)};
        } else {
            return std::nullopt;
        }
        DimensionPlace d;
        const double reach = 1.5 * out;  // a little longer than other dimensions are out, to clear the shape
        d.from = {center.x + dir.x * r, center.y + dir.y * r};
        d.to = {center.x + dir.x * (r + reach), center.y + dir.y * (r + reach)};
        d.through = d.from;
        d.arrowAtTo = false;
        d.textAtEnd = true;
        d.textAt = {center.x + dir.x * (r + reach + gap), center.y + dir.y * (r + reach + gap)};
        return d;
    };
    switch (c.kind) {
        case ConstraintKind::Length: {
            const auto* l = c.entities.empty() ? nullptr : lineOf(s, c.entities[0]);
            return l ? aligned(l->start, l->end) : std::nullopt;
        }
        case ConstraintKind::Distance: {
            const auto ab = twoPoints();
            return ab ? aligned(ab->first, ab->second) : std::nullopt;
        }
        case ConstraintKind::HorizontalDistance:
        case ConstraintKind::VerticalDistance: {
            const auto ab = twoPoints();
            if (!ab) return std::nullopt;
            const auto [a, b] = *ab;
            const bool across = c.kind == ConstraintKind::HorizontalDistance;  // measured along x
            DimensionPlace d;
            d.from = a;
            d.to = b;
            d.along = across ? Point2{1, 0} : Point2{0, 1};
            if (std::abs(across ? b.x - a.x : b.y - a.y) < 1e-9) return std::nullopt;
            // Its line above or below the points (beside them, for a vertical one), outside.
            if (across) {
                const bool above = (a.y + b.y) / 2 >= middle.y;
                d.through = {a.x, above ? std::max(a.y, b.y) + out : std::min(a.y, b.y) - out};
                d.textAt = {(a.x + b.x) / 2, d.through.y};
            } else {
                const bool right = (a.x + b.x) / 2 >= middle.x;
                d.through = {right ? std::max(a.x, b.x) + out : std::min(a.x, b.x) - out, a.y};
                d.textAt = {d.through.x, (a.y + b.y) / 2};
            }
            return d;
        }
        case ConstraintKind::Radius:
        case ConstraintKind::Diameter:
            return c.entities.empty() ? std::nullopt : leader(c.entities[0]);
        default:
            return std::nullopt;
    }
}

namespace {

// A dimension's direction, and the direction square to it towards its line.
std::pair<Point2, Point2> dimensionAxes(const DimensionPlace& d) {
    Point2 u = d.along;
    if (std::hypot(u.x, u.y) < 1e-12) u = {d.to.x - d.from.x, d.to.y - d.from.y};
    const double n = std::hypot(u.x, u.y);
    u = n > 1e-12 ? Point2{u.x / n, u.y / n} : Point2{1, 0};
    Point2 side{-u.y, u.x};
    const Point2 m = mid(d.from, d.to);
    if ((d.through.x - m.x) * side.x + (d.through.y - m.y) * side.y < 0) side = {-side.x, -side.y};
    return {u, side};
}

}  // namespace

std::pair<Point2, Point2> dimensionLine(const DimensionPlace& d) {
    const auto [u, side] = dimensionAxes(d);
    auto onLine = [&](Point2 p) {
        const double k = (d.through.x - p.x) * side.x + (d.through.y - p.y) * side.y;
        return Point2{p.x + side.x * k, p.y + side.y * k};
    };
    return {onLine(d.from), onLine(d.to)};
}

void pushOut(DimensionPlace& d, double by) {
    const auto side = dimensionAxes(d).second;
    d.through = {d.through.x + side.x * by, d.through.y + side.y * by};
    d.textAt = {d.textAt.x + side.x * by, d.textAt.y + side.y * by};
}

bool dimensionsClash(const DimensionPlace& a, const DimensionPlace& b, double near) {
    if (a.textAtEnd || b.textAtEnd) return false;  // leaders go their own way
    const auto [ua, sa] = dimensionAxes(a);
    const auto [ub, sb] = dimensionAxes(b);
    // Not side by side: more than about 10° apart.
    if (std::abs(ua.x * ub.y - ua.y * ub.x) > 0.18) return false;
    const auto [a0, a1] = dimensionLine(a);
    const auto [b0, b1] = dimensionLine(b);
    // How far apart the two lines are, and whether they overlap along their direction.
    const double apart = std::abs((b0.x - a0.x) * sa.x + (b0.y - a0.y) * sa.y);
    if (apart >= near) return false;
    auto along = [&](Point2 p) { return p.x * ua.x + p.y * ua.y; };
    const double aLo = std::min(along(a0), along(a1)), aHi = std::max(along(a0), along(a1));
    const double bLo = std::min(along(b0), along(b1)), bHi = std::max(along(b0), along(b1));
    return aLo < bHi + near && bLo < aHi + near;
}

std::string dimensionText(const sketch::Constraint& c, LengthUnit unit) {
    switch (c.kind) {
        case ConstraintKind::HorizontalDistance:
        case ConstraintKind::VerticalDistance:
            return lengthText(std::abs(c.value), unit);
        default:
            return labelText(c, unit);
    }
}

std::optional<LabelPlace> labelPlace(const sketch::Constraint& c, const sketch::Sketch& s, double gap,
                                     Point2 middle) {
    auto unit = [](Point2 v, Point2 fallback) {
        const double n = std::hypot(v.x, v.y);
        return n > 1e-12 ? Point2{v.x / n, v.y / n} : fallback;
    };
    auto from = [&](Point2 p) {
        return unit({p.x - middle.x, p.y - middle.y}, {std::sqrt(0.5), std::sqrt(0.5)});
    };
    auto place = [&](Point2 base, Point2 away) {
        return LabelPlace{{base.x + away.x * gap, base.y + away.y * gap}, away};
    };
    // Beside the middle of a line (or between two points), on the outside.
    auto besideSegment = [&](Point2 a, Point2 b) {
        const Point2 m = mid(a, b);
        Point2 n = unit({-(b.y - a.y), b.x - a.x}, {0, 1});
        if ((m.x - middle.x) * n.x + (m.y - middle.y) * n.y < 0) n = {-n.x, -n.y};
        return place(m, n);
    };
    auto besideLine = [&](const EntityId& id) -> std::optional<LabelPlace> {
        const auto* l = lineOf(s, id);
        if (!l) return std::nullopt;
        return besideSegment(l->start, l->end);
    };
    // On the edge of a circle or arc, on its outer side.
    auto onRound = [&](const EntityId& id) -> std::optional<LabelPlace> {
        const auto* e = s.find(id);
        if (!e) return std::nullopt;
        if (const auto* ci = std::get_if<sketch::CircleShape>(&e->shape)) {
            const Point2 out = from(ci->center);
            return place({ci->center.x + out.x * ci->radius, ci->center.y + out.y * ci->radius}, out);
        }
        if (const auto* a = std::get_if<sketch::ArcShape>(&e->shape)) {
            const double t = (a->startAngle + a->endAngle) / 2;
            const Point2 out{std::cos(t), std::sin(t)};
            return place({a->center.x + a->radius * out.x, a->center.y + a->radius * out.y}, out);
        }
        return std::nullopt;
    };
    auto near = [&](const EntityId& id) {
        auto l = besideLine(id);
        return l ? l : onRound(id);
    };
    auto besidePoint = [&](const PointRef& r) -> std::optional<LabelPlace> {
        const auto p = sketch::pointOf(s, r);
        if (!p) return std::nullopt;
        return place(*p, from(*p));
    };
    auto twoPoints = [&]() -> std::optional<std::pair<Point2, Point2>> {
        if (c.points.size() < 2) return std::nullopt;
        const auto a = sketch::pointOf(s, c.points[0]), b = sketch::pointOf(s, c.points[1]);
        if (!a || !b) return std::nullopt;
        return std::pair{*a, *b};
    };
    switch (c.kind) {
        case ConstraintKind::Coincident:
            return std::nullopt;
        case ConstraintKind::Fix:
        case ConstraintKind::PointOnLine:
        case ConstraintKind::PointOnCircle:
        case ConstraintKind::PointOnSpline:
        case ConstraintKind::Midpoint:
            return c.points.empty() ? std::nullopt : besidePoint(c.points[0]);
        case ConstraintKind::Symmetric: {  // between the two points, beside the mirror line
            const auto ab = twoPoints();
            return ab ? std::optional(besideSegment(ab->first, ab->second)) : std::nullopt;
        }
        case ConstraintKind::Smooth:
            if (c.entities.size() < 2) return std::nullopt;
            if (const auto joint = sketch::jointOf(s, c.entities[0], c.entities[1]))
                return besidePoint(joint->first);
            return std::nullopt;
        case ConstraintKind::Horizontal:
        case ConstraintKind::Vertical:
        case ConstraintKind::Length:
            return c.entities.empty() ? std::nullopt : besideLine(c.entities[0]);
        case ConstraintKind::Parallel:
        case ConstraintKind::Perpendicular:
        case ConstraintKind::Equal:
        case ConstraintKind::Tangent:
            if (c.entities.size() < 2) return std::nullopt;
            // Where two curves are joined end to end: beside the join.
            if (c.kind == ConstraintKind::Tangent) {
                if (const auto joint = sketch::jointOf(s, c.entities[0], c.entities[1]))
                    return besidePoint(joint->first);
            }
            return near(c.entities[1]);
        case ConstraintKind::Distance: {
            const auto ab = twoPoints();
            return ab ? std::optional(besideSegment(ab->first, ab->second)) : std::nullopt;
        }
        case ConstraintKind::HorizontalDistance: {  // below (or above) the two points, halfway across
            const auto ab = twoPoints();
            if (!ab) return std::nullopt;
            const auto [a, b] = *ab;
            const bool below = std::min(a.y, b.y) <= middle.y;
            return place({(a.x + b.x) / 2, below ? std::min(a.y, b.y) : std::max(a.y, b.y)},
                         {0, below ? -1.0 : 1.0});
        }
        case ConstraintKind::VerticalDistance: {  // beside the two points, halfway up
            const auto ab = twoPoints();
            if (!ab) return std::nullopt;
            const auto [a, b] = *ab;
            const bool right = std::max(a.x, b.x) >= middle.x;
            return place({right ? std::max(a.x, b.x) : std::min(a.x, b.x), (a.y + b.y) / 2},
                         {right ? 1.0 : -1.0, 0});
        }
        case ConstraintKind::Radius:
        case ConstraintKind::Diameter:
            return c.entities.empty() ? std::nullopt : onRound(c.entities[0]);
        case ConstraintKind::Angle: {
            if (c.entities.size() < 2) return std::nullopt;
            const auto* a = lineOf(s, c.entities[0]);
            const auto* b = lineOf(s, c.entities[1]);
            if (!a || !b) return std::nullopt;
            const Point2 m = mid(mid(a->start, a->end), mid(b->start, b->end));
            return place(m, from(m));
        }
    }
    return std::nullopt;
}

std::string constraintBase(sketch::ConstraintKind kind) {
    std::string base;
    bool upper = true;
    for (char ch : sketch::kindName(kind)) {
        if (ch == ' ') {
            upper = true;
            continue;
        }
        base += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch))) : ch;
        upper = false;
    }
    return base;
}

}  // namespace sb::plugins::sketcher
