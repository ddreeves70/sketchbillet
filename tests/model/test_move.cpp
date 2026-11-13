// Tests for the Move step: MoveFeature and feature.add_move.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <numbers>

#include "FakeKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;

namespace {

FeatureId add(Document& doc, std::unique_ptr<Feature> f) {
    const auto id = f->id();
    REQUIRE(doc.execute(std::make_unique<AddFeatureCommand>(std::move(f))).empty());
    return id;
}

kernel::Motion slide(double x, double y, double z) {
    kernel::Motion m;
    m.offset = {x, y, z};
    return m;
}

}  // namespace

TEST_CASE("a Move step uses up the body it moves, and keeps its volume", "[model][move]") {
    Document doc;
    const auto box = add(doc, std::make_unique<BoxFeature>("Box1", kernel::Vec3{}, kernel::Vec3{10, 10, 10}));
    const auto move = add(doc, std::make_unique<MoveFeature>("Move1", box, slide(20, 0, 0)));
    CHECK(doc.find(move)->dependencies() == std::vector<FeatureId>{box});

    test::FakeKernel k;
    const auto report = doc.regenerate(k);
    REQUIRE(report.ok());
    CHECK(test::FakeKernel::volumeOf(*report.shapeOf(move)) == 1000.0);
    CHECK(report.bodies() == std::vector<FeatureId>{move});  // Box1 isn't a finished body anymore
}

TEST_CASE("a Move of a step that's off is blocked", "[model][move]") {
    Document doc;
    const auto box = add(doc, std::make_unique<BoxFeature>("Box1", kernel::Vec3{}, kernel::Vec3{10, 10, 10}));
    const auto move = add(doc, std::make_unique<MoveFeature>("Move1", box, slide(1, 0, 0)));
    REQUIRE(
        doc.execute(std::make_unique<SuppressFeaturesCommand>(std::vector<FeatureId>{box}, true)).empty());
    test::FakeKernel k;
    CHECK(doc.regenerate(k).result(move)->status == FeatureStatus::Blocked);
}

TEST_CASE("a moved body is made of what the body it moves is made of", "[model][move][material]") {
    Document doc;
    const auto box = add(doc, std::make_unique<BoxFeature>("Box1", kernel::Vec3{}, kernel::Vec3{10, 10, 10}));
    const auto move = add(doc, std::make_unique<MoveFeature>("Move1", box, slide(1, 0, 0)));
    REQUIRE(doc.execute(std::make_unique<SetMaterialCommand>(box, "Brass (C360)")).empty());
    CHECK(doc.materialOf(move).name == "Brass (C360)");
    CHECK(doc.materialSource(move) == box);
}

TEST_CASE("copying a Move step copies the body it moves too", "[model][move][copy]") {
    Document doc;
    const auto box = add(doc, std::make_unique<BoxFeature>("Box1", kernel::Vec3{}, kernel::Vec3{10, 10, 10}));
    const auto move = add(doc, std::make_unique<MoveFeature>("Move1", box, slide(1, 2, 3)));
    const auto steps = copySteps(doc, {move});
    REQUIRE(steps.size() == 2);
    const auto& copy = dynamic_cast<const MoveFeature&>(*steps[1]);
    CHECK(copy.body() == steps[0]->id());
    CHECK(copy.motion().offset.y == 2.0);
}

