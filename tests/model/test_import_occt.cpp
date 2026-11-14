// Importing parts from STEP files (feature.import, feature.reload_import), with
// real geometry: the part keeps its size, steps can build on it, it's saved in
// the design, and reading the file again follows changes to it.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>

#include "sb/io/SketchBilletFile.h"
#include "sb/io/Steps.h"
#include "sb/io/ThreeMf.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/model/Import.h"
#include "sb/model/Material.h"
#include "sb/model/Naming.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;

namespace {

std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

struct Design {
    // A folder of its own: CTest runs test cases side by side.
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("sb_test_import_" + FeatureId::generate().toString());
    Document doc;
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();
    std::unique_ptr<sketch::ISketchSolver> solver = sketch::makePlaneGcsSolver();

    Design() {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }
    ~Design() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    // Writes a box (with a hole, if `hole` > 0) as a STEP file in the test folder.
    std::string writeStep(const std::string& name, kernel::Vec3 size, double hole = 0) {
        auto part = kernel->makeBox({0, 0, 0}, size.x, size.y, size.z).shape;
        if (hole > 0) {
            const auto rod = kernel->makeCylinder({size.x / 2, size.y / 2, -1}, {0, 0, 1}, hole, size.z + 2);
            part = kernel->boolean(kernel::BoolOp::Cut, part, rod.shape).shape;
        }
        const auto file = dir / name;
        REQUIRE(kernel->exportFile(part, kernel::ExportFormat::Step, file).empty());
        return utf8(file);
    }
    Diagnostics run(std::string_view name, Args args) {
        auto made = CommandRegistry::builtin().create(name, args, doc);
        if (!made.ok()) return made.issues;
        return doc.execute(std::move(made.command));
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
};

}  // namespace

TEST_CASE("An imported part is a body, named after its file", "[model][import][occt]") {
    Design d;
    const auto file = d.writeStep("bracket.step", {40, 20, 6}, 3);
    REQUIRE(d.run("feature.import", {{"file", file}}).empty());
    auto report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector{d.id("bracket")});
    CHECK(d.volume(report, "bracket") == Approx(40 * 20 * 6 - std::numbers::pi * 9 * 6));
    const auto& im = static_cast<const ImportFeature&>(*d.doc.find(d.id("bracket")));
    CHECK(im.fileName() == "bracket.step");
    CHECK(im.source() == file);
    CHECK(d.doc.undoLabel() == "Add bracket");

    // The same file again: bracket1. A name can be given too.
    REQUIRE(d.run("feature.import", {{"file", file}}).empty());
    CHECK(d.doc.featureAt(1).name() == "bracket1");
    REQUIRE(d.run("feature.import", {{"file", file}, {"name", std::string("Spare")}}).empty());
    CHECK(d.doc.featureAt(2).name() == "Spare");
    // Its faces have names of their own, told apart by where they are.
    report = d.rebuild();
    const auto& names = *report.result(d.id("bracket"))->names;
    CHECK(names.faces.size() == 7);
    std::vector<std::string> sorted = names.faces;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
}

TEST_CASE("Steps build on an imported part, and follow it when its file is read again",
          "[model][import][occt]") {
    Design d;
    const auto file = d.writeStep("plate.step", {40, 20, 6});
    REQUIRE(d.run("feature.import", {{"file", file}}).empty());
    auto report = d.rebuild();
    // A hole drilled down from its top face.
    const auto& names = *report.result(d.id("plate"))->names;
    int top = -1;
    for (std::size_t i = 0; i < names.elements.faces.size(); ++i)
        if (names.elements.faces[i].direction.z > 0.999) top = static_cast<int>(i);
    REQUIRE(top >= 0);
    REQUIRE(d.run("feature.add_sketch",
                  {{"body", d.id("plate")}, {"face", referTo(names, kernel::TopoKind::Face, top)}})
                .empty());
    REQUIRE(d.run("sketch.add_point", {{"sketch", d.last()}, {"at", sketch::Point2{10, 10}}}).empty());
    REQUIRE(d.run("feature.add_hole", {{"sketch", d.last()}, {"diameter", 4.0}}).empty());
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, "Hole1") == Approx(40 * 20 * 6 - std::numbers::pi * 4 * 6));

    // The other program makes it 10 mm thick: reading it again, the hole goes
    // through the thicker plate.
    d.writeStep("plate.step", {40, 20, 10});
    REQUIRE(d.run("feature.reload_import", {{"feature", d.id("plate")}}).empty());
    CHECK(d.doc.undoLabel() == "Edit plate");
    report = d.rebuild();
    REQUIRE(report.ok());
    CHECK(d.volume(report, "Hole1") == Approx(40 * 20 * 10 - std::numbers::pi * 4 * 10));
    // Undo puts the old contents back.
    REQUIRE(d.doc.undo());
    report = d.rebuild();
    CHECK(d.volume(report, "plate") == Approx(40 * 20 * 6));
}

