// The Revolve step: a sketch's closed shapes turned about a line in the sketch.
// Which line (an axis or a line of the sketch), how far, which way, joining and
// cutting, names that last through changes, and saving.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>

#include "sb/io/SketchBilletFile.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Document.h"
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

    // Sketch1 on the ground with a rectangle from (x0, y0) to (x1, y1).
    FeatureId sketchWithRectangle(double x0, double y0, double x1, double y1) {
        run("feature.add_sketch", {});
        const auto sketch = last();
        run("sketch.add_rectangle",
            {{"sketch", sketch}, {"from", sketch::Point2{x0, y0}}, {"to", sketch::Point2{x1, y1}}});
        return sketch;
    }
    const sketch::Entity& entity(const FeatureId& sketch, std::string_view name) {
        const auto& s = static_cast<const SketchFeature&>(*doc.find(sketch)).sketch();
        const auto* e = s.findEntity(std::string(name));
        REQUIRE(e);
        return *e;
    }
};

}  // namespace

TEST_CASE("Revolve turns a sketch's shape about its y axis: a full turn makes a tube",
          "[model][revolve][occt]") {
    Design d;
    const auto sketch = d.sketchWithRectangle(10, 0, 15, 20);
    d.run("feature.add_revolve", {{"sketch", sketch}});
    const auto tube = d.last();
    const auto& step = static_cast<const RevolveFeature&>(*d.doc.find(tube));
    CHECK(step.name() == "Revolve1");
    CHECK(step.axis().kind == RevolveAxis::Kind::Y);  // no helper line: the y axis
    CHECK(step.angle() == Approx(2 * kPi));
    CHECK(step.operation() == ExtrudeOp::NewBody);
    CHECK(d.doc.undoLabel() == "Add Revolve1");

    const auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector<FeatureId>{tube});
    CHECK(d.volume(report, tube) == Approx(kPi * (225 - 100) * 20));
    CHECK(d.kernel->validate(*report.shapeOf(tube)).empty());
    // Four faces, each named after the line it came from; no ends.
    const auto& names = *report.result(tube)->names;
    REQUIRE(names.faces.size() == 4);
    for (const auto& n : names.faces) CHECK(n.find("/side:") != std::string::npos);
    const auto& line = d.entity(sketch, "Line2");  // the outside: x = 15
    const auto outside =
        names.find(TopoKind::Face, shortId(tube.toString()) + "/side:" + shortId(line.id.toString()));
    REQUIRE(outside >= 0);
    CHECK(describeElement(d.doc, TopoKind::Face, names.faces[static_cast<std::size_t>(outside)]) ==
          "side from Line2 of Sketch1 (Revolve1)");
}

TEST_CASE("Revolve's axis: x, a line by name, or the only helper line", "[model][revolve][occt]") {
    Design d;

    SECTION("x: the rectangle above the x axis makes a tube along x") {
        const auto sketch = d.sketchWithRectangle(0, 10, 20, 15);
        d.run("feature.add_revolve", {{"sketch", sketch}, {"axis", std::string("x")}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, d.last()) == Approx(kPi * (225 - 100) * 20));
        const auto b = d.kernel->bounds(*report.shapeOf(d.last()));
        CHECK(b.max.x == Approx(20).margin(1e-6));
        CHECK(b.max.z == Approx(15).margin(1e-6));
    }
    SECTION("a side of the shape: turning a rectangle about its own side makes a solid rod") {
        const auto sketch = d.sketchWithRectangle(10, 0, 15, 20);
        d.run("feature.add_revolve", {{"sketch", sketch}, {"axis", std::string("Line4")}});  // x = 10
        const auto& step = static_cast<const RevolveFeature&>(*d.doc.find(d.last()));
        CHECK(step.axis() == RevolveAxis{RevolveAxis::Kind::Line, d.entity(sketch, "Line4").id});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, d.last()) == Approx(kPi * 25 * 20));
    }
    SECTION("the sketch's only helper line, when there's no axis=") {
        const auto sketch = d.sketchWithRectangle(10, 0, 15, 20);
        d.run("sketch.add_line", {{"sketch", sketch},
                                  {"from", sketch::Point2{5, 0}},
                                  {"to", sketch::Point2{5, 10}},
                                  {"construction", true}});
        d.run("feature.add_revolve", {{"sketch", sketch}});
        const auto& step = static_cast<const RevolveFeature&>(*d.doc.find(d.last()));
        CHECK(step.axis() == RevolveAxis{RevolveAxis::Kind::Line, d.entity(sketch, "Line5").id});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, d.last()) == Approx(kPi * (100 - 25) * 20));  // 5 to 10 mm from the line
    }
    SECTION("a line that isn't there is refused, and nothing is added") {
        const auto sketch = d.sketchWithRectangle(10, 0, 15, 20);
        const auto issues =
            d.tryRun("feature.add_revolve", {{"sketch", sketch}, {"axis", std::string("Line9")}});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].code == "revolve.axis");
        CHECK(
            issues[0].message ==
            "Sketch1 has no line called Line9. Use x or y (the sketch's axes), or a line of the sketch, like "
            "axis=Line1.");
        CHECK(d.doc.featureCount() == 1);
    }
}

