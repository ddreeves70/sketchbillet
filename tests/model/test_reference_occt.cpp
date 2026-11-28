// Bodies kept for reference: drawn see-through, to sketch on or design around,
// but not one of the design's bodies, so they have no mass and aren't exported.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/AssemblyMass.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/model/PartsList.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;

namespace {

// A 10 x 10 x 10 mm case, and a 40 x 20 x 5 mm phone beside it.
std::unique_ptr<Document> caseAndPhone() {
    auto doc = std::make_unique<Document>(DocKind::Part, "Case");
    REQUIRE(doc->execute(std::make_unique<AddFeatureCommand>(
                             std::make_unique<BoxFeature>("Case", kernel::Vec3{}, kernel::Vec3{10, 10, 10})))
                .empty());
    REQUIRE(doc->execute(std::make_unique<AddFeatureCommand>(std::make_unique<BoxFeature>(
                             "Phone", kernel::Vec3{20, 0, 0}, kernel::Vec3{40, 20, 5})))
                .empty());
    return doc;
}

}  // namespace

TEST_CASE("a body kept for reference is drawn but isn't one of the design's bodies",
          "[model][reference][occt]") {
    auto kernel = kernel::makeOcctKernel();
    auto doc = caseAndPhone();
    const auto box = doc->featureAt(0).id();
    const auto phone = doc->featureAt(1).id();
    CHECK(doc->regenerate(*kernel).bodies().size() == 2);

    const auto& registry = CommandRegistry::builtin();
    auto make = [&](const Args& args) {
        auto made = registry.create("feature.reference", args, *doc);
        REQUIRE(made.command);
        return std::move(made.command);
    };
    REQUIRE(doc->execute(make({{"steps", std::vector<FeatureId>{phone}}})).empty());
    CHECK(doc->find(phone)->reference());
    CHECK(doc->undoLabel() == "Use Phone for reference");
    auto report = doc->regenerate(*kernel);
    CHECK(report.bodies() == std::vector<FeatureId>{box});
    CHECK(report.referenceBodies() == std::vector<FeatureId>{phone});
    CHECK(report.shapeOf(phone));  // still built, to be drawn and sketched on

    // Asking again says so; a sketch has no body to keep.
    auto again = doc->execute(make({{"steps", std::vector<FeatureId>{phone}}}));
    REQUIRE(again.size() == 1);
    CHECK(again.front().code == "feature.already_reference");
    CHECK(again.front().message == "Phone is already for reference only.");
    REQUIRE(doc->execute(std::make_unique<AddFeatureCommand>(
                             std::make_unique<SketchFeature>("Sketch1", SketchPlane::XY)))
                .empty());
    const auto sketch = doc->featureAt(2).id();
    auto refused =
        doc->execute(std::make_unique<ReferenceFeaturesCommand>(std::vector<FeatureId>{sketch}, true));
    REQUIRE(refused.size() == 1);
    CHECK(refused.front().code == "feature.reference_no_body");
    CHECK(refused.front().message ==
          "Sketch1 is a sketch, not a body, so it's always only for reference. Pick a "
          "step that makes a body, like a Box or an Import.");
    REQUIRE(doc->undo());  // the sketch

    // Undo makes it a body again; so does on=false.
    REQUIRE(doc->undo());
    CHECK_FALSE(doc->find(phone)->reference());
    CHECK(doc->regenerate(*kernel).bodies().size() == 2);
    REQUIRE(doc->redo());
    REQUIRE(doc->execute(make({{"steps", std::vector<FeatureId>{phone}}, {"on", false}})).empty());
    CHECK(doc->undoLabel() == "Make Phone a body");
    CHECK(doc->regenerate(*kernel).referenceBodies().empty());
}

TEST_CASE("a reference body used up by a later step gives an ordinary body", "[model][reference][occt]") {
    auto kernel = kernel::makeOcctKernel();
    auto doc = caseAndPhone();
    const auto box = doc->featureAt(0).id();
    const auto phone = doc->featureAt(1).id();
    REQUIRE(doc->execute(std::make_unique<ReferenceFeaturesCommand>(std::vector<FeatureId>{phone}, true))
                .empty());
    REQUIRE(doc->execute(std::make_unique<AddFeatureCommand>(
                             std::make_unique<BooleanFeature>("Union1", kernel::BoolOp::Union, box, phone)))
                .empty());
    const auto report = doc->regenerate(*kernel);
    REQUIRE(report.bodies().size() == 1);
    CHECK(report.bodies().front() == doc->featureAt(2).id());
    CHECK(report.referenceBodies().empty());
}

TEST_CASE("a part's reference bodies have no mass in an assembly or its parts list",
          "[model][reference][assembly][occt]") {
    auto kernel = kernel::makeOcctKernel();
    auto part = caseAndPhone();
    const auto phone = part->featureAt(1).id();
    REQUIRE(part->execute(std::make_unique<ReferenceFeaturesCommand>(std::vector<FeatureId>{phone}, true))
                .empty());
    Document box(DocKind::Part, "Box");
    REQUIRE(box.execute(std::make_unique<MakeAssemblyCommand>()).empty());
    REQUIRE(box.execute(std::make_unique<InsertPartCommand>(std::move(part), 2)).empty());
    const auto report = box.regenerate(*kernel);
    // Two cases of 1000 mm^3 of PLA (1.24 g each): the 4000 mm^3 phones count for nothing.
    const auto rows = partsList(box, report, *kernel);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].bodies == 1);
    CHECK(rows[0].volumeEach == Approx(1000));
    CHECK(rows[0].gramsAll == Approx(2.48));
    CHECK(assemblyMass(box, report, *kernel).grams == Approx(2.48));
}
