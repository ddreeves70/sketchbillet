// IKernel::importStep(): parts read back from STEP files keep their size and
// shape (property tests on exported parts), and bad files are reported.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "sb/kernel_occt/OcctKernel.h"

using namespace sb::kernel;
using Catch::Approx;

namespace {

// A shape written as STEP by the kernel, and read back as text.
std::string asStep(IKernel& k, const ShapeHandle& shape) {
    // A name of its own: CTest runs test cases side by side.
    static std::random_device seed;
    const auto file =
        std::filesystem::temp_directory_path() / ("sb_test_import_" + std::to_string(seed()) + ".step");
    REQUIRE(k.exportFile(shape, ExportFormat::Step, file).empty());
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    in.close();
    std::filesystem::remove(file);
    return text.str();
}

}  // namespace

TEST_CASE("A part exported as STEP comes back the same", "[kernel][occt][import]") {
    auto k = makeOcctKernel();
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> size(1, 120), place(-80, 80), radius(0.2, 0.45);
    for (int n = 0; n < 12; ++n) {
        // A block with a round hole through it, somewhere.
        const Vec3 o{place(rng), place(rng), place(rng)};
        const double dx = size(rng), dy = size(rng), dz = size(rng);
        const double r = radius(rng) * std::min(dx, dy);
        const auto block = k->makeBox(o, dx, dy, dz).shape;
        const auto rod = k->makeCylinder({o.x + dx / 2, o.y + dy / 2, o.z - 1}, {0, 0, 1}, r, dz + 2).shape;
        const auto part = k->boolean(BoolOp::Cut, block, rod).shape;
        REQUIRE(part.valid());

        const auto read = k->importStep(asStep(*k, part));
        REQUIRE(read.ok());
        CHECK(read.issues.empty());
        CHECK(k->validate(read.shape).empty());
        const auto want = k->massProps(part), got = k->massProps(read.shape);
        CHECK(got.volume == Approx(want.volume).epsilon(1e-6));
        CHECK(got.area == Approx(want.area).epsilon(1e-6));
        const auto a = k->bounds(part), b = k->bounds(read.shape);
        CHECK(b.min.x == Approx(a.min.x).margin(1e-6));
        CHECK(b.max.z == Approx(a.max.z).margin(1e-6));
        const auto ta = k->topology(part), tb = k->topology(read.shape);
        CHECK(tb.solids == 1);
        CHECK(tb.faces == ta.faces);
        CHECK(tb.edges == ta.edges);
    }
}

TEST_CASE("Importing the same STEP text twice gives the same part", "[kernel][occt][import]") {
    auto k = makeOcctKernel();
    const auto part = k->makeCylinder({0, 0, 0}, {1, 1, 0}, 7, 30).shape;
    const auto text = asStep(*k, part);
    const auto a = k->importStep(text), b = k->importStep(text);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    const auto ea = k->elements(a.shape), eb = k->elements(b.shape);
    REQUIRE(ea.faces.size() == eb.faces.size());
    for (std::size_t i = 0; i < ea.faces.size(); ++i) {
        CHECK(ea.faces[i].center.x == eb.faces[i].center.x);
        CHECK(ea.faces[i].size == eb.faces[i].size);
    }
}

TEST_CASE("Two separate solids in one STEP file come back as one shape with both", "[kernel][occt][import]") {
    auto k = makeOcctKernel();
    const auto a = k->makeBox({0, 0, 0}, 10, 10, 10).shape;
    const auto b = k->makeBox({30, 0, 0}, 5, 5, 5).shape;
    // Joined with a Boolean union that doesn't touch: one shape, two solids.
    const auto both = k->boolean(BoolOp::Union, a, b).shape;
    REQUIRE(k->topology(both).solids == 2);
    const auto read = k->importStep(asStep(*k, both));
    REQUIRE(read.ok());
    CHECK(k->topology(read.shape).solids == 2);
    CHECK(k->massProps(read.shape).volume == Approx(1000 + 125));
}

TEST_CASE("Files that aren't STEP, or have no solid, are reported", "[kernel][occt][import]") {
    auto k = makeOcctKernel();
    for (const std::string& text : {std::string(), std::string("solid cube\nendsolid cube\n"),
                                    std::string("ISO-10303-21;\nHEADER;\nthis is not right\n")}) {
        const auto r = k->importStep(text);
        CHECK_FALSE(r.ok());
        REQUIRE(r.issues.size() == 1);
        CHECK(r.issues.front().code == "import.step.unreadable");
    }
    // A flat face: a surface, with no inside.
    Loop2 triangle{{Line2{{0, 0}, {10, 0}}}, {Line2{{10, 0}, {10, 10}}}, {Line2{{10, 10}, {0, 0}}}};
    const auto face = k->makeFace({triangle, {}}, Plane{});
    REQUIRE(face.ok());
    const auto r = k->importStep(asStep(*k, face.shape));
    CHECK_FALSE(r.ok());
    REQUIRE(r.issues.size() == 1);
    CHECK(r.issues.front().code == "import.step.no_solid");
}
