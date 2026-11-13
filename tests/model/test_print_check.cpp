// Tests for the print check (sb/model/PrintCheck.h): what it finds and says.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

#include "sb/model/PrintCheck.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;

namespace {

// A box mesh x from 0 to lx and so on, each face listing its own corners, the
// way tessellate() gives them.
kernel::Mesh box(float lx, float ly, float lz) {
    kernel::Mesh m;
    auto c = [&](int i) { return std::array<float, 3>{i & 1 ? lx : 0.f, i & 2 ? ly : 0.f, i & 4 ? lz : 0.f}; };
    const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (int i : f) m.vertices.push_back(c(i));
        m.triangles.push_back({base, base + 1, base + 2});
        m.triangles.push_back({base, base + 2, base + 3});
    }
    return m;
}

}  // namespace

TEST_CASE("a closed part with thick enough walls is ready to print", "[model][print]") {
    const auto r = checkPrint(box(60, 30, 4), "Tag", {});
    CHECK(r.ready());
    CHECK(r.edges.closed());
    CHECK(r.thinnest == Approx(4));
    CHECK(r.thinArea == 0);
    REQUIRE(r.issues.size() == 1);
    CHECK(r.issues[0].code == "print.ready");
    CHECK(r.issues[0].severity == Severity::Info);
    CHECK(r.issues[0].message ==
          "Tag is ready to print: its surface is closed all round, and its thinnest wall is 4 mm "
          "(0.8 mm or more prints well).");
}

TEST_CASE("walls thinner than the minimum are a warning that says where", "[model][print]") {
    const auto r = checkPrint(box(40, 20, 0.5f), "Shim", {});
    CHECK_FALSE(r.ready());
    CHECK(r.thinnest == Approx(0.5));
    CHECK(r.thinArea == Approx(2 * 40 * 20));  // the top and bottom
    CHECK(r.thinnestAt.z == Approx(0).margin(1e-6));  // found from the bottom first
    REQUIRE(r.issues.size() == 1);
    CHECK(r.issues[0].code == "print.thin");
    CHECK(r.issues[0].severity == Severity::Warning);
    CHECK(r.issues[0].message ==
          "Shim has walls thinner than 0.8 mm: the thinnest is 0.5 mm thick, near x 13.33, y 13.33, z 0 mm. "
          "Most printers can't print walls that thin well, so they may come out weak, with holes, or not at "
          "all. Make them at least 0.8 mm thick.");

    SECTION("a smaller minimum, for a finer printer, lets it through") {
        PrintCheckOptions fine;
        fine.minWall = 0.4;
        CHECK(checkPrint(box(40, 20, 0.5f), "Shim", fine).ready());
    }
    SECTION("lengths come in the design's unit") {
        const auto inches = checkPrint(box(40, 20, 0.5f), "Shim", {}, LengthUnit::Inch);
        CHECK(inches.issues[0].message.find("thinner than 0.031 in: the thinnest is 0.02 in thick") !=
              std::string::npos);
    }
}

TEST_CASE("a surface with gaps can't be printed, and says why", "[model][print]") {
    kernel::Mesh open = box(10, 10, 10);
    open.triangles.resize(10);  // one face missing
    const auto r = checkPrint(open, "Lid", {});
    CHECK_FALSE(r.ready());
    CHECK(r.edges.open == 4);
    CHECK(std::isinf(r.thinnest));  // not measured
    REQUIRE(r.issues.size() == 1);
    CHECK(r.issues[0].code == "print.open");
    CHECK(r.issues[0].severity == Severity::Error);
    CHECK(r.issues[0].message ==
          "Lid has 4 places where its surface isn't closed, so a slicer can't tell what's inside it. This "
          "happens when a step makes a surface instead of a solid, or two bodies only touch along an edge. "
          "Check the steps that made it, and join touching bodies so they overlap by at least 0.1 mm.");

    const auto empty = checkPrint(kernel::Mesh{}, "Nothing", {});
    REQUIRE(empty.issues.size() == 1);
    CHECK(empty.issues[0].code == "print.nothing");
}