TEST_CASE("Revolve's angle and side: part of a turn, flipped, or both ways", "[model][revolve][occt]") {
    Design d;
    const auto sketch = d.sketchWithRectangle(10, 0, 20, 5);
    const double quarter = 50 * 15 * kPi / 2;  // area x the path of its middle

    SECTION("one way: +x turns toward -z, with a start and an end face") {
        d.run("feature.add_revolve", {{"sketch", sketch}, {"angle", 90.0}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, d.last()) == Approx(quarter));
        const auto b = d.kernel->bounds(*report.shapeOf(d.last()));
        CHECK(b.min.z == Approx(-20).margin(1e-6));
        CHECK(b.max.z == Approx(0).margin(1e-6));
        const auto& names = *report.result(d.last())->names;
        const auto me = shortId(d.last().toString());
        int ends = 0;
        for (const auto& n : names.faces)
            ends += n.starts_with(me + "/start:") || n.starts_with(me + "/end:");
        CHECK(ends == 2);
    }
    SECTION("flip: toward +z instead") {
        d.run("feature.add_revolve", {{"sketch", sketch}, {"angle", 90.0}, {"side", std::string("flip")}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        const auto b = d.kernel->bounds(*report.shapeOf(d.last()));
        CHECK(b.min.z == Approx(0).margin(1e-6));
        CHECK(b.max.z == Approx(20).margin(1e-6));
    }
    SECTION("both: half each way, so it's even about the sketch") {
        d.run("feature.add_revolve", {{"sketch", sketch}, {"angle", 90.0}, {"side", std::string("both")}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, d.last()) == Approx(quarter));
        const auto b = d.kernel->bounds(*report.shapeOf(d.last()));
        CHECK(b.min.z == Approx(-20 * std::sin(kPi / 4)).margin(1e-6));
        CHECK(b.max.z == Approx(20 * std::sin(kPi / 4)).margin(1e-6));
        CHECK(b.max.x == Approx(20).margin(1e-6));
    }
    SECTION("the angle changes in degrees, and the faces keep their names") {
        d.run("feature.add_revolve", {{"sketch", sketch}, {"angle", 90.0}});
        const auto step = d.last();
        auto before = *d.rebuild().result(step)->names;
        d.run("feature.set_value", {{"feature", step}, {"setting", std::string("angle")}, {"value", 180.0}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(static_cast<const RevolveFeature&>(*d.doc.find(step)).angle() == Approx(kPi));
        CHECK(d.volume(report, step) == Approx(quarter * 2));
        auto after = report.result(step)->names->faces;
        auto old = before.faces;
        std::sort(old.begin(), old.end());
        std::sort(after.begin(), after.end());
        CHECK(after == old);
    }
}

TEST_CASE("Revolve explains what's wrong, and how to fix it", "[model][revolve][occt]") {
    Design d;

    SECTION("a shape that crosses the axis") {
        const auto sketch = d.sketchWithRectangle(-5, 0, 5, 10);
        d.run("feature.add_revolve", {{"sketch", sketch}});
        const auto report = d.rebuild();
        const auto* r = report.result(d.last());
        REQUIRE(r->status == FeatureStatus::Failed);
        CHECK(r->issues.at(0).code == "kernel.revolve.crosses");
    }
    SECTION("its axis line was deleted") {
        const auto sketch = d.sketchWithRectangle(10, 0, 15, 20);
        d.run("sketch.add_line", {{"sketch", sketch},
                                  {"from", sketch::Point2{0, 0}},
                                  {"to", sketch::Point2{0, 10}},
                                  {"construction", true}});
        d.run("feature.add_revolve", {{"sketch", sketch}});
        REQUIRE(d.rebuild().ok());
        d.run("sketch.delete", {{"sketch", sketch}, {"item", std::string("Line5")}});
        const auto report = d.rebuild();
        const auto* r = report.result(d.last());
        REQUIRE(r->status == FeatureStatus::Failed);
        CHECK(r->issues.at(0).code == "revolve.no_axis");
        CHECK(r->issues.at(0).message ==
              "Revolve1 can't find the line it turns about: it was deleted from the sketch. Pick an axis for "
              "it "
              "in Properties.");
    }
    SECTION("no angle") {
        const auto sketch = d.sketchWithRectangle(10, 0, 15, 20);
        d.run("feature.add_revolve", {{"sketch", sketch}, {"angle", 0.0}});
        const auto report = d.rebuild();
        CHECK(report.result(d.last())->issues.at(0).code == "revolve.angle");
    }
}

TEST_CASE("Revolve cuts a groove round a shaft", "[model][revolve][cut][occt]") {
    Design d;
    // A 20 mm rod along y, radius 10; a groove 2 mm deep and 4 mm wide, 8 mm up.
    d.run("feature.add_cylinder", {{"base", kernel::Vec3{0, 0, 0}},
                                   {"axis", kernel::Vec3{0, 1, 0}},
                                   {"radius", 10.0},
                                   {"height", 20.0}});
    const auto rod = d.last();
    const auto sketch = d.sketchWithRectangle(8, 8, 12, 12);
    d.run("feature.add_revolve", {{"sketch", sketch}, {"operation", std::string("cut")}, {"body", rod}});
    const auto groove = d.last();
    const auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector<FeatureId>{groove});
    CHECK(d.volume(report, groove) == Approx(kPi * 100 * 20 - kPi * (100 - 64) * 4));
    CHECK(d.kernel->validate(*report.shapeOf(groove)).empty());

    // Without a body, a cut says what to do.
    const auto issues =
        d.tryRun("feature.add_revolve", {{"sketch", sketch}, {"operation", std::string("cut")}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "revolve.no_body");
}

TEST_CASE("Revolve saves and opens with its axis, angle, side and body", "[model][revolve][occt][file]") {
    Design d;
    d.run("feature.add_box", {{"corner", kernel::Vec3{-30, 0, -30}}, {"size", kernel::Vec3{60, 20, 60}}});
    const auto block = d.last();
    const auto sketch = d.sketchWithRectangle(10, 5, 15, 10);
    d.run("feature.add_revolve", {{"sketch", sketch},
                                  {"axis", std::string("Line4")},
                                  {"angle", 120.0},
                                  {"side", std::string("both")},
                                  {"operation", std::string("cut")},
                                  {"body", block}});
    const auto cut = d.last();
    const double before = d.volume(d.rebuild(), cut);
    const auto path = std::filesystem::temp_directory_path() / "sb-test-revolve.sb";
    REQUIRE(io::saveFile(d.doc, path).empty());
    auto opened = io::openFile(path);
    std::filesystem::remove(path);
    REQUIRE(opened.ok());
    const auto& step = static_cast<const RevolveFeature&>(*opened.document->find(cut));
    CHECK(step.axis() == RevolveAxis{RevolveAxis::Kind::Line, d.entity(sketch, "Line4").id});
    CHECK(step.angle() == Approx(120 * kPi / 180));
    CHECK(step.side() == ExtrudeSide::Both);
    CHECK(step.operation() == ExtrudeOp::Cut);
    CHECK(step.body() == block);
    const auto report = opened.document->regenerate(*d.kernel, d.solver.get());
    REQUIRE(report.ok());
    CHECK(d.kernel->massProps(*report.shapeOf(cut)).volume == Approx(before));
    // A 5 mm square turned 120 degrees about its own side: its area times the
    // path of its middle, 2.5 mm out.
    CHECK(before == Approx(60 * 20 * 60 - 25 * 2.5 * 2 * kPi / 3));
}
