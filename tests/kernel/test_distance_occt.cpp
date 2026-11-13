// IKernel::distance(): the shortest distance between faces, edges and points,
// checked against sizes worked out from the shapes (property tests).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

#include "sb/kernel_occt/OcctKernel.h"

using namespace sb::kernel;
using Catch::Approx;

namespace {

// The element of `kind` whose middle is at `at`.
int at(const Elements& parts, TopoKind kind, Vec3 p) {
    const auto& list = parts.of(kind);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& c = list[i].center;
        if (std::abs(c.x - p.x) < 1e-6 && std::abs(c.y - p.y) < 1e-6 && std::abs(c.z - p.z) < 1e-6)
            return static_cast<int>(i);
    }
    FAIL("nothing at " << p.x << ", " << p.y << ", " << p.z);
    return -1;
}

}  // namespace

TEST_CASE("Distances across random boxes match their sizes", "[kernel][occt][measure]") {
    auto k = makeOcctKernel();
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> size(0.5, 80), place(-50, 50);
    for (int n = 0; n < 25; ++n) {
        const Vec3 o{place(rng), place(rng), place(rng)};
        const double dx = size(rng), dy = size(rng), dz = size(rng);
        const auto box = k->makeBox(o, dx, dy, dz).shape;
        const auto parts = k->elements(box);
        const auto f = [&](Vec3 p) { return ElementOf{box, TopoKind::Face, at(parts, TopoKind::Face, p)}; };
        const auto v = [&](Vec3 p) {
            return ElementOf{box, TopoKind::Vertex, at(parts, TopoKind::Vertex, p)};
        };
        const auto e = [&](Vec3 p) { return ElementOf{box, TopoKind::Edge, at(parts, TopoKind::Edge, p)}; };
        const Vec3 mid{o.x + dx / 2, o.y + dy / 2, o.z + dz / 2};

        // Opposite faces: the box's size, with the nearest points on each face.
        const auto bottom = f({mid.x, mid.y, o.z}), top = f({mid.x, mid.y, o.z + dz});
        auto gap = k->distance(bottom, top);
        REQUIRE(gap.ok());
        CHECK(gap.distance == Approx(dz));
        CHECK(gap.onA.z == Approx(o.z));
        CHECK(gap.onB.z == Approx(o.z + dz));
        CHECK(k->distance(f({o.x, mid.y, mid.z}), f({o.x + dx, mid.y, mid.z})).distance == Approx(dx));
        // Faces that share an edge touch.
        CHECK(k->distance(bottom, f({o.x, mid.y, mid.z})).distance == Approx(0).margin(1e-9));
        // Opposite corners: the diagonal.
        gap = k->distance(v(o), v({o.x + dx, o.y + dy, o.z + dz}));
        CHECK(gap.distance == Approx(std::sqrt(dx * dx + dy * dy + dz * dz)));
        CHECK(gap.onA.x == Approx(o.x));
        CHECK(gap.onB.y == Approx(o.y + dy));
        // Opposite edges along x: across the y-z diagonal.
        CHECK(k->distance(e({mid.x, o.y, o.z}), e({mid.x, o.y + dy, o.z + dz})).distance ==
              Approx(std::hypot(dy, dz)));
        // A corner and the face across from it.
        CHECK(k->distance(v(o), top).distance == Approx(dz));
        // The same both ways, and the same every time.
        CHECK(k->distance(top, bottom).distance == k->distance(bottom, top).distance);
        const auto again = k->distance(bottom, top);
        CHECK(again.onA.x == k->distance(bottom, top).onA.x);
    }
}

TEST_CASE("Distances between two shapes, and round edges", "[kernel][occt][measure]") {
    auto k = makeOcctKernel();
    const auto a = k->makeBox({0, 0, 0}, 10, 10, 10).shape;
    const auto b = k->makeBox({25, 3, 0}, 10, 10, 10).shape;
    // a's right face and b's left face: 15 mm apart.
    const auto gap = k->distance({a, TopoKind::Face, at(k->elements(a), TopoKind::Face, {10, 5, 5})},
                                 {b, TopoKind::Face, at(k->elements(b), TopoKind::Face, {25, 8, 5})});
    REQUIRE(gap.ok());
    CHECK(gap.distance == Approx(15));
    CHECK(gap.onA.x == Approx(10));
    CHECK(gap.onB.x == Approx(25));

    // A cylinder's top rim and a point beside it: from the rim, not the center.
    const auto c = k->makeCylinder({0, 0, 0}, {0, 0, 1}, 4, 6).shape;
    const auto p = k->makeBox({10, 0, 6}, 1, 1, 1).shape;
    const auto rim = at(k->elements(c), TopoKind::Edge, {0, 0, 6});
    const auto corner = at(k->elements(p), TopoKind::Vertex, {10, 0, 6});
    CHECK(k->distance({c, TopoKind::Edge, rim}, {p, TopoKind::Vertex, corner}).distance == Approx(6));
}

TEST_CASE("Measuring something that isn't on the shape is reported, not thrown", "[kernel][occt][measure]") {
    auto k = makeOcctKernel();
    const auto box = k->makeBox({0, 0, 0}, 10, 10, 10).shape;
    for (const ElementOf& bad : {ElementOf{box, TopoKind::Face, 6}, ElementOf{box, TopoKind::Edge, -1},
                                 ElementOf{box, TopoKind::Vertex, 8}}) {
        const auto gap = k->distance({box, TopoKind::Face, 0}, bad);
        CHECK_FALSE(gap.ok());
        REQUIRE(gap.issues.size() == 1);
        CHECK(gap.issues.front().code == "measure.no_element");
    }
    const auto none = k->distance({ShapeHandle{}, TopoKind::Face, 0}, {box, TopoKind::Face, 0});
    CHECK_FALSE(none.ok());
    CHECK(none.issues.front().code == "shape.missing");
}
