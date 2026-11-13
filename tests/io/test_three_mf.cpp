// Tests for 3MF export (sb/io/ThreeMf.h) with meshes made by hand.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "ThreeMfReader.h"
#include "sb/io/ThreeMf.h"
#include "sb/model/Material.h"

using namespace sb;

namespace {

// A tetrahedron with its corner at `at`, meshed face by face the way
// tessellate() does: each face lists its own three points.
kernel::Mesh tetrahedron(float at) {
    const std::array<float, 3> o{at, 0, 0}, x{at + 10, 0, 0}, y{at, 10, 0}, z{at, 0, 10};
    kernel::Mesh m;
    for (const auto& face : {std::array{o, y, x}, std::array{o, x, z}, std::array{o, z, y}, std::array{x, y, z}}) {
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (const auto& p : face) m.vertices.push_back(p);
        m.triangles.push_back({base, base + 1, base + 2});
    }
    return m;
}

model::Material named(std::string name, model::Color color) {
    model::Material m = model::defaultMaterial();
    m.name = std::move(name);
    m.color = color;
    return m;
}

}  // namespace

TEST_CASE("a 3MF file holds each part's name, material, color and closed mesh", "[io][3mf]") {
    io::Bytes bytes;
    REQUIRE(io::write3mfBytes({{"Left", tetrahedron(0), named("PLA", {120, 170, 220})},
                               {"Right & <co>", tetrahedron(20), named("PETG \"clear\"", {0, 255, 16})}},
                              "Two parts", bytes)
                .empty());
    const auto parts = test::readParts(bytes);
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].name == "Left");
    CHECK(parts[0].material == "PLA");
    CHECK(parts[0].color == "#78AADC");
    CHECK(parts[1].name == "Right &amp; &lt;co&gt;");  // as written: XML text
    CHECK(parts[1].material == "PETG &quot;clear&quot;");
    CHECK(parts[1].color == "#00FF10");
    for (const auto& p : parts) {
        CHECK(p.vertices.size() == 4);  // welded: each corner once
        CHECK(p.triangles.size() == 4);
        CHECK(p.badEdges() == 0);
        CHECK(p.volume() == 1000.0 / 6);  // facing out, so the volume is positive
    }
    CHECK(parts[1].vertices[0][0] == 20);

    const std::string model = test::entry(bytes, "3D/3dmodel.model");
    CHECK(model.find("unit=\"millimeter\"") != std::string::npos);
    CHECK(model.find("<metadata name=\"Title\">Two parts</metadata>") != std::string::npos);
    CHECK(model.find("<item objectid=\"2\"/>") != std::string::npos);
    CHECK(model.find("<item objectid=\"3\"/>") != std::string::npos);
    CHECK(test::entry(bytes, "[Content_Types].xml").find("3dmanufacturing-3dmodel+xml") != std::string::npos);
    CHECK(test::entry(bytes, "_rels/.rels").find("Target=\"/3D/3dmodel.model\"") != std::string::npos);
}

TEST_CASE("the same parts always give the same 3MF bytes", "[io][3mf]") {
    io::Bytes a, b;
    REQUIRE(io::write3mfBytes({{"Tet", tetrahedron(0), model::defaultMaterial()}}, "T", a).empty());
    REQUIRE(io::write3mfBytes({{"Tet", tetrahedron(0), model::defaultMaterial()}}, "T", b).empty());
    CHECK(a == b);
}

TEST_CASE("3MF export writes the file, or says why it couldn't", "[io][3mf]") {
    const auto dir = std::filesystem::temp_directory_path() / "sb_test_3mf";
    std::filesystem::create_directories(dir);
    const auto path = dir / "tet.3mf";
    {
        std::ofstream old(path);
        old << "an older file";
    }
    REQUIRE(io::write3mf({{"Tet", tetrahedron(0), model::defaultMaterial()}}, "T", path).empty());
    CHECK(std::filesystem::file_size(path) > 100);  // replaced
    CHECK_FALSE(std::filesystem::exists(dir / "tet.3mf.saving"));

    auto issues = io::write3mf({}, "T", dir / "empty.3mf");
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "export.nothing");
    CHECK_FALSE(std::filesystem::exists(dir / "empty.3mf"));

    issues = io::write3mf({{"Tet", tetrahedron(0), model::defaultMaterial()}}, "T", dir / "no folder" / "tet.3mf");
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "export.cant_write");
    CHECK(issues[0].message ==
          "SketchBillet couldn't write tet.3mf. Check that the folder exists, that you're allowed to save there, "
          "and that the disk isn't full.");
    std::filesystem::remove_all(dir);
}
