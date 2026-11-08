// Extrude that joins onto a body or cuts into it: a peg or a pocket in one
// step. A sketch on a face joins its body unless told otherwise, and a cut
// from a face goes into the body. Faces keep their names through both.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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

    GeometryRef top(const FeatureId& body) {
        const auto report = rebuild();
        const auto& names = *report.result(body)->names;
        for (std::size_t i = 0; i < names.elements.faces.size(); ++i) {
            const auto& f = names.elements.faces[i];
            if (f.geom == kernel::GeomKind::Plane && f.direction.z > 0.999 && f.center.z == Approx(10)) {
                return referTo(names, TopoKind::Face, static_cast<int>(i));
            }
        }
        FAIL("no top");
        return {};
    }

    // A 40 x 20 x 10 mm block and a sketch on its top.
    std::pair<FeatureId, FeatureId> blockAndTopSketch() {
        run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}, {"name", std::string("Block")}});
        const auto block = last();
        run("feature.add_sketch", {{"body", block}, {"face", top(block)}});
        return {block, last()};
    }

    double volume(const RebuildReport& r, const FeatureId& id) {
        return kernel->massProps(*r.shapeOf(id)).volume;
    }
};

}  // namespace

TEST_CASE("a peg sketched on a block's top joins the block by default", "[model][extrude][join][occt]") {
    Design d;
    const auto [block, sketch] = d.blockAndTopSketch();
    d.run("sketch.add_circle", {{"sketch", sketch}, {"center", sketch::Point2{20, 10}}, {"radius", 5.0}});
    d.run("feature.add_extrude", {{"sketch", sketch}, {"distance", 8.0}});
    const auto peg = d.last();
    const auto& step = static_cast<const ExtrudeFeature&>(*d.doc.find(peg));
    CHECK(step.operation() == ExtrudeOp::Join);
    CHECK(step.body() == block);
    CHECK(step.side() == ExtrudeSide::One);

    auto report = d.rebuild();
    REQUIRE(report.ok());
    // One part: the block is used up by the join.
    CHECK(report.bodies() == std::vector<FeatureId>{peg});
    CHECK(d.volume(report, peg) == Approx(8000 + kPi * 25 * 8));
    CHECK(d.kernel->validate(*report.shapeOf(peg)).empty());
    // Faces keep their names: the block's top (now with a ring cut out of it),
    // and the peg's end.
    const auto& names = *report.result(peg)->names;
    CHECK(names.find(TopoKind::Face, shortId(block.toString()) + "/top") >= 0);
    CHECK(names.find(TopoKind::Face, shortId(peg.toString()) + "/end:" +
                                         shortId(static_cast<const SketchFeature&>(*d.doc.find(sketch))
                                                     .sketch()
                                                     .entities.front()
                                                     .id.toString())) >= 0);

    // A taller block: the peg is still on top, and the part grew.
    d.run("feature.set_value", {{"feature", block}, {"setting", std::string("size_z")}, {"value", 25.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, peg) == Approx(20000 + kPi * 25 * 8));
    CHECK(d.kernel->bounds(*report.shapeOf(peg)).max.z == Approx(33));
}

TEST_CASE("a pocket cut from a block's top goes into the block", "[model][extrude][cut][occt]") {
    Design d;
    const auto [block, sketch] = d.blockAndTopSketch();
    d.run("sketch.add_rectangle",
          {{"sketch", sketch}, {"from", sketch::Point2{10, 5}}, {"to", sketch::Point2{30, 15}}});

    SECTION("4 mm deep") {
        d.run("feature.add_extrude",
              {{"sketch", sketch}, {"distance", 4.0}, {"operation", std::string("cut")}});
        const auto pocket = d.last();
        CHECK(static_cast<const ExtrudeFeature&>(*d.doc.find(pocket)).side() == ExtrudeSide::Flip);
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(report.result(pocket)->issues.empty());
        CHECK(report.bodies() == std::vector<FeatureId>{pocket});
        CHECK(d.volume(report, pocket) == Approx(8000 - 20 * 10 * 4));
        const auto box = d.kernel->bounds(*report.shapeOf(pocket));
        CHECK(box.max.z == Approx(10));  // nothing sticks out
        CHECK(box.min.z == Approx(0));
    }

    SECTION("all the way through") {
        d.run("feature.add_extrude",
              {{"sketch", sketch}, {"distance", 15.0}, {"operation", std::string("cut")}});
        const auto hole = d.last();
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, hole) == Approx(8000 - 20 * 10 * 10));
        CHECK(report.result(hole)->names->faces.size() == 6 + 4);  // the block's six, and four walls
    }

    SECTION("the wrong way: it says so, and changes nothing") {
        d.run("feature.add_extrude", {{"sketch", sketch},
                                      {"distance", 4.0},
                                      {"operation", std::string("cut")},
                                      {"side", std::string("one")}});
        const auto missed = d.last();
        const auto report = d.rebuild();
        const auto* r = report.result(missed);
        REQUIRE(r->status == FeatureStatus::Ok);
        REQUIRE(r->issues.size() == 1);
        CHECK(r->issues[0].code == "extrude.cut_missed");
        CHECK(r->issues[0].severity == Severity::Warning);
        CHECK(r->issues[0].message ==
              "Extrude1 doesn't reach the body, so nothing was cut. Try Flip, to go into it, or a longer "
              "distance.");
        CHECK(d.volume(report, missed) == Approx(8000));
    }
}

