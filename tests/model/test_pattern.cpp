// The Pattern step: copies of holes, joins and cuts, or of a whole body, in a
// row, a grid or round a circle. Every volume is worked out by hand.

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
#include "sb/model/Naming.h"
#include "sb/model/Pattern.h"
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

    // A sketch on the top (the face facing +z) of a body.
    FeatureId sketchOnTop(const FeatureId& body) {
        const auto report = rebuild();
        const auto& names = *report.result(body)->names;
        int top = -1;
        double highest = -1e9;
        for (std::size_t i = 0; i < names.elements.faces.size(); ++i) {
            const auto& f = names.elements.faces[i];
            if (f.direction.z > 0.999 && f.center.z > highest) {
                top = static_cast<int>(i);
                highest = f.center.z;
            }
        }
        REQUIRE(top >= 0);
        run("feature.add_sketch", {{"body", body}, {"face", referTo(names, TopoKind::Face, top)}});
        return last();
    }
};

}  // namespace

TEST_CASE("where a pattern's copies go", "[model][pattern]") {
    PatternLayout row;
    row.count = 3;
    row.spacing = 10;
    auto m = patternMotions(row);
    REQUIRE(m.size() == 2);  // the first is the original
    CHECK(m[0].offset.x == 10);
    CHECK(m[1].offset.x == 20);
    CHECK(m[1].angle == 0);

    PatternLayout grid = row;
    grid.direction = {2, 0, 0};  // only its direction counts
    grid.count2 = 2;
    grid.spacing2 = 5;
    m = patternMotions(grid);
    REQUIRE(m.size() == 5);
    CHECK(m[1].offset.x == 20);
    CHECK(m[2].offset.x == 0);  // the second row starts again at the start
    CHECK(m[2].offset.y == 5);
    CHECK(m[4].offset.x == 20);

    PatternLayout ring;
    ring.kind = PatternLayout::Kind::Circular;
    ring.count = 4;
    m = patternMotions(ring);
    REQUIRE(m.size() == 3);
    CHECK(m[0].angle == Approx(kPi / 2));  // evenly all the way round
    CHECK(m[2].angle == Approx(3 * kPi / 2));
    ring.angle = kPi / 2;
    m = patternMotions(ring);
    CHECK(m[2].angle == Approx(kPi / 2));  // the last one at the end of the angle
    CHECK(m[0].angle == Approx(kPi / 6));
}

TEST_CASE("a ring of bolt holes: one hole, patterned round a circle", "[model][pattern][occt]") {
    Design d;
    d.run("feature.add_box", {{"corner", kernel::Vec3{-30, -30, 0}}, {"size", kernel::Vec3{60, 60, 5}}});
    const auto plate = d.last();
    const auto sketch = d.sketchOnTop(plate);
    d.run("sketch.add_point", {{"sketch", sketch}, {"at", sketch::Point2{20, 0}}});
    d.run("feature.add_hole", {{"sketch", sketch}});  // M3: 3.4 mm, through
    const auto hole = d.last();
    d.run("feature.add_pattern", {{"steps", std::vector{hole}}, {"kind", std::string("circular")}});
    const auto ring = d.last();
    const auto& step = static_cast<const PatternFeature&>(*d.doc.find(ring));
    CHECK(step.body() == hole);
    CHECK(step.layout().count == 6);
    CHECK(d.doc.undoLabel() == "Add Pattern1");

    auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector<FeatureId>{ring});  // the plate with its holes, one body
    const double hole34 = kPi * 1.7 * 1.7 * 5;
    CHECK(d.volume(report, ring) == Approx(18000 - 6 * hole34));
    CHECK(d.kernel->validate(*report.shapeOf(ring)).empty());
    // Copy 3 is 120 degrees round: at 20 cos 120, 20 sin 120. Its faces say so.
    const auto& names = *report.result(ring)->names;
    int found = -1;
    for (std::size_t i = 0; i < names.faces.size(); ++i)
        if (names.faces[i].find("/copy3:") != std::string::npos &&
            names.faces[i].find("/wall:") != std::string::npos)
            found = static_cast<int>(i);
    REQUIRE(found >= 0);
    const auto& c = names.elements.faces[static_cast<std::size_t>(found)].center;
    CHECK(c.x == Approx(-10));
    CHECK(c.y == Approx(20 * std::sin(2 * kPi / 3)));
    CHECK(describeElement(d.doc, TopoKind::Face, names.faces[static_cast<std::size_t>(found)]) ==
          "copy 3 (Pattern1) of wall of Hole1 at Point1");

    // The copies follow the hole: an M4 hole makes M4 copies. And 8 of them.
    d.run("feature.set_value", {{"feature", hole}, {"setting", std::string("diameter")}, {"value", 4.5}});
    d.run("feature.set_value", {{"feature", ring}, {"setting", std::string("count")}, {"value", 8.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, ring) == Approx(18000 - 8 * kPi * 2.25 * 2.25 * 5));
}

