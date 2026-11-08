// Sketching on a face of a body: the plane comes from the face at every
// rebuild, so the sketch (and what's made from it) follows the face when
// earlier steps change; the body stays in the design; and a face that's gone
// stops the sketch with a plain message until a face is picked again.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>

#include "sb/io/SketchBilletFile.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Document.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;
using kernel::TopoKind;

namespace {

constexpr double kPi = std::numbers::pi;

bool same(const kernel::Vec3& a, const kernel::Vec3& b) {
    return std::abs(a.x - b.x) < 1e-12 && std::abs(a.y - b.y) < 1e-12 && std::abs(a.z - b.z) < 1e-12;
}

kernel::ElementInfo flat(kernel::Vec3 normal, kernel::Point3 center) {
    kernel::ElementInfo f;
    f.geom = kernel::GeomKind::Plane;
    f.direction = normal;
    f.center = center;
    return f;
}

struct Design {
    Document doc;
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();
    std::unique_ptr<sketch::ISketchSolver> solver = sketch::makePlaneGcsSolver();

    Diagnostics tryRun(std::string_view name, Args args) {
        auto made = CommandRegistry::builtin().create(name, args, doc);
        if (!made.ok()) return made.issues;
        return doc.execute(std::move(made.command));
    }
    void run(std::string_view name, Args args) {
        const auto issues = tryRun(name, std::move(args));
        INFO(name << ": " << (issues.empty() ? std::string() : issues.front().message));
        REQUIRE(issues.empty());
    }
    FeatureId last() const { return doc.featureAt(doc.featureCount() - 1).id(); }
    RebuildReport rebuild() { return doc.regenerate(*kernel, solver.get()); }

    // The face of a body's result facing `normal`, as a reference.
    GeometryRef faceFacing(const FeatureId& body, kernel::Vec3 normal) {
        const auto report = rebuild();
        const auto& names = *report.result(body)->names;
        for (std::size_t i = 0; i < names.elements.faces.size(); ++i) {
            const auto& f = names.elements.faces[i];
            if (f.geom == kernel::GeomKind::Plane && same(f.direction, normal)) {
                return referTo(names, TopoKind::Face, static_cast<int>(i));
            }
        }
        FAIL("no face facing that way");
        return {};
    }

    // A 40 x 20 mm block, 10 mm tall, with its height in Extrude1; and a sketch
    // on its top with a 5 mm circle at (20, 10), extruded 8 mm up.
    std::pair<FeatureId, FeatureId> blockWithBoss() {
        run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}, {"name", std::string("Block")}});
        const auto block = last();
        run("feature.add_sketch", {{"body", block}, {"face", faceFacing(block, {0, 0, 1})}});
        const auto sketch = last();
        run("sketch.add_circle", {{"sketch", sketch}, {"center", sketch::Point2{20, 10}}, {"radius", 5.0}});
        run("feature.add_extrude", {{"sketch", sketch}, {"distance", 8.0}});
        return {block, last()};
    }
};

}  // namespace

TEST_CASE("a face's sketch plane: seen from outside, level x, origin nearest the design's",
          "[model][sketch][face]") {
    // A top face: just like XY, lifted to the face.
    auto top = faceAxes(flat({0, 0, 1}, {20, 10, 10}));
    REQUIRE(top);
    CHECK(same(top->origin, {0, 0, 10}));
    CHECK(same(top->x, {1, 0, 0}));
    CHECK(same(top->y, {0, 1, 0}));
    // A bottom face: seen from below, so y is flipped (x × y still points out).
    auto bottom = faceAxes(flat({0, 0, -1}, {20, 10, 0}));
    REQUIRE(bottom);
    CHECK(same(bottom->x, {1, 0, 0}));
    CHECK(same(bottom->y, {0, -1, 0}));
    // Front and right faces: like XZ and YZ, moved to the face.
    auto front = faceAxes(flat({0, -1, 0}, {20, -5, 5}));
    REQUIRE(front);
    CHECK(same(front->origin, {0, -5, 0}));
    CHECK(same(front->x, {1, 0, 0}));
    CHECK(same(front->y, {0, 0, 1}));
    auto right = faceAxes(flat({1, 0, 0}, {40, 10, 5}));
    REQUIRE(right);
    CHECK(same(right->origin, {40, 0, 0}));
    CHECK(same(right->x, {0, 1, 0}));
    CHECK(same(right->y, {0, 0, 1}));
    // A sloped face: x stays level, y climbs the slope.
    const double s = std::sqrt(0.5);
    auto slope = faceAxes(flat({0, -s, s}, {0, 0, 10}));
    REQUIRE(slope);
    CHECK(slope->x.z == Approx(0).margin(1e-12));
    CHECK(slope->y.z > 0);
    CHECK(slope->origin.y == Approx(-5));
    CHECK(slope->origin.z == Approx(5));
    // A curved face has no plane.
    kernel::ElementInfo round;
    round.geom = kernel::GeomKind::Cylinder;
    round.direction = {0, 0, 1};
    CHECK_FALSE(faceAxes(round));
    // The base plane nearest a face: for older SketchBillets and for orientation.
    CHECK(nearestPlane({0, 0, -1}) == SketchPlane::XY);
    CHECK(nearestPlane({0, 1, 0.2}) == SketchPlane::XZ);
    CHECK(nearestPlane({-1, 0.1, 0.1}) == SketchPlane::YZ);
}

