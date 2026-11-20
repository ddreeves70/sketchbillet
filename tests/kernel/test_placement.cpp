// Property tests for placements: composing, inverting, and moving to and from
// the kernel's Motion.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <random>

#include "sb/kernel/Placement.h"

using namespace sb::kernel;
using Catch::Approx;

namespace {

constexpr double kPi = std::numbers::pi;

bool near(Vec3 a, Vec3 b, double tolerance = 1e-9) {
    return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z) <= tolerance;
}

// A placement turned some way and slid somewhere, from a seed.
Placement randomPlacement(std::mt19937& rng) {
    std::uniform_real_distribution<double> any(-1, 1), far(-200, 200);
    for (;;) {
        Placement p{
            {far(rng), far(rng), far(rng)}, {any(rng), any(rng), any(rng)}, {any(rng), any(rng), any(rng)}};
        if (const auto tidy = tidyPlacement(p)) return *tidy;
    }
}

// Where a Motion moves a point: turned about its line, then slid.
Point3 moved(const Motion& m, Point3 q) {
    const double l = std::hypot(m.axis.x, m.axis.y, m.axis.z);
    const Vec3 k{m.axis.x / l, m.axis.y / l, m.axis.z / l};
    const Vec3 v{q.x - m.center.x, q.y - m.center.y, q.z - m.center.z};
    const double c = std::cos(m.angle), s = std::sin(m.angle);
    const double kv = k.x * v.x + k.y * v.y + k.z * v.z;
    const Vec3 kxv{k.y * v.z - k.z * v.y, k.z * v.x - k.x * v.z, k.x * v.y - k.y * v.x};
    const Vec3 r{v.x * c + kxv.x * s + k.x * kv * (1 - c), v.y * c + kxv.y * s + k.y * kv * (1 - c),
                 v.z * c + kxv.z * s + k.z * kv * (1 - c)};
    return {r.x + m.center.x + m.offset.x, r.y + m.center.y + m.offset.y, r.z + m.center.z + m.offset.z};
}

}  // namespace

TEST_CASE("the default placement leaves everything where it was", "[kernel][placement]") {
    const Placement p;
    CHECK(near(apply(p, {3, -4, 5}), {3, -4, 5}));
    CHECK(near(yAxisOf(p), {0, 1, 0}));
    const Motion m = toMotion(p);
    CHECK(m.angle == 0);
    CHECK(near(m.offset, {}));
}

TEST_CASE("a placement moves points of the placed thing around it", "[kernel][placement]") {
    // A wheel 40 mm along x, its z pointing along -y (lying on its side).
    const Placement wheel{{40, -32, 12}, {1, 0, 0}, {0, -1, 0}};
    CHECK(near(yAxisOf(wheel), {0, 0, 1}));
    CHECK(near(apply(wheel, {0, 0, 5}), {40, -37, 12}));
    CHECK(near(apply(wheel, {0, 2, 0}), {40, -32, 14}));
    CHECK(near(applyDirection(wheel, {0, 0, 1}), {0, -1, 0}));  // directions don't slide
}

TEST_CASE("placements compose, and each has an inverse", "[kernel][placement]") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> far(-100, 100);
    for (int i = 0; i < 200; ++i) {
        const Placement a = randomPlacement(rng), b = randomPlacement(rng);
        const Point3 q{far(rng), far(rng), far(rng)};
        CHECK(near(apply(compose(a, b), q), apply(a, apply(b, q)), 1e-9));
        CHECK(samePlacement(compose(a, inverse(a)), Placement{}, 1e-9));
        CHECK(samePlacement(compose(inverse(a), a), Placement{}, 1e-9));
        CHECK(near(apply(inverse(a), apply(a, q)), q, 1e-9));
    }
}

TEST_CASE("a placement and its Motion move points the same way", "[kernel][placement]") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> far(-100, 100);
    for (int i = 0; i < 200; ++i) {
        const Placement p = randomPlacement(rng);
        const Motion m = toMotion(p);
        CHECK(m.angle >= 0);
        CHECK(m.angle <= kPi + 1e-12);
        const Point3 q{far(rng), far(rng), far(rng)};
        CHECK(near(moved(m, q), apply(p, q), 1e-8));
        CHECK(samePlacement(fromMotion(m), p, 1e-9));
    }
}

TEST_CASE("half turns, and turns about a line away from the origin, survive the round trip",
          "[kernel][placement]") {
    // Exactly half a turn about each axis, and about a slanted one.
    for (const Vec3 axis : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}, Vec3{1, 1, 0}, Vec3{-1, 2, 3}}) {
        const Placement p = fromMotion(Motion{{5, 6, 7}, {}, axis, kPi});
        const Motion m = toMotion(p);
        CHECK(m.angle == Approx(kPi));
        CHECK(samePlacement(fromMotion(m), p, 1e-9));
    }
    // A quarter turn about the vertical line through (10, 0, 0): the origin goes to (10, -10, 0).
    const Placement p = fromMotion(Motion{{}, {10, 0, 0}, {0, 0, 1}, kPi / 2});
    CHECK(near(p.origin, {10, -10, 0}));
    CHECK(near(p.xAxis, {0, 1, 0}));
    CHECK(near(p.zAxis, {0, 0, 1}));
}

TEST_CASE("tidying makes directions unit and square, or says there's no way to tell", "[kernel][placement]") {
    const auto tidy = tidyPlacement({{1, 2, 3}, {2, 0, 1}, {0, 0, 5}});
    REQUIRE(tidy);
    CHECK(near(tidy->zAxis, {0, 0, 1}));
    CHECK(near(tidy->xAxis, {1, 0, 0}));
    CHECK(near(tidy->origin, {1, 2, 3}));
    CHECK_FALSE(tidyPlacement({{}, {0, 0, 2}, {0, 0, 1}}));  // x along z
    CHECK_FALSE(tidyPlacement({{}, {1, 0, 0}, {0, 0, 0}}));  // no z
    CHECK_FALSE(tidyPlacement({{}, {0, 0, 0}, {0, 0, 1}}));  // no x
}
