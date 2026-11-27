// IKernel::exportStepAssembly(): an assembly written as one STEP file keeps its
// structure (each part once, a named, placed copy for each component), and
// reads back as the same solids in the same places.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <string>

#include "sb/kernel_occt/OcctKernel.h"

using namespace sb::kernel;
using Catch::Approx;

namespace {

std::filesystem::path tempStep() {
    static std::random_device seed;
    return std::filesystem::temp_directory_path() / ("sb_test_assembly_" + std::to_string(seed()) + ".step");
}

std::string read(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::size_t count(const std::string& text, const std::string& what) {
    std::size_t n = 0;
    for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}

// A car: a 80 x 40 x 10 mm body, and a wheel part (a 12 mm cylinder with a
// 4 mm box hub beside it: two bodies), used three times, one turned on its side.
struct Car {
    StepAssembly assembly;
    std::vector<std::vector<ShapeHandle>> bodies;  // each part's
};

Car makeCar(IKernel& k) {
    Car car;
    const auto body = k.makeBox({0, 0, 0}, 80, 40, 10);
    const auto tyre = k.makeCylinder({0, 0, 0}, {0, 0, 1}, 12, 6);
    const auto hub = k.makeBox({-2, -2, 6}, 4, 4, 4);
    REQUIRE(body.ok());
    REQUIRE(tyre.ok());
    REQUIRE(hub.ok());
    car.assembly.name = "Car";
    car.assembly.parts.push_back({"Body", {{body.shape, "Body", std::array<std::uint8_t, 3>{30, 90, 200}}}});
    car.assembly.parts.push_back(
        {"Wheel",
         {{tyre.shape, "Tyre", std::array<std::uint8_t, 3>{20, 20, 20}}, {hub.shape, "Hub", std::nullopt}}});
    car.bodies = {{body.shape}, {tyre.shape, hub.shape}};
    car.assembly.copies.push_back({0, "Body 1", {}});
    car.assembly.copies.push_back({1, "Wheel 1", {{0, -20, 0}}});
    car.assembly.copies.push_back({1, "Wheel 2", {{80, -20, 0}}});
    car.assembly.copies.push_back({1, "Wheel 3", {{40, 60, 5}, {0, 1, 0}, {0, -1, 0}}});
    // Wheel 3's x and z can't both point along y: tidy it to a real placement.
    car.assembly.copies.back().at = *tidyPlacement({{40, 60, 5}, {1, 0, 0}, {0, -1, 0}});
    return car;
}

}  // namespace

TEST_CASE("An assembly written as STEP keeps each part once and a named, placed copy for each component",
          "[kernel][occt][step_assembly]") {
    auto k = makeOcctKernel();
    const auto car = makeCar(*k);
    const auto file = tempStep();
    REQUIRE(k->exportStepAssembly(car.assembly, file).empty());
    const auto text = read(file);
    std::filesystem::remove(file);

    // The structure: the car, the body, the wheel and its two bodies, each a product once.
    CHECK(count(text, "PRODUCT('Car'") == 1);
    CHECK(count(text, "PRODUCT('Body'") == 1);
    CHECK(count(text, "PRODUCT('Wheel'") == 1);
    CHECK(count(text, "PRODUCT('Tyre'") == 1);
    CHECK(count(text, "PRODUCT('Hub'") == 1);
    // One cylinder surface: the wheel's shape is written once, not three times.
    CHECK(count(text, "CYLINDRICAL_SURFACE") == 1);
    // Four copies in the car, and two bodies in the wheel.
    CHECK(count(text, "NEXT_ASSEMBLY_USAGE_OCCURRENCE") == 6);
    for (const char* name : {"'Body 1'", "'Wheel 1'", "'Wheel 2'", "'Wheel 3'"})
        CHECK(count(text, name) >= 1);
    // In millimeters, with the colors.
    CHECK(text.find("SI_UNIT(.MILLI.,.METRE.)") != std::string::npos);
    CHECK(text.find("COLOUR_RGB") != std::string::npos);

    // Read back as plain solids: each copy where it was put.
    const auto back = k->importStep(text);
    REQUIRE(back.ok());
    CHECK(k->topology(back.shape).solids == 7);  // the body, and three wheels of two
    double volume = 0;
    Box3 want{{1e9, 1e9, 1e9}, {-1e9, -1e9, -1e9}};
    for (const auto& copy : car.assembly.copies)
        for (const auto& b : car.bodies[copy.part]) {
            const auto moved = k->transform(b, toMotion(copy.at));
            REQUIRE(moved.ok());
            volume += k->massProps(moved.shape).volume;
            const auto box = k->bounds(moved.shape);
            want.min = Point3{std::min(want.min.x, box.min.x), std::min(want.min.y, box.min.y),
                              std::min(want.min.z, box.min.z)};
            want.max = Point3{std::max(want.max.x, box.max.x), std::max(want.max.y, box.max.y),
                              std::max(want.max.z, box.max.z)};
        }
    CHECK(k->massProps(back.shape).volume == Approx(volume).epsilon(1e-6));
    const auto got = k->bounds(back.shape);
    CHECK(got.min.x == Approx(want.min.x).margin(1e-5));
    CHECK(got.min.y == Approx(want.min.y).margin(1e-5));
    CHECK(got.min.z == Approx(want.min.z).margin(1e-5));
    CHECK(got.max.x == Approx(want.max.x).margin(1e-5));
    CHECK(got.max.y == Approx(want.max.y).margin(1e-5));
    CHECK(got.max.z == Approx(want.max.z).margin(1e-5));
}

TEST_CASE("Writing an assembly as STEP twice gives the same file", "[kernel][occt][step_assembly]") {
    auto k = makeOcctKernel();
    const auto car = makeCar(*k);
    std::string texts[2];
    for (auto& t : texts) {
        const auto file = tempStep();
        REQUIRE(k->exportStepAssembly(car.assembly, file).empty());
        // Without the header's time stamp.
        t = std::regex_replace(read(file), std::regex(R"(FILE_NAME\([^;]*;)"), "");
        std::filesystem::remove(file);
    }
    CHECK(texts[0] == texts[1]);
}

TEST_CASE("An assembly with no bodies, or a copy of a missing part, isn't written as STEP",
          "[kernel][occt][step_assembly]") {
    auto k = makeOcctKernel();
    const auto file = tempStep();
    StepAssembly empty{"Empty", {{"Axle", {}}}, {{0, "Axle 1", {}}}};
    auto issues = k->exportStepAssembly(empty, file);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "export.step.empty");
    CHECK_FALSE(std::filesystem::exists(file));

    StepAssembly wrong{"Wrong", {}, {{3, "Ghost 1", {}}}};
    issues = k->exportStepAssembly(wrong, file);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "export.step.part");
    CHECK_FALSE(std::filesystem::exists(file));
}
