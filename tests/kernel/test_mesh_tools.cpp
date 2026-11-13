// Tests for kernel::weld(): joining a mesh's copies of the same point.

#include <catch2/catch_test_macros.hpp>

#include "sb/kernel/MeshTools.h"

using namespace sb::kernel;

TEST_CASE("weld joins points at the same place, and keeps the order", "[kernel][mesh]") {
    // Two triangles of a square, meshed apart: the diagonal's points are listed twice.
    Mesh m;
    m.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    m.triangles = {{0, 1, 2}, {3, 4, 5}};
    const Mesh w = weld(m);
    CHECK(w.vertices == std::vector<std::array<float, 3>>{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}});
    CHECK(w.triangles == std::vector<std::array<std::uint32_t, 3>>{{0, 1, 2}, {0, 2, 3}});
}

TEST_CASE("weld drops triangles that collapse, and leaves a tidy mesh alone", "[kernel][mesh]") {
    Mesh m;
    m.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 0, 0}};
    m.triangles = {{0, 1, 2}, {0, 1, 3}};  // the second is a line once 3 joins 1
    const Mesh w = weld(m);
    CHECK(w.vertices.size() == 3);
    CHECK(w.triangles == std::vector<std::array<std::uint32_t, 3>>{{0, 1, 2}});
    // Welding again changes nothing.
    const Mesh again = weld(w);
    CHECK(again.vertices == w.vertices);
    CHECK(again.triangles == w.triangles);
    CHECK(weld(Mesh{}).triangles.empty());
}

TEST_CASE("weld only joins exact matches", "[kernel][mesh]") {
    Mesh m;
    m.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1.0001f, 0, 0}};
    m.triangles = {{0, 1, 2}, {0, 3, 2}};
    CHECK(weld(m).vertices.size() == 4);
}
