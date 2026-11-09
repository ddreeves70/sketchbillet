#include "sb/model/Feature.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <numbers>
#include <utility>

#include "sb/model/ConstructionPlane.h"
#include "sb/model/Holes.h"
#include "sb/model/Mirror.h"
#include "sb/model/Pattern.h"
#include "sb/model/Shell.h"
#include "sb/sketch/Profiles.h"

namespace sb::model {

namespace {

// A result's names, or nullptr if it has none.
const ShapeNames* namesOf(const ResultLookup& earlier, const FeatureId& id) {
    const FeatureResult* r = earlier.resultOf(id);
    return r ? r->names.get() : nullptr;
}

// Names a shape made from inputs' shapes, from the kernel's history.
Built carried(kernel::IKernel& kernel, kernel::OpResult op, const std::vector<const ShapeNames*>& inputs,
              const FeatureId& creator, const OriginNamer& other = {}) {
    Built out;
    if (op.ok()) {
        out.names = std::make_shared<const ShapeNames>(
            carryNames(kernel.elements(op.shape), op.history, inputs, shortId(creator.toString()), other));
    }
    out.op = std::move(op);
    return out;
}

// Names a new shape's faces by which way they face: a box's are "left" (smallest
// x), "right", "front" (smallest y), "back", "bottom" and "top"; a cylinder's
// are "bottom", "top" (the end its axis points to) and "side" (the round one).
Built byDirection(kernel::IKernel& kernel, kernel::OpResult op, const FeatureId& creator,
                  const kernel::Vec3& up = {0, 0, 1}) {
    Built out;
    if (op.ok()) {
        auto elements = kernel.elements(op.shape);
        const std::string id = shortId(creator.toString());
        std::vector<std::string> faces;
        for (const auto& f : elements.faces) {
            const auto& n = f.direction;
            std::string role = "side";  // a cylinder's round face
            if (f.geom == kernel::GeomKind::Plane) {
                const double along = n.x * up.x + n.y * up.y + n.z * up.z;
                const bool box = up.x == 0 && up.y == 0 && up.z == 1 && std::abs(along) < 0.5;
                if (box) {
                    role = std::abs(n.x) > std::abs(n.y) ? (n.x > 0 ? "right" : "left")
                                                         : (n.y > 0 ? "back" : "front");
                } else {
                    role = along > 0 ? "top" : "bottom";
                }
            }
            faces.push_back(id + "/" + role);
        }
        out.names = std::make_shared<const ShapeNames>(nameFromFaces(std::move(elements), std::move(faces)));
    }
    out.op = std::move(op);
    return out;
}

Built missingInput() {
    Built b;
    b.op.issues.push_back({"feature.input_missing",
                           Severity::Error,
                           "This step uses another step that is missing or has a problem.",
                           {}});
    return b;
}

}  // namespace

BoxFeature::BoxFeature(std::string name, kernel::Vec3 corner, kernel::Vec3 size)
    : BoxFeature(FeatureId::generate(), std::move(name), corner, size) {}

BoxFeature::BoxFeature(FeatureId id, std::string name, kernel::Vec3 corner, kernel::Vec3 size)
    : Feature(id, std::move(name)), corner_(corner), size_(size) {}

Built BoxFeature::build(kernel::IKernel& kernel, const ResultLookup&) const {
    // Faces: left, right, front, back, top and bottom.
    return byDirection(kernel, kernel.makeBox(corner_, size_.x, size_.y, size_.z), id());
}

CylinderFeature::CylinderFeature(std::string name, kernel::Vec3 base, kernel::Vec3 axis, double radius,
                                 double height)
    : CylinderFeature(FeatureId::generate(), std::move(name), base, axis, radius, height) {}

CylinderFeature::CylinderFeature(FeatureId id, std::string name, kernel::Vec3 base, kernel::Vec3 axis,
                                 double radius, double height)
    : Feature(id, std::move(name)), base_(base), axis_(axis), radius_(radius), height_(height) {}

Built CylinderFeature::build(kernel::IKernel& kernel, const ResultLookup&) const {
    // Faces: side, top and bottom (top is the end the axis points to).
    return byDirection(kernel, kernel.makeCylinder(base_, axis_, radius_, height_), id(), axis_);
}

BooleanFeature::BooleanFeature(std::string name, kernel::BoolOp op, FeatureId a, FeatureId b)
    : BooleanFeature(FeatureId::generate(), std::move(name), op, a, b) {}

BooleanFeature::BooleanFeature(FeatureId id, std::string name, kernel::BoolOp op, FeatureId a, FeatureId b)
    : Feature(id, std::move(name)), op_(op), a_(a), b_(b) {}

Built BooleanFeature::build(kernel::IKernel& kernel, const ResultLookup& earlier) const {
    const auto* sa = earlier.shapeOf(a_);
    const auto* sb = earlier.shapeOf(b_);
    if (!sa || !sb) return missingInput();
    // Every face keeps the name it had on its body.
    return carried(kernel, kernel.boolean(op_, *sa, *sb), {namesOf(earlier, a_), namesOf(earlier, b_)}, id());
}

MoveFeature::MoveFeature(std::string name, FeatureId body, kernel::Motion motion)
    : MoveFeature(FeatureId::generate(), std::move(name), body, motion) {}

MoveFeature::MoveFeature(FeatureId id, std::string name, FeatureId body, kernel::Motion motion)
    : Feature(id, std::move(name)), body_(body), motion_(motion) {}

Built MoveFeature::build(kernel::IKernel& kernel, const ResultLookup& earlier) const {
    const auto* s = earlier.shapeOf(body_);
    if (!s) return missingInput();
    return carried(kernel, kernel.transform(*s, motion_), {namesOf(earlier, body_)}, id());
}

SketchFeature::SketchFeature(std::string name, SketchPlane plane)
    : SketchFeature(FeatureId::generate(), std::move(name), plane, {}) {}

SketchFeature::SketchFeature(FeatureId id, std::string name, SketchPlane plane, sketch::Sketch sketch)
    : Feature(id, std::move(name)), plane_(plane), sketch_(std::move(sketch)) {}

Built SketchFeature::build(kernel::IKernel&, const ResultLookup&) const {
    Built b;
    b.op.issues.push_back({"sketch.no_solver",
                           Severity::Error,
                           "Sketches are solved by the sketch solver, not the kernel.",
                           {}});
    return b;
}

PlaneAxes planeAxes(SketchPlane plane) {
    switch (plane) {
        case SketchPlane::XY:
            return {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        case SketchPlane::XZ:
            return {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
        case SketchPlane::YZ:
            return {{0, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    }
    return {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
}

kernel::Vec3 toDesign(const PlaneAxes& a, const sketch::Point2& p) {
    return {a.origin.x + p.x * a.x.x + p.y * a.y.x, a.origin.y + p.x * a.x.y + p.y * a.y.y,
            a.origin.z + p.x * a.x.z + p.y * a.y.z};
}

sketch::Point2 toSketch(const PlaneAxes& a, const kernel::Vec3& p) {
    const kernel::Vec3 d{p.x - a.origin.x, p.y - a.origin.y, p.z - a.origin.z};
    return {d.x * a.x.x + d.y * a.x.y + d.z * a.x.z, d.x * a.y.x + d.y * a.y.y + d.z * a.y.z};
}

kernel::Vec3 toDesign(SketchPlane plane, const sketch::Point2& p) {
    return toDesign(planeAxes(plane), p);
}

sketch::Point2 toSketch(SketchPlane plane, const kernel::Vec3& p) {
    return toSketch(planeAxes(plane), p);
}

SketchPlane nearestPlane(const kernel::Vec3& n) {
    const double x = std::abs(n.x), y = std::abs(n.y), z = std::abs(n.z);
    if (z >= x && z >= y) return SketchPlane::XY;
    return y >= x ? SketchPlane::XZ : SketchPlane::YZ;
}

std::optional<PlaneAxes> faceAxes(const kernel::ElementInfo& face) {
    const auto& n = face.direction;
    const double len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (face.geom != kernel::GeomKind::Plane || len < 0.5) return std::nullopt;
    const kernel::Vec3 z{n.x / len, n.y / len, n.z / len};
    auto cross = [](const kernel::Vec3& a, const kernel::Vec3& b) {
        return kernel::Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    kernel::Vec3 x{1, 0, 0};
    if (std::abs(z.z) < 1 - 1e-9) {
        // Level: square to the design's z and to the normal.
        const auto h = cross({0, 0, 1}, z);
        const double l = std::sqrt(h.x * h.x + h.y * h.y + h.z * h.z);
        x = {h.x / l, h.y / l, h.z / l};
    }
    const auto y = cross(z, x);
    const double d = face.center.x * z.x + face.center.y * z.y + face.center.z * z.z;
    // Snap tiny rounding to 0, so a top face's axes are exactly the design's.
    auto clean = [](kernel::Vec3 v) {
        for (double* c : {&v.x, &v.y, &v.z}) {
            if (std::abs(*c) < 1e-12) *c = 0;
        }
        return v;
    };
    return PlaneAxes{clean({z.x * d, z.y * d, z.z * d}), clean(x), clean(y)};
}

void SketchFeature::remapInputs(const std::map<FeatureId, FeatureId>& map) {
    if (auto it = map.find(onPlane_); it != map.end()) onPlane_ = it->second;
    if (!face_) return;
    if (auto it = map.find(face_->body); it != map.end()) face_->body = it->second;
    remapRef(face_->face, map);
}

}  // namespace sb::model

namespace sb::model {

ExtrudeFeature::ExtrudeFeature(std::string name, FeatureId sketch, double distance, ExtrudeSide side)
    : Feature(std::move(name)), sketch_(sketch), distance_(distance), side_(side) {}

ExtrudeFeature::ExtrudeFeature(FeatureId id, std::string name, FeatureId sketch, double distance,
                               ExtrudeSide side)
    : Feature(id, std::move(name)), sketch_(sketch), distance_(distance), side_(side) {}

namespace {

// A sketch loop as the kernel takes it. Each curve's entity is added to `curves`,
// in the same order (for naming the faces made from them).
kernel::Loop2 toKernel(const sketch::Loop& loop, const sketch::Sketch& s, std::vector<EntityId>& curves) {
    kernel::Loop2 out;
    for (const auto& e : loop.edges) {
        const auto* entity = s.find(e.entity);
        if (!entity) continue;  // findProfiles only lists entities of the sketch
        curves.push_back(e.entity);
        kernel::Edge2 edge;
        edge.reversed = e.reversed;
        if (const auto* l = std::get_if<sketch::LineShape>(&entity->shape)) {
            edge.curve = kernel::Line2{{l->start.x, l->start.y}, {l->end.x, l->end.y}};
        } else if (const auto* a = std::get_if<sketch::ArcShape>(&entity->shape)) {
            edge.curve = kernel::Arc2{{a->center.x, a->center.y}, a->radius, a->startAngle, a->endAngle};
        } else if (const auto* c = std::get_if<sketch::CircleShape>(&entity->shape)) {
            edge.curve = kernel::Circle2{{c->center.x, c->center.y}, c->radius};
        } else if (const auto* sp = std::get_if<sketch::SplineShape>(&entity->shape)) {
            kernel::Spline2 k{{}, sp->weights, sp->degree, sp->closed};
            for (const auto& p : sp->poles) k.poles.push_back({p.x, p.y});
            edge.curve = std::move(k);
        }
        out.push_back(edge);
    }
    return out;
}

Built extrudeFailed(std::string code, std::string message) {
    Built b;
    b.op.issues.push_back({std::move(code), Severity::Error, std::move(message), {}});
    return b;
}

}  // namespace

namespace {

// A sketch step's solved sketch and its closed shapes, or why it can't be used
// (`failure` set): not solved yet, or no closed shape. `verb` is "extrude" or
// "revolve", for codes and messages.
struct SketchShapes {
    sketch::Sketch solved;
    sketch::Profiles found;
    PlaneAxes axes;
    std::optional<Built> failure;
};

SketchShapes shapesOf(const ResultLookup& earlier, const FeatureId& sketchId, const std::string& verb,
                      const std::string& Verb) {
    SketchShapes out;
    const FeatureResult* r = earlier.resultOf(sketchId);
    if (!r || !r->sketch || !r->sketch->ok() || !r->plane) {
        // Rebuilds block this step first; this is only for callers that don't.
        out.failure =
            extrudeFailed(verb + ".sketch", Verb + " needs a sketch that solved. Fix the sketch first.");
        return out;
    }
    out.solved = sketch::Sketch{r->sketch->entities, {}};
    out.axes = *r->plane;
    out.found = sketch::findProfiles(out.solved);
    if (out.found.shapes.empty()) {
        std::string why = "The sketch has no closed shape to " + verb + ".";
        if (!out.found.openEnds.empty()) {
            std::string ends;
            for (const auto& end : out.found.openEnds) {
                const auto* e = out.solved.find(end.entity);
                ends += (ends.empty() ? "" : ", ") + (e ? e->name : std::string("?")) + "'s " +
                        (end.role == sketch::PointRole::Start ? "start" : "end");
            }
            why += " These ends aren't joined: " + ends + ". Join them so the lines go all the way round.";
        } else if (!out.found.issues.empty()) {
            why += " " + out.found.issues.front().message;
        } else {
            why += " Draw a closed shape in it, like a rectangle or a circle.";
        }
        out.failure = extrudeFailed(verb + ".no_shape", why);
    }
    return out;
}

// Makes each closed shape's face on `plane` and sweeps it with `sweep` (extrude
// or revolve), naming the faces: each side for the sketch curve it was swept
// from ("side:<curve>"), and each shape's ends for its outline ("start:<curve>",
// "end:<curve>", with the outline curve whose id comes first). Several shapes
// make one body.
Built sweepShapes(kernel::IKernel& kernel, const SketchShapes& shapes, const kernel::Plane& plane,
                  const FeatureId& id,
                  const std::function<kernel::OpResult(const kernel::ShapeHandle&)>& sweep) {
    const std::string me = shortId(id.toString());
    Built out;
    for (const auto& shape : shapes.found.shapes) {
        std::vector<EntityId> curves;
        kernel::Profile2 profile{toKernel(shape.outer, shapes.solved, curves), {}};
        const std::string outline = shortId(std::min_element(curves.begin(), curves.end())->toString());
        for (const auto& hole : shape.holes) profile.holes.push_back(toKernel(hole, shapes.solved, curves));
        auto face = kernel.makeFace(profile, plane);
        if (!face.ok()) return {face, nullptr};
        auto solid = sweep(face.shape);
        if (!solid.ok()) return {solid, nullptr};
        const auto& edgeCurves = face.history.edges;
        auto named = carried(kernel, std::move(solid), {}, id, [&](const kernel::Origin& o) -> std::string {
            switch (o.kind) {
                case kernel::OriginKind::Start:
                    return me + "/start:" + outline;
                case kernel::OriginKind::End:
                    return me + "/end:" + outline;
                case kernel::OriginKind::Edge: {
                    const auto e = static_cast<std::size_t>(o.index);
                    if (e >= edgeCurves.size() || edgeCurves[e].empty()) return {};
                    const auto c = static_cast<std::size_t>(edgeCurves[e].front().index);
                    return c < curves.size() ? me + "/side:" + shortId(curves[c].toString()) : std::string();
                }
                default:
                    return {};
            }
        });
        if (out.op.shape.valid()) {
            auto joined = kernel.boolean(kernel::BoolOp::Union, out.op.shape, named.op.shape);
            if (!joined.ok()) return {joined, nullptr};
            named = carried(kernel, std::move(joined), {out.names.get(), named.names.get()}, id);
        }
        out = std::move(named);
    }
    return out;
}

// Joins a swept solid onto a body, or cuts it out of one; every face keeps its
// name. `verb` ("extrude" or "revolve") starts the codes; `tryHint` says what to
// change when it doesn't work or a cut misses.
Built combineWithBody(kernel::IKernel& kernel, const ResultLookup& earlier, Built out, ExtrudeOp op,
                      const FeatureId& body, const std::string& name, const FeatureId& id,
                      const std::string& verb, const std::string& tryHint, const std::string& missHint) {
    const auto* target = earlier.shapeOf(body);
    if (!target) {
        return extrudeFailed(verb + ".no_body", name + " needs a body to " +
                                                    (op == ExtrudeOp::Join ? "join onto" : "cut from") +
                                                    ". Pick one in Properties, or make it a new body.");
    }
    const bool cut = op == ExtrudeOp::Cut;
    auto combined = kernel.boolean(cut ? kernel::BoolOp::Cut : kernel::BoolOp::Union, *target, out.op.shape);
    if (!combined.ok()) {
        return extrudeFailed(
            verb + ".combine_failed",
            name + (cut ? " couldn't cut its shape out of the body." : " couldn't join the body.") +
                " Check that they touch, or " + tryHint + ".");
    }
    Diagnostics notes;
    if (cut && std::abs(kernel.massProps(combined.shape).volume - kernel.massProps(*target).volume) < 1e-9) {
        // A cut that misses changes nothing: usually it goes the wrong way.
        notes.push_back({verb + ".cut_missed",
                         Severity::Warning,
                         name + " doesn't reach the body, so nothing was cut. " + missHint,
                         {}});
    }
    auto named = carried(kernel, std::move(combined), {namesOf(earlier, body), out.names.get()}, id);
    named.op.issues.insert(named.op.issues.end(), notes.begin(), notes.end());
    return named;
}

// Pieces that weren't in any shape (three lines meeting, shapes crossing) are worth knowing about.
void addShapeNotes(Built& out, const SketchShapes& shapes) {
    for (const auto& d : shapes.found.issues)
        out.op.issues.push_back({d.code, Severity::Warning, d.message, d.detail});
}

kernel::Vec3 cross(const kernel::Vec3& a, const kernel::Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

}  // namespace

Built ExtrudeFeature::build(kernel::IKernel& kernel, const ResultLookup& earlier) const {
    if (!(distance_ > 0)) {
        return extrudeFailed("extrude.distance",
                             "Extrude needs a distance greater than 0 mm. Type one in Properties, like 10.");
    }
    const auto shapes = shapesOf(earlier, sketch_, "extrude", "Extrude");
    if (shapes.failure) return *shapes.failure;

    // Which way: the sketch's front (the side it's seen from) unless flipped; for
    // both sides, start half the distance behind the sketch.
    const PlaneAxes& axes = shapes.axes;
    const kernel::Vec3 n = cross(axes.x, axes.y);
    const double sign = side_ == ExtrudeSide::Flip ? -1 : 1;
    const kernel::Vec3 sweep{n.x * distance_ * sign, n.y * distance_ * sign, n.z * distance_ * sign};
    const double back = side_ == ExtrudeSide::Both ? -distance_ / 2 : 0;
    const kernel::Plane plane{
        {axes.origin.x + n.x * back, axes.origin.y + n.y * back, axes.origin.z + n.z * back}, axes.x, axes.y};

    Built out = sweepShapes(kernel, shapes, plane, id(),
                            [&](const kernel::ShapeHandle& face) { return kernel.extrude(face, sweep); });
    if (!out.op.ok()) return out;
    if (op_ != ExtrudeOp::NewBody) {
        const StepTool tool{out.op.shape, out.names, op_ == ExtrudeOp::Cut};
        out = combineWithBody(kernel, earlier, std::move(out), op_, body_, name(), id(), "extrude",
                              "try another distance", "Try Flip, to go into it, or a longer distance.");
        if (!out.op.ok()) return out;
        out.tools = {tool};
    }
    addShapeNotes(out, shapes);
    return out;
}

RevolveFeature::RevolveFeature(std::string name, FeatureId sketch, RevolveAxis axis, double angle)
    : Feature(std::move(name)), sketch_(sketch), axis_(axis), angle_(angle) {}

RevolveFeature::RevolveFeature(FeatureId id, std::string name, FeatureId sketch, RevolveAxis axis,
                               double angle)
    : Feature(id, std::move(name)), sketch_(sketch), axis_(axis), angle_(angle) {}

Built RevolveFeature::build(kernel::IKernel& kernel, const ResultLookup& earlier) const {
    constexpr double kFull = 2 * std::numbers::pi;
    if (!(angle_ > 0) || angle_ > kFull + 1e-9) {
        return extrudeFailed(
            "revolve.angle",
            "Revolve needs an angle more than 0 degrees and at most 360 degrees. Type one in "
            "Properties, like 360 for a full turn.");
    }
    const auto shapes = shapesOf(earlier, sketch_, "revolve", "Revolve");
    if (shapes.failure) return *shapes.failure;

    // The line, in the sketch, then in the design.
    sketch::Point2 from{0, 0}, to{0, 1};
    if (axis_.kind == RevolveAxis::Kind::X) {
        to = {1, 0};
    } else if (axis_.kind == RevolveAxis::Kind::Line) {
        const auto* e = shapes.solved.find(axis_.line);
        const auto* line = e ? std::get_if<sketch::LineShape>(&e->shape) : nullptr;
        if (!line) {
            return extrudeFailed("revolve.no_axis",
                                 name() +
                                     " can't find the line it turns about: it was deleted from the sketch. "
                                     "Pick an axis for it in Properties.");
        }
        from = line->start;
        to = line->end;
        if (std::hypot(to.x - from.x, to.y - from.y) < 1e-9) {
            return extrudeFailed("revolve.no_axis", name() + "'s axis, " + e->name +
                                                        ", has no length, so it has no direction. Pick "
                                                        "another axis in Properties.");
        }
    }
    const PlaneAxes& axes = shapes.axes;
    const kernel::Vec3 p = toDesign(axes, from), q = toDesign(axes, to);
    kernel::Vec3 dir{q.x - p.x, q.y - p.y, q.z - p.z};
    if (side_ == ExtrudeSide::Flip) dir = {-dir.x, -dir.y, -dir.z};

    // For both ways, the sketch's plane is turned back half the angle first (the
    // axis lies in it, so it stays put).
    kernel::Plane plane{axes.origin, axes.x, axes.y};
    if (side_ == ExtrudeSide::Both) {
        const double len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        const kernel::Vec3 k{dir.x / len, dir.y / len, dir.z / len};
        const double a = -angle_ / 2, c = std::cos(a), s = std::sin(a);
        // Rodrigues' formula: v turned by a about k.
        auto turn = [&](const kernel::Vec3& v) {
            const auto kv = cross(k, v);
            const double d = (k.x * v.x + k.y * v.y + k.z * v.z) * (1 - c);
            return kernel::Vec3{v.x * c + kv.x * s + k.x * d, v.y * c + kv.y * s + k.y * d,
                                v.z * c + kv.z * s + k.z * d};
        };
        const kernel::Vec3 o = turn({axes.origin.x - p.x, axes.origin.y - p.y, axes.origin.z - p.z});
        plane = {{p.x + o.x, p.y + o.y, p.z + o.z}, turn(axes.x), turn(axes.y)};
    }

    Built out = sweepShapes(kernel, shapes, plane, id(), [&](const kernel::ShapeHandle& face) {
        return kernel.revolve(face, p, dir, std::min(angle_, kFull));
    });
    if (!out.op.ok()) return out;
    if (op_ != ExtrudeOp::NewBody) {
        const StepTool tool{out.op.shape, out.names, op_ == ExtrudeOp::Cut};
        out = combineWithBody(kernel, earlier, std::move(out), op_, body_, name(), id(), "revolve",
                              "try another axis or angle", "Check its axis, or try Flip.");
        if (!out.op.ok()) return out;
        out.tools = {tool};
    }
    addShapeNotes(out, shapes);
    return out;
}

// ---------------------------------------------------------------------------
// Numbers by name
// ---------------------------------------------------------------------------

namespace {

// The x, y or z of a vector by a key's ending, like "size_x".
double* axisOf(kernel::Vec3& v, std::string_view key, std::string_view start) {
    if (key.size() != start.size() + 2 || !key.starts_with(start) || key[start.size()] != '_') return nullptr;
    switch (key.back()) {
        case 'x':
            return &v.x;
        case 'y':
            return &v.y;
        case 'z':
            return &v.z;
        default:
            return nullptr;
    }
}

void addAxes(std::vector<StepNumber>& out, const std::string& start, bool positive) {
    for (const char* axis : {"_x", "_y", "_z"}) out.push_back({start + axis, kLength, positive});
}

}  // namespace

std::vector<StepNumber> BoxFeature::numbers() const {
    std::vector<StepNumber> out;
    addAxes(out, "corner", false);
    addAxes(out, "size", true);
    return out;
}

std::optional<double> BoxFeature::number(std::string_view key) const {
    auto copy = *this;
    if (double* v = axisOf(copy.corner_, key, "corner")) return *v;
    if (double* v = axisOf(copy.size_, key, "size")) return *v;
    return std::nullopt;
}

bool BoxFeature::setNumber(std::string_view key, double value) {
    double* v = axisOf(corner_, key, "corner");
    if (!v) v = axisOf(size_, key, "size");
    if (!v) return false;
    *v = value;
    return true;
}

std::vector<StepNumber> CylinderFeature::numbers() const {
    std::vector<StepNumber> out;
    addAxes(out, "base", false);
    out.push_back({"radius", kLength, true});
    out.push_back({"height", kLength, true});
    return out;
}

std::optional<double> CylinderFeature::number(std::string_view key) const {
    if (key == "radius") return radius_;
    if (key == "height") return height_;
    auto base = base_;
    if (double* v = axisOf(base, key, "base")) return *v;
    return std::nullopt;
}

bool CylinderFeature::setNumber(std::string_view key, double value) {
    double* v = key == "radius" ? &radius_ : key == "height" ? &height_ : axisOf(base_, key, "base");
    if (!v) return false;
    *v = value;
    return true;
}

std::vector<StepNumber> MoveFeature::numbers() const {
    std::vector<StepNumber> out;
    addAxes(out, "offset", false);
    addAxes(out, "center", false);
    out.push_back({"angle", kAngle, false});
    return out;
}

std::optional<double> MoveFeature::number(std::string_view key) const {
    auto m = motion_;
    if (key == "angle") return m.angle;
    if (double* v = axisOf(m.offset, key, "offset")) return *v;
    if (double* v = axisOf(m.center, key, "center")) return *v;
    return std::nullopt;
}

bool MoveFeature::setNumber(std::string_view key, double value) {
    double* v = key == "angle" ? &motion_.angle : axisOf(motion_.offset, key, "offset");
    if (!v) v = axisOf(motion_.center, key, "center");
    if (!v) return false;
    *v = value;
    return true;
}

std::vector<StepNumber> ExtrudeFeature::numbers() const {
    return {{"distance", kLength, true}};
}

std::optional<double> ExtrudeFeature::number(std::string_view key) const {
    if (key == "distance") return distance_;
    return std::nullopt;
}

bool ExtrudeFeature::setNumber(std::string_view key, double value) {
    if (key != "distance") return false;
    distance_ = value;
    return true;
}

std::vector<StepNumber> RevolveFeature::numbers() const {
    return {{"angle", kAngle, true}};
}

std::optional<double> RevolveFeature::number(std::string_view key) const {
    if (key == "angle") return angle_;
    return std::nullopt;
}

bool RevolveFeature::setNumber(std::string_view key, double value) {
    if (key != "angle") return false;
    angle_ = value;
    return true;
}

// ---------------------------------------------------------------------------
// Fillet and Chamfer
// ---------------------------------------------------------------------------

std::vector<StepNumber> EdgeFeature::numbers() const {
    return {{sizeKey(), kLength, true}};
}

std::optional<double> EdgeFeature::number(std::string_view key) const {
    if (key == sizeKey()) return size_;
    return std::nullopt;
}

bool EdgeFeature::setNumber(std::string_view key, double value) {
    if (key != sizeKey()) return false;
    size_ = value;
    return true;
}

void EdgeFeature::remapInputs(const std::map<FeatureId, FeatureId>& map) {
    if (auto it = map.find(body_); it != map.end()) body_ = it->second;
    // Edge names hold the ids of the steps that made their faces: a copy's edges
    // are on the copied steps' faces.
    for (auto& ref : edges_) remapRef(ref, map);
}

Built EdgeFeature::build(kernel::IKernel& kernel, const ResultLookup& earlier) const {
    const bool fillet = type() == FeatureType::Fillet;
    auto failed = [](std::string code, std::string message) {
        Built b;
        b.op.issues.push_back({std::move(code), Severity::Error, std::move(message), {}});
        return b;
    };
    if (!(size_ > 0)) {
        return failed(fillet ? "fillet.radius" : "chamfer.distance",
                      fillet
                          ? "A fillet needs a radius greater than 0 mm. Type one in Properties, like 2."
                          : "A chamfer needs a distance greater than 0 mm. Type one in Properties, like 1.");
    }
    if (edges_.empty()) {
        return failed("edges.none",
                      name() + " has no edges. Pick the edges to " + (fillet ? "round." : "bevel."));
    }
    const auto* shape = earlier.shapeOf(body_);
    if (!shape) return missingInput();
    const ShapeNames* names = namesOf(earlier, body_);

    // Find each edge again. Anything less than sure stops the step.
    std::vector<int> found;
    Diagnostics notes;
    for (const auto& ref : edges_) {
        const auto where = names ? resolve(ref, *names) : Resolution{};
        if (where.status == ResolveStatus::Ambiguous) {
            return failed("edges.ambiguous", name() + " can't tell which edge to " +
                                                 (fillet ? "round" : "bevel") + ": an earlier change made " +
                                                 std::to_string(where.candidates.size()) +
                                                 " edges that look alike where one was. Pick " + name() +
                                                 ", then Modify > Change edges.");
        }
        if (!where.found()) {
            return failed("edges.missing", name() + " can't find an edge it " +
                                               (fillet ? "rounds" : "bevels") +
                                               ": an earlier change took it away. Pick " + name() +
                                               ", then Modify > Change edges.");
        }
        if (where.status == ResolveStatus::Matched && notes.empty()) {
            notes.push_back({"edges.matched",
                             Severity::Info,
                             name() + " found an edge by where it is, because an earlier change renamed it. "
                                      "Check it's the right one.",
                             {}});
        }
        if (std::find(found.begin(), found.end(), where.index) == found.end()) found.push_back(where.index);
    }

    const std::string me = shortId(id().toString());
    const std::string what = role();
    auto out = carried(kernel, apply(kernel, *shape, found), {names}, id(), [&](const kernel::Origin& o) {
        // A round or bevel is named for the edge it replaced; a corner where
        // several meet, for the vertex.
        const auto i = static_cast<std::size_t>(o.index);
        if (o.kind == kernel::OriginKind::Edge && names && i < names->edges.size()) {
            return me + "/" + what + ":" + names->edges[i];
        }
        if (o.kind == kernel::OriginKind::Vertex && names && i < names->vertices.size()) {
            return me + "/corner:" + names->vertices[i];
        }
        return std::string();
    });
    if (out.op.ok()) out.op.issues.insert(out.op.issues.end(), notes.begin(), notes.end());
    return out;
}

}  // namespace sb::model

// ---------------------------------------------------------------------------
// Hole
// ---------------------------------------------------------------------------

namespace sb::model {

const std::vector<ScrewSize>& screwSizes() {
    // Clearance: normal fit; tap: the usual tap drill; counterbore: a socket head
    // screw's head (ISO 4762) + 1 mm across, its height + 0.5 mm deep;
    // countersink: a flat head screw's head (ISO 10642, 90 degrees), widest.
    static const std::vector<ScrewSize> sizes{
        {"M2", 2.4, 1.6, 4.8, 2.5, 0},      {"M2.5", 2.9, 2.05, 5.5, 3.0, 0},
        {"M3", 3.4, 2.5, 6.5, 3.5, 6.72},   {"M4", 4.5, 3.3, 8.0, 4.5, 8.96},
        {"M5", 5.5, 4.2, 9.5, 5.5, 11.2},   {"M6", 6.6, 5.0, 11.0, 6.5, 13.44},
        {"M8", 9.0, 6.8, 14.0, 8.5, 17.92},
    };
    return sizes;
}

const ScrewSize* findScrewSize(std::string_view name) {
    for (const auto& s : screwSizes()) {
        if (s.name.size() == name.size() &&
            std::equal(s.name.begin(), s.name.end(), name.begin(),
                       [](char a, char b) { return std::toupper(a) == std::toupper(b); })) {
            return &s;
        }
    }
    return nullptr;
}

HoleFeature::HoleFeature(std::string name, FeatureId body, HolePlacement placement, double diameter)
    : HoleFeature(FeatureId::generate(), std::move(name), body, std::move(placement), diameter) {}

HoleFeature::HoleFeature(FeatureId id, std::string name, FeatureId body, HolePlacement placement,
                         double diameter)
    : Feature(id, std::move(name)),
      body_(body),
      placement_(std::move(placement)),
      diameter_(diameter),
      cbDiameter_(diameter * 1.8),
      cbDepth_(diameter),
      csDiameter_(diameter * 2),
      csAngle_(std::numbers::pi / 2) {}

bool HoleFeature::setSize(std::string_view size, HoleFit fit) {
    const auto* s = findScrewSize(size);
    if (!s) return false;
    diameter_ = fit == HoleFit::Tap ? s->tap : s->clearance;
    cbDiameter_ = s->counterboreDiameter;
    cbDepth_ = s->counterboreDepth;
    if (s->countersinkDiameter > 0) csDiameter_ = s->countersinkDiameter;
    csAngle_ = std::numbers::pi / 2;
    size_ = s->name;
    fit_ = fit;
    return true;
}

std::vector<StepNumber> HoleFeature::numbers() const {
    std::vector<StepNumber> out{{"diameter", kLength, true}};
    if (!through_) out.push_back({"depth", kLength, true});
    if (kind_ == HoleKind::Counterbore) {
        out.push_back({"counterbore_diameter", kLength, true});
        out.push_back({"counterbore_depth", kLength, true});
    }
    if (kind_ == HoleKind::Countersink) {
        out.push_back({"countersink_diameter", kLength, true});
        out.push_back({"countersink_angle", kAngle, true});
    }
    if (placement_.kind == HolePlacement::Kind::FromEdges) {
        out.push_back({"distance_a", kLength, false});
        out.push_back({"distance_b", kLength, false});
    }
    return out;
}

std::optional<double> HoleFeature::number(std::string_view key) const {
    if (key == "diameter") return diameter_;
    if (key == "depth") return depth_;
    if (key == "counterbore_diameter") return cbDiameter_;
    if (key == "counterbore_depth") return cbDepth_;
    if (key == "countersink_diameter") return csDiameter_;
    if (key == "countersink_angle") return csAngle_;
    if (key == "distance_a") return placement_.distanceA;
    if (key == "distance_b") return placement_.distanceB;
    return std::nullopt;
}

bool HoleFeature::setNumber(std::string_view key, double value) {
    double* v = key == "diameter"               ? &diameter_
                : key == "depth"                ? &depth_
                : key == "counterbore_diameter" ? &cbDiameter_
                : key == "counterbore_depth"    ? &cbDepth_
                : key == "countersink_diameter" ? &csDiameter_
                : key == "countersink_angle"    ? &csAngle_
                : key == "distance_a"           ? &placement_.distanceA
                : key == "distance_b"           ? &placement_.distanceB
                                                : nullptr;
    if (!v) return false;
    if (*v != value && key != "depth" && key != "distance_a" && key != "distance_b") size_.clear();
    *v = value;
    return true;
}

std::vector<FeatureId> HoleFeature::dependencies() const {
    if (placement_.kind == HolePlacement::Kind::SketchPoints) return {body_, placement_.sketch};
    return {body_};
}

std::vector<FeatureId> HoleFeature::sketchInputs() const {
    if (placement_.kind == HolePlacement::Kind::SketchPoints) return {placement_.sketch};
    return {};
}

void HoleFeature::remapInputs(const std::map<FeatureId, FeatureId>& map) {
    if (auto it = map.find(body_); it != map.end()) body_ = it->second;
    if (auto it = map.find(placement_.sketch); it != map.end()) placement_.sketch = it->second;
    remapRef(placement_.edgeA, map);
    remapRef(placement_.edgeB, map);
}

namespace {

double dot(const kernel::Vec3& a, const kernel::Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

kernel::Vec3 scaled(const kernel::Vec3& v, double k) {
    return {v.x * k, v.y * k, v.z * k};
}

kernel::Vec3 plus(const kernel::Vec3& a, const kernel::Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

kernel::Vec3 minus(const kernel::Vec3& a, const kernel::Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

kernel::Vec3 unit(const kernel::Vec3& v) {
    const double l = std::sqrt(dot(v, v));
    return l > 0 ? scaled(v, 1 / l) : v;
}

// One hole to drill: where it starts on the face, which way it goes (a unit
// vector into the body), and a short id for its faces' names.
struct HoleAt {
    kernel::Vec3 at;
    kernel::Vec3 into;
    std::string id;
    std::string label;  // for messages, like "at Point2"
};

Built stepFailed(std::string code, std::string message) {
    Built b;
    b.op.issues.push_back({std::move(code), Severity::Error, std::move(message), {}});
    return b;
}

}  // namespace

Built HoleFeature::build(kernel::IKernel& kernel, const ResultLookup& earlier) const {
    const std::string me = shortId(id().toString());
    if (!(diameter_ > 0)) {
        return stepFailed("hole.diameter",
                          name() +
                              " needs a diameter greater than 0 mm. Type one in Properties, or pick a "
                              "screw size like M3.");
    }
    if (!through_ && !(depth_ > 0)) {
        return stepFailed("hole.depth", name() + " needs a depth greater than 0 mm, or Through all.");
    }
    const double r = diameter_ / 2;
    double top = 0;  // how deep the counterbore or countersink goes (mm)
    if (kind_ == HoleKind::Counterbore) {
        if (!(cbDiameter_ > diameter_) || !(cbDepth_ > 0)) {
            return stepFailed("hole.counterbore",
                              name() +
                                  "'s counterbore must be wider than the hole and more than 0 mm deep. "
                                  "Change its diameter or depth in Properties.");
        }
        top = cbDepth_;
    } else if (kind_ == HoleKind::Countersink) {
        if (!(csDiameter_ > diameter_) || !(csAngle_ > 0) || !(csAngle_ < std::numbers::pi)) {
            return stepFailed("hole.countersink",
                              name() +
                                  "'s countersink must be wider than the hole, with an angle between 0 "
                                  "and 180 degrees, like 90. Change it in Properties.");
        }
        top = (csDiameter_ / 2 - r) / std::tan(csAngle_ / 2);
    }
    if (!through_ && top >= depth_) {
        char mm[32];
        std::snprintf(mm, sizeof mm, "%g", depth_);
        return stepFailed("hole.too_shallow",
                          name() + " is " + mm + " mm deep, but its " +
                              (kind_ == HoleKind::Counterbore ? "counterbore" : "countersink") +
                              " goes deeper than that. Make the hole deeper, or the top smaller.");
    }
    const auto* shape = earlier.shapeOf(body_);
    if (!shape) return missingInput();
    const ShapeNames* names = namesOf(earlier, body_);

    // Where each hole goes.
    std::vector<HoleAt> holes;
    Diagnostics notes;
    if (placement_.kind == HolePlacement::Kind::SketchPoints) {
        const FeatureResult* r0 = earlier.resultOf(placement_.sketch);
        if (!r0 || !r0->sketch || !r0->sketch->ok() || !r0->plane) {
            return stepFailed("hole.sketch", name() + " needs a sketch that solved. Fix the sketch first.");
        }
        const PlaneAxes& axes = *r0->plane;
        kernel::Vec3 into = unit(cross(axes.y, axes.x));  // out of the sketch's back
        if (flip_) into = scaled(into, -1);
        for (const auto& e : r0->sketch->entities) {
            const auto* p = std::get_if<sketch::PointShape>(&e.shape);
            if (!p || e.construction) continue;
            holes.push_back({toDesign(axes, p->at), into, shortId(e.id.toString()), "at " + e.name});
        }
        if (holes.empty()) {
            return stepFailed("hole.no_points",
                              name() +
                                  "'s sketch has no points to drill at. Add a point where each hole goes "
                                  "(the Point tool), or place the hole from two edges.");
        }
    } else {
        if (!names) return missingInput();
        // Both edges, found again by name; they must be straight.
        int found[2] = {-1, -1};
        const GeometryRef* refs[2] = {&placement_.edgeA, &placement_.edgeB};
        for (int k = 0; k < 2; ++k) {
            const auto where = resolve(*refs[k], *names);
            if (where.status == ResolveStatus::Ambiguous || !where.found()) {
                return stepFailed(
                    "hole.edge_missing",
                    name() + " can't find one of the edges it's placed from: an earlier change " +
                        (where.status == ResolveStatus::Ambiguous ? "made several like it" : "took it away") +
                        ". Delete " + name() + " and add it again from two edges.");
            }
            if (where.status == ResolveStatus::Matched && notes.empty()) {
                notes.push_back({"hole.edge_matched",
                                 Severity::Info,
                                 name() +
                                     " found an edge by where it is, because an earlier change renamed it. "
                                     "Check the hole is in the right place.",
                                 {}});
            }
            found[k] = where.index;
        }
        const auto& el = names->elements;
        const auto& a = el.edges[static_cast<std::size_t>(found[0])];
        const auto& b = el.edges[static_cast<std::size_t>(found[1])];
        if (a.geom != kernel::GeomKind::Line || b.geom != kernel::GeomKind::Line) {
            return stepFailed("hole.edge_curved",
                              name() +
                                  " can only be placed from straight edges. Pick two straight edges of a "
                                  "flat face.");
        }
        // The flat face both edges border.
        int face = -1;
        for (const int fa : el.edgeFaces[static_cast<std::size_t>(found[0])]) {
            for (const int fb : el.edgeFaces[static_cast<std::size_t>(found[1])]) {
                if (fa == fb && el.faces[static_cast<std::size_t>(fa)].geom == kernel::GeomKind::Plane)
                    face = fa;
            }
        }
        if (face < 0) {
            return stepFailed("hole.no_face",
                              name() +
                                  "'s two edges aren't both on one flat face. Pick two edges of the face "
                                  "the hole goes in.");
        }
        const auto& f = el.faces[static_cast<std::size_t>(face)];
        const kernel::Vec3 n = unit(f.direction);
        // Lines a distance in from each edge, toward the face's middle.
        auto inward = [&](const kernel::ElementInfo& e) {
            kernel::Vec3 u = unit(cross(n, e.direction));
            if (dot(u, minus(f.center, e.center)) < 0) u = scaled(u, -1);
            return u;
        };
        const kernel::Vec3 pa = plus(a.center, scaled(inward(a), placement_.distanceA));
        const kernel::Vec3 pb = plus(b.center, scaled(inward(b), placement_.distanceB));
        const double across = dot(cross(a.direction, b.direction), n);
        if (std::abs(across) < 1e-9) {
            return stepFailed("hole.edges_parallel",
                              name() +
                                  "'s two edges are parallel, so they can't say where along them the hole "
                                  "goes. Pick two edges that meet at a corner.");
        }
        // pa + t * a.direction lies on the line through pb along b.direction.
        const double t = dot(cross(minus(pb, pa), b.direction), n) / across;
        kernel::Vec3 into = scaled(n, -1);
        if (flip_) into = n;
        holes.push_back({plus(pa, scaled(a.direction, t)), into, "edges", "from its edges"});
    }

    // All the way through: farther than the body is across.
    const auto box = kernel.bounds(*shape);
    const double across = std::sqrt(dot(minus(box.max, box.min), minus(box.max, box.min)));

    Built out{kernel::OpResult{*shape, {}, {}}, names ? std::make_shared<const ShapeNames>(*names) : nullptr};
    std::vector<StepTool> tools;
    for (const auto& h : holes) {
        // Half the hole's cross-section, in a plane through its axis: x out from
        // the axis, y down into the body, starting 1 mm above the face.
        const double deep = through_ ? across + 1 : depth_;
        std::vector<std::pair<sketch::Point2, std::string>>
            pts;  // each point, and the face its next line makes
        pts.push_back({{0, -1}, ""});
        if (kind_ == HoleKind::Counterbore) {
            const double R = cbDiameter_ / 2;
            pts.push_back({{R, -1}, "counterbore"});
            pts.push_back({{R, cbDepth_}, "step"});
            pts.push_back({{r, cbDepth_}, "wall"});
        } else if (kind_ == HoleKind::Countersink) {
            const double t2 = std::tan(csAngle_ / 2);
            pts.push_back({{csDiameter_ / 2 + t2, -1}, "countersink"});
            pts.push_back({{r, top}, "wall"});
        } else {
            pts.push_back({{r, -1}, "wall"});
        }
        pts.push_back({{r, deep}, "floor"});
        pts.push_back({{0, deep}, ""});
        kernel::Loop2 loop;
        std::vector<std::string> roles;
        for (std::size_t k = 0; k < pts.size(); ++k) {
            const auto& p = pts[k].first;
            const auto& q = pts[(k + 1) % pts.size()].first;
            loop.push_back(kernel::Edge2{kernel::Line2{{p.x, p.y}, {q.x, q.y}}});
            roles.push_back(pts[k].second);
        }
        // The plane: x square to the axis, y along it (into the body).
        const kernel::Vec3 helper = std::abs(h.into.z) < 0.9 ? kernel::Vec3{0, 0, 1} : kernel::Vec3{1, 0, 0};
        const kernel::Vec3 x = unit(cross(helper, h.into));
        auto face = kernel.makeFace({loop, {}}, kernel::Plane{h.at, x, h.into});
        if (!face.ok()) return {face, nullptr};
        auto tool = kernel.revolve(face.shape, h.at, h.into, 2 * std::numbers::pi);
        if (!tool.ok()) return {tool, nullptr};
        const auto& edgeCurves = face.history.edges;
        auto named = carried(kernel, std::move(tool), {}, id(), [&](const kernel::Origin& o) -> std::string {
            const auto e = static_cast<std::size_t>(o.index);
            if (o.kind != kernel::OriginKind::Edge || e >= edgeCurves.size() || edgeCurves[e].empty())
                return {};
            const auto c = static_cast<std::size_t>(edgeCurves[e].front().index);
            return c < roles.size() && !roles[c].empty() ? me + "/" + roles[c] + ":" + h.id : std::string();
        });
        const double before = kernel.massProps(out.op.shape).volume;
        auto cut = kernel.boolean(kernel::BoolOp::Cut, out.op.shape, named.op.shape);
        if (!cut.ok()) {
            return stepFailed("hole.cut_failed",
                              name() + " couldn't drill its hole " + h.label + ". Check it's on the body.");
        }
        tools.push_back({named.op.shape, named.names, true});
        auto next = carried(kernel, std::move(cut), {out.names.get(), named.names.get()}, id());
        if (std::abs(kernel.massProps(next.op.shape).volume - before) < 1e-9) {
            notes.push_back(
                {"hole.missed",
                 Severity::Warning,
                 name() + "'s hole " + h.label +
                     " isn't on the body, so it drilled nothing. Move it onto the body, or try Flip.",
                 {}});
        }
        out = std::move(next);
    }
    out.op.issues.insert(out.op.issues.end(), notes.begin(), notes.end());
    out.tools = std::move(tools);
    return out;
}

}  // namespace sb::model

// ---------------------------------------------------------------------------
// Pattern
// ---------------------------------------------------------------------------

namespace sb::model {

std::vector<kernel::Motion> patternMotions(const PatternLayout& l) {
    std::vector<kernel::Motion> out;
    if (l.kind == PatternLayout::Kind::Linear) {
        const kernel::Vec3 a = unit(l.direction), b = unit(l.direction2);
        for (int j = 0; j < std::max(l.count2, 1); ++j) {
            for (int i = 0; i < std::max(l.count, 1); ++i) {
                if (i == 0 && j == 0) continue;
                kernel::Motion m;
                m.offset = plus(scaled(a, i * l.spacing), scaled(b, j * l.spacing2));
                out.push_back(m);
            }
        }
        return out;
    }
    // Round a circle: evenly all the way round, or spread over the angle with
    // the last one at its end.
    const bool full = l.angle > 2 * std::numbers::pi - 1e-9;
    const int n = std::max(l.count, 1);
    const double step = full ? 2 * std::numbers::pi / n : (n > 1 ? l.angle / (n - 1) : 0);
    for (int i = 1; i < n; ++i) {
        kernel::Motion m;
        m.center = l.center;
        m.axis = l.axis;
        m.angle = i * step;
        out.push_back(m);
    }
    return out;
}

PatternFeature::PatternFeature(std::string name, FeatureId body, std::vector<FeatureId> steps,
                               PatternLayout layout)
    : PatternFeature(FeatureId::generate(), std::move(name), body, std::move(steps), layout) {}

PatternFeature::PatternFeature(FeatureId id, std::string name, FeatureId body, std::vector<FeatureId> steps,
                               PatternLayout layout)
    : Feature(id, std::move(name)), body_(body), steps_(std::move(steps)), layout_(layout) {}

std::vector<StepNumber> PatternFeature::numbers() const {
    std::vector<StepNumber> out{{"count", kPlainNumber, true}};
    if (layout_.kind == PatternLayout::Kind::Linear) {
        out.push_back({"spacing", kLength, false});
        out.push_back({"count2", kPlainNumber, true});
        out.push_back({"spacing2", kLength, false});
    } else {
        out.push_back({"angle", kAngle, true});
    }
    return out;
}

std::optional<double> PatternFeature::number(std::string_view key) const {
    if (key == "count") return layout_.count;
    if (key == "count2") return layout_.count2;
    if (key == "spacing") return layout_.spacing;
    if (key == "spacing2") return layout_.spacing2;
    if (key == "angle") return layout_.angle;
    return std::nullopt;
}

bool PatternFeature::setNumber(std::string_view key, double value) {
    // Counts are whole numbers: a formula's 2.6 makes 3 copies.
    if (key == "count") {
        layout_.count = static_cast<int>(std::lround(value));
    } else if (key == "count2") {
        layout_.count2 = static_cast<int>(std::lround(value));
    } else if (key == "spacing") {
        layout_.spacing = value;
    } else if (key == "spacing2") {
        layout_.spacing2 = value;
    } else if (key == "angle") {
        layout_.angle = value;
    } else {
        return false;
    }
    return true;
}

std::vector<FeatureId> PatternFeature::dependencies() const {
    std::vector<FeatureId> out{body_};
    for (const auto& s : steps_)
        if (s != body_) out.push_back(s);
    return out;
}

std::vector<FeatureId> PatternFeature::referenceInputs() const {
    // The steps it repeats are only looked at; the body is what it changes.
    std::vector<FeatureId> out;
    for (const auto& s : steps_)
        if (s != body_) out.push_back(s);
    return out;
}

void PatternFeature::remapInputs(const std::map<FeatureId, FeatureId>& map) {
    if (auto it = map.find(body_); it != map.end()) body_ = it->second;
    for (auto& s : steps_)
        if (auto it = map.find(s); it != map.end()) s = it->second;
}

namespace {

// One copy a Pattern or Mirror makes: how it moves a shape, the role its faces'
// names get ("copy3", "mirror"), and what messages call it ("copy 3").
struct CopyOf {
    std::function<kernel::OpResult(const kernel::ShapeHandle&)> move;
    std::string role;
    std::string label;
};

// A shape moved by `copy`, its faces named "<step>/<role>:<name>".
Built movedCopy(kernel::IKernel& kernel, const kernel::ShapeHandle& shape, const ShapeNames* names,
                const CopyOf& copy, const FeatureId& step) {
    std::shared_ptr<const ShapeNames> renamed;
    if (names) {
        std::vector<std::string> faces;
        const std::string prefix = shortId(step.toString()) + "/" + copy.role + ":";
        for (const auto& f : names->faces) faces.push_back(prefix + f);
        renamed = std::make_shared<const ShapeNames>(nameFromFaces(names->elements, std::move(faces)));
    }
    return carried(kernel, copy.move(shape), {renamed.get()}, step);
}

// What a Pattern and a Mirror share: copies of what `steps` did (their holes,
// joins and cuts) made on `body`, or, with no steps, copies of `body` itself
// joined to it. `verb` ("pattern", "mirror") starts the codes; `hint` says what to
// check when a copy misses.
Built copyInto(kernel::IKernel& kernel, const ResultLookup& earlier, const FeatureId& id,
               const std::string& name, const FeatureId& body, const std::vector<FeatureId>& steps,
               const std::vector<CopyOf>& copies, const std::string& verb, const std::string& hint) {
    const auto* shape = earlier.shapeOf(body);
    if (!shape) return missingInput();
    const ShapeNames* names = namesOf(earlier, body);
    Built out{
        kernel::OpResult{*shape, {}, {}}, names ? std::make_shared<const ShapeNames>(*names) : nullptr, {}};
    if (steps.empty()) {
        // Copies of the whole body, joined where they touch.
        for (const auto& c : copies) {
            auto copy = movedCopy(kernel, *shape, names, c, id);
            if (!copy.op.ok()) return copy;
            auto joined = kernel.boolean(kernel::BoolOp::Union, out.op.shape, copy.op.shape);
            if (!joined.ok()) {
                return stepFailed(verb + ".join_failed",
                                  name + " couldn't join its " + c.label + " to the body. " + hint);
            }
            out = carried(kernel, std::move(joined), {out.names.get(), copy.names.get()}, id);
        }
        return out;
    }

    // Copies of what the steps did: their holes, joins and cuts, again at each copy.
    std::vector<const StepTool*> tools;
    for (const auto& s : steps) {
        const FeatureResult* r = earlier.resultOf(s);
        if (!r || r->status != FeatureStatus::Ok) return missingInput();
        if (r->tools.empty()) {
            return stepFailed(verb + ".not_repeatable",
                              name +
                                  " can repeat holes, and extrudes and revolves that join or cut. One of its "
                                  "steps makes a body of its own: " +
                                  verb + " that body instead.");
        }
        for (const auto& t : r->tools) tools.push_back(&t);
    }
    Diagnostics notes;
    for (const auto& c : copies) {
        for (const auto* tool : tools) {
            auto copy = movedCopy(kernel, tool->shape, tool->names.get(), c, id);
            if (!copy.op.ok()) return copy;
            const double before = kernel.massProps(out.op.shape).volume;
            auto done = kernel.boolean(tool->cut ? kernel::BoolOp::Cut : kernel::BoolOp::Union, out.op.shape,
                                       copy.op.shape);
            if (!done.ok()) {
                return stepFailed(
                    verb + ".combine_failed",
                    name + " couldn't " + (tool->cut ? "cut" : "join") + " its " + c.label + ". " + hint);
            }
            auto next = carried(kernel, std::move(done), {out.names.get(), copy.names.get()}, id);
            if (tool->cut && std::abs(kernel.massProps(next.op.shape).volume - before) < 1e-9) {
                notes.push_back({verb + ".copy_missed",
                                 Severity::Warning,
                                 name + "'s " + c.label + " isn't on the body, so it cut nothing. " + hint,
                                 {}});
            }
            out = std::move(next);
        }
    }
    out.op.issues.insert(out.op.issues.end(), notes.begin(), notes.end());
    return out;
}

}  // namespace

Built PatternFeature::build(kernel::IKernel& kernel, const ResultLookup& earlier) const {
    const auto& l = layout_;
    if (l.count < 1 || l.count2 < 1) {
        return stepFailed(
            "pattern.count",
            name() + " needs at least 1 of each, the first one included. Type a count in Properties.");
    }
    const auto length = [](const kernel::Vec3& v) { return std::sqrt(dot(v, v)); };
    if (l.kind == PatternLayout::Kind::Linear) {
        if (length(l.direction) < 1e-12 || (l.count2 > 1 && length(l.direction2) < 1e-12)) {
            return stepFailed("pattern.direction",
                              name() + "'s direction can't be 0, 0, 0. Give it one, like 1, 0, 0 along x.");
        }
        if ((l.count > 1 && l.spacing == 0) || (l.count2 > 1 && l.spacing2 == 0)) {
            return stepFailed(
                "pattern.spacing",
                name() + "'s copies are 0 mm apart, all in one place. Give a spacing, like 10 mm.");
        }
    } else {
        if (length(l.axis) < 1e-12) {
            return stepFailed("pattern.axis",
                              name() + "'s axis can't be 0, 0, 0. Give it a direction, like 0, 0, 1 for up.");
        }
        if (!(l.angle > 0) || l.angle > 2 * std::numbers::pi + 1e-9) {
            return stepFailed("pattern.angle",
                              name() + " needs an angle more than 0 and at most 360 degrees.");
        }
    }
    const auto motions = patternMotions(l);
    constexpr std::size_t kMost = 200;
    if (motions.size() + 1 > kMost) {
        return stepFailed(
            "pattern.too_many",
            name() + " would make " + std::to_string(motions.size() + 1) +
                " copies. SketchBillet makes up to 200 in one pattern: use fewer, or two patterns.");
    }
    std::vector<CopyOf> copies;
    for (std::size_t k = 0; k < motions.size(); ++k) {
        const auto n = std::to_string(k + 2);
        copies.push_back(
            {[&kernel, m = motions[k]](const kernel::ShapeHandle& s) { return kernel.transform(s, m); },
             "copy" + n, "copy " + n});
    }
    return copyInto(kernel, earlier, id(), name(), body_, steps_, copies, "pattern",
                    "Check the spacing and direction.");
}

MirrorFeature::MirrorFeature(std::string name, FeatureId body, std::vector<FeatureId> steps,
                             MirrorPlane plane)
    : MirrorFeature(FeatureId::generate(), std::move(name), body, std::move(steps), std::move(plane)) {}

MirrorFeature::MirrorFeature(FeatureId id, std::string name, FeatureId body, std::vector<FeatureId> steps,
                             MirrorPlane plane)
    : Feature(id, std::move(name)), body_(body), steps_(std::move(steps)), plane_(std::move(plane)) {}

std::vector<StepNumber> MirrorFeature::numbers() const {
    if (plane_.kind == MirrorPlane::Kind::Face) return {};
    return {{"offset", kLength, false}};
}

std::optional<double> MirrorFeature::number(std::string_view key) const {
    if (key == "offset") return plane_.offset;
    return std::nullopt;
}

bool MirrorFeature::setNumber(std::string_view key, double value) {
    if (key != "offset") return false;
    plane_.offset = value;
    return true;
}

std::vector<FeatureId> MirrorFeature::dependencies() const {
    std::vector<FeatureId> out{body_};
    for (const auto& s : steps_)
        if (s != body_) out.push_back(s);
    return out;
}

std::vector<FeatureId> MirrorFeature::referenceInputs() const {
    std::vector<FeatureId> out;
    for (const auto& s : steps_)
        if (s != body_) out.push_back(s);
    return out;
}

void MirrorFeature::remapInputs(const std::map<FeatureId, FeatureId>& map) {
    if (auto it = map.find(body_); it != map.end()) body_ = it->second;
    for (auto& s : steps_)
        if (auto it = map.find(s); it != map.end()) s = it->second;
    remapRef(plane_.face, map);
}

Built MirrorFeature::build(kernel::IKernel& kernel, const ResultLookup& earlier) const {
    // The plane: a point on it and its normal.
    kernel::Point3 point{};
    kernel::Vec3 normal{1, 0, 0};
    switch (plane_.kind) {
        case MirrorPlane::Kind::YZ:
            point = {plane_.offset, 0, 0};
            break;
        case MirrorPlane::Kind::XZ:
            point = {0, plane_.offset, 0};
            normal = {0, 1, 0};
            break;
        case MirrorPlane::Kind::XY:
            point = {0, 0, plane_.offset};
            normal = {0, 0, 1};
            break;
        case MirrorPlane::Kind::Face: {
            const ShapeNames* names = namesOf(earlier, body_);
            if (!names) return missingInput();
            const auto where = resolve(plane_.face, *names);
            if (where.status == ResolveStatus::Ambiguous || !where.found()) {
                return stepFailed(
                    "mirror.face_missing",
                    name() + " can't find the face it mirrors across: an earlier change " +
                        (where.status == ResolveStatus::Ambiguous ? "made several like it" : "took it away") +
                        ". Pick a plane for it in Properties.");
            }
            const auto& f = names->elements.faces[static_cast<std::size_t>(where.index)];
            if (f.geom != kernel::GeomKind::Plane) {
                return stepFailed(
                    "mirror.face_curved",
                    name() + " can only mirror across a flat face. Pick a flat face, or a plane.");
            }
            point = f.center;
            normal = f.direction;
            break;
        }
    }
    const std::vector<CopyOf> copies{
        {[&kernel, point, normal](const kernel::ShapeHandle& s) { return kernel.mirror(s, point, normal); },
         "mirror", "mirror image"}};
    return copyInto(kernel, earlier, id(), name(), body_, steps_, copies, "mirror",
                    "Check the mirror's plane.");
}

PlaneFeature::PlaneFeature(std::string name, PlaneFrom from)
    : PlaneFeature(FeatureId::generate(), std::move(name), std::move(from)) {}

PlaneFeature::PlaneFeature(FeatureId id, std::string name, PlaneFrom from)
    : Feature(id, std::move(name)), from_(std::move(from)) {}

std::vector<StepNumber> PlaneFeature::numbers() const {
    return {{"offset", kLength, false}, {"angle", kAngle, false}};
}

std::optional<double> PlaneFeature::number(std::string_view key) const {
    if (key == "offset") return offset_;
    if (key == "angle") return angle_;
    return std::nullopt;
}

bool PlaneFeature::setNumber(std::string_view key, double value) {
    if (key == "offset") {
        offset_ = value;
    } else if (key == "angle") {
        angle_ = value;
    } else {
        return false;
    }
    return true;
}

std::vector<FeatureId> PlaneFeature::dependencies() const {
    if (from_.kind == PlaneFrom::Kind::Face) return {from_.face.body};
    if (from_.kind == PlaneFrom::Kind::Plane) return {from_.plane};
    return {};
}

void PlaneFeature::remapInputs(const std::map<FeatureId, FeatureId>& map) {
    if (auto it = map.find(from_.face.body); it != map.end()) from_.face.body = it->second;
    if (auto it = map.find(from_.plane); it != map.end()) from_.plane = it->second;
    remapRef(from_.face.face, map);
    remapRef(hinge_.edge, map);
}

Built PlaneFeature::build(kernel::IKernel&, const ResultLookup&) const {
    return stepFailed("plane.no_build", "Planes are worked out by the rebuild, not the kernel.");
}

std::optional<PlaneAxes> PlaneFeature::where(const ResultLookup& earlier, Diagnostic& why) const {
    auto fail = [&](std::string code, std::string message) -> std::optional<PlaneAxes> {
        why = {std::move(code), Severity::Error, std::move(message), {}};
        return std::nullopt;
    };
    // Where it starts.
    PlaneAxes start;
    const ShapeNames* names = nullptr;
    switch (from_.kind) {
        case PlaneFrom::Kind::Base:
            start = planeAxes(from_.base);
            break;
        case PlaneFrom::Kind::Plane: {
            const FeatureResult* r = earlier.resultOf(from_.plane);
            if (!r || r->status != FeatureStatus::Ok || !r->plane) {
                return fail(
                    "plane.from_missing",
                    name() + " starts from a plane that isn't there. Pick what it starts from again.");
            }
            start = *r->plane;
            break;
        }
        case PlaneFrom::Kind::Face: {
            names = namesOf(earlier, from_.face.body);
            if (!names) {
                return fail("plane.face_missing",
                            name() + " starts from a face of a body that isn't there. Pick its face again.");
            }
            const auto found = resolve(from_.face.face, *names);
            if (!found.found()) {
                return fail(
                    "plane.face_missing",
                    name() +
                        (found.status == ResolveStatus::Ambiguous
                             ? " can't tell which face it starts from: an earlier change split it."
                             : " can't find the face it starts from: an earlier change took it away.") +
                        " Pick its face again.");
            }
            const auto axes = faceAxes(names->elements.faces[static_cast<std::size_t>(found.index)]);
            if (!axes) {
                return fail(
                    "plane.face_curved",
                    name() + " starts from a curved face, and a plane needs a flat one. Pick a flat face.");
            }
            start = *axes;
            break;
        }
    }

    // Turned about the hinge.
    PlaneAxes out = start;
    if (angle_ != 0) {
        kernel::Vec3 point = start.origin, dir = start.x;
        if (hinge_.kind == PlaneHinge::Kind::Y) dir = start.y;
        if (hinge_.kind == PlaneHinge::Kind::Edge) {
            if (!names) {
                return fail("plane.hinge",
                            name() +
                                " can only turn about an edge when it starts from a face. Turn it about its "
                                "x or y instead.");
            }
            const auto found = resolve(hinge_.edge, *names);
            if (!found.found()) {
                return fail("plane.hinge_missing",
                            name() +
                                " can't find the edge it turns about: an earlier change took it away. Pick "
                                "its hinge again.");
            }
            const auto& e = names->elements.edges[static_cast<std::size_t>(found.index)];
            const kernel::Vec3 n = unit(cross(start.x, start.y));
            if (e.geom != kernel::GeomKind::Line || std::abs(dot(unit(e.direction), n)) > 1e-6 ||
                std::abs(dot(minus(e.center, start.origin), n)) > 1e-6) {
                return fail("plane.hinge",
                            name() +
                                " turns about an edge, and the edge must be straight and on the face it "
                                "starts from. Pick an edge of that face.");
            }
            point = e.center;
            dir = e.direction;
        }
        const kernel::Vec3 k = unit(dir);
        const double c = std::cos(angle_), s = std::sin(angle_);
        auto turn = [&](const kernel::Vec3& v) {  // Rodrigues' formula
            const auto kv = cross(k, v);
            const double d = dot(k, v) * (1 - c);
            return kernel::Vec3{v.x * c + kv.x * s + k.x * d, v.y * c + kv.y * s + k.y * d,
                                v.z * c + kv.z * s + k.z * d};
        };
        out.origin = plus(point, turn(minus(start.origin, point)));
        out.x = turn(start.x);
        out.y = turn(start.y);
    }
    // Moved along the way it faces.
    const kernel::Vec3 n = unit(cross(out.x, out.y));
    out.origin = plus(out.origin, scaled(n, offset_));
    return out;
}

ShellFeature::ShellFeature(std::string name, FeatureId body, std::vector<GeometryRef> faces, double thickness)
    : ShellFeature(FeatureId::generate(), std::move(name), body, std::move(faces), thickness) {}

ShellFeature::ShellFeature(FeatureId id, std::string name, FeatureId body, std::vector<GeometryRef> faces,
                           double thickness)
    : Feature(id, std::move(name)), body_(body), faces_(std::move(faces)), thickness_(thickness) {}

std::vector<StepNumber> ShellFeature::numbers() const {
    return {{"thickness", kLength, true}};
}

std::optional<double> ShellFeature::number(std::string_view key) const {
    if (key == "thickness") return thickness_;
    return std::nullopt;
}

bool ShellFeature::setNumber(std::string_view key, double value) {
    if (key != "thickness") return false;
    thickness_ = value;
    return true;
}

void ShellFeature::remapInputs(const std::map<FeatureId, FeatureId>& map) {
    if (auto it = map.find(body_); it != map.end()) body_ = it->second;
    for (auto& f : faces_) remapRef(f, map);
}

Built ShellFeature::build(kernel::IKernel& kernel, const ResultLookup& earlier) const {
    if (!(thickness_ > 0)) {
        return stepFailed("shell.thickness",
                          name() + " needs walls thicker than 0 mm. Type a thickness in Properties, like 2.");
    }
    if (faces_.empty()) {
        return stepFailed("shell.no_faces",
                          name() + " has no faces to leave open. Pick at least one, like the top.");
    }
    const auto* shape = earlier.shapeOf(body_);
    if (!shape) return missingInput();
    const ShapeNames* names = namesOf(earlier, body_);

    // Find each face again. Anything less than sure stops the step.
    std::vector<int> found;
    Diagnostics notes;
    for (const auto& ref : faces_) {
        const auto where = names ? resolve(ref, *names) : Resolution{};
        if (!where.found()) {
            return stepFailed(
                "shell.face_missing",
                name() + " can't find a face it leaves open: an earlier change " +
                    (where.status == ResolveStatus::Ambiguous ? "made several like it" : "took it away") +
                    ". Delete " + name() + " and add it again with the faces to leave open.");
        }
        if (where.status == ResolveStatus::Matched && notes.empty()) {
            notes.push_back({"shell.face_matched",
                             Severity::Info,
                             name() +
                                 " found a face by where it is, because an earlier change renamed it. Check "
                                 "it's the right one.",
                             {}});
        }
        if (std::find(found.begin(), found.end(), where.index) == found.end()) found.push_back(where.index);
    }

    const std::string me = shortId(id().toString());
    auto out =
        carried(kernel, kernel.shell(*shape, found, thickness_), {names}, id(), [&](const kernel::Origin& o) {
            // An inside wall is named for the outside face it's the inside of.
            const auto i = static_cast<std::size_t>(o.index);
            if (o.kind == kernel::OriginKind::Inside && names && i < names->faces.size()) {
                return me + "/inside:" + names->faces[i];
            }
            return std::string();
        });
    if (out.op.ok()) out.op.issues.insert(out.op.issues.end(), notes.begin(), notes.end());
    return out;
}

}  // namespace sb::model
