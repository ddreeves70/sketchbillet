// The Mirror step: a mirror image of holes, joins and cuts, or of a whole body,
// across a plane (YZ, XZ or XY, moved by an offset) or a flat face. Every volume
// and position is worked out by hand.

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
#include "sb/model/Mirror.h"
#include "sb/model/Naming.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;
using kernel::TopoKind;

namespace {

constexpr double kPi = std::numbers::pi;

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
    double volume(const RebuildReport& r, const FeatureId& id) {
        return kernel->massProps(*r.shapeOf(id)).volume;
    }
    auto bounds(const RebuildReport& r, const FeatureId& id) { return kernel->bounds(*r.shapeOf(id)); }

    // A face of a step's body, by which way it faces.
    GeometryRef faceFacing(const FeatureId& body, kernel::Vec3 d) {
        const auto report = rebuild();
        const auto& names = *report.result(body)->names;
        for (std::size_t i = 0; i < names.elements.faces.size(); ++i) {
            const auto& f = names.elements.faces[i];
            if (f.direction.x * d.x + f.direction.y * d.y + f.direction.z * d.z > 0.999)
                return referTo(names, TopoKind::Face, static_cast<int>(i));
        }
        FAIL("no face that way");
        return {};
    }
};

}  // namespace

TEST_CASE("Mirror a whole body: half a part becomes the whole", "[model][mirror][occt]") {
    Design d;
    SECTION("touching the plane: one solid, twice as long") {
        d.run("feature.add_box", {{"size", kernel::Vec3{20, 10, 5}}});
        d.run("feature.add_mirror", {{"body", d.last()}});
        const auto mirror = d.last();
        CHECK(d.doc.undoLabel() == "Add Mirror1");
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(report.bodies() == std::vector<FeatureId>{mirror});
        CHECK(d.volume(report, mirror) == Approx(2000));
        CHECK(d.kernel->topology(*report.shapeOf(mirror)).solids == 1);
        CHECK(d.bounds(report, mirror).min.x == Approx(-20).margin(1e-6));
        CHECK(d.bounds(report, mirror).max.x == Approx(20).margin(1e-6));
    }
    SECTION("away from the plane: two pieces of one body") {
        d.run("feature.add_box", {{"corner", kernel::Vec3{5, 0, 0}}, {"size", kernel::Vec3{10, 10, 10}}});
        d.run("feature.add_mirror", {{"steps", std::vector{d.last()}}});  // one body step: the body
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, d.last()) == Approx(2000));
        CHECK(d.kernel->topology(*report.shapeOf(d.last())).solids == 2);
        CHECK(d.bounds(report, d.last()).min.x == Approx(-15).margin(1e-6));
    }
    SECTION("across XY moved up 10 mm: a stack") {
        d.run("feature.add_box", {{"size", kernel::Vec3{10, 10, 10}}});
        d.run("feature.add_mirror", {{"body", d.last()}, {"plane", std::string("xy")}, {"offset", 10.0}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.bounds(report, d.last()).max.z == Approx(20).margin(1e-6));
        CHECK(d.kernel->topology(*report.shapeOf(d.last())).solids == 1);
    }
    SECTION("across one of its own faces") {
        d.run("feature.add_box", {{"corner", kernel::Vec3{5, 0, 0}}, {"size", kernel::Vec3{10, 10, 10}}});
        const auto box = d.last();
        d.run("feature.add_mirror", {{"body", box}, {"face", d.faceFacing(box, {1, 0, 0})}});  // x = 15
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.bounds(report, d.last()).max.x == Approx(25).margin(1e-6));
        CHECK(d.kernel->topology(*report.shapeOf(d.last())).solids == 1);
    }
}

TEST_CASE("Mirror a hole: it's matched on the other side, and follows the original",
          "[model][mirror][occt]") {
    Design d;
    d.run("feature.add_box", {{"corner", kernel::Vec3{-30, 0, 0}}, {"size", kernel::Vec3{60, 20, 5}}});
    const auto plate = d.last();
    const auto report0 = d.rebuild();
    const auto& names0 = *report0.result(plate)->names;
    int top = -1;
    for (std::size_t i = 0; i < names0.elements.faces.size(); ++i)
        if (names0.elements.faces[i].direction.z > 0.999) top = static_cast<int>(i);
    d.run("feature.add_sketch", {{"body", plate}, {"face", referTo(names0, TopoKind::Face, top)}});
    const auto sketch = d.last();
    d.run("sketch.add_point", {{"sketch", sketch}, {"at", sketch::Point2{20, 10}}});
    d.run("feature.add_hole", {{"sketch", sketch}});
    const auto hole = d.last();
    d.run("feature.add_mirror", {{"steps", std::vector{hole}}});
    const auto mirror = d.last();
    CHECK(static_cast<const MirrorFeature&>(*d.doc.find(mirror)).body() == hole);

    auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, mirror) == Approx(6000 - 2 * kPi * 1.7 * 1.7 * 5));
    // The mirror image of the hole's wall is at x = -20, and says what it is.
    const auto& names = *report.result(mirror)->names;
    int image = -1;
    for (std::size_t i = 0; i < names.faces.size(); ++i)
        if (names.faces[i].find("/mirror:") != std::string::npos &&
            names.faces[i].find("/wall:") != std::string::npos)
            image = static_cast<int>(i);
    REQUIRE(image >= 0);
    CHECK(names.elements.faces[static_cast<std::size_t>(image)].center.x == Approx(-20));
    CHECK(describeElement(d.doc, TopoKind::Face, names.faces[static_cast<std::size_t>(image)]) ==
          "mirror image (Mirror1) of wall of Hole1 at Point1");

    // A bigger hole: its mirror image is bigger too.
    d.run("feature.set_value", {{"feature", hole}, {"setting", std::string("diameter")}, {"value", 6.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, mirror) == Approx(6000 - 2 * kPi * 9 * 5));
}

