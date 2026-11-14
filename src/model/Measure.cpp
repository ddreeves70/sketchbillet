#include "sb/model/Measure.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

#include "sb/model/Document.h"

namespace sb::model {

bool Measurement::ok() const {
    return std::none_of(issues.begin(), issues.end(),
                        [](const Diagnostic& d) { return d.severity == Severity::Error; });
}

namespace {

using kernel::GeomKind;
using kernel::TopoKind;
using kernel::Vec3;

Vec3 minus(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
double dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
double norm(const Vec3& a) {
    return std::sqrt(dot(a, a));
}
// The part of `v` across direction `d` (a unit vector): what's left after taking
// away the part along it.
Vec3 across(const Vec3& v, const Vec3& d) {
    const double along = dot(v, d);
    return {v.x - along * d.x, v.y - along * d.y, v.z - along * d.z};
}

Diagnostic error(std::string code, std::string message) {
    return {std::move(code), Severity::Error, std::move(message), {}};
}

bool flatFace(const MeasuredElement& e) {
    return e.kind == TopoKind::Face && e.info.geom == GeomKind::Plane;
}
bool straightEdge(const MeasuredElement& e) {
    return e.kind == TopoKind::Edge && e.info.geom == GeomKind::Line;
}
// A whole circle: an edge that goes all the way round, like a hole's rim.
bool wholeCircle(const MeasuredElement& e) {
    return e.kind == TopoKind::Edge && e.info.geom == GeomKind::Circle && e.info.radius > 0 &&
           std::abs(e.info.size - 2 * std::numbers::pi * e.info.radius) < 1e-6 * std::max(1.0, e.info.size);
}

constexpr double kParallel = 1e-9;  // 1 - |cos| below this: parallel

// The angle and gap between flat faces and straight edges.
void compare(const MeasuredElement& a, const MeasuredElement& b, Measurement& out) {
    const bool aFlat = flatFace(a), bFlat = flatFace(b);
    if (!(aFlat || straightEdge(a)) || !(bFlat || straightEdge(b))) return;
    const double c = std::min(1.0, std::abs(dot(a.info.direction, b.info.direction)));
    // A face's direction is its normal; an edge's runs along it.
    const bool mixed = aFlat != bFlat;
    out.angle = mixed ? std::asin(c) : std::acos(c);
    const bool parallel = mixed ? c < kParallel : 1 - c < kParallel;
    if (!parallel) {
        out.angle = std::max(0.0, *out.angle);
        return;
    }
    out.angle = 0.0;
    const Vec3 gap = minus(b.info.center, a.info.center);
    if (aFlat) {
        out.apart = std::abs(dot(gap, a.info.direction));
    } else if (bFlat) {
        out.apart = std::abs(dot(gap, b.info.direction));
    } else {
        out.apart = norm(across(gap, a.info.direction));  // two straight lines
    }
}

}  // namespace

Measurement measure(const Document& doc, const RebuildReport& report, const kernel::IKernel& kernel,
                    const std::vector<MeasurePick>& picks) {
    Measurement out;
    if (picks.empty() || picks.size() > 2) {
        out.issues.push_back(error("measure.count",
                                   "Pick one face, edge or point to measure its size, or two to measure "
                                   "between them."));
        return out;
    }
    std::vector<kernel::ElementOf> on;
    for (const auto& p : picks) {
        const auto* step = doc.find(p.body);
        const std::string name = step ? step->name() : "That step";
        const auto* r = report.result(p.body);
        if (!step || !r || r->status != FeatureStatus::Ok || !r->shape.valid() || !r->names) {
            out.issues.push_back(
                error("measure.no_body",
                      name + " has no body to measure. Pick a step that built, or fix it first."));
            return out;
        }
        const auto found = resolve(p.ref, *r->names);
        if (!found.found()) {
            out.issues.push_back(error(
                "measure.missing",
                "What you picked on " + name + " isn't there any more, after a change. Pick it again."));
            return out;
        }
        const auto& infos = r->names->elements.of(p.ref.kind);
        out.elements.push_back({name, p.ref.kind, infos[static_cast<std::size_t>(found.index)]});
        on.push_back({r->shape, p.ref.kind, found.index});
    }
    if (on.size() < 2) return out;

    const auto gap = kernel.distance(on[0], on[1]);
    if (!gap.ok()) {
        out.issues.insert(out.issues.end(), gap.issues.begin(), gap.issues.end());
        return out;
    }
    out.distance = gap.distance;
    out.from = gap.onA;
    out.to = gap.onB;

    const auto& a = out.elements[0];
    const auto& b = out.elements[1];
    compare(a, b, out);
    if (wholeCircle(a) && wholeCircle(b)) {
        const Vec3 between = minus(b.info.center, a.info.center);
        const bool sameWay = 1 - std::abs(dot(a.info.direction, b.info.direction)) < kParallel;
        out.centers = sameWay ? norm(across(between, a.info.direction)) : norm(between);
    }
    return out;
}

namespace {

// The number part of lengthText(): "40" of "40 mm".
std::string number(double mm, LengthUnit unit) {
    const std::string t = lengthText(mm, unit);
    return t.substr(0, t.rfind(' '));
}

std::string areaText(double mm2, LengthUnit unit) {
    const double per = mmPerUnit(unit);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", mm2 / (per * per));
    return std::string(buf) + " " + std::string(unitSymbol(unit)) + "^2";
}

std::string pointText(const Vec3& p, LengthUnit unit) {
    return "x " + number(p.x, unit) + ", y " + number(p.y, unit) + ", z " + number(p.z, unit) + " " +
           std::string(unitSymbol(unit));
}

std::string degrees(double radians) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", radians * 180 / std::numbers::pi);
    std::string t = buf;
    while (t.back() == '0') t.pop_back();
    if (t.back() == '.') t.pop_back();
    return t + "°";
}

std::string what(const MeasuredElement& e) {
    if (e.kind == TopoKind::Vertex) return "point";
    const bool face = e.kind == TopoKind::Face;
    switch (e.info.geom) {
        case GeomKind::Plane:
            return "flat face";
        case GeomKind::Cylinder:
            return "cylinder face";
        case GeomKind::Cone:
            return "cone face";
        case GeomKind::Sphere:
            return "ball face";
        case GeomKind::Torus:
            return "ring face";
        case GeomKind::Line:
            return "straight edge";
        case GeomKind::Circle:
            return "round edge";
        case GeomKind::Ellipse:
            return "oval edge";
        default:
            return face ? "curved face" : "curved edge";
    }
}

std::string size(const MeasuredElement& e, LengthUnit unit) {
    const auto& i = e.info;
    if (e.kind == TopoKind::Vertex) return "at " + pointText(i.center, unit);
    const std::string round = i.radius > 0 ? "radius " + lengthText(i.radius, unit) + " (" +
                                                 lengthText(2 * i.radius, unit) + " across), "
                                           : std::string();
    if (e.kind == TopoKind::Face) return round + "area " + areaText(i.size, unit);
    return round + lengthText(i.size, unit) + " long";
}

}  // namespace

std::vector<std::string> describe(const Measurement& m, LengthUnit unit) {
    std::vector<std::string> out;
    for (const auto& e : m.elements) out.push_back(e.body + ", " + what(e) + ": " + size(e, unit));
    if (m.distance) {
        const Vec3 d = minus(m.to, m.from);
        out.push_back("Distance: " + lengthText(*m.distance, unit) +
                      (*m.distance < 1e-9 ? std::string(" (they touch)") : " (" + pointText(d, unit) + ")"));
    }
    // Straight across, when that's not the distance already (edges side by side, say).
    if (m.apart && !(m.distance && std::abs(*m.apart - *m.distance) < 1e-6))
        out.push_back("Straight across: " + lengthText(*m.apart, unit));
    if (m.angle) out.push_back("Angle: " + degrees(*m.angle) + (m.apart ? " (they're parallel)" : ""));
    if (m.centers) out.push_back("Centers: " + lengthText(*m.centers, unit) + " apart");
    return out;
}

}  // namespace sb::model
