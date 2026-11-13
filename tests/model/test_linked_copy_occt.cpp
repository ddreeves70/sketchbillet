// Linked copies with real geometry: a copy follows a change to its original's
// sketch, joins to it, and has faces of its own to sketch on.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <numbers>
#include <string>

#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/model/Naming.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;

namespace {

struct Design {
    Document doc;
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();
    std::unique_ptr<sketch::ISketchSolver> solver = sketch::makePlaneGcsSolver();

    void run(std::string_view name, Args args) {
        auto made = CommandRegistry::builtin().create(name, args, doc);
        INFO(name << ": " << (made.ok() ? std::string() : made.issues.front().message));
        REQUIRE(made.ok());
        const auto issues = doc.execute(std::move(made.command));
        INFO(name << ": " << (issues.empty() ? std::string() : issues.front().message));
        REQUIRE(issues.empty());
    }
    FeatureId id(const std::string& name) const {
        for (std::size_t i = 0; i < doc.featureCount(); ++i)
            if (doc.featureAt(i).name() == name) return doc.featureAt(i).id();
        FAIL("no step " << name);
        return {};
    }
    FeatureId last() const { return doc.featureAt(doc.featureCount() - 1).id(); }
    RebuildReport rebuild() { return doc.regenerate(*kernel, solver.get()); }
    double volume(const RebuildReport& r, const std::string& name) {
        const auto* shape = r.shapeOf(id(name));
        REQUIRE(shape);
        return kernel->massProps(*shape).volume;
    }

    // A plate 40 mm wide (Width), 10 mm deep and 4 mm thick, and a linked copy
    // of it turned a quarter turn about the plate's back bottom edge, so it hangs
    // down from there and the two make an L.
    Design() {
        run("feature.add_sketch", {{"plane", std::string("xy")}});
        const auto sketch = last();
        run("sketch.add_rectangle", {{"sketch", sketch}, {"from", sketch::Point2{0, 0}}, {"to", sketch::Point2{40, 10}}});
        run("sketch.constrain", {{"sketch", sketch}, {"kind", std::string("fix")}, {"on", std::string("Line1.start")}});
        run("sketch.constrain", {{"sketch", sketch},
                                 {"kind", std::string("length")},
                                 {"on", std::string("Line1")},
                                 {"value", 40.0},
                                 {"name", std::string("Width")}});
        run("sketch.constrain", {{"sketch", sketch}, {"kind", std::string("length")}, {"on", std::string("Line2")}, {"value", 10.0}});
        run("feature.add_extrude", {{"sketch", sketch}, {"distance", 4.0}, {"name", std::string("Plate")}});
        run("feature.add_move", {{"body", id("Plate")},
                                 {"turn", 90.0},
                                 {"axis", kernel::Vec3{1, 0, 0}},
                                 {"about", kernel::Vec3{0, 10, 0}},
                                 {"copy", true}});
    }
};

}  // namespace

TEST_CASE("A linked copy follows its original's sketch, and joins to it", "[model][move][linked][occt]") {
    Design d;
    auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector{d.id("Plate"), d.id("Copy1")});
    CHECK(d.volume(report, "Copy1") == Approx(40 * 10 * 4));
    // Hanging down: the copy is 40 mm wide, 4 mm deep and 10 mm tall.
    const auto box = d.kernel->bounds(*report.shapeOf(d.id("Copy1")));
    CHECK(box.max.z - box.min.z == Approx(10));
    CHECK(box.max.y - box.min.y == Approx(4));

    d.run("sketch.set_value", {{"sketch", d.id("Sketch1")}, {"constraint", std::string("Width")}, {"value", 55.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, "Copy1") == Approx(55 * 10 * 4));

    // The original and its copy, joined into an L: they share no volume.
    d.run("feature.add_boolean", {{"operation", std::string("union")},
                                  {"a", d.id("Plate")},
                                  {"b", d.id("Copy1")},
                                  {"name", std::string("Angle")}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector{d.id("Angle")});
    CHECK(d.volume(report, "Angle") == Approx(2 * 55 * 10 * 4));
    CHECK(d.kernel->validate(*report.shapeOf(d.id("Angle"))).empty());
}

TEST_CASE("A linked copy's faces can be sketched on, apart from its original's", "[model][move][linked][occt]") {
    Design d;
    auto report = d.rebuild();
    // The copy's face that looks forward (-y).
    const auto& names = *report.result(d.id("Copy1"))->names;
    int front = -1;
    for (std::size_t i = 0; i < names.elements.faces.size(); ++i) {
        const auto& f = names.elements.faces[i];
        if (f.geom == kernel::GeomKind::Plane && f.direction.y < -0.999) front = static_cast<int>(i);
    }
    REQUIRE(front >= 0);
    d.run("feature.add_sketch",
          {{"body", d.id("Copy1")}, {"face", referTo(names, kernel::TopoKind::Face, front)}});
    d.run("sketch.add_point", {{"sketch", d.last()}, {"at", sketch::Point2{20, -5}}});
    d.run("feature.add_hole", {{"sketch", d.last()}, {"diameter", 3.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    // The hole goes through the copy (4 mm), not the original, which keeps its volume.
    CHECK(d.volume(report, "Hole1") == Approx(40 * 10 * 4 - std::numbers::pi * 1.5 * 1.5 * 4));
    CHECK(d.volume(report, "Plate") == Approx(40 * 10 * 4));
    CHECK(report.bodies() == std::vector{d.id("Plate"), d.id("Hole1")});

    // A change to the original still reaches the copy's hole.
    d.run("sketch.set_value", {{"sketch", d.id("Sketch1")}, {"constraint", std::string("Width")}, {"value", 50.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, "Hole1") == Approx(50 * 10 * 4 - std::numbers::pi * 1.5 * 1.5 * 4));
}
