// The Hole step: holes at a sketch's points or from two edges; simple,
// counterbored and countersunk; through or to a depth; screw sizes; names;
// messages; and saving. Every volume is worked out by hand.

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
#include "sb/model/Holes.h"
#include "sb/model/Naming.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;
using kernel::TopoKind;

namespace {

constexpr double kPi = std::numbers::pi;

// A hole's volume: a cylinder of diameter d, h deep.
double cylinder(double d, double h) {
    return kPi * d * d / 4 * h;
}

struct Design {
    Document doc;
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();
    std::unique_ptr<sketch::ISketchSolver> solver = sketch::makePlaneGcsSolver();
    FeatureId block, sketch;

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

    // A 40 x 20 x 10 mm block, and a sketch on its top.
    Design() {
        run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}, {"name", std::string("Block")}});
        block = last();
        const auto report = rebuild();
        const auto& names = *report.result(block)->names;
        int top = -1;
        for (std::size_t i = 0; i < names.elements.faces.size(); ++i) {
            const auto& f = names.elements.faces[i];
            if (f.direction.z > 0.999) top = static_cast<int>(i);
        }
        REQUIRE(top >= 0);
        run("feature.add_sketch", {{"body", block}, {"face", referTo(names, TopoKind::Face, top)}});
        sketch = last();
    }
    void point(double x, double y) {
        run("sketch.add_point", {{"sketch", sketch}, {"at", sketch::Point2{x, y}}});
    }
    // An edge of a step's body, by its middle.
    GeometryRef edgeAt(const FeatureId& body, kernel::Vec3 p) {
        const auto report = rebuild();
        const auto& names = *report.result(body)->names;
        for (std::size_t i = 0; i < names.elements.edges.size(); ++i) {
            const auto& c = names.elements.edges[i].center;
            if (std::abs(c.x - p.x) < 1e-9 && std::abs(c.y - p.y) < 1e-9 && std::abs(c.z - p.z) < 1e-9)
                return referTo(names, TopoKind::Edge, static_cast<int>(i));
        }
        FAIL("no edge there");
        return {};
    }
};

}  // namespace

TEST_CASE("screw sizes: clearance, tap, counterbore and countersink", "[model][hole]") {
    REQUIRE(screwSizes().size() == 7);
    const auto* m3 = findScrewSize("m3");
    REQUIRE(m3);
    CHECK(m3->name == "M3");
    CHECK(m3->clearance == 3.4);
    CHECK(m3->tap == 2.5);
    CHECK(m3->counterboreDiameter == 6.5);  // a 5.5 mm head + 1 mm
    CHECK(m3->counterboreDepth == 3.5);     // a 3 mm head + 0.5 mm
    CHECK(m3->countersinkDiameter == 6.72);
    CHECK(findScrewSize("M2")->countersinkDiameter == 0);  // no ISO 10642 screw this small
    CHECK(findScrewSize("M7") == nullptr);
    for (std::size_t i = 1; i < screwSizes().size(); ++i) {
        const auto& s = screwSizes()[i];
        INFO(s.name);
        CHECK(s.tap < s.clearance);
        CHECK(s.clearance < s.counterboreDiameter);
        CHECK(screwSizes()[i - 1].clearance < s.clearance);
    }
}

TEST_CASE("Hole drills at every point of a sketch on a face, M3 and through by default",
          "[model][hole][occt]") {
    Design d;
    d.point(10, 10);
    d.point(30, 10);
    d.run("feature.add_hole", {{"sketch", d.sketch}});
    const auto hole = d.last();
    const auto& step = static_cast<const HoleFeature&>(*d.doc.find(hole));
    CHECK(step.name() == "Hole1");
    CHECK(step.body() == d.block);  // the body the sketch is on
    CHECK(step.size() == "M3");
    CHECK(step.diameter() == 3.4);
    CHECK(step.through());
    CHECK(step.kind() == HoleKind::Simple);

    auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector<FeatureId>{hole});
    CHECK(d.volume(report, hole) == Approx(8000 - 2 * cylinder(3.4, 10)));
    CHECK(d.kernel->validate(*report.shapeOf(hole)).empty());
    // Each hole's wall is named for its point, and says so in words.
    const auto& names = *report.result(hole)->names;
    const auto& pt = *static_cast<const SketchFeature&>(*d.doc.find(d.sketch)).sketch().findEntity("Point1");
    const int wall =
        names.find(TopoKind::Face, shortId(hole.toString()) + "/wall:" + shortId(pt.id.toString()));
    REQUIRE(wall >= 0);
    CHECK(describeElement(d.doc, TopoKind::Face, names.faces[static_cast<std::size_t>(wall)]) ==
          "wall of Hole1 at Point1");

    // A thicker block: through is still through.
    d.run("feature.set_value", {{"feature", d.block}, {"setting", std::string("size_z")}, {"value", 25.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, hole) == Approx(20000 - 2 * cylinder(3.4, 25)));
}

