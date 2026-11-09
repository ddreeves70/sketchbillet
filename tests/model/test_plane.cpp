// Plane steps (construction planes): moved out and tilted from a base plane, a
// face or another plane; sketches on them; following the face; messages; saving.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>

#include "sb/io/SketchBilletFile.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/ConstructionPlane.h"
#include "sb/model/Document.h"
#include "sb/model/Naming.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;
using kernel::TopoKind;

namespace {

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

    // A 10 x 10 mm square sketched on `on`, extruded 5 mm: a block to show where the plane is.
    FeatureId blockOn(const FeatureId& on) {
        run("feature.add_sketch", {{"on", on}});
        const auto sketch = last();
        run("sketch.add_rectangle",
            {{"sketch", sketch}, {"from", sketch::Point2{0, 0}}, {"to", sketch::Point2{10, 10}}});
        run("feature.add_extrude", {{"sketch", sketch}, {"distance", 5.0}});
        return last();
    }
    GeometryRef element(const FeatureId& body, TopoKind kind, kernel::Vec3 at) {
        const auto report = rebuild();
        const auto& names = *report.result(body)->names;
        const auto& list = kind == TopoKind::Face ? names.elements.faces : names.elements.edges;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& c = list[i].center;
            if (std::abs(c.x - at.x) < 1e-9 && std::abs(c.y - at.y) < 1e-9 && std::abs(c.z - at.z) < 1e-9)
                return referTo(names, kind, static_cast<int>(i));
        }
        FAIL("nothing there");
        return {};
    }
};

kernel::Vec3 normalOf(const PlaneAxes& a) {
    return {a.x.y * a.y.z - a.x.z * a.y.y, a.x.z * a.y.x - a.x.x * a.y.z, a.x.x * a.y.y - a.x.y * a.y.x};
}

}  // namespace

