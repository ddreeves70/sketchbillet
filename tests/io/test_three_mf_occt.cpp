// 3MF export of real designs: every body comes out as a closed skin with the
// right volume, named after its step and made of its material.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

#include "ThreeMfReader.h"
#include "sb/io/ThreeMf.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;

namespace {

struct Design {
    Document doc{DocKind::Part, "Bracket"};
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();
    std::unique_ptr<sketch::ISketchSolver> solver = sketch::makePlaneGcsSolver();

    FeatureId run(std::string_view name, Args args) {
        auto made = CommandRegistry::builtin().create(name, args, doc);
        INFO(name << ": " << (made.ok() ? std::string() : made.issues.front().message));
        REQUIRE(made.ok());
        REQUIRE(doc.execute(std::move(made.command)).empty());
        return doc.featureAt(doc.featureCount() - 1).id();
    }
    // Exports a step and reads the file back.
    test::ThreeMfPart exported(const RebuildReport& report, const FeatureId& step) {
        const auto path = std::filesystem::temp_directory_path() / "sb_test_part.3mf";
        REQUIRE(io::export3mf(doc, report, *kernel, step, path).empty());
        std::ifstream f(path, std::ios::binary);
        const io::Bytes bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        std::filesystem::remove(path);
        auto parts = test::readParts(bytes);
        REQUIRE(parts.size() == 1);
        CHECK(test::entry(bytes, "3D/3dmodel.model").find(">Bracket</metadata>") != std::string::npos);
        return parts.front();
    }
};

}  // namespace

TEST_CASE("3MF export of a cut, rounded and revolved design gives closed skins", "[io][3mf][occt]") {
    Design d;
    // A plate with a hole, its corners rounded.
    const auto plate = d.run("feature.add_box", {{"size", kernel::Vec3{60, 30, 6}}, {"name", std::string("Plate")}});
    const auto pin = d.run("feature.add_cylinder", {{"radius", 4.0}, {"height", 10.0}, {"base", kernel::Vec3{15, 15, -2}}});
    const auto cut = d.run("feature.add_boolean", {{"operation", std::string("cut")}, {"a", plate}, {"b", pin}});
    // A ring, revolved from a rectangle beside the z axis, off to one side.
    const auto sketch = d.run("feature.add_sketch", {{"plane", std::string("xz")}});
    d.run("sketch.add_rectangle", {{"sketch", sketch}, {"from", sketch::Point2{8, 0}}, {"to", sketch::Point2{12, 5}}});
    const auto ring = d.run("feature.add_revolve", {{"sketch", sketch}, {"axis", std::string("y")}});
    REQUIRE(d.doc.execute(std::make_unique<SetMaterialCommand>(ring, "petg")).empty());

    auto report = d.doc.regenerate(*d.kernel, d.solver.get());
    REQUIRE(report.ok());
    const auto volume = [&](const FeatureId& id) { return d.kernel->massProps(*report.shapeOf(id)).volume; };

    const auto holed = d.exported(report, cut);
    CHECK(holed.name == "Cut1");
    CHECK(holed.material == "PLA");
    CHECK(holed.badEdges() == 0);
    // Meshed 0.05 mm from the true surface, the hole's polygon cuts a little less.
    CHECK(holed.volume() == Approx(volume(cut)).epsilon(0.001));

    const auto revolved = d.exported(report, ring);
    CHECK(revolved.material == "PETG");
    CHECK(revolved.badEdges() == 0);
    CHECK(revolved.volume() == Approx(volume(ring)).epsilon(0.01));
}

TEST_CASE("3MF export of a step that didn't build says so", "[io][3mf][occt]") {
    Design d;
    const auto box = d.run("feature.add_box", {{"size", kernel::Vec3{10, 10, 10}}});
    const auto report = d.doc.regenerate(*d.kernel, d.solver.get());
    const auto issues = io::export3mf(d.doc, report, *d.kernel, FeatureId::generate(), "never.3mf");
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "export.nothing");
    CHECK_FALSE(std::filesystem::exists("never.3mf"));
    (void)box;
}
