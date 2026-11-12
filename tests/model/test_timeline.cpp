// The timeline: the rollback marker, new steps put in at it, and moving steps in
// the order. Every volume is worked out by hand.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>

#include "sb/io/SketchBilletFile.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/model/Naming.h"
#include "sb/model/Shell.h"
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
    FeatureId id(const std::string& name) const {
        for (std::size_t i = 0; i < doc.featureCount(); ++i)
            if (doc.featureAt(i).name() == name) return doc.featureAt(i).id();
        FAIL("no step " << name);
        return {};
    }
    std::vector<std::string> order() const {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < doc.featureCount(); ++i) out.push_back(doc.featureAt(i).name());
        return out;
    }
    RebuildReport rebuild() { return doc.regenerate(*kernel, solver.get()); }
    double volume(const RebuildReport& r, const std::string& name) {
        const auto* shape = r.shapeOf(id(name));
        REQUIRE(shape);
        return kernel->massProps(*shape).volume;
    }
    // A step's faces or edges, picked by a test on their geometry.
    template <typename Pick>
    std::vector<GeometryRef> pick(const std::string& step, TopoKind kind, Pick test) {
        const auto report = rebuild();
        const auto& names = *report.result(id(step))->names;
        std::vector<GeometryRef> out;
        const auto& infos = names.elements.of(kind);
        for (std::size_t i = 0; i < infos.size(); ++i)
            if (test(infos[i])) out.push_back(referTo(names, kind, static_cast<int>(i)));
        return out;
    }
    // A box 40 x 30 x 20 mm shelled to 2 mm walls, open at the top.
    void tray() {
        run("feature.add_box", {{"size", kernel::Vec3{40, 30, 20}}});
        const auto top = pick("Box1", TopoKind::Face, [](const kernel::ElementInfo& e) {
            return e.geom == kernel::GeomKind::Plane && e.direction.z > 0.999;
        });
        run("feature.add_shell", {{"body", id("Box1")}, {"faces", top}});
    }
};

}  // namespace

TEST_CASE("Rolling back keeps the later steps but doesn't build them", "[model][timeline][occt]") {
    Design d;
    d.tray();
    d.run("feature.add_cylinder", {{"radius", 5.0}, {"height", 10.0}, {"base", kernel::Vec3{60, 0, 0}}});
    REQUIRE(d.doc.markerIndex() == 3);
    CHECK_FALSE(d.doc.rolledBack());
    CHECK_FALSE(d.doc.marker());

    d.run("timeline.rollback", {{"after", d.id("Box1")}});
    CHECK(d.doc.undoLabel() == "Roll back to after Box1");
    CHECK(d.doc.markerIndex() == 1);
    CHECK(d.doc.rolledBack());
    CHECK(d.doc.marker() == d.id("Box1"));
    CHECK(d.doc.afterMarker(d.id("Shell1")));
    CHECK_FALSE(d.doc.afterMarker(d.id("Box1")));
    auto report = d.rebuild();
    CHECK(report.ok());  // rolled-back steps aren't problems
    CHECK(report.result(d.id("Shell1"))->status == FeatureStatus::RolledBack);
    CHECK(report.result(d.id("Cylinder1"))->status == FeatureStatus::RolledBack);
    CHECK_FALSE(report.shapeOf(d.id("Shell1")));
    CHECK(report.bodies() == std::vector<FeatureId>{d.id("Box1")});  // the solid box, as it was then

    SECTION("to the start, and forward to the end") {
        d.run("timeline.rollback", {{"to", std::string("start")}});
        CHECK(d.doc.undoLabel() == "Roll back to the start");
        CHECK(d.doc.markerIndex() == 0);
        CHECK(d.doc.marker() == FeatureId{});
        CHECK(d.rebuild().bodies().empty());
        d.run("timeline.rollback", {{"to", std::string("end")}});
        CHECK(d.doc.undoLabel() == "Roll forward to the end");
        CHECK_FALSE(d.doc.rolledBack());
        CHECK(d.rebuild().bodies().size() == 2);
    }
    SECTION("undo and redo put the marker back") {
        d.doc.undo();
        CHECK_FALSE(d.doc.rolledBack());
        d.doc.redo();
        CHECK(d.doc.markerIndex() == 1);
    }
    SECTION("the marker's already there") {
        const auto issues = d.tryRun("timeline.rollback", {{"after", d.id("Box1")}});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].message == "The marker is already there.");
    }
    SECTION("saying where, one way") {
        const auto issues = d.tryRun("timeline.rollback", {});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].message == "Say where the marker goes: after=<step>, or to=start or to=end.");
    }
    SECTION("deleting the step the marker is after keeps it in the same place") {
        d.run("timeline.rollback", {{"after", d.id("Shell1")}});
        d.run("feature.delete", {{"feature", d.id("Cylinder1")}});  // after the marker: fine
        CHECK(d.doc.markerIndex() == 2);
        CHECK_FALSE(d.doc.rolledBack());  // nothing after it now: at the end
        d.doc.undo();
        CHECK(d.doc.markerIndex() == 2);
        CHECK(d.doc.rolledBack());
        // The marker's step itself: it stays after the step before.
        d.run("feature.delete", {{"feature", d.id("Shell1")}});
        CHECK(d.doc.markerIndex() == 1);
        CHECK(d.doc.marker() == d.id("Box1"));
        d.doc.undo();
        CHECK(d.doc.marker() == d.id("Shell1"));
    }
}