TEST_CASE("Hole goes to a depth with a flat floor, or is sized to tap", "[model][hole][occt]") {
    Design d;
    d.point(20, 10);
    SECTION("4 mm deep") {
        d.run("feature.add_hole", {{"sketch", d.sketch}, {"depth", 4.0}, {"diameter", 5.0}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, d.last()) == Approx(8000 - cylinder(5, 4)));
        CHECK(static_cast<const HoleFeature&>(*d.doc.find(d.last())).size().empty());  // a size of its own
        const auto& names = *report.result(d.last())->names;
        int floors = 0;
        for (const auto& n : names.faces) floors += n.find("/floor:") != std::string::npos;
        CHECK(floors == 1);
    }
    SECTION("M4, to tap") {
        d.run("feature.add_hole",
              {{"sketch", d.sketch}, {"size", std::string("M4")}, {"fit", std::string("tap")}});
        const auto& step = static_cast<const HoleFeature&>(*d.doc.find(d.last()));
        CHECK(step.diameter() == 3.3);
        CHECK(step.fit() == HoleFit::Tap);
        CHECK(d.volume(d.rebuild(), d.last()) == Approx(8000 - cylinder(3.3, 10)));
    }
}

TEST_CASE("Hole is counterbored or countersunk for a screw head", "[model][hole][occt]") {
    Design d;
    d.point(20, 10);
    SECTION("counterbore: M3, 6.5 mm across and 3.5 mm deep at the top") {
        d.run("feature.add_hole", {{"sketch", d.sketch}, {"kind", std::string("counterbore")}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, d.last()) == Approx(8000 - cylinder(6.5, 3.5) - cylinder(3.4, 6.5)));
        CHECK(d.kernel->validate(*report.shapeOf(d.last())).empty());
        const auto& names = *report.result(d.last())->names;
        int step = 0;
        for (const auto& n : names.faces) step += n.find("/step:") != std::string::npos;
        CHECK(step == 1);
    }
    SECTION("countersink: M3, 6.72 mm across at the face, 90 degrees") {
        d.run("feature.add_hole", {{"sketch", d.sketch}, {"kind", std::string("countersink")}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        // A cone from 3.36 mm round at the face down to the hole's 1.7 mm,
        // 1.66 mm deep (45 degrees each side), then the hole.
        const double R = 3.36, r = 1.7, h = R - r;
        const double cone = kPi * h / 3 * (R * R + R * r + r * r);
        CHECK(d.volume(report, d.last()) == Approx(8000 - cone - cylinder(3.4, 10 - h)));
    }
    SECTION("a counterbore deeper than the hole says so") {
        d.run("feature.add_hole",
              {{"sketch", d.sketch}, {"kind", std::string("counterbore")}, {"depth", 3.0}});
        const auto report = d.rebuild();
        const auto* r = report.result(d.last());
        REQUIRE(r->status == FeatureStatus::Failed);
        CHECK(r->issues.at(0).code == "hole.too_shallow");
        CHECK(
            r->issues.at(0).message ==
            "Hole1 is 3 mm deep, but its counterbore goes deeper than that. Make the hole deeper, or the top "
            "smaller.");
    }
}

TEST_CASE("Hole from two edges: a distance in from each", "[model][hole][occt]") {
    Design d;
    // The block's top front edge (along x, at y = 0) and top left edge (along y, at x = 0).
    const auto front = d.edgeAt(d.block, {20, 0, 10});
    const auto left = d.edgeAt(d.block, {0, 10, 10});
    d.run("feature.add_hole", {{"body", d.block},
                               {"edges", std::vector{front, left}},
                               {"distances", sketch::Point2{5, 12}},
                               {"diameter", 4.0}});
    const auto hole = d.last();
    const auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, hole) == Approx(8000 - cylinder(4, 10)));
    // The hole's wall is 12 mm in from the left and 5 mm in from the front.
    const auto& names = *report.result(hole)->names;
    const int wall = names.find(TopoKind::Face, shortId(hole.toString()) + "/wall:edges");
    REQUIRE(wall >= 0);
    const auto& c = names.elements.faces[static_cast<std::size_t>(wall)].center;
    CHECK(c.x == Approx(12));
    CHECK(c.y == Approx(5));
    CHECK(describeElement(d.doc, TopoKind::Face, names.faces[static_cast<std::size_t>(wall)]) ==
          "wall of Hole1");
    const std::string wallName = names.faces[static_cast<std::size_t>(wall)];
    // Where the wall is after a rebuild (each rebuild replaces the last one's results).
    const auto wallX = [&] {
        const auto r = d.rebuild();
        REQUIRE(r.ok());
        const auto& n = *r.result(hole)->names;
        const int at = n.find(TopoKind::Face, wallName);
        REQUIRE(at >= 0);
        return n.elements.faces[static_cast<std::size_t>(at)].center.x;
    };

    // It moves with the edges: a wider block keeps it 12 mm from the left.
    d.run("feature.set_value", {{"feature", d.block}, {"setting", std::string("size_x")}, {"value", 60.0}});
    CHECK(wallX() == Approx(12));

    // A distance changes like any other number.
    d.run("feature.set_value", {{"feature", hole}, {"setting", std::string("distance_b")}, {"value", 30.0}});
    CHECK(wallX() == Approx(30));
}

