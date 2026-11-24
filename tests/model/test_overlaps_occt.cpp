// Checking an assembly: components that overlap, and its mass and balance point.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <numbers>

#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/AssemblyMass.h"
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

TEST_CASE("an assembly's mass is each component's, and it balances where its mass is, as placed",
          "[model][assembly][mass][occt]") {
    Assembly box;
    // PLA, 1.24 g/cm^3: 4000 mm^3 is 4.96 g, and 1000 mm^3 is 1.24 g.
    box.insert(boxPart("Base", {20, 20, 10}));
    const auto cube = box.insert(boxPart("Cube", {10, 10, 10}), at(100, 0, 0));
    const auto report = box.doc.regenerate(*box.kernel);
    auto mass = assemblyMass(box.doc, report, *box.kernel);
    CHECK(mass.volume == Approx(5000));
    CHECK(mass.grams == Approx(6.2));
    CHECK(mass.newtons == Approx(6.2 / 1000 * 9.81));  // g is 9.81 m/s^2
    CHECK(mass.missing == 0);
    // Base's middle is at 10, 10, 5 mm and Cube's at 105, 5, 5 mm: weighed by
    // their masses, it balances at 29, 9, 5 mm.
    CHECK(mass.center.x == Approx(29));
    CHECK(mass.center.y == Approx(9));
    CHECK(mass.center.z == Approx(5));

    // Turned a quarter turn about z, Cube's middle moves to 95, 5, 5 mm; and a
    // second Cube, at 0, 0, 10 mm on top of Base, adds 1.24 g at 5, 5, 15 mm.
    Placement turned = at(100, 0, 0);
    turned.xAxis = {0, 1, 0};
    REQUIRE(box.doc.execute(std::make_unique<PlaceComponentCommand>(cube, turned)).empty());
    REQUIRE(box.doc
                .execute(std::make_unique<AddComponentsCommand>(box.doc.findComponent(cube)->part, 1,
                                                                at(0, 0, 10), Vec3{}))
                .empty());
    mass = assemblyMass(box.doc, box.doc.regenerate(*box.kernel), *box.kernel);
    CHECK(mass.grams == Approx(7.44));
    CHECK(mass.center.x == Approx((4.96 * 10 + 1.24 * 95 + 1.24 * 5) / 7.44));
    CHECK(mass.center.y == Approx((4.96 * 10 + 1.24 * 5 + 1.24 * 5) / 7.44));
    CHECK(mass.center.z == Approx((4.96 * 5 + 1.24 * 5 + 1.24 * 15) / 7.44));

    // An empty part counts as missing, and adds nothing.
    box.insert(std::make_unique<Document>(DocKind::Part, "Later"));
    const auto more = assemblyMass(box.doc, box.doc.regenerate(*box.kernel), *box.kernel);
    CHECK(more.missing == 1);
    CHECK(more.grams == Approx(7.44));
}

TEST_CASE("an empty assembly weighs nothing and balances at its origin", "[model][assembly][mass][occt]") {
    Assembly box;
    const auto mass = assemblyMass(box.doc, box.doc.regenerate(*box.kernel), *box.kernel);
    CHECK(mass.grams == 0);
    CHECK(mass.center.x == 0);
    CHECK(mass.missing == 0);
}

TEST_CASE("a press fit is meant to overlap: the check leaves it out, and says when one doesn't overlap",
          "[model][assembly][overlaps][occt]") {
    Assembly box;
    const auto base = box.insert(boxPart("Base", {20, 20, 10}));
    const auto lid = box.insert(boxPart("Lid", {10, 10, 10}), at(15, 15, 5));      // 125 mm^3 into Base
    const auto side = box.insert(boxPart("Side", {10, 10, 10}), at(20, 0, 0));     // touches Base
    const auto spare = box.insert(boxPart("Spare", {10, 10, 10}), at(100, 0, 0));  // far away

    REQUIRE(box.doc.execute(std::make_unique<AddPressFitCommand>(PressFit{{}, lid, base})).empty());
    CHECK(box.doc.pressFits().size() == 1);
    CHECK_FALSE(box.doc.pressFits()[0].id.isNil());
    CHECK(box.doc.findPressFit(base, lid) == &box.doc.pressFits()[0]);  // either way round
    CHECK(box.doc.findPressFit(base, side) == nullptr);
    CHECK(box.doc.undoLabel() == "Mark press fit");
    auto check = box.check();
    CHECK(check.clear());
    CHECK(check.overlaps.empty());
    REQUIRE(check.pressFits.size() == 1);
    CHECK(check.pressFits[0].volume == Approx(125));
    REQUIRE(check.issues.size() == 2);
    CHECK(check.issues[0].message ==
          "Box: no components overlap, but for 1 press fit (4 components, 2 pairs close enough to check).");
    CHECK(check.issues[1].code == "assembly.press_fit");
    CHECK(check.issues[1].severity == Severity::Info);
    CHECK(check.issues[1].message ==
          "Base 1 and Lid 1 overlap by 125.00 mm^3, around x 17.5, y 17.5, z 7.5 mm: they're a press fit, so "
          "that's meant.");

    // Marked twice, or with itself: refused.
    auto again = box.doc.execute(std::make_unique<AddPressFitCommand>(PressFit{{}, base, lid}));
    REQUIRE(again.size() == 1);
    CHECK(again[0].code == "press_fit.exists");
    CHECK(box.doc.execute(std::make_unique<AddPressFitCommand>(PressFit{{}, base, base}))[0].code ==
          "press_fit.same");

    // Press fits that don't overlap, touching or far apart, will be loose.
    REQUIRE(box.doc.execute(std::make_unique<AddPressFitCommand>(PressFit{{}, base, side})).empty());
    REQUIRE(box.doc.execute(std::make_unique<AddPressFitCommand>(PressFit{{}, base, spare})).empty());
    check = box.check();
    CHECK_FALSE(check.clear());
    int loose = 0;
    for (const auto& i : check.issues) {
        if (i.code != "assembly.press_fit_loose") continue;
        ++loose;
        CHECK(i.severity == Severity::Warning);
    }
    CHECK(loose == 2);
    CHECK(check.issues.back().message ==
          "Base 1 and Spare 1 are marked as a press fit, but they don't overlap, so they'll be loose. Make "
          "one a "
          "little bigger so they overlap, or unmark the press fit if loose is what you want.");

    // Unmarked, it overlaps again; undo marks it again.
    REQUIRE(box.doc.execute(std::make_unique<DeletePressFitCommand>(base, lid)).empty());
    CHECK(box.doc.undoLabel() == "Unmark press fit");
    CHECK(box.check().overlaps.size() == 1);
    CHECK(box.doc.execute(std::make_unique<DeletePressFitCommand>(base, lid))[0].code == "press_fit.missing");
    REQUIRE(box.doc.undo());
    CHECK(box.doc.findPressFit(lid, base));
    CHECK(box.doc.pressFits()[0].a == lid);  // back where it was

    // Deleting a component takes its press fits with it; undo puts them back.
    REQUIRE(box.doc.execute(std::make_unique<DeleteComponentCommand>(base)).empty());
    CHECK(box.doc.pressFits().empty());
    REQUIRE(box.doc.undo());
    REQUIRE(box.doc.pressFits().size() == 3);
    CHECK(box.doc.pressFits()[0].b == base);
    CHECK(box.doc.pressFits()[2].b == spare);
}