TEST_CASE("A new step goes in at the marker, and takes its body's place", "[model][timeline][occt]") {
    Design d;
    d.tray();
    d.run("timeline.rollback", {{"after", d.id("Box1")}});

    // Round the box's four upright edges, before the shell.
    const auto upright = d.pick("Box1", TopoKind::Edge, [](const kernel::ElementInfo& e) {
        return e.geom == kernel::GeomKind::Line && std::abs(e.direction.z) > 0.999;
    });
    REQUIRE(upright.size() == 4);
    d.run("feature.add_fillet", {{"body", d.id("Box1")}, {"edges", upright}, {"radius", 5.0}});
    CHECK(d.order() == std::vector<std::string>{"Box1", "Fillet1", "Shell1"});
    CHECK(d.doc.marker() == d.id("Fillet1"));  // the marker moved past it
    CHECK(d.doc.afterMarker(d.id("Shell1")));
    // The shell now hollows the rounded box, and still finds its open top.
    const auto& shell = static_cast<const ShellFeature&>(*d.doc.find(d.id("Shell1")));
    CHECK(shell.body() == d.id("Fillet1"));

    d.run("timeline.rollback", {{"to", std::string("end")}});
    auto report = d.rebuild();
    REQUIRE(report.ok());
    // Outside: 40 x 30 x 20 less four 5 mm roundings. Inside: 36 x 26 x 18, its
    // corners rounded to 3 mm (5 mm less the 2 mm wall).
    const double outside = 24000 - 4 * (25 - 25 * kPi / 4) * 20;
    const double inside = (36 * 26 - 4 * (9 - 9 * kPi / 4)) * 18;
    CHECK(d.volume(report, "Fillet1") == Approx(outside));
    CHECK(d.volume(report, "Shell1") == Approx(outside - inside));
    CHECK(report.bodies() == std::vector<FeatureId>{d.id("Shell1")});

    SECTION("undo takes it out, and the shell uses the box again") {
        d.doc.undo();  // roll forward
        d.doc.undo();  // the fillet
        CHECK(d.order() == std::vector<std::string>{"Box1", "Shell1"});
        CHECK(static_cast<const ShellFeature&>(*d.doc.find(d.id("Shell1"))).body() == d.id("Box1"));
        CHECK(d.doc.marker() == d.id("Box1"));
        d.doc.redo();
        CHECK(static_cast<const ShellFeature&>(*d.doc.find(d.id("Shell1"))).body() == d.id("Fillet1"));
    }
    SECTION("a duplicate goes in at the marker too") {
        d.run("timeline.rollback", {{"after", d.id("Box1")}});
        d.run("feature.duplicate", {{"feature", d.id("Box1")}});
        CHECK(d.order() == std::vector<std::string>{"Box1", "Box2", "Fillet1", "Shell1"});
        CHECK(d.doc.marker() == d.id("Box2"));
    }
    SECTION("a sketch on the box's face stays on the box") {
        d.run("timeline.rollback", {{"after", d.id("Box1")}});
        const auto front = d.pick("Box1", TopoKind::Face, [](const kernel::ElementInfo& e) {
            return e.geom == kernel::GeomKind::Plane && e.direction.y < -0.999;
        });
        d.run("feature.add_sketch", {{"body", d.id("Box1")}, {"face", front.front()}});
        CHECK(static_cast<const ShellFeature&>(*d.doc.find(d.id("Shell1"))).body() == d.id("Fillet1"));
    }
}

