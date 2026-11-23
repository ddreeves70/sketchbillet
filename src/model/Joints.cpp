// Placing an assembly's components along their joints (docs/design/assemblies.md,
// "Placing parts along chains"): each joint's two origins are found on the parts'
// built bodies, then the components are placed outward from the grounded ones,
// one joint at a time. Every step is exact. A joint between two components that
// are both placed already closes a loop: it's checked, never moved.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <vector>

#include "sb/model/Assembly.h"
#include "sb/model/Document.h"

namespace sb::model {

const char* jointKindName(JointKind kind) {
    switch (kind) {
        case JointKind::Rigid:
            return "rigid";
        case JointKind::Revolute:
            return "revolute";
        case JointKind::Slider:
            return "slider";
    }
    return "rigid";
}

namespace {

constexpr double kMeetMm = 0.01;       // a loop's sides meet within this (mm)
constexpr double kMeetDegrees = 0.01;  // and their directions within this

using kernel::Placement;
using kernel::Vec3;

double dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double length(const Vec3& v) {
    return std::sqrt(dot(v, v));
}

std::string mm(double v) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", v);
    return text;
}

// A frame at `point` whose z is `z`. Its x is the part's x as nearly as it can be
// (or its y, when z is along x), so the same pick always gives the same frame.
std::optional<Placement> frameAt(const kernel::Point3& point, Vec3 z) {
    if (length(z) < 1e-12) z = {0, 0, 1};
    const double n = length(z);
    z = {z.x / n, z.y / n, z.z / n};
    Vec3 x{1, 0, 0};
    if (std::abs(dot(x, z)) > 0.9) x = {0, 1, 0};
    const double along = dot(x, z);
    x = {x.x - along * z.x, x.y - along * z.y, x.z - along * z.z};
    return kernel::tidyPlacement(Placement{point, x, z});
}

// A direction whose sign the kernel chose (an edge's, an axis's), the same way
// every time: its largest part positive.
Vec3 steady(Vec3 v) {
    const double ax = std::abs(v.x), ay = std::abs(v.y), az = std::abs(v.z);
    const double big = ax >= ay && ax >= az ? v.x : ay >= az ? v.y : v.z;
    if (big < 0) v = {-v.x, -v.y, -v.z};
    return v;
}

const char* kindWord(kernel::TopoKind kind) {
    return kind == kernel::TopoKind::Face ? "face" : kind == kernel::TopoKind::Edge ? "edge" : "corner";
}

// Where a joint origin is on its component's part, in the part's own place.
// Says what's wrong in `why` if it can't be found.
std::optional<Placement> originFrame(const Document& assembly, const RebuildReport& report,
                                     const JointOrigin& o, std::string& why) {
    const Component* c = assembly.findComponent(o.component);
    if (!c) {
        why = "can't find one of its components";
        return std::nullopt;
    }
    if (o.body.isNil()) return Placement{};  // the part's own origin
    const Document* part = assembly.findPart(c->part);
    const RebuildReport* built = report.part(c->part);
    const Feature* step = part ? part->find(o.body) : nullptr;
    if (!part || !built || !step) {
        const std::string where = part ? part->name() : c->name;
        why = "can't find the body it sits on in " + where + ": a change to " + where + " took it away";
        return std::nullopt;
    }
    const FeatureResult* r = built->result(o.body);
    if (!r || !r->names || r->status != FeatureStatus::Ok) {
        why = "can't use " + step->name() + " in " + part->name() + ": it didn't build";
        return std::nullopt;
    }
    if (!o.element) return frameAt({0, 0, 0}, {0, 0, 1});
    const auto& ref = *o.element;
    const Resolution found = resolve(ref, *r->names);
    if (!found.found()) {
        why = std::string("can't find the ") + kindWord(ref.kind) + " it sits on in " + part->name() +
              (found.status == ResolveStatus::Ambiguous
                   ? ": a change to " + part->name() + " left two that look the same"
                   : ": a change to " + part->name() + " took it away");
        return std::nullopt;
    }
    const auto& elements = r->names->elements;
    const auto index = static_cast<std::size_t>(found.index);
    const kernel::ElementInfo& e = elements.of(ref.kind).at(index);
    switch (ref.kind) {
        case kernel::TopoKind::Vertex:
            return frameAt(e.center, {0, 0, 1});
        case kernel::TopoKind::Face:
            // A flat face points out of the part; a round one along its axis.
            return frameAt(e.center, e.geom == kernel::GeomKind::Plane ? e.direction : steady(e.direction));
        case kernel::TopoKind::Edge: {
            kernel::Point3 at = e.center;
            if (e.geom == kernel::GeomKind::Line && o.at != OriginAt::Middle &&
                index < elements.edgeVertices.size() && elements.edgeVertices[index].size() == 2) {
                const int v = elements.edgeVertices[index][o.at == OriginAt::Start ? 0 : 1];
                at = elements.vertices.at(static_cast<std::size_t>(v)).center;
            }
            Vec3 z = steady(e.direction);
            // A round edge on a flat face (a hole's rim, an axle's end) points out
            // of that face, so a peg and a hole meet the right way round.
            if (e.geom == kernel::GeomKind::Circle && index < elements.edgeFaces.size()) {
                for (const int f : elements.edgeFaces[index]) {
                    const auto& face = elements.faces.at(static_cast<std::size_t>(f));
                    if (face.geom == kernel::GeomKind::Plane && std::abs(dot(face.direction, z)) > 0.999) {
                        z = face.direction;
                        break;
                    }
                }
            }
            return frameAt(at, z);
        }
    }
    return std::nullopt;
}

// How b's origin sits against a's: turned and slid along z by the joint's
// settings and value, and (unless flipped) facing back at it.
Placement jointMotion(const Joint& j) {
    double turn = j.angle;
    double slide = j.offset;
    if (j.kind == JointKind::Revolute) turn += j.value;
    if (j.kind == JointKind::Slider) slide += j.value;
    const Placement move{{0, 0, slide}, {std::cos(turn), std::sin(turn), 0}, {0, 0, 1}};
    // Facing back: turned half way round about x, so the two z directions meet.
    const Placement face = j.flip ? Placement{} : Placement{{0, 0, 0}, {1, 0, 0}, {0, 0, -1}};
    return kernel::compose(move, face);
}

}  // namespace

