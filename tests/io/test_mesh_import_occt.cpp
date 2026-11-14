// Reading meshes from STL and 3MF files (sb/kernel/MeshFiles.h), SketchBillet's
// own unzip, and making solids from meshes (IKernel::solidFromMesh): property
// tests against shapes of known size, and files made by libzip and by
// SketchBillet's own exporters.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <zip.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/kernel/Unzip.h"
#include "sb/io/ThreeMf.h"
#include "sb/kernel/MeshFiles.h"
#include "sb/kernel/MeshTools.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/Material.h"

using namespace sb;
using namespace sb::kernel;
using Catch::Approx;

namespace {

// The volume inside a closed mesh (mm^3).
double volumeOf(const Mesh& m) {
    double v = 0;
    for (const auto& t : m.triangles) {
        const auto &a = m.vertices[t[0]], &b = m.vertices[t[1]], &c = m.vertices[t[2]];
        v +=
            static_cast<double>(a[0]) *
                (static_cast<double>(b[1]) * c[2] - static_cast<double>(b[2]) * c[1]) -
            static_cast<double>(a[1]) *
                (static_cast<double>(b[0]) * c[2] - static_cast<double>(b[2]) * c[0]) +
            static_cast<double>(a[2]) * (static_cast<double>(b[0]) * c[1] - static_cast<double>(b[1]) * c[0]);
    }
    return v / 6;
}

// A zip archive of these files, made by libzip, deflated (or stored).
std::string zipOf(const std::vector<std::pair<std::string, std::string>>& files, bool deflate = true) {
    zip_error_t err;
    zip_error_init(&err);
    zip_source_t* src = zip_source_buffer_create(nullptr, 0, 0, &err);
    zip_source_keep(src);
    zip_t* za = zip_open_from_source(src, ZIP_TRUNCATE, &err);
    REQUIRE(za);
    for (const auto& [name, text] : files) {
        zip_source_t* s = zip_source_buffer(za, text.data(), text.size(), 0);
        const auto i = zip_file_add(za, name.c_str(), s, ZIP_FL_ENC_UTF_8);
        REQUIRE(i >= 0);
        zip_set_file_compression(za, static_cast<zip_uint64_t>(i), deflate ? ZIP_CM_DEFLATE : ZIP_CM_STORE,
                                 9);
    }
    REQUIRE(zip_close(za) == 0);
    zip_source_open(src);
    zip_stat_t st;
    zip_source_stat(src, &st);
    std::string out(st.size, '\0');
    zip_source_read(src, out.data(), st.size);
    zip_source_close(src);
    zip_source_free(src);
    return out;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// A cube as text STL: 12 triangles, counter-clockwise from outside.
std::string cubeStl(double s) {
    const double c[8][3] = {{0, 0, 0}, {s, 0, 0}, {s, s, 0}, {0, s, 0},
                            {0, 0, s}, {s, 0, s}, {s, s, s}, {0, s, s}};
    const int f[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                          {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    std::ostringstream out;
    out << "solid cube\n";
    for (const auto& t : f) {
        out << "  facet normal 0 0 0\n    outer loop\n";
        for (int k : t) out << "      vertex " << c[k][0] << " " << c[k][1] << " " << c[k][2] << "\n";
        out << "    endloop\n  endfacet\n";
    }
    out << "endsolid cube\n";
    return out.str();
}

}  // namespace

TEST_CASE("SketchBillet's unzip reads what libzip packs", "[kernel][import][unzip]") {
    std::mt19937 rng(5);
    std::vector<std::pair<std::string, std::string>> files;
    // Empty, tiny, random (barely packs), repeating (packs a lot, long copies), and mixed text.
    files.emplace_back("empty", "");
    files.emplace_back("one", "x");
    std::string random(70000, '\0');
    for (auto& ch : random) ch = static_cast<char>(rng() & 0xff);
    files.emplace_back("dir/random.bin", random);
    files.emplace_back("repeat.txt", std::string(200000, 'a') + std::string(1000, 'b'));
    std::string text;
    for (int i = 0; i < 20000; ++i)
        text += "<vertex x=\"" + std::to_string(rng() % 1000) + ".5\" y=\"1\" z=\"2\"/>\n";
    files.emplace_back("3D/3dmodel.model", text);
    for (const bool deflate : {true, false}) {
        const auto zip = zipOf(files, deflate);
        std::string why;
        const auto entries = unzip::list(zip, why);
        INFO(why);
        REQUIRE(entries.size() == files.size());
        for (std::size_t i = 0; i < files.size(); ++i) {
            CHECK(entries[i].name == files[i].first);
            const auto got = unzip::read(zip, entries[i], 1u << 30, why);
            INFO(files[i].first << ": " << why);
            REQUIRE(got);
            CHECK(*got == files[i].second);
        }
        // A limit smaller than a file is refused, not filled.
        CHECK_FALSE(unzip::read(zip, entries[3], 1000, why));
    }
    // Not a zip, and a damaged one.
    std::string why;
    CHECK(unzip::list("not a zip archive at all, just some text", why).empty());
    auto zip = zipOf(files);
    const auto entries = unzip::list(zip, why);
    REQUIRE(entries.size() == files.size());
    zip[entries[4].dataOffset + 10] ^= 0x55;  // damage the model's packed bytes
    const auto damaged = unzip::read(zip, entries[4], 1u << 30, why);
    if (damaged) CHECK(*damaged != files[4].second);  // a flip can still decode: then it differs
}

TEST_CASE("A solid made from a part's mesh matches the mesh", "[kernel][occt][import][mesh]") {
    auto k = makeOcctKernel();
    std::mt19937 rng(17);
    std::uniform_real_distribution<double> size(2, 90), place(-60, 60), radius(0.15, 0.4);
    for (int n = 0; n < 8; ++n) {
        const Vec3 o{place(rng), place(rng), place(rng)};
        const double dx = size(rng), dy = size(rng), dz = size(rng);
        const double r = radius(rng) * std::min(dx, dy);
        const auto block = k->makeBox(o, dx, dy, dz).shape;
        const auto rod = k->makeCylinder({o.x + dx / 2, o.y + dy / 2, o.z - 1}, {0, 0, 1}, r, dz + 2).shape;
        const auto part = k->boolean(BoolOp::Cut, block, rod).shape;
        const auto mesh = weld(k->tessellate(part, MeshQuality{}));
        REQUIRE(checkEdges(mesh).closed());

        const auto solid = k->solidFromMesh(mesh);
        INFO((solid.issues.empty() ? std::string() : solid.issues.front().message));
        REQUIRE(solid.ok());
        CHECK(k->validate(solid.shape).empty());
        CHECK(k->topology(solid.shape).solids == 1);
        // Exactly the mesh's volume (flat faces), close to the part's (round hole).
        CHECK(k->massProps(solid.shape).volume == Approx(volumeOf(mesh)).epsilon(1e-6));
        CHECK(k->massProps(solid.shape).volume == Approx(k->massProps(part).volume).epsilon(2e-3));
        const auto a = k->bounds(part), b = k->bounds(solid.shape);
        CHECK(b.min.x == Approx(a.min.x).margin(1e-3));
        CHECK(b.max.z == Approx(a.max.z).margin(1e-3));
        // The block's six sides are one face each, not triangles: 4 sides, top and bottom
        // (with the hole in them), and the hole's many flat strips.
        const auto faces = k->elements(solid.shape).faces;
        int big = 0;
        for (const auto& f : faces)
            if (f.geom == GeomKind::Plane && f.size > 0.5 * std::min({dx * dy, dx * dz, dy * dz})) ++big;
        CHECK(big >= 6);
    }
}

TEST_CASE("A box from an STL file has six faces, and the same mesh always gives the same solid",
          "[kernel][occt][import][mesh]") {
    auto k = makeOcctKernel();
    const auto read = readStl(cubeStl(20));
    REQUIRE(read.ok());
    CHECK(read.mesh.vertices.size() == 8);  // joined at the corners
    CHECK(read.mesh.triangles.size() == 12);
    const auto a = k->solidFromMesh(read.mesh), b = k->solidFromMesh(read.mesh);
    REQUIRE(a.ok());
    CHECK(k->topology(a.shape).faces == 6);
    CHECK(k->massProps(a.shape).volume == Approx(8000));
    const auto ea = k->elements(a.shape), eb = k->elements(b.shape);
    REQUIRE(ea.faces.size() == eb.faces.size());
    for (std::size_t i = 0; i < ea.faces.size(); ++i) CHECK(ea.faces[i].center.x == eb.faces[i].center.x);

    // Inches: 25.4 mm a number.
    const auto inches = readStl(cubeStl(1), 25.4);
    REQUIRE(inches.ok());
    CHECK(k->massProps(k->solidFromMesh(inches.mesh).shape).volume ==
          Approx(25.4 * 25.4 * 25.4).epsilon(1e-5));
}

TEST_CASE("STL files exported by SketchBillet read back the same", "[kernel][occt][import][mesh]") {
    auto k = makeOcctKernel();
    const auto part = k->makeCylinder({5, 5, 0}, {0, 0, 1}, 8, 12).shape;
    const auto file = std::filesystem::temp_directory_path() /
                      ("sb_test_mesh_" + std::to_string(std::random_device{}()) + ".stl");
    REQUIRE(k->exportFile(part, ExportFormat::Stl, file).empty());  // binary STL
    const auto bytes = readFile(file);
    std::filesystem::remove(file);
    const auto read = readStl(bytes);
    REQUIRE(read.ok());
    const auto expected = weld(k->tessellate(part, MeshQuality{}));
    CHECK(read.mesh.triangles.size() == expected.triangles.size());
    CHECK(volumeOf(read.mesh) == Approx(volumeOf(expected)).epsilon(1e-6));
    const auto solid = k->solidFromMesh(read.mesh);
    REQUIRE(solid.ok());
    CHECK(k->massProps(solid.shape).volume == Approx(std::numbers::pi * 64 * 12).epsilon(5e-3));
}

TEST_CASE("3MF files: SketchBillet's own, and ones with units, parts and placements",
          "[kernel][occt][import][mesh]") {
    auto k = makeOcctKernel();
    // SketchBillet's own 3MF export (deflated by libzip).
    const auto part = k->makeBox({0, 0, 0}, 30, 20, 10).shape;
    io::Bytes bytes;
    REQUIRE(io::write3mfBytes({{"Block", k->tessellate(part, MeshQuality{}), model::defaultMaterial()}},
                              "Block", bytes)
                .empty());
    auto read = read3mf(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    REQUIRE(read.ok());
    CHECK(volumeOf(read.mesh) == Approx(6000));
    CHECK(k->topology(k->solidFromMesh(read.mesh).shape).faces == 6);

    // In inches, a 1 inch cube placed twice: once through a component moved 2 in
    // along x, once by the build moved 5 in along y.
    const std::string model = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="inch" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
 <!-- a comment -->
 <resources>
  <object id="1" type="model"><mesh>
   <vertices>
    <vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="1" y="1" z="0"/><vertex x="0" y="1" z="0"/>
    <vertex x="0" y="0" z="1"/><vertex x="1" y="0" z="1"/><vertex x="1" y="1" z="1"/><vertex x="0" y="1" z="1"/>
   </vertices>
   <triangles>
    <triangle v1="0" v2="2" v3="1"/><triangle v1="0" v2="3" v3="2"/><triangle v1="4" v2="5" v3="6"/>
    <triangle v1="4" v2="6" v3="7"/><triangle v1="0" v2="1" v3="5"/><triangle v1="0" v2="5" v3="4"/>
    <triangle v1="1" v2="2" v3="6"/><triangle v1="1" v2="6" v3="5"/><triangle v1="2" v2="3" v3="7"/>
    <triangle v1="2" v2="7" v3="6"/><triangle v1="3" v2="0" v3="4"/><triangle v1='3' v2='4' v3='7'/>
   </triangles>
  </mesh></object>
  <object id="2" type="model"><components>
   <component objectid="1" transform="1 0 0 0 1 0 0 0 1 2 0 0"/>
  </components></object>
 </resources>
 <build>
  <item objectid="1"/>
  <item objectid="2" transform="1 0 0 0 1 0 0 0 1 0 5 0"/>
 </build>
</model>)";
    const std::string rels = R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Target="/3D/part.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>)";
    read = read3mf(zipOf({{"_rels/.rels", rels}, {"3D/part.model", model}}));
    INFO((read.issues.empty() ? std::string() : read.issues.front().detail));
    REQUIRE(read.ok());
    const double in3 = 25.4 * 25.4 * 25.4;
    CHECK(volumeOf(read.mesh) == Approx(2 * in3).epsilon(1e-5));
    const auto two = k->solidFromMesh(read.mesh);
    REQUIRE(two.ok());
    CHECK(k->topology(two.shape).solids == 2);
    const auto box = k->bounds(two.shape);
    CHECK(box.max.x == Approx(3 * 25.4).margin(1e-3));  // the component's cube: 2 to 3 in
    CHECK(box.max.y == Approx(6 * 25.4).margin(1e-3));  // placed 5 in along y: 5 to 6 in
}

TEST_CASE("Mesh files that can't be read, or can't be solid, are reported", "[kernel][occt][import][mesh]") {
    auto k = makeOcctKernel();
    CHECK(readStl("this is not an STL file").issues.front().code == "import.stl.unreadable");
    CHECK(readStl("solid empty\nendsolid empty\n").issues.front().code == "import.mesh.empty");
    std::string huge(84, '\0');
    huge[80] = '\xff';
    huge[81] = '\xff';
    huge[82] = '\xff';  // 16 million triangles, in a file that says so
    huge.resize(84 + 16777215ull * 50);
    CHECK(readStl(huge).issues.front().code == "import.mesh.too_big");
    CHECK(read3mf("PK not really").issues.front().code == "import.3mf.unreadable");
    CHECK(read3mf(zipOf({{"3D/3dmodel.model", "<model><resources><object id='1'"}})).issues.front().code ==
          "import.3mf.unreadable");
    CHECK(read3mf(zipOf({{"3D/3dmodel.model", "<model><build><item objectid='9'/></build></model>"}}))
              .issues.front()
              .detail == "object 9 isn't in the model (it may be in another file)");

    // A cube with one side missing: open, so no inside.
    auto open = readStl(cubeStl(10));
    REQUIRE(open.ok());
    open.mesh.triangles.pop_back();
    const auto r = k->solidFromMesh(open.mesh);
    CHECK_FALSE(r.ok());
    REQUIRE(r.issues.size() == 1);
    CHECK(r.issues.front().code == "import.mesh.open");
    CHECK(r.issues.front().message.find("3 edges with a gap") != std::string::npos);
}