TEST_CASE("Moving a step keeps every step after the steps it uses", "[model][timeline][occt]") {
    Design d;
    d.tray();
    d.run(
        "feature.add_box",
        {{"size", kernel::Vec3{5, 5, 5}}, {"corner", kernel::Vec3{60, 0, 0}}, {"name", std::string("Pin")}});
    d.run("feature.add_cylinder",
          {{"radius", 3.0}, {"height", 4.0}, {"base", kernel::Vec3{80, 0, 0}}, {"name", std::string("Peg")}});
    CHECK(d.order() == std::vector<std::string>{"Box1", "Shell1", "Pin", "Peg"});

    d.run("timeline.move", {{"feature", d.id("Peg")}, {"before", d.id("Box1")}});
    CHECK(d.doc.undoLabel() == "Move Peg in the timeline");
    CHECK(d.order() == std::vector<std::string>{"Peg", "Box1", "Shell1", "Pin"});
    d.run("timeline.move", {{"feature", d.id("Peg")}, {"after", d.id("Shell1")}});
    CHECK(d.order() == std::vector<std::string>{"Box1", "Shell1", "Peg", "Pin"});
    d.run("timeline.move", {{"feature", d.id("Peg")}, {"to", std::string("end")}});
    CHECK(d.order() == std::vector<std::string>{"Box1", "Shell1", "Pin", "Peg"});
    d.doc.undo();
    CHECK(d.order() == std::vector<std::string>{"Box1", "Shell1", "Peg", "Pin"});
    REQUIRE(d.rebuild().ok());

    SECTION("not before a step it uses, nor after a step that uses it") {
        auto issues = d.tryRun("timeline.move", {{"feature", d.id("Shell1")}, {"to", std::string("start")}});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].message ==
              "Shell1 uses Box1, so it has to come after Box1. Put it somewhere after Box1.");
        issues = d.tryRun("timeline.move", {{"feature", d.id("Box1")}, {"to", std::string("end")}});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].message ==
              "Shell1 uses Box1, so Box1 has to come before Shell1. Put it somewhere before Shell1.");
        CHECK(d.order() == std::vector<std::string>{"Box1", "Shell1", "Peg", "Pin"});
    }
    SECTION("already there, or before itself") {
        auto issues = d.tryRun("timeline.move", {{"feature", d.id("Peg")}, {"after", d.id("Shell1")}});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].message == "Peg is already there.");
        issues = d.tryRun("timeline.move", {{"feature", d.id("Peg")}, {"before", d.id("Peg")}});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].message == "A step can't go before or after itself. Pick another step.");
    }
    SECTION("the marker stays after the same step") {
        d.run("timeline.rollback", {{"after", d.id("Shell1")}});
        // Pin is after the marker; moved before it, it's built.
        d.run("timeline.move", {{"feature", d.id("Pin")}, {"to", std::string("start")}});
        CHECK(d.doc.markerIndex() == 3);
        CHECK_FALSE(d.doc.afterMarker(d.id("Pin")));
        // The marker's own step moves: the marker stays after the one before.
        d.run("timeline.move", {{"feature", d.id("Shell1")}, {"to", std::string("end")}});
        CHECK(d.order() == std::vector<std::string>{"Pin", "Box1", "Peg", "Shell1"});
        CHECK(d.doc.marker() == d.id("Box1"));
        CHECK(d.doc.afterMarker(d.id("Shell1")));
        d.doc.undo();
        CHECK(d.doc.marker() == d.id("Shell1"));
    }
    SECTION("folders: it leaves one it's moved out of, and joins one it's put inside") {
        d.run("folder.add", {{"steps", std::vector<FeatureId>{d.id("Box1"), d.id("Shell1")}},
                             {"name", std::string("Tray")}});
        const auto tray = d.doc.findFolder("Tray")->id;
        d.run("timeline.move", {{"feature", d.id("Pin")}, {"after", d.id("Box1")}});
        CHECK(d.order() == std::vector<std::string>{"Box1", "Pin", "Shell1", "Peg"});
        CHECK(d.doc.folderOf(d.id("Pin")) == tray);
        CHECK(d.doc.findFolder("Tray")->steps ==
              std::vector<FeatureId>{d.id("Box1"), d.id("Pin"), d.id("Shell1")});
        d.run("timeline.move", {{"feature", d.id("Pin")}, {"to", std::string("end")}});
        CHECK(d.doc.folderOf(d.id("Pin")) == FolderId{});
        CHECK(d.doc.findFolder("Tray")->steps == std::vector<FeatureId>{d.id("Box1"), d.id("Shell1")});
        // Next to its folder, at its edge, a step stays in it.
        d.run("timeline.move", {{"feature", d.id("Shell1")}, {"after", d.id("Peg")}});
        CHECK(d.order() == std::vector<std::string>{"Box1", "Peg", "Shell1", "Pin"});
        CHECK(d.doc.folderOf(d.id("Shell1")) == FolderId{});  // Peg is between them now
        d.doc.undo();
        CHECK(d.doc.findFolder("Tray")->steps == std::vector<FeatureId>{d.id("Box1"), d.id("Shell1")});
    }
}

TEST_CASE("The rollback marker is saved with the design", "[model][timeline][occt][file]") {
    Design d;
    d.tray();
    const auto path = std::filesystem::temp_directory_path() / "sb-test-timeline.sb";
    auto reopen = [&] {
        REQUIRE(io::saveFile(d.doc, path).empty());
        auto opened = io::openFile(path);
        REQUIRE(opened.ok());
        CHECK_FALSE(opened.document->canUndo());  // opening isn't an undo step
        CHECK_FALSE(opened.document->isModified());
        return std::move(opened.document);
    };
    CHECK_FALSE(reopen()->rolledBack());

    d.run("timeline.rollback", {{"after", d.id("Box1")}});
    auto opened = reopen();
    CHECK(opened->marker() == d.id("Box1"));
    CHECK(opened->regenerate(*d.kernel, d.solver.get()).result(d.id("Shell1"))->status ==
          FeatureStatus::RolledBack);

    d.run("timeline.rollback", {{"to", std::string("start")}});
    CHECK(reopen()->markerIndex() == 0);
    std::filesystem::remove(path);
}
