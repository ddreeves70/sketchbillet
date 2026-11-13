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
        run("sketch.add_rectangle",
            {{"sketch", sketch}, {"from", sketch::Point2{0, 0}}, {"to", sketch::Point2{40, 10}}});
        run("sketch.constrain",
            {{"sketch", sketch}, {"kind", std::string("fix")}, {"on", std::string("Line1.start")}});
        run("sketch.constrain", {{"sketch", sketch},
                                 {"kind", std::string("length")},
                                 {"on", std::string("Line1")},
                                 {"value", 40.0},
                                 {"name", std::string("Width")}});
        run("sketch.constrain", {{"sketch", sketch},
                                 {"kind", std::string("length")},
                                 {"on", std::string("Line2")},
                                 {"value", 10.0}});
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

    d.run("sketch.set_value",
          {{"sketch", d.id("Sketch1")}, {"constraint", std::string("Width")}, {"value", 55.0}});
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

TEST_CASE("A linked copy's faces can be sketched on, apart from its original's",
          "[model][move][linked][occt]") {
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
    d.run("sketch.set_value",
          {{"sketch", d.id("Sketch1")}, {"constraint", std::string("Width")}, {"value", 50.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, "Hole1") == Approx(50 * 10 * 4 - std::numbers::pi * 1.5 * 1.5 * 4));
}

namespace {

// Every step built with nothing to say: no warnings, like a face found by how it
// looks instead of by its name.
void checkQuiet(const Design& d, const RebuildReport& report) {
    for (std::size_t i = 0; i < d.doc.featureCount(); ++i) {
        const auto& f = d.doc.featureAt(i);
        const auto* r = report.result(f.id());
        REQUIRE(r);
        INFO(f.name() << ": " << (r->issues.empty() ? std::string() : r->issues.front().message));
        CHECK(r->issues.empty());
    }
}

}  // namespace

TEST_CASE("An unlinked copy stops following its original, and has steps of its own",
          "[model][move][linked][occt]") {
    Design d;
    d.run("feature.unlink", {{"feature", d.id("Copy1")}});
    CHECK(d.doc.undoLabel() == "Unlink Copy1");
    // The copy's own steps, just before it, named like duplicates.
    REQUIRE(d.doc.featureCount() == 5);
    CHECK(d.doc.featureAt(2).name() == "Sketch2");
    CHECK(d.doc.featureAt(3).name() == "Plate1");
    CHECK(d.doc.featureAt(4).name() == "Copy1");
    CHECK_FALSE(static_cast<const MoveFeature&>(d.doc.featureAt(4)).copy());

    // It looks just the same, where it was.
    auto report = d.rebuild();
    REQUIRE(report.ok());
    checkQuiet(d, report);
    CHECK(report.bodies() == std::vector{d.id("Plate"), d.id("Copy1")});
    CHECK(d.volume(report, "Copy1") == Approx(40 * 10 * 4));
    auto box = d.kernel->bounds(*report.shapeOf(d.id("Copy1")));
    CHECK(box.max.z - box.min.z == Approx(10));

    // A change to the original leaves the copy alone...
    d.run("sketch.set_value",
          {{"sketch", d.id("Sketch1")}, {"constraint", std::string("Width")}, {"value", 55.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, "Plate") == Approx(55 * 10 * 4));
    CHECK(d.volume(report, "Copy1") == Approx(40 * 10 * 4));
    // ...and a change to the copy's own sketch leaves the original alone.
    d.run("sketch.set_value",
          {{"sketch", d.id("Sketch2")}, {"constraint", std::string("Width")}, {"value", 30.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, "Plate") == Approx(55 * 10 * 4));
    CHECK(d.volume(report, "Copy1") == Approx(30 * 10 * 4));
    box = d.kernel->bounds(*report.shapeOf(d.id("Copy1")));
    CHECK(box.max.x - box.min.x == Approx(30));

    // Undo puts the link back: the two sketch changes, then the unlink.
    REQUIRE(d.doc.undo());
    REQUIRE(d.doc.undo());
    REQUIRE(d.doc.undo());
    CHECK(d.doc.featureCount() == 3);
    CHECK(static_cast<const MoveFeature&>(d.doc.featureAt(2)).copy());
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, "Copy1") == Approx(40 * 10 * 4));
    // And redo takes it away again, with the same steps.
    REQUIRE(d.doc.redo());
    CHECK(d.doc.featureCount() == 5);
    CHECK(d.doc.featureAt(3).name() == "Plate1");
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector{d.id("Plate"), d.id("Copy1")});
}

TEST_CASE("Steps built on a linked copy still find their faces after it's unlinked",
          "[model][move][linked][occt]") {
    Design d;
    auto report = d.rebuild();
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

    d.run("feature.unlink", {{"feature", d.id("Copy1")}});
    report = d.rebuild();
    REQUIRE(report.ok());
    checkQuiet(d, report);  // the hole's sketch found its face by name, not by how it looks
    CHECK(d.volume(report, "Hole1") == Approx(40 * 10 * 4 - std::numbers::pi * 1.5 * 1.5 * 4));
    CHECK(report.bodies() == std::vector{d.id("Plate"), d.id("Hole1")});

    // The copy's own sketch changes the copy, and its hole moves with it.
    d.run("sketch.set_value",
          {{"sketch", d.id("Sketch3")}, {"constraint", std::string("Width")}, {"value", 50.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    checkQuiet(d, report);
    CHECK(d.volume(report, "Hole1") == Approx(50 * 10 * 4 - std::numbers::pi * 1.5 * 1.5 * 4));
    CHECK(d.volume(report, "Plate") == Approx(40 * 10 * 4));
}

TEST_CASE("A linked copy joined to its original can be unlinked", "[model][move][linked][occt]") {
    Design d;
    d.run("feature.add_boolean", {{"operation", std::string("union")},
                                  {"a", d.id("Plate")},
                                  {"b", d.id("Copy1")},
                                  {"name", std::string("Angle")}});
    d.run("feature.unlink", {{"feature", d.id("Copy1")}});
    auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector{d.id("Angle")});
    CHECK(d.volume(report, "Angle") == Approx(2 * 40 * 10 * 4));
    // Now only the top of the L follows Width.
    d.run("sketch.set_value",
          {{"sketch", d.id("Sketch1")}, {"constraint", std::string("Width")}, {"value", 55.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, "Angle") == Approx((55 + 40) * 10 * 4));
}

TEST_CASE("Only a linked copy can be unlinked", "[model][move][linked][occt]") {
    Design d;
    d.run("feature.add_move", {{"body", d.id("Copy1")}, {"by", kernel::Vec3{0, 0, 5}}});
    for (const auto* name : {"Plate", "Move1"}) {
        auto made = CommandRegistry::builtin().create("feature.unlink", {{"feature", d.id(name)}}, d.doc);
        REQUIRE(made.ok());
        const auto issues = d.doc.execute(std::move(made.command));
        REQUIRE(issues.size() == 1);
        CHECK(issues.front().code == "unlink.not_linked");
        CHECK(issues.front().message.find(std::string(name) + " isn't a linked copy") == 0);
    }
    CHECK(d.doc.featureCount() == 4);
}
