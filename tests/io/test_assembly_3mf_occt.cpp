// Exporting an assembly to 3MF: each part's mesh once, and a build item for each
// component, where it is (assembled) or side by side on the plate (to print).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <sstream>

#include "ThreeMfReader.h"
#include "sb/io/ThreeMf.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"

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

// A car: a 40 x 20 x 10 mm body, and three wheels, each a 10 mm box and a 10 mm
// "tyre" box 20 mm along x (two bodies). The second wheel is turned on its side.
void makeCar(Document& doc) {
    REQUIRE(doc.execute(std::make_unique<MakeAssemblyCommand>()).empty());
    REQUIRE(doc.execute(std::make_unique<InsertPartCommand>(boxPart("Body", {40, 20, 10}), 1)).empty());
    auto wheel = boxPart("Wheel", {10, 10, 10});
    REQUIRE(wheel
                ->execute(std::make_unique<AddFeatureCommand>(
                    std::make_unique<BoxFeature>("Tyre", kernel::Vec3{20, 0, 0}, kernel::Vec3{10, 10, 10})))
                .empty());
    REQUIRE(doc.execute(std::make_unique<InsertPartCommand>(std::move(wheel), 3)).empty());
    REQUIRE(doc.execute(std::make_unique<PlaceComponentCommand>(
                            doc.components()[2].id, kernel::Placement{{100, 50, 7}, {0, 1, 0}, {1, 0, 0}}))
                .empty());
}

io::Bytes readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Item {
    std::string object;
    std::array<double, 12> m{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
};

std::vector<Item> items(const std::string& model) {
    std::vector<Item> out;
    const std::regex item(R"re(<item objectid="(\d+)"( transform="([^"]*)")?/>)re");
    for (std::sregex_iterator it(model.begin(), model.end(), item), end; it != end; ++it) {
        Item i{(*it)[1], {}};
        i.m = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        if ((*it)[3].matched) {
            std::istringstream text((*it)[3].str());
            for (auto& v : i.m) text >> v;
        }
        out.push_back(i);
    }
    return out;
}

// Where a 3MF transform puts a point: the row (x, y, z, 1) times the matrix.
std::array<double, 3> place(const Item& i, const std::array<double, 3>& p) {
    const auto& m = i.m;
    return {p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9], p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10],
            p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11]};
}

// The box around the parts of an item, once placed.
kernel::Box3 boxOf(const Item& i, const std::vector<const test::ThreeMfPart*>& meshes) {
    kernel::Box3 box{{1e9, 1e9, 1e9}, {-1e9, -1e9, -1e9}};
    for (const auto* mesh : meshes)
        for (const auto& v : mesh->vertices) {
            const auto p = place(i, v);
            box.min = kernel::Point3{std::min(box.min.x, p[0]), std::min(box.min.y, p[1]),
                                     std::min(box.min.z, p[2])};
            box.max = kernel::Point3{std::max(box.max.x, p[0]), std::max(box.max.y, p[1]),
                                     std::max(box.max.z, p[2])};
        }
    return box;
}

}  // namespace

TEST_CASE("an assembly's 3MF has each part's mesh once and a copy where each component is",
          "[io][three_mf][assembly][occt]") {
    auto kernel = kernel::makeOcctKernel();
    Document doc(DocKind::Part, "Car");
    makeCar(doc);
    const auto& report = doc.regenerate(*kernel);
    const auto path = std::filesystem::temp_directory_path() / "sb_assembled.3mf";
    REQUIRE(io::exportAssembly3mf(doc, report, *kernel, io::AssemblyLayout::Assembled, path).empty());
    const auto bytes = readFile(path);
    std::filesystem::remove(path);

    // Three meshes: the body, and the wheel's two bodies, each once.
    const auto meshes = test::readParts(bytes);
    REQUIRE(meshes.size() == 3);
    CHECK(meshes[0].name == "Body");
    CHECK(meshes[1].name == "Wheel Box1");
    CHECK(meshes[2].name == "Wheel Tyre");
    for (const auto& m : meshes) CHECK(m.badEdges() == 0);
    // The wheel is one object made of its two meshes.
    const auto model = test::entry(bytes, "3D/3dmodel.model");
    CHECK(model.find(R"(<object id="5" type="model" name="Wheel">)") != std::string::npos);
    CHECK(model.find(R"(<component objectid="3"/>)") != std::string::npos);
    CHECK(model.find(R"(<component objectid="4"/>)") != std::string::npos);

    // Four copies, each where its component is.
    const auto all = items(model);
    REQUIRE(all.size() == 4);
    CHECK(all[0].object == "2");
    for (int k = 1; k < 4; ++k) CHECK(all[k].object == "5");
    for (std::size_t k = 0; k < 4; ++k) {
        const auto& c = doc.components()[k];
        const auto* at = report.placementOf(c.id);
        REQUIRE(at);
        std::vector<const test::ThreeMfPart*> parts;
        if (k == 0)
            parts = {&meshes[0]};
        else
            parts = {&meshes[1], &meshes[2]};
        const auto got = boxOf(all[k], parts);
        // What the kernel says, moving the part's bodies the same way.
        kernel::Box3 want{{1e9, 1e9, 1e9}, {-1e9, -1e9, -1e9}};
        const auto* built = report.part(c.part);
        REQUIRE(built);
        for (const auto& body : built->bodies()) {
            const auto moved = kernel->transform(*built->shapeOf(body), kernel::toMotion(*at));
            REQUIRE(moved.ok());
            const auto b = kernel->bounds(moved.shape);
            want.min = kernel::Point3{std::min(want.min.x, b.min.x), std::min(want.min.y, b.min.y),
                                      std::min(want.min.z, b.min.z)};
            want.max = kernel::Point3{std::max(want.max.x, b.max.x), std::max(want.max.y, b.max.y),
                                      std::max(want.max.z, b.max.z)};
        }
        CHECK(got.min.x == Approx(want.min.x).margin(1e-6));
        CHECK(got.min.y == Approx(want.min.y).margin(1e-6));
        CHECK(got.min.z == Approx(want.min.z).margin(1e-6));
        CHECK(got.max.x == Approx(want.max.x).margin(1e-6));
        CHECK(got.max.y == Approx(want.max.y).margin(1e-6));
        CHECK(got.max.z == Approx(want.max.z).margin(1e-6));
    }
    // The turned wheel: x along y, z along x, so y along z.
    CHECK(all[2].m == std::array<double, 12>{0, 1, 0, 0, 0, 1, 1, 0, 0, 100, 50, 7});
}

