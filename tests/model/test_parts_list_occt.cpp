// An assembly's parts list: each part once, with its count, materials and mass.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/model/Material.h"
#include "sb/model/PartsList.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;

namespace {

std::unique_ptr<Document> boxPart(const std::string& name, kernel::Vec3 size) {
    auto part = std::make_unique<Document>(DocKind::Part, name);
    REQUIRE(part->execute(std::make_unique<AddFeatureCommand>(
                              std::make_unique<BoxFeature>("Box1", kernel::Vec3{}, size)))
                .empty());
    return part;
}

}  // namespace

TEST_CASE("a parts list has each part once, with how many, its materials and its mass each and in all",
          "[model][assembly][parts_list][occt]") {
    auto kernel = kernel::makeOcctKernel();
    Document car(DocKind::Part, "Car");
    REQUIRE(car.execute(std::make_unique<MakeAssemblyCommand>()).empty());
    // A 20 x 20 x 10 mm body (4000 mm^3 of PLA: 4.96 g), and three 10 mm
    // wheels, each a PLA box and a TPU box (1000 mm^3 each: 1.24 g + 1.21 g).
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(boxPart("Body", {20, 20, 10}), 1)).empty());
    auto wheel = boxPart("Wheel", {10, 10, 10});
    REQUIRE(wheel
                ->execute(std::make_unique<AddFeatureCommand>(
                    std::make_unique<BoxFeature>("Tyre", kernel::Vec3{20, 0, 0}, kernel::Vec3{10, 10, 10})))
                .empty());
    const auto tyre = wheel->featureAt(1).id();
    REQUIRE(wheel->execute(std::make_unique<SetMaterialCommand>(tyre, "TPU")).empty());
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(std::move(wheel), 3)).empty());
    // And a part with nothing in it yet.
    REQUIRE(
        car.execute(std::make_unique<InsertPartCommand>(std::make_unique<Document>(DocKind::Part, "Axle"), 1))
            .empty());

    const auto rows = partsList(car, car.regenerate(*kernel), *kernel);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].item == 1);
    CHECK(rows[0].name == "Body");
    CHECK(rows[0].count == 1);
    CHECK(rows[0].material == "PLA");
    CHECK(rows[0].bodies == 1);
    CHECK(rows[0].volumeEach == Approx(4000));
    CHECK(rows[0].gramsEach == Approx(4.96));
    CHECK(rows[0].gramsAll == Approx(4.96));
    CHECK_FALSE(rows[0].linked);
    CHECK(rows[0].file.empty());
    CHECK(rows[0].built);

    const double tpu =
        car.findPart(car.components()[1].part)->findMaterial("TPU")->density;  // g/cm^3, 1000 mm^3
    CHECK(rows[1].name == "Wheel");
    CHECK(rows[1].count == 3);
    CHECK(rows[1].material == "PLA, TPU (flexible)");
    CHECK(rows[1].bodies == 2);
    CHECK(rows[1].volumeEach == Approx(2000));
    CHECK(rows[1].gramsEach == Approx(1.24 + tpu));
    CHECK(rows[1].gramsAll == Approx(3 * (1.24 + tpu)));

    CHECK(rows[2].name == "Axle");
    CHECK(rows[2].bodies == 0);
    CHECK(rows[2].material.empty());
    CHECK(rows[2].gramsAll == 0);

    // As CSV: a heading, then a row each; text with a comma is quoted.
    const auto csv = partsListCsv(rows, LengthUnit::Millimeter);
    CHECK(csv.starts_with(
        "Item,Part,Count,Material,Mass each (g),Mass in all (g),Volume each (mm^3),Kept,File\n"
        "1,Body,1,PLA,4.96,4.96,4000.00,inside,\n"
        "2,Wheel,3,\"PLA, TPU (flexible)\","));
    CHECK(csv.ends_with("3,Axle,1,,0.00,0.00,0.00,inside,\n"));
    // In inches, the volumes are in cubic inches: 4000 mm^3 is 0.2441 in^3.
    CHECK(partsListCsv(rows, LengthUnit::Inch).find("Volume each (in^3)") != std::string::npos);
    CHECK(partsListCsv(rows, LengthUnit::Inch).find(",0.2441,") != std::string::npos);

    // A part has no parts list.
    Document plate(DocKind::Part, "Plate");
    CHECK(partsList(plate, plate.regenerate(*kernel), *kernel).empty());
}