TEST_CASE("a boss sketched on a block's top follows the top when the block changes",
          "[model][sketch][face][occt]") {
    Design d;
    const auto [block, boss] = d.blockWithBoss();
    auto report = d.rebuild();
    REQUIRE(report.ok());
    // Both bodies are in the design: sketching on the block doesn't use it up.
    CHECK(report.bodies() == std::vector<FeatureId>{block, boss});
    auto box = d.kernel->bounds(*report.shapeOf(boss));
    CHECK(box.min.z == Approx(10));
    CHECK(box.max.z == Approx(18));
    CHECK(box.min.x == Approx(15));
    CHECK(d.kernel->massProps(*report.shapeOf(boss)).volume == Approx(kPi * 25 * 8));

    // A taller block: the boss starts on the new top.
    d.run("feature.set_value", {{"feature", block}, {"setting", std::string("size_z")}, {"value", 25.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    box = d.kernel->bounds(*report.shapeOf(boss));
    CHECK(box.min.z == Approx(25));
    CHECK(box.max.z == Approx(33));
    // The sketch's x and y are the design's, so the circle didn't slide.
    CHECK(box.min.x == Approx(15));
    CHECK(box.min.y == Approx(5));
}

TEST_CASE("a sketch on the block's front draws upright, and extrudes out of the front",
          "[model][sketch][face][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
    const auto block = d.last();
    d.run("feature.add_sketch", {{"body", block}, {"face", d.faceFacing(block, {0, -1, 0})}});
    const auto sketch = d.last();
    CHECK(static_cast<const SketchFeature*>(d.doc.find(sketch))->plane() == SketchPlane::XZ);
    d.run("sketch.add_rectangle",
          {{"sketch", sketch}, {"from", sketch::Point2{5, 2}}, {"to", sketch::Point2{15, 8}}});
    d.run("feature.add_extrude", {{"sketch", sketch}, {"distance", 3.0}});
    const auto tab = d.last();
    const auto report = d.rebuild();
    REQUIRE(report.ok());
    const auto box = d.kernel->bounds(*report.shapeOf(tab));
    CHECK(box.min.x == Approx(5));
    CHECK(box.max.x == Approx(15));
    CHECK(box.min.y == Approx(-3));  // out of the front, toward -y
    CHECK(box.max.y == Approx(0));
    CHECK(box.min.z == Approx(2));
    CHECK(box.max.z == Approx(8));
}

TEST_CASE("a sketch whose face is gone says so, and works again on a face picked again",
          "[model][sketch][face][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
    const auto block = d.last();
    d.run("feature.add_cylinder", {{"base", kernel::Vec3{20, 10, -1}}, {"radius", 30.0}, {"height", 20.0}});
    const auto big = d.last();
    // An intersect keeps the block's top while the cylinder is big.
    d.run("feature.add_boolean", {{"operation", std::string("intersect")}, {"a", block}, {"b", big}});
    const auto both = d.last();
    d.run("feature.add_sketch", {{"body", both}, {"face", d.faceFacing(both, {0, 0, 1})}});
    const auto sketch = d.last();
    d.run("sketch.add_circle", {{"sketch", sketch}, {"center", sketch::Point2{20, 10}}, {"radius", 3.0}});
    d.run("feature.add_extrude", {{"sketch", sketch}, {"distance", 5.0}});
    const auto boss = d.last();
    REQUIRE(d.rebuild().ok());

    // The cylinder now stops below the top: the top is the cylinder's, not the block's.
    d.run("feature.set_value", {{"feature", big}, {"setting", std::string("height")}, {"value", 8.0}});
    auto report = d.rebuild();
    const auto* r = report.result(sketch);
    REQUIRE(r->status == FeatureStatus::Failed);
    CHECK(r->issues.at(0).code == "sketch.face_missing");
    CHECK(
        r->issues.at(0).message ==
        "Sketch1 can't find the face it's on: an earlier change took it away. Pick a flat face for it again: "
        "pick the sketch, then Sketch > Move to a face.");
    CHECK(report.result(boss)->status == FeatureStatus::Blocked);

    // Put it on the new top: everything builds, as one undo step.
    d.run("feature.set_sketch_face",
          {{"feature", sketch}, {"body", both}, {"face", d.faceFacing(both, {0, 0, 1})}});
    CHECK(d.doc.undoLabel() == "Move Sketch1 to a face");
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.kernel->bounds(*report.shapeOf(boss)).min.z == Approx(7));
    REQUIRE(d.doc.undo());
    CHECK(d.rebuild().result(sketch)->status == FeatureStatus::Failed);
}

TEST_CASE("moving a sketch to a face checks the face", "[model][sketch][face][occt]") {
    Design d;
    d.run("feature.add_cylinder", {{"radius", 5.0}, {"height", 10.0}});
    const auto pin = d.last();
    d.run("feature.add_sketch", {});
    const auto sketch = d.last();
    d.run("feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}});
    const auto later = d.last();
    auto report = d.rebuild();
    const auto& pinNames = *report.result(pin)->names;
    GeometryRef side;
    for (std::size_t i = 0; i < pinNames.faces.size(); ++i) {
        if (pinNames.elements.faces[i].geom == kernel::GeomKind::Cylinder) {
            side = referTo(pinNames, TopoKind::Face, static_cast<int>(i));
        }
    }
    auto issues = d.tryRun("feature.set_sketch_face", {{"feature", sketch}, {"body", pin}, {"face", side}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "sketch.face_curved");
    CHECK(issues[0].message ==
          "A sketch needs a flat face, and that face is curved. Pick a flat face of Cylinder1.");
    issues = d.tryRun("feature.set_sketch_face",
                      {{"feature", sketch}, {"body", later}, {"face", d.faceFacing(later, {0, 0, 1})}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "sketch.face_later");
    issues = d.tryRun("feature.set_sketch_face",
                      {{"feature", pin}, {"body", later}, {"face", d.faceFacing(later, {0, 0, 1})}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "sketch.not_sketch");
}

TEST_CASE("a sketch on a face saves and opens on that face", "[model][sketch][face][occt][file]") {
    Design d;
    const auto [block, boss] = d.blockWithBoss();
    const double before = d.kernel->massProps(*d.rebuild().shapeOf(boss)).volume;
    const auto path = std::filesystem::temp_directory_path() / "sb-test-sketch-face.sb";
    REQUIRE(io::saveFile(d.doc, path).empty());
    auto opened = io::openFile(path);
    std::filesystem::remove(path);
    REQUIRE(opened.ok());
    const auto& sketch = static_cast<const SketchFeature&>(opened.document->featureAt(1));
    REQUIRE(sketch.face());
    CHECK(sketch.face()->body == block);
    CHECK(sketch.face()->face.name == shortId(block.toString()) + "/top");
    CHECK(sketch.face()->face.signature.geom == kernel::GeomKind::Plane);
    CHECK(sketch.plane() == SketchPlane::XY);
    const auto report = opened.document->regenerate(*d.kernel, d.solver.get());
    REQUIRE(report.ok());
    CHECK(d.kernel->massProps(*report.shapeOf(boss)).volume == Approx(before));
    CHECK(d.kernel->bounds(*report.shapeOf(boss)).min.z == Approx(10));
}