TEST_CASE("a grid of pegs: an extrude that joins, patterned in rows", "[model][pattern][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
    const auto block = d.last();
    const auto sketch = d.sketchOnTop(block);
    d.run("sketch.add_circle", {{"sketch", sketch}, {"center", sketch::Point2{5, 5}}, {"radius", 2.0}});
    d.run("feature.add_extrude", {{"sketch", sketch}, {"distance", 5.0}});  // joins the block
    const auto peg = d.last();
    d.run("feature.add_pattern", {{"steps", std::vector{peg}},
                                  {"count", std::int64_t{4}},
                                  {"spacing", 10.0},
                                  {"count2", std::int64_t{2}},
                                  {"spacing2", 10.0}});
    const auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, d.last()) == Approx(8000 + 8 * kPi * 4 * 5));
    const auto b = d.kernel->bounds(*report.shapeOf(d.last()));
    CHECK(b.max.z == Approx(15));
}

TEST_CASE("grooves round a rod: a revolve that cuts, in a row", "[model][pattern][occt]") {
    Design d;
    d.run("feature.add_cylinder", {{"base", kernel::Vec3{0, 0, 0}},
                                   {"axis", kernel::Vec3{0, 1, 0}},
                                   {"radius", 10.0},
                                   {"height", 40.0}});
    const auto rod = d.last();
    d.run("feature.add_sketch", {});
    const auto sketch = d.last();
    d.run("sketch.add_rectangle",
          {{"sketch", sketch}, {"from", sketch::Point2{8, 4}}, {"to", sketch::Point2{12, 6}}});
    d.run("feature.add_revolve", {{"sketch", sketch}, {"operation", std::string("cut")}, {"body", rod}});
    d.run("feature.add_pattern", {{"steps", std::vector{d.last()}},
                                  {"direction", kernel::Vec3{0, 1, 0}},
                                  {"spacing", 8.0},
                                  {"count", std::int64_t{4}}});
    const auto report = d.rebuild();
    REQUIRE(report.ok());
    // Each groove is a ring 8 to 10 mm out and 2 mm wide.
    CHECK(d.volume(report, d.last()) == Approx(kPi * 100 * 40 - 4 * kPi * (100 - 64) * 2));
}

TEST_CASE("copies of a whole body, joined where they touch", "[model][pattern][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{10, 10, 10}}});
    const auto cube = d.last();

    SECTION("apart: three cubes in one body") {
        d.run("feature.add_pattern", {{"body", cube}, {"spacing", 15.0}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(report.bodies() == std::vector<FeatureId>{d.last()});  // the cube is used up
        CHECK(d.volume(report, d.last()) == Approx(3000));
        CHECK(d.kernel->topology(*report.shapeOf(d.last())).solids == 3);
        CHECK(d.kernel->bounds(*report.shapeOf(d.last())).max.x == Approx(40));
    }
    SECTION("touching: one solid 30 mm long") {
        d.run("feature.add_pattern", {{"body", cube}, {"spacing", 10.0}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, d.last()) == Approx(3000));
        CHECK(d.kernel->topology(*report.shapeOf(d.last())).solids == 1);
    }
    SECTION("round a circle: four cubes about a line beside them") {
        d.run("feature.add_pattern", {{"body", cube},
                                      {"kind", std::string("circular")},
                                      {"count", std::int64_t{4}},
                                      {"center", kernel::Vec3{-10, -10, 0}}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, d.last()) == Approx(4000));
        const auto b = d.kernel->bounds(*report.shapeOf(d.last()));
        CHECK(b.min.x == Approx(-30));
        CHECK(b.max.y == Approx(10));
    }
}

TEST_CASE("a pattern of steps changes the body they became", "[model][pattern][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{60, 20, 10}}});
    const auto block = d.last();
    const auto sketch = d.sketchOnTop(block);
    d.run("sketch.add_point", {{"sketch", sketch}, {"at", sketch::Point2{10, 10}}});
    d.run("feature.add_hole", {{"sketch", sketch}});
    const auto hole = d.last();
    // A fillet on the block's top front edge, after the hole.
    const auto report0 = d.rebuild();
    const auto& names = *report0.result(hole)->names;
    int edge = -1;
    for (std::size_t i = 0; i < names.elements.edges.size(); ++i) {
        const auto& c = names.elements.edges[i].center;
        if (std::abs(c.y) < 1e-9 && std::abs(c.z - 10) < 1e-9) edge = static_cast<int>(i);
    }
    REQUIRE(edge >= 0);
    d.run("feature.add_fillet",
          {{"body", hole}, {"edges", std::vector{referTo(names, TopoKind::Edge, edge)}}, {"radius", 1.0}});
    const auto fillet = d.last();
    d.run("feature.add_pattern", {{"steps", std::vector{hole}}, {"spacing", 20.0}});
    const auto& step = static_cast<const PatternFeature&>(*d.doc.find(d.last()));
    CHECK(step.body() == fillet);  // not Hole1's own body, which the fillet changed
    const auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector<FeatureId>{d.last()});
    CHECK(d.volume(report, d.last()) == Approx(d.volume(report, fillet) - 2 * kPi * 1.7 * 1.7 * 10));
}

TEST_CASE("Pattern explains what's wrong, and how to fix it", "[model][pattern][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
    const auto block = d.last();

    SECTION("nothing to repeat") {
        const auto issues = d.tryRun("feature.add_pattern", {});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].code == "pattern.what");
    }
    SECTION("one step that makes a body of its own copies the body") {
        d.run("feature.add_pattern", {{"steps", std::vector{block}}, {"spacing", 50.0}});
        const auto& step = static_cast<const PatternFeature&>(*d.doc.find(d.last()));
        CHECK(step.body() == block);
        CHECK(step.steps().empty());
        CHECK(d.volume(d.rebuild(), d.last()) == Approx(3 * 8000));
    }
    SECTION("a body among steps to repeat, or a sketch, can't be repeated") {
        d.run("feature.add_sketch", {});
        const auto sketch = d.last();
        d.run("feature.add_box", {{"corner", kernel::Vec3{100, 0, 0}}, {"size", kernel::Vec3{5, 5, 5}}});
        const auto other = d.last();
        for (const auto& steps : {std::vector{sketch}, std::vector{block, other}}) {
            const auto issues = d.tryRun("feature.add_pattern", {{"steps", steps}});
            REQUIRE(issues.size() == 1);
            CHECK(issues[0].code == "pattern.not_repeatable");
        }
        CHECK(d.tryRun("feature.add_pattern", {{"steps", std::vector{block}}}).empty() == true);
    }
    SECTION("too many copies") {
        d.run(
            "feature.add_pattern",
            {{"body", block}, {"count", std::int64_t{20}}, {"count2", std::int64_t{11}}, {"spacing", 50.0}});
        const auto report = d.rebuild();
        CHECK(
            report.result(d.last())->issues.at(0).message ==
            "Pattern1 would make 220 copies. SketchBillet makes up to 200 in one pattern: use fewer, or two "
            "patterns.");
    }
    SECTION("copies that run off the body") {
        const auto sketch = d.sketchOnTop(block);
        d.run("sketch.add_point", {{"sketch", sketch}, {"at", sketch::Point2{10, 10}}});
        d.run("feature.add_hole", {{"sketch", sketch}});
        d.run("feature.add_pattern", {{"steps", std::vector{d.last()}}, {"spacing", 20.0}});  // 10, 30, 50
        const auto report = d.rebuild();
        const auto* r = report.result(d.last());
        REQUIRE(r->status == FeatureStatus::Ok);
        REQUIRE(r->issues.size() == 1);
        CHECK(r->issues[0].message ==
              "Pattern1's copy 3 isn't on the body, so it cut nothing. Check the spacing and direction.");
    }
    SECTION("no spacing") {
        d.run("feature.add_pattern", {{"body", block}, {"spacing", 0.0}});
        CHECK(d.rebuild().result(d.last())->issues.at(0).code == "pattern.spacing");
    }
}