TEST_CASE("An imported part is saved in the design, and opens without its file", "[model][import][occt]") {
    Design d;
    const auto file = d.writeStep("knob.stp", {12, 12, 12}, 2);
    REQUIRE(d.run("feature.import", {{"file", file}}).empty());
    const auto before = static_cast<const ImportFeature&>(*d.doc.find(d.id("knob"))).content();
    io::Bytes bytes;
    REQUIRE(io::writeBytes(d.doc, bytes).empty());
    std::filesystem::remove(d.dir / "knob.stp");

    auto opened = io::readBytes(bytes, "knob.sb");
    REQUIRE(opened.ok());
    auto& doc = *opened.document;
    const auto& im = static_cast<const ImportFeature&>(doc.featureAt(0));
    CHECK(im.content() == before);  // byte for byte
    CHECK(im.source() == file);
    const auto report = doc.regenerate(*d.kernel, d.solver.get());
    REQUIRE(report.ok());
    CHECK(d.kernel->massProps(*report.shapeOf(im.id())).volume ==
          Approx(12 * 12 * 12 - std::numbers::pi * 4 * 12));

    // Reading it again now says where it looked.
    const auto problems = [&] {
        auto made = CommandRegistry::builtin().create("feature.reload_import", {{"feature", im.id()}}, doc);
        REQUIRE(made.ok());
        return doc.execute(std::move(made.command));
    }();
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().code == "import.no_file");
    CHECK(problems.front().message.find("knob.stp") != std::string::npos);
}

TEST_CASE("Copied and pasted, an imported part brings its file", "[model][import][occt]") {
    Design d;
    REQUIRE(d.run("feature.import", {{"file", d.writeStep("block.step", {5, 6, 7})}}).empty());
    const auto text = io::stepsToText(d.doc, {d.id("block")});
    std::filesystem::remove(d.dir / "block.step");
    auto pasted = io::stepsFromText(text);
    REQUIRE(pasted.issues.empty());
    REQUIRE(pasted.steps.size() == 1);
    const auto& im = static_cast<const ImportFeature&>(*pasted.steps.front());
    CHECK(im.content() == static_cast<const ImportFeature&>(*d.doc.find(d.id("block"))).content());
    CHECK(d.kernel->massProps(d.kernel->importStep(im.content()).shape).volume == Approx(5 * 6 * 7));
}

TEST_CASE("Importing tells the student what went wrong", "[model][import][occt]") {
    Design d;
    auto problems = d.run("feature.import", {{"file", utf8(d.dir / "missing.step")}});
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().code == "import.no_file");
    problems = d.run("feature.import", {{"file", utf8(d.dir / "part.dwg")}});
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().code == "import.format");
    CHECK(problems.front().message ==
          "SketchBillet can import STEP (.step or .stp), STL and 3MF files, so it can't import part.dwg. "
          "Export "
          "the part from the other program as one of those.");
    std::ofstream(d.dir / "empty.step").close();
    problems = d.run("feature.import", {{"file", utf8(d.dir / "empty.step")}});
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().code == "import.empty");
    CHECK(d.doc.featureCount() == 0);

    // A file with the right ending that isn't STEP inside: the step is added, and fails.
    std::ofstream(d.dir / "fake.step") << "solid fake\nendsolid fake\n";
    REQUIRE(d.run("feature.import", {{"file", utf8(d.dir / "fake.step")}}).empty());
    const auto report = d.rebuild();
    const auto* r = report.result(d.id("fake"));
    REQUIRE(r);
    CHECK(r->status == FeatureStatus::Failed);
    CHECK(r->issues.front().code == "import.step.unreadable");

    REQUIRE(d.run("feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}}).empty());
    problems = d.run("feature.reload_import", {{"feature", d.id("Box1")}});
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().code == "import.not_import");
}

