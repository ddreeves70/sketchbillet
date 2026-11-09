// The Shell step: a body hollowed out to walls of a thickness, with some faces
// left open. Every volume is worked out by hand.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
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
    FeatureId last() const { return doc.featureAt(doc.featureCount() - 1).id(); }
    RebuildReport rebuild() { return doc.regenerate(*kernel, solver.get()); }
    double volume(const RebuildReport& r, const FeatureId& id) {
        return kernel->massProps(*r.shapeOf(id)).volume;
    }

    // A flat face of a step's body, by which way it faces.
    GeometryRef faceFacing(const FeatureId& body, kernel::Vec3 d) {
        const auto report = rebuild();
        const auto& names = *report.result(body)->names;
        for (std::size_t i = 0; i < names.elements.faces.size(); ++i) {
            const auto& f = names.elements.faces[i];
            if (f.geom == kernel::GeomKind::Plane &&
                f.direction.x * d.x + f.direction.y * d.y + f.direction.z * d.z > 0.999)
                return referTo(names, TopoKind::Face, static_cast<int>(i));
        }
        FAIL("no face that way");
        return {};
    }
};

}  // namespace

TEST_CASE("Shell hollows a box into a tray, open at the top", "[model][shell][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{40, 30, 20}}});
    const auto box = d.last();
    d.run("feature.add_shell", {{"body", box}, {"faces", std::vector{d.faceFacing(box, {0, 0, 1})}}});
    const auto shell = d.last();
    CHECK(d.doc.find(shell)->name() == "Shell1");
    CHECK(d.doc.undoLabel() == "Add Shell1");
    auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector<FeatureId>{shell});
    // 40 x 30 x 20 less the inside, 36 x 26 x 18.
    CHECK(d.volume(report, shell) == Approx(24000 - 36 * 26 * 18));
    CHECK(d.kernel->topology(*report.shapeOf(shell)).faces == 11);

    SECTION("its inside walls have words") {
        const auto& names = *report.result(shell)->names;
        std::vector<std::string> words;
        for (const auto& n : names.faces) words.push_back(describeElement(d.doc, TopoKind::Face, n));
        CHECK(std::count(words.begin(), words.end(), "inside of bottom of Box1 (Shell1)") == 1);
        CHECK(std::count(words.begin(), words.end(), "inside of left side of Box1 (Shell1)") == 1);
        CHECK(std::count(words.begin(), words.end(), "top of Box1") == 1);  // the rim
    }
    SECTION("thicker walls, by name") {
        d.run("feature.set_value",
              {{"feature", shell}, {"setting", std::string("thickness")}, {"value", 5.0}});
        report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, shell) == Approx(24000 - 30 * 20 * 15));
        d.doc.undo();
        CHECK(d.volume(d.rebuild(), shell) == Approx(24000 - 36 * 26 * 18));
    }
    SECTION("it follows the box") {
        d.run("feature.set_value", {{"feature", box}, {"setting", std::string("size_z")}, {"value", 30.0}});
        report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, shell) == Approx(36000 - 36 * 26 * 28));
    }
    SECTION("a later step can use an inside wall") {
        // A fillet needs edges; a face on the inside wall is enough to check it resolves.
        // The inside floor faces up, like the rim, but is 2 mm up, not 20 mm.
        const auto& names = *report.result(shell)->names;
        GeometryRef floor;
        for (std::size_t i = 0; i < names.elements.faces.size(); ++i) {
            const auto& f = names.elements.faces[i];
            if (f.direction.z > 0.999 && f.center.z < 10)
                floor = referTo(names, TopoKind::Face, static_cast<int>(i));
        }
        CHECK(floor.name.find("/inside:") != std::string::npos);
        d.run("feature.set_value",
              {{"feature", shell}, {"setting", std::string("thickness")}, {"value", 3.0}});
        report = d.rebuild();
        CHECK(resolve(floor, *report.result(shell)->names).status == ResolveStatus::Exact);
    }
}

