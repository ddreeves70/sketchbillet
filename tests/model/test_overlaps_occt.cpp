// Checking an assembly for components that overlap.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <numbers>

#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/model/Overlaps.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;
using kernel::Placement;
using kernel::Vec3;

namespace {

std::unique_ptr<Document> boxPart(const std::string& name, Vec3 size) {
    auto part = std::make_unique<Document>(DocKind::Part, name);
    REQUIRE(
        part->execute(std::make_unique<AddFeatureCommand>(std::make_unique<BoxFeature>("Box1", Vec3{}, size)))
            .empty());
    return part;
}

struct Assembly {
    Document doc{DocKind::Part, "Box"};
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();

    Assembly() { REQUIRE(doc.execute(std::make_unique<MakeAssemblyCommand>()).empty()); }

    ComponentId insert(std::unique_ptr<Document> part, kernel::Placement at = {}) {
        auto cmd = std::make_unique<InsertPartCommand>(std::move(part), 1, at);
        auto* raw = cmd.get();
        REQUIRE(doc.execute(std::move(cmd)).empty());
        return raw->added().front();
    }

    OverlapCheck check() { return checkOverlaps(doc, doc.regenerate(*kernel), *kernel); }
};

Placement at(double x, double y, double z) {
    Placement p;
    p.origin = {x, y, z};
    return p;
}

}  // namespace

TEST_CASE("overlapping components are found, with how much they share and where; touching ones aren't",
          "[model][assembly][overlaps][occt]") {
    Assembly box;
    const auto base = box.insert(boxPart("Base", {20, 20, 10}));
    // A corner of Lid sinks into Base: 5 x 5 x 5 mm.
    const auto lid = box.insert(boxPart("Lid", {10, 10, 10}), at(15, 15, 5));
    // Side only touches Base's face at x 20 mm.
    box.insert(boxPart("Side", {10, 10, 10}), at(20, 0, 0));
    // Far away: not even checked.
    box.insert(boxPart("Spare", {10, 10, 10}), at(100, 0, 0));
    // Turned a quarter turn about z, at x 5 mm: it fills x -5 to 5, y 0 to 10,
    // so its half from x 0 to 5 is inside Base.
    Placement turned = at(5, 0, 0);
    turned.xAxis = {0, 1, 0};
    const auto peg = box.insert(boxPart("Peg", {10, 10, 10}), turned);

    const auto check = box.check();
    CHECK_FALSE(check.clear());
    CHECK(check.pairsChecked == 3);  // Base with Lid, Side and Peg
    REQUIRE(check.overlaps.size() == 2);
    const auto& first = check.overlaps[0];
    CHECK(first.a == base);
    CHECK(first.b == lid);
    CHECK(first.volume == Approx(125));
    CHECK(first.center.x == Approx(17.5));
    CHECK(first.center.y == Approx(17.5));
    CHECK(first.center.z == Approx(7.5));
    REQUIRE(first.shapes.size() == 1);
    const auto bounds = box.kernel->bounds(first.shapes[0]);
    CHECK(bounds.min.x == Approx(15).margin(1e-6));
    CHECK(bounds.max.z == Approx(10).margin(1e-6));
    CHECK(box.kernel->massProps(first.shapes[0]).volume == Approx(125));
    const auto& second = check.overlaps[1];
    CHECK(second.a == base);
    CHECK(second.b == peg);
    CHECK(second.volume == Approx(500));
    CHECK(second.center.x == Approx(2.5));

    REQUIRE(check.issues.size() == 2);
    CHECK(check.issues[0].code == "assembly.overlap");
    CHECK(check.issues[0].severity == Severity::Warning);
    CHECK(check.issues[0].message ==
          "Base 1 and Lid 1 overlap by 125.00 mm^3, around x 17.5, y 17.5, z 7.5 mm. Parts can't be in the "
          "same "
          "place, so they won't go together. Move one of them, or change one of their parts so they fit.");
}

TEST_CASE("an assembly with nothing overlapping says so, in its own unit",
          "[model][assembly][overlaps][occt]") {
    Assembly box;
    box.insert(boxPart("Base", {20, 20, 10}));
    box.insert(boxPart("Lid", {20, 20, 2}), at(0, 0, 10));  // sits on top
    auto check = box.check();
    CHECK(check.clear());
    CHECK(check.overlaps.empty());
    REQUIRE(check.issues.size() == 1);
    CHECK(check.issues[0].code == "assembly.no_overlaps");
    CHECK(check.issues[0].severity == Severity::Info);
    CHECK(check.issues[0].message ==
          "Box: no components overlap (2 components, 1 pair close enough to check).");

    // Pushed 1 mm down into it: 400 mm^3, in inches.
    REQUIRE(box.doc.execute(std::make_unique<SetUnitsCommand>(LengthUnit::Inch)).empty());
    REQUIRE(box.doc.execute(std::make_unique<PlaceComponentCommand>(box.doc.components()[1].id, at(0, 0, 9)))
                .empty());
    check = box.check();
    REQUIRE(check.overlaps.size() == 1);
    CHECK(check.overlaps[0].volume == Approx(400));
    CHECK(check.issues[0].message.starts_with("Base 1 and Lid 1 overlap by 0.02 in^3, around x 0.394, "));
}