TEST_CASE("Mirror a peg across XZ, moved by an offset", "[model][mirror][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
    const auto block = d.last();
    const auto report0 = d.rebuild();
    const auto& names0 = *report0.result(block)->names;
    int top = -1;
    for (std::size_t i = 0; i < names0.elements.faces.size(); ++i)
        if (names0.elements.faces[i].direction.z > 0.999) top = static_cast<int>(i);
    d.run("feature.add_sketch", {{"body", block}, {"face", referTo(names0, TopoKind::Face, top)}});
    const auto sketch = d.last();
    d.run("sketch.add_circle", {{"sketch", sketch}, {"center", sketch::Point2{20, 5}}, {"radius", 2.0}});
    d.run("feature.add_extrude", {{"sketch", sketch}, {"distance", 6.0}});
    const auto peg = d.last();
    // Across the plane y = 10: the peg at y = 5 is matched at y = 15.
    d.run("feature.add_mirror",
          {{"steps", std::vector{peg}}, {"plane", std::string("xz")}, {"offset", 10.0}});
    auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, d.last()) == Approx(8000 + 2 * kPi * 4 * 6));
    const auto b = d.bounds(report, d.last());
    CHECK(b.max.z == Approx(16).margin(1e-6));
    // The offset changes like any other number: across y = 15, the image is off the block, in the air.
    d.run("feature.set_value", {{"feature", d.last()}, {"setting", std::string("offset")}, {"value", 20.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, d.last()) == Approx(8000 + 2 * kPi * 4 * 6));  // a peg floating at y = 35
    CHECK(d.bounds(report, d.last()).max.y == Approx(37).margin(1e-6));
}

TEST_CASE("Mirror explains what's wrong, and how to fix it", "[model][mirror][occt]") {
    Design d;
    SECTION("nothing to mirror") {
        const auto issues = d.tryRun("feature.add_mirror", {});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].message ==
              "Say what to mirror: steps=, like steps=Hole1, or body=, for a whole body.");
    }
    SECTION("a sketch") {
        d.run("feature.add_sketch", {});
        const auto issues = d.tryRun("feature.add_mirror", {{"steps", std::vector{d.last()}}});
        REQUIRE(issues.size() == 1);
        CHECK(
            issues[0].message ==
            "Sketch1 can't be mirrored: a mirror of steps repeats holes, and extrudes and revolves that join "
            "or cut. For a mirror image of a whole body, use body= instead.");
    }
    SECTION("a curved face") {
        d.run("feature.add_cylinder", {{"radius", 5.0}, {"height", 10.0}});
        const auto rod = d.last();
        const auto report0 = d.rebuild();
        const auto& names = *report0.result(rod)->names;
        int side = -1;
        for (std::size_t i = 0; i < names.elements.faces.size(); ++i)
            if (names.elements.faces[i].geom == kernel::GeomKind::Cylinder) side = static_cast<int>(i);
        d.run("feature.add_mirror", {{"body", rod}, {"face", referTo(names, TopoKind::Face, side)}});
        const auto report = d.rebuild();
        CHECK(report.result(d.last())->issues.at(0).code == "mirror.face_curved");
    }
}

TEST_CASE("Mirror saves and opens, across a plane or a face", "[model][mirror][occt][file]") {
    Design d;
    d.run("feature.add_box", {{"corner", kernel::Vec3{5, 0, 0}}, {"size", kernel::Vec3{10, 10, 10}}});
    const auto box = d.last();
    d.run("feature.add_mirror", {{"body", box}, {"plane", std::string("xz")}, {"offset", -3.0}});
    const auto first = d.last();
    d.run("feature.add_mirror", {{"body", first}, {"face", d.faceFacing(first, {1, 0, 0})}});
    const auto second = d.last();
    const double before = d.volume(d.rebuild(), second);
    const auto path = std::filesystem::temp_directory_path() / "sb-test-mirror.sb";
    REQUIRE(io::saveFile(d.doc, path).empty());
    auto opened = io::openFile(path);
    std::filesystem::remove(path);
    REQUIRE(opened.ok());
    const auto& a = static_cast<const MirrorFeature&>(*opened.document->find(first));
    CHECK(a.plane().kind == MirrorPlane::Kind::XZ);
    CHECK(a.plane().offset == -3.0);
    CHECK(a.body() == box);
    const auto& b = static_cast<const MirrorFeature&>(*opened.document->find(second));
    CHECK(b.plane().kind == MirrorPlane::Kind::Face);
    const auto report = opened.document->regenerate(*d.kernel, d.solver.get());
    REQUIRE(report.ok());
    CHECK(d.kernel->massProps(*report.shapeOf(second)).volume == Approx(before));
    CHECK(before == Approx(4000));  // the box, across y = -3, then both across x = 15
}