TEST_CASE("a linked copy keeps the body it copies, and follows it", "[model][move][linked]") {
    Document doc;
    const auto box = add(doc, std::make_unique<BoxFeature>("Box1", kernel::Vec3{}, kernel::Vec3{10, 10, 10}));
    auto step = std::make_unique<MoveFeature>("Copy1", box, slide(20, 0, 0));
    step->setCopy(true);
    const auto copy = add(doc, std::move(step));
    CHECK(doc.find(copy)->dependencies() == std::vector<FeatureId>{box});
    CHECK(doc.find(copy)->referenceInputs() == std::vector<FeatureId>{box});

    test::FakeKernel k;
    auto report = doc.regenerate(k);
    REQUIRE(report.ok());
    CHECK(report.bodies() == std::vector<FeatureId>{box, copy});  // both are finished bodies
    CHECK(test::FakeKernel::volumeOf(*report.shapeOf(copy)) == 1000.0);
    REQUIRE(doc.execute(std::make_unique<SetMaterialCommand>(box, "Brass (C360)")).empty());
    CHECK(doc.materialOf(copy).name == "Brass (C360)");  // made of what the original is made of

    // The original changes; the copy follows.
    REQUIRE(doc.execute(std::make_unique<SetNumberCommand>(box, "size_z", 25.0)).empty());
    report = doc.regenerate(k, nullptr, &report);
    CHECK(test::FakeKernel::volumeOf(*report.shapeOf(copy)) == 2500.0);
    CHECK(report.reused().empty());

    SECTION("a Boolean of the original and its copy uses up both") {
        const auto both = add(doc, std::make_unique<BooleanFeature>("Both", kernel::BoolOp::Union, box, copy));
        report = doc.regenerate(k);
        REQUIRE(report.ok());
        CHECK(report.bodies() == std::vector<FeatureId>{both});
    }
    SECTION("turning it into a plain move uses the original up") {
        auto plain = doc.find(copy)->clone();
        static_cast<MoveFeature&>(*plain).setCopy(false);
        REQUIRE(doc.execute(std::make_unique<EditFeatureCommand>(std::move(plain))).empty());
        CHECK(doc.regenerate(k).bodies() == std::vector<FeatureId>{copy});
        doc.undo();
        CHECK(doc.regenerate(k).bodies() == std::vector<FeatureId>{box, copy});
    }
    SECTION("copying a linked copy copies its original too, like any step's inputs") {
        const auto steps = copySteps(doc, {copy});
        REQUIRE(steps.size() == 2);
        const auto& again = dynamic_cast<const MoveFeature&>(*steps[1]);
        CHECK(again.copy());
        CHECK(again.body() == steps[0]->id());
    }
}

TEST_CASE("feature.add_move with copy=true adds a linked copy named Copy1", "[model][move][linked][registry]") {
    Document doc;
    const auto box = add(doc, std::make_unique<BoxFeature>("Box1", kernel::Vec3{}, kernel::Vec3{10, 10, 10}));
    auto made = CommandRegistry::builtin().create(
        "feature.add_move", {{"body", box}, {"by", kernel::Vec3{0, 15, 0}}, {"copy", true}}, doc);
    REQUIRE(made.ok());
    REQUIRE(doc.execute(std::move(made.command)).empty());
    const auto& m = dynamic_cast<const MoveFeature&>(doc.featureAt(1));
    CHECK(m.name() == "Copy1");
    CHECK(m.copy());
    CHECK(doc.undoLabel() == "Add Copy1");
}

TEST_CASE("feature.add_move takes degrees and stores radians", "[model][move][registry]") {
    Document doc;
    const auto box = add(doc, std::make_unique<BoxFeature>("Box1", kernel::Vec3{}, kernel::Vec3{10, 10, 10}));
    auto made = CommandRegistry::builtin().create(
        "feature.add_move", {{"body", box}, {"turn", 90.0}, {"by", kernel::Vec3{5, 0, 0}}}, doc);
    REQUIRE(made.ok());
    REQUIRE(doc.execute(std::move(made.command)).empty());
    REQUIRE(doc.featureCount() == 2);
    const auto& m = dynamic_cast<const MoveFeature&>(doc.featureAt(1));
    CHECK(m.name() == "Move1");
    CHECK(m.body() == box);
    CHECK(m.motion().angle == Approx(std::numbers::pi / 2));
    CHECK(m.motion().axis.z == 1.0);  // up, unless told otherwise
    CHECK(m.motion().offset.x == 5.0);
    CHECK(m.motion().center.x == 0.0);
    CHECK_FALSE(m.copy());  // a plain move, unless told otherwise

    made = CommandRegistry::builtin().create("feature.add_move", {{"by", kernel::Vec3{5, 0, 0}}}, doc);
    REQUIRE_FALSE(made.ok());
    CHECK(made.issues[0].code == "command.arg_missing");
}
