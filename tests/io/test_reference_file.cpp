// The reference flag is saved with a step, in .sb files and on the clipboard.

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "sb/io/SketchBilletFile.h"
#include "sb/io/Steps.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"

using namespace sb;
using namespace sb::model;

TEST_CASE("a reference body is saved and opened as one, and copied as one", "[io][reference]") {
    Document doc(DocKind::Part, "Case");
    REQUIRE(doc.execute(std::make_unique<AddFeatureCommand>(
                            std::make_unique<BoxFeature>("Case", kernel::Vec3{}, kernel::Vec3{10, 10, 10})))
                .empty());
    REQUIRE(doc.execute(std::make_unique<AddFeatureCommand>(std::make_unique<BoxFeature>(
                            "Phone", kernel::Vec3{20, 0, 0}, kernel::Vec3{40, 20, 5})))
                .empty());
    const auto phone = doc.featureAt(1).id();
    REQUIRE(
        doc.execute(std::make_unique<ReferenceFeaturesCommand>(std::vector<FeatureId>{phone}, true)).empty());

    io::Bytes bytes;
    REQUIRE(io::writeBytes(doc, bytes).empty());
    auto opened = io::readBytes(bytes, "Case.sb");
    REQUIRE(opened.document);
    CHECK_FALSE(opened.document->featureAt(0).reference());
    CHECK(opened.document->featureAt(1).reference());

    auto pasted = io::stepsFromText(io::stepsToText(doc, {phone}));
    REQUIRE(pasted.steps.size() == 1);
    CHECK(pasted.steps.front()->reference());
}