TEST_CASE("an assembly's 3MF to print lays each part out as many times as it's used, apart, on the plate",
          "[io][three_mf][assembly][occt]") {
    auto kernel = kernel::makeOcctKernel();
    Document doc(DocKind::Part, "Car");
    makeCar(doc);
    const auto& report = doc.regenerate(*kernel);
    const auto path = std::filesystem::temp_directory_path() / "sb_to_print.3mf";
    REQUIRE(io::exportAssembly3mf(doc, report, *kernel, io::AssemblyLayout::ToPrint, path).empty());
    const auto bytes = readFile(path);
    std::filesystem::remove(path);

    const auto meshes = test::readParts(bytes);
    REQUIRE(meshes.size() == 3);
    const auto all = items(test::entry(bytes, "3D/3dmodel.model"));
    REQUIRE(all.size() == 4);
    std::vector<kernel::Box3> boxes;
    for (std::size_t k = 0; k < 4; ++k) {
        // As modeled: not turned, only moved.
        CHECK(std::equal(all[k].m.begin(), all[k].m.begin() + 9,
                         std::array<double, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1}.begin()));
        std::vector<const test::ThreeMfPart*> parts;
        if (k == 0)
            parts = {&meshes[0]};
        else
            parts = {&meshes[1], &meshes[2]};
        boxes.push_back(boxOf(all[k], parts));
        CHECK(boxes.back().min.z == Approx(0).margin(1e-6));  // on the plate
        CHECK(boxes.back().min.x >= -1e-6);
        CHECK(boxes.back().min.y >= -1e-6);
    }
    // Same size as modeled: the body 40 x 20 mm, each wheel 30 x 10 mm.
    CHECK(boxes[0].max.x - boxes[0].min.x == Approx(40));
    CHECK(boxes[1].max.x - boxes[1].min.x == Approx(30));
    // No two copies closer than 5 mm.
    for (std::size_t a = 0; a < 4; ++a)
        for (std::size_t b = a + 1; b < 4; ++b) {
            const double gapX = std::max(boxes[a].min.x - boxes[b].max.x, boxes[b].min.x - boxes[a].max.x);
            const double gapY = std::max(boxes[a].min.y - boxes[b].max.y, boxes[b].min.y - boxes[a].max.y);
            CHECK(std::max(gapX, gapY) >= 5 - 1e-6);
        }
}

TEST_CASE("an assembly with nothing built can't be exported to 3MF", "[io][three_mf][assembly][occt]") {
    auto kernel = kernel::makeOcctKernel();
    Document doc(DocKind::Part, "Empty");
    REQUIRE(doc.execute(std::make_unique<MakeAssemblyCommand>()).empty());
    REQUIRE(
        doc.execute(std::make_unique<InsertPartCommand>(std::make_unique<Document>(DocKind::Part, "Axle"), 1))
            .empty());
    const auto path = std::filesystem::temp_directory_path() / "sb_empty.3mf";
    const auto issues =
        io::exportAssembly3mf(doc, doc.regenerate(*kernel), *kernel, io::AssemblyLayout::ToPrint, path);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "export.nothing");
    CHECK_FALSE(std::filesystem::exists(path));
}