TEST_CASE("A design with an imported part saved in Nov 2026 still opens and builds",
          "[model][import][occt]") {
    // tests/files/schema1-imported-part.sb: the demo tag, exported as STEP and imported
    // as Tag. Checked in, never made again: every SketchBillet must open it.
    auto opened = io::openFile(std::filesystem::path(SB_TEST_FILES_DIR) / "schema1-imported-part.sb");
    REQUIRE(opened.ok());
    auto& doc = *opened.document;
    REQUIRE(doc.featureCount() == 1);
    REQUIRE(doc.featureAt(0).type() == FeatureType::Import);
    CHECK(doc.featureAt(0).name() == "Tag");
    auto kernel = kernel::makeOcctKernel();
    const auto report = doc.regenerate(*kernel);
    REQUIRE(report.ok());
    // The tag: 60 x 30 x 4 mm, less a 5 mm hole 4 mm deep.
    CHECK(kernel->massProps(*report.shapeOf(doc.featureAt(0).id())).volume ==
          Approx(60 * 30 * 4 - std::numbers::pi * 2.5 * 2.5 * 4));
}

TEST_CASE("Parts come in from STL and 3MF files too, saved in the design", "[model][import][occt][mesh]") {
    Design d;
    // A 30 x 20 x 10 mm block, as STL (binary, from SketchBillet's own export) and as 3MF.
    const auto block = d.kernel->makeBox({0, 0, 0}, 30, 20, 10).shape;
    const auto stl = d.dir / "block.stl";
    REQUIRE(d.kernel->exportFile(block, kernel::ExportFormat::Stl, stl).empty());
    io::Bytes bytes;
    REQUIRE(
        io::write3mfBytes({{"Block", d.kernel->tessellate(block, kernel::MeshQuality{}), defaultMaterial()}},
                          "Block", bytes)
            .empty());
    std::ofstream(d.dir / "block.3mf", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    REQUIRE(d.run("feature.import", {{"file", utf8(stl)}}).empty());
    REQUIRE(d.run("feature.import", {{"file", utf8(d.dir / "block.3mf")}, {"name", std::string("Printed")}})
                .empty());
    // The same STL, in inches.
    REQUIRE(d.run("feature.import",
                  {{"file", utf8(stl)}, {"unit", std::string("in")}, {"name", std::string("Big")}})
                .empty());
    auto report = d.rebuild();
    REQUIRE(report.ok());
    for (const char* name : {"block", "Printed"}) {
        INFO(name);
        CHECK(d.volume(report, name) == Approx(6000));
        CHECK(d.kernel->topology(*report.shapeOf(d.id(name))).faces == 6);  // one face a side, not triangles
    }
    CHECK(d.volume(report, "Big") == Approx(6000 * 25.4 * 25.4 * 25.4));

    // The unit can be changed afterwards, like Properties does.
    auto inMm = d.doc.find(d.id("Big"))->clone();
    static_cast<ImportFeature&>(*inMm).setUnit(LengthUnit::Millimeter);
    REQUIRE(d.doc.execute(std::make_unique<EditFeatureCommand>(std::move(inMm))).empty());
    report = d.rebuild();
    CHECK(d.volume(report, "Big") == Approx(6000));
    REQUIRE(d.doc.undo());

    // Saved in the design byte for byte, with the unit; and on the clipboard.
    io::Bytes saved;
    REQUIRE(io::writeBytes(d.doc, saved).empty());
    auto opened = io::readBytes(saved, "blocks.sb");
    REQUIRE(opened.ok());
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& a = static_cast<const ImportFeature&>(d.doc.featureAt(i));
        const auto& b = static_cast<const ImportFeature&>(opened.document->featureAt(i));
        CHECK(b.format() == a.format());
        CHECK(b.content() == a.content());
        CHECK(b.unit() == a.unit());
    }
    CHECK(static_cast<const ImportFeature&>(opened.document->featureAt(2)).unit() == LengthUnit::Inch);
    auto pasted = io::stepsFromText(io::stepsToText(d.doc, {d.id("Printed"), d.id("Big")}));
    REQUIRE(pasted.issues.empty());
    REQUIRE(pasted.steps.size() == 2);
    CHECK(static_cast<const ImportFeature&>(*pasted.steps[0]).content() ==
          static_cast<const ImportFeature&>(*d.doc.find(d.id("Printed"))).content());
    CHECK(static_cast<const ImportFeature&>(*pasted.steps[1]).unit() == LengthUnit::Inch);

    // unit= is only for STL.
    const auto problems =
        d.run("feature.import", {{"file", utf8(d.dir / "block.3mf")}, {"unit", std::string("in")}});
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().code == "import.unit");
}