TEST_CASE("a plane above the ground: sketch on it, and the sketch moves with it", "[model][plane][occt]") {
    Design d;
    d.run("feature.add_plane", {{"offset", 20.0}});
    const auto plane = d.last();
    CHECK(d.doc.find(plane)->name() == "Plane1");
    CHECK(d.doc.undoLabel() == "Add Plane1");
    const auto block = d.blockOn(plane);

    auto report = d.rebuild();
    REQUIRE(report.ok());
    const auto* r = report.result(plane);
    REQUIRE(r->plane);
    CHECK(r->plane->origin.z == 20);
    CHECK(normalOf(*r->plane).z == 1);
    CHECK(report.bodies() == std::vector<FeatureId>{block});  // a plane isn't a body
    auto b = d.kernel->bounds(*report.shapeOf(block));
    CHECK(b.min.z == Approx(20).margin(1e-6));
    CHECK(b.max.z == Approx(25).margin(1e-6));

    d.run("feature.set_value", {{"feature", plane}, {"setting", std::string("offset")}, {"value", -8.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    b = d.kernel->bounds(*report.shapeOf(block));
    CHECK(b.min.z == Approx(-8).margin(1e-6));
}

TEST_CASE("a plane tilted about its x or y", "[model][plane][occt]") {
    Design d;
    SECTION("30 degrees about x, from the ground: it faces up and toward the front") {
        d.run("feature.add_plane", {{"angle", 30.0}});
        const auto report = d.rebuild();
        const auto n = normalOf(*report.result(d.last())->plane);
        CHECK(n.x == Approx(0).margin(1e-12));
        CHECK(n.y == Approx(-0.5));
        CHECK(n.z == Approx(std::sqrt(3.0) / 2));
    }
    SECTION("90 degrees about y, from the front plane (XZ), then 15 mm out: it faces the right") {
        d.run("feature.add_plane",
              {{"plane", std::string("xz")}, {"angle", 90.0}, {"hinge", std::string("y")}, {"offset", 15.0}});
        const auto report = d.rebuild();
        const auto& a = *report.result(d.last())->plane;
        const auto n = normalOf(a);
        CHECK(std::abs(n.x) == Approx(1));
        CHECK(a.origin.x == Approx(15 * n.x));
    }
}

TEST_CASE("a plane off a face follows the face", "[model][plane][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
    const auto box = d.last();
    d.run("feature.add_plane",
          {{"body", box}, {"face", d.element(box, TopoKind::Face, {20, 10, 10})}, {"offset", 5.0}});
    const auto plane = d.last();
    auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.result(plane)->plane->origin.z == Approx(15));
    CHECK(report.bodies() == std::vector<FeatureId>{box});  // the box is only looked at
    d.run("feature.set_value", {{"feature", box}, {"setting", std::string("size_z")}, {"value", 30.0}});
    report = d.rebuild();
    CHECK(report.result(plane)->plane->origin.z == Approx(35));

    // Another plane, 10 mm on from that one.
    d.run("feature.add_plane", {{"from", plane}, {"offset", 10.0}});
    report = d.rebuild();
    CHECK(report.result(d.last())->plane->origin.z == Approx(45));
}

TEST_CASE("a plane tilted about an edge of a face, like a lid on a hinge", "[model][plane][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
    const auto box = d.last();
    // The top, tilted 45 degrees about its front edge (y = 0, z = 10).
    d.run("feature.add_plane", {{"body", box},
                                {"face", d.element(box, TopoKind::Face, {20, 10, 10})},
                                {"edge", std::vector{d.element(box, TopoKind::Edge, {20, 0, 10})}},
                                {"angle", 45.0}});
    const auto report = d.rebuild();
    REQUIRE(report.ok());
    const auto& a = *report.result(d.last())->plane;
    const auto n = normalOf(a);
    CHECK(std::abs(n.y) == Approx(std::sqrt(0.5)));
    CHECK(n.z == Approx(std::sqrt(0.5)));
    // The hinge stays put: the front edge is still on the plane.
    const kernel::Vec3 onEdge{37, 0, 10};
    const double off =
        (onEdge.x - a.origin.x) * n.x + (onEdge.y - a.origin.y) * n.y + (onEdge.z - a.origin.z) * n.z;
    CHECK(off == Approx(0).margin(1e-9));
}

TEST_CASE("planes explain what's wrong, and how to fix it", "[model][plane][occt]") {
    Design d;
    SECTION("a curved face") {
        d.run("feature.add_cylinder", {{"radius", 5.0}, {"height", 10.0}});
        const auto rod = d.last();
        const auto report0 = d.rebuild();
        const auto& names = *report0.result(rod)->names;
        int side = -1;
        for (std::size_t i = 0; i < names.elements.faces.size(); ++i)
            if (names.elements.faces[i].geom == kernel::GeomKind::Cylinder) side = static_cast<int>(i);
        d.run("feature.add_plane", {{"body", rod}, {"face", referTo(names, TopoKind::Face, side)}});
        const auto report = d.rebuild();
        CHECK(report.result(d.last())->status == FeatureStatus::Failed);
        CHECK(report.result(d.last())->issues.at(0).code == "plane.face_curved");
    }
    SECTION("an edge that isn't on the face") {
        d.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
        const auto box = d.last();
        d.run("feature.add_plane", {{"body", box},
                                    {"face", d.element(box, TopoKind::Face, {20, 10, 10})},
                                    {"edge", std::vector{d.element(box, TopoKind::Edge, {20, 0, 0})}},
                                    {"angle", 45.0}});
        CHECK(d.rebuild().result(d.last())->issues.at(0).code == "plane.hinge");
    }
    SECTION("sketching on something that isn't a plane") {
        d.run("feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}});
        const auto issues = d.tryRun("feature.add_sketch", {{"on", d.last()}});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].message ==
              "Box1 isn't a construction plane. Use on= with a Plane step, like on=Plane1.");
    }
    SECTION("a sketch on a plane that's turned off") {
        d.run("feature.add_plane", {{"offset", 5.0}});
        const auto plane = d.last();
        d.run("feature.add_sketch", {{"on", plane}});
        d.run("feature.suppress", {{"steps", std::vector{plane}}});
        const auto report = d.rebuild();
        const auto* r = report.result(d.last());
        CHECK(r->status == FeatureStatus::Blocked);
        CHECK(r->issues.at(0).message ==
              "This step uses Plane1, which is turned off. Turn it back on, or change "
              "this step.");
    }
}

TEST_CASE("planes, and sketches on them, save and open", "[model][plane][occt][file]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
    const auto box = d.last();
    d.run("feature.add_plane", {{"body", box},
                                {"face", d.element(box, TopoKind::Face, {20, 10, 10})},
                                {"edge", std::vector{d.element(box, TopoKind::Edge, {20, 0, 10})}},
                                {"angle", 30.0}});
    const auto hinged = d.last();
    d.run("feature.add_plane", {{"from", hinged}, {"offset", 4.0}, {"hinge", std::string("y")}});
    const auto onward = d.last();
    d.run("feature.add_plane", {{"plane", std::string("yz")}, {"offset", -3.0}});
    const auto block = d.blockOn(onward);
    const auto before = d.rebuild();
    REQUIRE(before.ok());
    const double volume = d.kernel->massProps(*before.shapeOf(block)).volume;
    const auto centre = d.kernel->bounds(*before.shapeOf(block));

    const auto path = std::filesystem::temp_directory_path() / "sb-test-plane.sb";
    REQUIRE(io::saveFile(d.doc, path).empty());
    auto opened = io::openFile(path);
    std::filesystem::remove(path);
    REQUIRE(opened.ok());
    const auto& a = static_cast<const PlaneFeature&>(*opened.document->find(hinged));
    CHECK(a.from().kind == PlaneFrom::Kind::Face);
    CHECK(a.hinge().kind == PlaneHinge::Kind::Edge);
    CHECK(a.angle() == Approx(std::numbers::pi / 6));
    const auto& b = static_cast<const PlaneFeature&>(*opened.document->find(onward));
    CHECK(b.from().kind == PlaneFrom::Kind::Plane);
    CHECK(b.from().plane == hinged);
    CHECK(b.hinge().kind == PlaneHinge::Kind::Y);
    CHECK(b.offset() == 4);
    const auto& sketch = static_cast<const SketchFeature&>(opened.document->featureAt(4));
    CHECK(sketch.onPlane() == onward);
    const auto after = opened.document->regenerate(*d.kernel, d.solver.get());
    REQUIRE(after.ok());
    CHECK(d.kernel->massProps(*after.shapeOf(block)).volume == Approx(volume));
    CHECK(d.kernel->bounds(*after.shapeOf(block)).max.z == Approx(centre.max.z));
    CHECK(volume == Approx(500));
}