TEST_CASE("Hole explains what's wrong, and how to fix it", "[model][hole][occt]") {
    Design d;
    SECTION("no sketch and no edges") {
        const auto issues = d.tryRun("feature.add_hole", {{"body", d.block}});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].code == "hole.where");
    }
    SECTION("a screw size SketchBillet doesn't know") {
        const auto issues = d.tryRun("feature.add_hole", {{"sketch", d.sketch}, {"size", std::string("M7")}});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].message ==
              "SketchBillet doesn't know the screw size M7. Use M2, M2.5, M3, M4, M5, M6 or M8, or give "
              "diameter= "
              "in mm.");
    }
    SECTION("a sketch with no points") {
        d.run("feature.add_hole", {{"sketch", d.sketch}});
        const auto report = d.rebuild();
        CHECK(report.result(d.last())->issues.at(0).code == "hole.no_points");
    }
    SECTION("a point off the body drills nothing, and says so") {
        d.point(20, 10);
        d.point(80, 10);
        d.run("feature.add_hole", {{"sketch", d.sketch}});
        const auto report = d.rebuild();
        const auto* r = report.result(d.last());
        REQUIRE(r->status == FeatureStatus::Ok);
        REQUIRE(r->issues.size() == 1);
        CHECK(r->issues[0].code == "hole.missed");
        CHECK(
            r->issues[0].message ==
            "Hole1's hole at Point2 isn't on the body, so it drilled nothing. Move it onto the body, or try "
            "Flip.");
        CHECK(d.volume(report, d.last()) == Approx(8000 - cylinder(3.4, 10)));
    }
    SECTION("parallel edges") {
        const auto front = d.edgeAt(d.block, {20, 0, 10});
        const auto back = d.edgeAt(d.block, {20, 20, 10});
        d.run("feature.add_hole", {{"body", d.block}, {"edges", std::vector{front, back}}});
        const auto report = d.rebuild();
        CHECK(report.result(d.last())->issues.at(0).code == "hole.edges_parallel");
    }
    SECTION("edges that aren't on one face") {
        const auto topFront = d.edgeAt(d.block, {20, 0, 10});
        const auto bottomBack = d.edgeAt(d.block, {20, 20, 0});
        d.run("feature.add_hole", {{"body", d.block}, {"edges", std::vector{topFront, bottomBack}}});
        const auto report = d.rebuild();
        CHECK(report.result(d.last())->issues.at(0).code == "hole.no_face");
    }
}

TEST_CASE("Hole saves and opens with its placement, kind, size and depth", "[model][hole][occt][file]") {
    Design d;
    d.point(10, 10);
    d.run("feature.add_hole", {{"sketch", d.sketch},
                               {"kind", std::string("counterbore")},
                               {"size", std::string("M4")},
                               {"depth", 8.0}});
    const auto points = d.last();
    const auto front = d.edgeAt(points, {20, 0, 10});
    const auto right = d.edgeAt(points, {40, 10, 10});
    d.run("feature.add_hole", {{"body", points},
                               {"edges", std::vector{front, right}},
                               {"distances", sketch::Point2{10, 8}},
                               {"kind", std::string("countersink")},
                               {"countersink_angle", 82.0}});
    const auto edges = d.last();
    const auto before = d.rebuild();
    REQUIRE(before.ok());
    const double volume = d.volume(before, edges);
    const auto path = std::filesystem::temp_directory_path() / "sb-test-hole.sb";
    REQUIRE(io::saveFile(d.doc, path).empty());
    auto opened = io::openFile(path);
    std::filesystem::remove(path);
    REQUIRE(opened.ok());
    const auto& a = static_cast<const HoleFeature&>(*opened.document->find(points));
    CHECK(a.placement().kind == HolePlacement::Kind::SketchPoints);
    CHECK(a.placement().sketch == d.sketch);
    CHECK(a.kind() == HoleKind::Counterbore);
    CHECK(a.size() == "M4");
    CHECK(a.diameter() == 4.5);
    CHECK(a.counterboreDiameter() == 8.0);
    CHECK_FALSE(a.through());
    CHECK(a.depth() == 8.0);
    const auto& b = static_cast<const HoleFeature&>(*opened.document->find(edges));
    CHECK(b.placement().kind == HolePlacement::Kind::FromEdges);
    CHECK(b.placement().edgeA == front);
    CHECK(b.placement().distanceB == 8.0);
    CHECK(b.kind() == HoleKind::Countersink);
    CHECK(b.countersinkAngle() == Approx(82 * kPi / 180));
    CHECK(b.size().empty());  // its countersink angle is its own
    const auto report = opened.document->regenerate(*d.kernel, d.solver.get());
    REQUIRE(report.ok());
    CHECK(d.kernel->massProps(*report.shapeOf(edges)).volume == Approx(volume));
}
