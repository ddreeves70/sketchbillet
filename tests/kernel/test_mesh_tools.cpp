// Tests for MeshTools: weld(), checkEdges() and wallThickness().

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

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

// ---------------------------------------------------------------------------
// checkEdges and wallThickness
// ---------------------------------------------------------------------------

namespace {

// A closed box mesh, x from 0 to lx and so on, facing out, already welded.
Mesh box(float lx, float ly, float lz) {
    Mesh m;
    for (int i = 0; i < 8; ++i) m.vertices.push_back({i & 1 ? lx : 0.f, i & 2 ? ly : 0.f, i & 4 ? lz : 0.f});
    m.triangles = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6},   // bottom (-z), top (+z)
                   {0, 1, 4}, {1, 5, 4}, {2, 6, 3}, {3, 6, 7},   // front (-y), back (+y)
                   {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};  // left (-x), right (+x)
    return m;
}

// A tube round the z axis: `n` sides, radii r < R, height h, facing out.
Mesh tube(int n, float r, float R, float h) {
    Mesh m;
    for (int i = 0; i < n; ++i) {
        const double a = 2 * std::numbers::pi * i / n;
        const auto c = static_cast<float>(std::cos(a)), s = static_cast<float>(std::sin(a));
        m.vertices.push_back({r * c, r * s, 0});  // 4i: inner bottom
        m.vertices.push_back({R * c, R * s, 0});  // 4i+1: outer bottom
        m.vertices.push_back({r * c, r * s, h});  // 4i+2: inner top
        m.vertices.push_back({R * c, R * s, h});  // 4i+3: outer top
    }
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(n); ++i) {
        const std::uint32_t a = 4 * i, b = 4 * ((i + 1) % static_cast<std::uint32_t>(n));
        m.triangles.push_back({a + 1, b + 1, b + 3});  // outside
        m.triangles.push_back({a + 1, b + 3, a + 3});
        m.triangles.push_back({a, a + 2, b + 2});  // inside, facing the axis
        m.triangles.push_back({a, b + 2, b});
        m.triangles.push_back({a + 2, a + 3, b + 3});  // top
        m.triangles.push_back({a + 2, b + 3, b + 2});
        m.triangles.push_back({a, b, b + 1});  // bottom
        m.triangles.push_back({a, b + 1, a + 1});
    }
    return m;
}

// The same as wallThickness, the slow way: every triangle against every other.
std::vector<double> slowThickness(const Mesh& m) {
    using V = std::array<double, 3>;
    auto at = [&](std::uint32_t v) { return V{m.vertices[v][0], m.vertices[v][1], m.vertices[v][2]}; };
    auto sub = [](V a, V b) { return V{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
    auto cross = [](V a, V b) { return V{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
    auto dot = [](V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    std::vector<double> out;
    for (std::size_t i = 0; i < m.triangles.size(); ++i) {
        const auto& t = m.triangles[i];
        const V n = cross(sub(at(t[1]), at(t[0])), sub(at(t[2]), at(t[0])));
        const double len = std::sqrt(dot(n, n));
        const V d{-n[0] / len, -n[1] / len, -n[2] / len};
        V o{};
        for (int k = 0; k < 3; ++k) o[k] = (at(t[0])[k] + at(t[1])[k] + at(t[2])[k]) / 3;
        double best = std::numeric_limits<double>::infinity();
        for (std::size_t j = 0; j < m.triangles.size(); ++j) {
            if (j == i) continue;
            const auto& u = m.triangles[j];
            const V e1 = sub(at(u[1]), at(u[0])), e2 = sub(at(u[2]), at(u[0])), p = cross(d, e2);
            const double det = dot(e1, p);
            if (det >= -1e-15) continue;
            const V s = sub(o, at(u[0])), q = cross(s, e1);
            const double a = dot(s, p) / det, b = dot(d, q) / det, dist = dot(e2, q) / det;
            if (a >= -1e-9 && b >= -1e-9 && a + b <= 1 + 1e-9 && dist > 1e-9) best = std::min(best, dist);
        }
        out.push_back(best);
    }
    return out;
}

}  // namespace

TEST_CASE("checkEdges finds gaps and crowded edges", "[kernel][mesh]") {
    CHECK(checkEdges(box(10, 10, 10)).closed());
    CHECK(checkEdges(tube(32, 10, 12, 5)).closed());

    Mesh open = box(10, 10, 10);
    open.triangles.pop_back();  // a hole: the triangle's 3 edges have nothing on one side
    CHECK(checkEdges(open).open == 3);
    CHECK_FALSE(checkEdges(open).closed());

    Mesh crowded = box(10, 10, 10);
    crowded.triangles.push_back(crowded.triangles.front());  // a triangle twice
    CHECK(checkEdges(crowded).crowded == 3);

    Mesh flipped = box(10, 10, 10);
    std::swap(flipped.triangles[0][1], flipped.triangles[0][2]);  // one facing in
    CHECK_FALSE(checkEdges(flipped).closed());
}

TEST_CASE("wallThickness measures straight through the solid", "[kernel][mesh]") {
    const auto t = wallThickness(box(30, 20, 2));
    REQUIRE(t.size() == 12);
    for (int i : {0, 1, 2, 3}) CHECK(t[i] == Catch::Approx(2));   // bottom and top: 2 mm apart
    for (int i : {4, 5, 6, 7}) CHECK(t[i] == Catch::Approx(20));  // front and back
    for (int i : {8, 9, 10, 11}) CHECK(t[i] == Catch::Approx(30));

    // A tube 2 mm thick and 5 mm tall: its walls measure about 2 mm (its sides
    // are flat, so a little under), its ends 5 mm.
    const Mesh pipe = tube(64, 10, 12, 5);
    const auto w = wallThickness(pipe);
    for (std::size_t i = 0; i < w.size(); ++i) {
        INFO("triangle " << i);
        if (i % 8 < 4) CHECK(w[i] == Catch::Approx(2).margin(0.03));
        else CHECK(w[i] == Catch::Approx(5));
    }
}

TEST_CASE("wallThickness gives the same as testing every triangle", "[kernel][mesh]") {
    // Two tubes, one inside the other with a gap: lines cross the gap and the tree has to find the right wall.
    Mesh m = tube(48, 10, 11.5f, 6);
    const Mesh outer = tube(40, 14, 15, 9);
    const auto base = static_cast<std::uint32_t>(m.vertices.size());
    m.vertices.insert(m.vertices.end(), outer.vertices.begin(), outer.vertices.end());
    for (auto t : outer.triangles) m.triangles.push_back({t[0] + base, t[1] + base, t[2] + base});
    const auto fast = wallThickness(m);
    const auto slow = slowThickness(m);
    REQUIRE(fast.size() == slow.size());
    for (std::size_t i = 0; i < fast.size(); ++i) {
        INFO("triangle " << i);
        CHECK(fast[i] == Catch::Approx(slow[i]).epsilon(1e-12));
    }
}

TEST_CASE("wallThickness of a skin with a gap gives infinity where nothing is hit", "[kernel][mesh]") {
    Mesh open = box(10, 10, 4);
    open.triangles.erase(open.triangles.begin() + 2, open.triangles.begin() + 4);  // no top
    const auto t = wallThickness(open);
    CHECK(std::isinf(t[0]));  // the bottom looks up, where the top is missing
    CHECK(std::isinf(t[1]));
    CHECK(t[2] == Catch::Approx(10));  // the front still meets the back
}