TEST_CASE("a sketch on a base plane joins or cuts the body it's told to", "[model][extrude][join][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
    const auto block = d.last();
    d.run("feature.add_sketch", {});
    const auto sketch = d.last();
    d.run("sketch.add_circle", {{"sketch", sketch}, {"center", sketch::Point2{20, 10}}, {"radius", 3.0}});

    // Without a body, a join can't work: it says what to do, and adds nothing.
    const auto issues =
        d.tryRun("feature.add_extrude", {{"sketch", sketch}, {"operation", std::string("join")}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "extrude.no_body");
    CHECK(issues[0].message ==
          "To join, say which body with body=, like body=Box1, or draw the sketch on a face of the body.");
    CHECK(d.doc.featureCount() == 2);

    // A hole up through the block from the ground: a cut, told which body.
    d.run("feature.add_extrude",
          {{"sketch", sketch}, {"distance", 10.0}, {"operation", std::string("cut")}, {"body", block}});
    const auto hole = d.last();
    CHECK(static_cast<const ExtrudeFeature&>(*d.doc.find(hole)).side() == ExtrudeSide::One);
    auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, hole) == Approx(8000 - kPi * 9 * 10));
}

TEST_CASE("an edge of a joined peg keeps its fillet through changes",
          "[model][extrude][join][fillet][occt]") {
    Design d;
    const auto [block, sketch] = d.blockAndTopSketch();
    d.run("sketch.add_circle", {{"sketch", sketch}, {"center", sketch::Point2{20, 10}}, {"radius", 5.0}});
    d.run("feature.add_extrude", {{"sketch", sketch}, {"distance", 8.0}});
    const auto peg = d.last();
    // The rim at the top of the peg.
    auto report = d.rebuild();
    const auto& names = *report.result(peg)->names;
    int rim = -1;
    for (std::size_t i = 0; i < names.elements.edges.size(); ++i) {
        const auto& e = names.elements.edges[i];
        if (e.geom == kernel::GeomKind::Circle && e.center.z == Approx(18)) rim = static_cast<int>(i);
    }
    REQUIRE(rim >= 0);
    d.run("feature.add_fillet",
          {{"body", peg}, {"edges", std::vector{referTo(names, TopoKind::Edge, rim)}}, {"radius", 1.0}});
    const auto fillet = d.last();
    d.run("feature.set_value", {{"feature", block}, {"setting", std::string("size_z")}, {"value", 12.0}});
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.result(fillet)->issues.empty());  // found by its exact name
    CHECK(d.kernel->bounds(*report.shapeOf(fillet)).max.z == Approx(20));
}

TEST_CASE("join and cut save and open", "[model][extrude][occt][file]") {
    Design d;
    const auto [block, sketch] = d.blockAndTopSketch();
    d.run("sketch.add_circle", {{"sketch", sketch}, {"center", sketch::Point2{20, 10}}, {"radius", 4.0}});
    d.run("feature.add_extrude", {{"sketch", sketch}, {"distance", 6.0}, {"operation", std::string("cut")}});
    const auto hole = d.last();
    const double before = d.volume(d.rebuild(), hole);
    const auto path = std::filesystem::temp_directory_path() / "sb-test-extrude-cut.sb";
    REQUIRE(io::saveFile(d.doc, path).empty());
    auto opened = io::openFile(path);
    std::filesystem::remove(path);
    REQUIRE(opened.ok());
    const auto& step = static_cast<const ExtrudeFeature&>(*opened.document->find(hole));
    CHECK(step.operation() == ExtrudeOp::Cut);
    CHECK(step.body() == block);
    CHECK(step.side() == ExtrudeSide::Flip);
    const auto report = opened.document->regenerate(*d.kernel, d.solver.get());
    REQUIRE(report.ok());
    CHECK(d.kernel->massProps(*report.shapeOf(hole)).volume == Approx(before));
    CHECK(before == Approx(8000 - kPi * 16 * 6));
}