TEST_CASE("Pattern saves and opens, a row or a circle", "[model][pattern][occt][file]") {
    Design d;
    d.run("feature.add_box", {{"corner", kernel::Vec3{-30, -30, 0}}, {"size", kernel::Vec3{60, 60, 5}}});
    const auto sketch = d.sketchOnTop(d.last());
    d.run("sketch.add_point", {{"sketch", sketch}, {"at", sketch::Point2{20, 0}}});
    d.run("feature.add_hole", {{"sketch", sketch}});
    const auto hole = d.last();
    d.run("feature.add_pattern", {{"steps", std::vector{hole}},
                                  {"kind", std::string("circular")},
                                  {"count", std::int64_t{3}},
                                  {"angle", 90.0}});
    const auto ring = d.last();
    d.run("feature.add_pattern", {{"body", ring},
                                  {"direction", kernel::Vec3{0, 0, 1}},
                                  {"spacing", 6.0},
                                  {"count", std::int64_t{2}},
                                  {"count2", std::int64_t{1}}});
    const auto stack = d.last();
    const double before = d.volume(d.rebuild(), stack);
    const auto path = std::filesystem::temp_directory_path() / "sb-test-pattern.sb";
    REQUIRE(io::saveFile(d.doc, path).empty());
    auto opened = io::openFile(path);
    std::filesystem::remove(path);
    REQUIRE(opened.ok());
    const auto& a = static_cast<const PatternFeature&>(*opened.document->find(ring));
    CHECK(a.steps() == std::vector{hole});
    CHECK(a.layout().kind == PatternLayout::Kind::Circular);
    CHECK(a.layout().count == 3);
    CHECK(a.layout().angle == Approx(kPi / 2));
    CHECK(a.layout().axis.z == 1);
    const auto& b = static_cast<const PatternFeature&>(*opened.document->find(stack));
    CHECK(b.steps().empty());
    CHECK(b.body() == ring);
    CHECK(b.layout().direction.z == 1);
    CHECK(b.layout().spacing == 6);
    const auto report = opened.document->regenerate(*d.kernel, d.solver.get());
    REQUIRE(report.ok());
    CHECK(d.kernel->massProps(*report.shapeOf(stack)).volume == Approx(before));
    CHECK(before == Approx(2 * (18000 - 3 * kPi * 1.7 * 1.7 * 5)));
}