TEST_CASE("Shell makes a cup and a tube from a cylinder", "[model][shell][occt]") {
    Design d;
    d.run("feature.add_cylinder", {{"radius", 20.0}, {"height", 30.0}});
    const auto rod = d.last();
    const auto top = d.faceFacing(rod, {0, 0, 1});
    SECTION("a cup, open at the top") {
        d.run("feature.add_shell", {{"body", rod}, {"faces", std::vector{top}}, {"thickness", 2.0}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        // Outside less the inside: radius 18, 28 tall.
        CHECK(d.volume(report, d.last()) == Approx(kPi * (400 * 30 - 324 * 28)));
    }
    SECTION("a tube, open at both ends") {
        d.run(
            "feature.add_shell",
            {{"body", rod}, {"faces", std::vector{top, d.faceFacing(rod, {0, 0, -1})}}, {"thickness", 2.0}});
        const auto report = d.rebuild();
        REQUIRE(report.ok());
        CHECK(d.volume(report, d.last()) == Approx(kPi * (400 - 324) * 30));
    }
}

TEST_CASE("Shell explains what's wrong, and how to fix it", "[model][shell][occt]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{20, 20, 20}}});
    const auto box = d.last();
    const auto top = d.faceFacing(box, {0, 0, 1});
    SECTION("no faces") {
        const auto issues =
            d.tryRun("feature.add_shell", {{"body", box}, {"faces", std::vector<GeometryRef>{}}});
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].message == "Shell needs at least one face in faces. Pick the faces to change.");
    }
    SECTION("walls 0 mm thick") {
        d.run("feature.add_shell", {{"body", box}, {"faces", std::vector{top}}, {"thickness", 0.0}});
        const auto report = d.rebuild();
        CHECK(report.result(d.last())->issues.at(0).message ==
              "Shell1 needs walls thicker than 0 mm. Type a thickness in Properties, like 2.");
    }
    SECTION("walls too thick to fit") {
        d.run("feature.add_shell", {{"body", box}, {"faces", std::vector{top}}, {"thickness", 12.0}});
        const auto report = d.rebuild();
        CHECK(report.result(d.last())->issues.at(0).code == "kernel.shell.failed");
    }
    SECTION("a face that's gone") {
        GeometryRef gone = top;
        gone.name = "nothing/like:this";
        gone.signature.size = 1;
        gone.signature.center = {100, 100, 100};
        d.doc.execute(std::make_unique<AddFeatureCommand>(
            std::make_unique<ShellFeature>("Shell1", box, std::vector{gone}, 2.0)));
        const auto report = d.rebuild();
        const auto& issue = report.result(d.last())->issues.at(0);
        CHECK(issue.code == "shell.face_missing");
        CHECK(issue.message ==
              "Shell1 can't find a face it leaves open: an earlier change took it away. Delete Shell1 and "
              "add it "
              "again with the faces to leave open.");
    }
}

TEST_CASE("Shell saves and opens", "[model][shell][occt][file]") {
    Design d;
    d.run("feature.add_box", {{"size", kernel::Vec3{40, 30, 20}}});
    const auto box = d.last();
    d.run("feature.add_shell",
          {{"body", box},
           {"faces", std::vector{d.faceFacing(box, {0, 0, 1}), d.faceFacing(box, {1, 0, 0})}},
           {"thickness", 1.5}});
    const auto shell = d.last();
    const double before = d.volume(d.rebuild(), shell);
    const auto path = std::filesystem::temp_directory_path() / "sb-test-shell.sb";
    REQUIRE(io::saveFile(d.doc, path).empty());
    auto opened = io::openFile(path);
    std::filesystem::remove(path);
    REQUIRE(opened.ok());
    const auto& s = static_cast<const ShellFeature&>(*opened.document->find(shell));
    CHECK(s.body() == box);
    CHECK(s.thickness() == 1.5);
    CHECK(s.faces() == static_cast<const ShellFeature&>(*d.doc.find(shell)).faces());
    const auto report = opened.document->regenerate(*d.kernel, d.solver.get());
    REQUIRE(report.ok());
    CHECK(d.kernel->massProps(*report.shapeOf(shell)).volume == Approx(before));
    // Open at the top and the right: 40 x 30 x 20 less 38.5 x 27 x 18.5.
    CHECK(before == Approx(24000 - 38.5 * 27 * 18.5));
}