void Document::placeComponents(RebuildReport& report) const {
    for (const auto& c : components_) {
        report.placements_[c.id] = c.placement;
        report.byJoint_[c.id] = false;
    }
    // Each joint's origins on its parts, found first.
    struct Usable {
        const Joint* joint;
        Placement a, b;  // the origins, each in its own part's place
    };
    std::vector<Usable> usable;
    for (const auto& j : joints_) {
        auto& result = report.joints_[j.id];
        if (j.off) continue;
        std::string why;
        auto a = originFrame(*this, report, j.a, why);
        std::optional<Placement> b;
        if (a) b = originFrame(*this, report, j.b, why);
        if (!a || !b) {
            result.issues.push_back({"joint.origin_missing",
                                     Severity::Warning,
                                     j.name + " " + why + ", so it doesn't place anything. Choose " + j.name +
                                         "'s origins again (joint.set joint=\"" + j.name + "\" a=... b=...).",
                                     {}});
            continue;
        }
        usable.push_back({&j, *a, *b});
    }

    // Outward from the grounded components, one joint at a time; each placed
    // component remembers the one it was placed from, to name a loop.
    std::set<ComponentId> placed;
    std::map<ComponentId, ComponentId> from;
    for (const auto& c : components_) {
        if (c.grounded) placed.insert(c.id);
    }
    auto spread = [&] {
        for (bool more = true; more;) {
            more = false;
            for (const auto& u : usable) {
                const auto& j = *u.joint;
                const bool hasA = placed.count(j.a.component) > 0, hasB = placed.count(j.b.component) > 0;
                if (hasA == hasB) continue;
                const Placement motion = jointMotion(j);
                if (hasA) {
                    // b's origin goes where a's origin, moved by the joint, is.
                    const Placement atA = kernel::compose(report.placements_[j.a.component], u.a);
                    report.placements_[j.b.component] =
                        kernel::compose(kernel::compose(atA, motion), kernel::inverse(u.b));
                    placed.insert(j.b.component);
                    from[j.b.component] = j.a.component;
                    report.byJoint_[j.b.component] = true;
                } else {
                    const Placement atB = kernel::compose(report.placements_[j.b.component], u.b);
                    report.placements_[j.a.component] =
                        kernel::compose(kernel::compose(atB, kernel::inverse(motion)), kernel::inverse(u.a));
                    placed.insert(j.a.component);
                    from[j.a.component] = j.b.component;
                    report.byJoint_[j.a.component] = true;
                }
                report.joints_[j.id].placed = true;
                more = true;
            }
        }
    };
    spread();
    // Joined components no grounded one reaches: the first of each group stays
    // where it is, and the rest are placed from it.
    for (const auto& c : components_) {
        if (placed.count(c.id)) continue;
        const bool joined = std::any_of(usable.begin(), usable.end(), [&](const Usable& u) {
            return u.joint->a.component == c.id || u.joint->b.component == c.id;
        });
        if (!joined) continue;
        placed.insert(c.id);
        spread();
    }

    // The way back from a component to where it was placed from: to name a loop.
    auto chain = [&](ComponentId id) {
        std::vector<ComponentId> out{id};
        for (auto it = from.find(id); it != from.end(); it = from.find(it->second)) out.push_back(it->second);
        return out;
    };
    auto nameOf = [&](const ComponentId& id) {
        const auto* c = findComponent(id);
        return c ? c->name : std::string("?");
    };
    for (const auto& u : usable) {
        const auto& j = *u.joint;
        auto& result = report.joints_[j.id];
        result.aFrame = kernel::compose(report.placements_[j.a.component], u.a);
        result.bFrame = kernel::compose(report.placements_[j.b.component], u.b);
        if (result.placed) continue;
        // Both sides were placed some other way: it closes a loop. Fine if its
        // sides meet; otherwise it says so, and moves nothing.
        result.loop = true;
        const Placement want = kernel::compose(*result.aFrame, jointMotion(j));
        const auto& got = *result.bFrame;
        const Vec3 gap{got.origin.x - want.origin.x, got.origin.y - want.origin.y,
                       got.origin.z - want.origin.z};
        const double apart = length(gap);
        const double turned =
            std::acos(std::clamp(dot(got.zAxis, want.zAxis), -1.0, 1.0)) * 180.0 / std::numbers::pi;
        if (apart <= kMeetMm && turned <= kMeetDegrees) continue;
        // Name the loop: from a, back through what placed it, to where b's way meets it.
        const auto fromA = chain(j.a.component);
        const auto fromB = chain(j.b.component);
        std::vector<ComponentId> loop;
        for (const auto& id : fromA) {
            loop.push_back(id);
            const auto meet = std::find(fromB.begin(), fromB.end(), id);
            if (meet != fromB.end()) {
                for (auto it = std::make_reverse_iterator(meet); it != fromB.rend(); ++it)
                    loop.push_back(*it);
                break;
            }
        }
        if (loop.empty() || loop.back() != j.b.component) loop = {j.a.component, j.b.component};
        loop.push_back(j.a.component);
        std::string names;
        for (std::size_t i = 0; i < loop.size(); ++i) names += (i ? ", " : "") + nameOf(loop[i]);
        std::string off = apart > kMeetMm ? "its two sides are " + mm(apart) + " mm apart" : std::string();
        if (turned > kMeetDegrees) {
            char deg[32];
            std::snprintf(deg, sizeof deg, "%.2f", turned);
            off += (off.empty() ? "its two sides" : ", and") + std::string(" point ") + deg +
                   "\xC2\xB0 away from each other";
        }
        result.issues.push_back({"joint.loop",
                                 Severity::Warning,
                                 j.name + " makes a loop (" + names + "), and " + off +
                                     ". SketchBillet can't move parts in loops yet, so " + j.name +
                                     " doesn't move anything. Turn " + j.name +
                                     " off, or change a value in the loop so its sides meet.",
                                 {}});
    }
}

}  // namespace sb::model
