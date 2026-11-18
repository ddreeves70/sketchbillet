// Tests for the squares to click in the view (Choices.h): their corners, and
// which one is under the mouse.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "sb/view/Choices.h"

using namespace sb;
using Catch::Approx;

namespace {

// Seen from above, 1 pixel per mm, with the origin at 100, 100 and y up.
std::optional<view::ScreenPoint> fromAbove(const kernel::Vec3& p) {
    return view::ScreenPoint{100 + p.x, 100 - p.y};
}

// The three base planes, as New sketch shows them, 10 to 50 mm out.
std::vector<view::ViewChoice> basePlanes() {
    auto c = [](const char* id, kernel::Vec3 x, kernel::Vec3 y) {
        return view::ViewChoice{id, id, {0, 0, 0}, x, y, 10, 50, false, {}};
    };
    return {c("xy", {1, 0, 0}, {0, 1, 0}), c("xz", {1, 0, 0}, {0, 0, 1}), c("yz", {0, 1, 0}, {0, 0, 1})};
}

}  // namespace

TEST_CASE("A choice's corners go round it, from `from` to `to` along both directions", "[view][choices]") {
    const auto xz = basePlanes()[1];
    const auto corners = view::choiceCorners(xz);
    CHECK(corners[0].x == Approx(10));
    CHECK(corners[0].z == Approx(10));
    CHECK(corners[2].x == Approx(50));
    CHECK(corners[2].z == Approx(50));
    for (const auto& p : corners) CHECK(p.y == Approx(0));  // all on the plane
    // Its area is (to - from)^2, as a square's should be.
    const auto& a = corners[0];
    const auto& b = corners[1];
    const auto& d = corners[3];
    const double side1 = std::hypot(b.x - a.x, b.y - a.y, b.z - a.z);
    const double side2 = std::hypot(d.x - a.x, d.y - a.y, d.z - a.z);
    CHECK(side1 * side2 == Approx(40.0 * 40.0));
    // The words go inside it.
    const auto label = view::choiceLabelAt(xz);
    CHECK(label.x > 10);
    CHECK(label.x < 50);
    CHECK(label.z > 10);
    CHECK(label.z < 50);
}

TEST_CASE("The choice under the mouse: inside a square, or near one seen edge on", "[view][choices]") {
    const auto planes = basePlanes();
    // Inside XY's square, seen from above.
    CHECK(view::choiceAt(planes, fromAbove, {130, 70}, 4) == 0u);
    // XZ and YZ stand up: from above they're lines, along x and along y.
    CHECK(view::choiceAt(planes, fromAbove, {130, 101}, 4) == 1u);
    CHECK(view::choiceAt(planes, fromAbove, {99, 70}, 4) == 2u);
    // The gap near the origin, and anywhere else, is nothing.
    CHECK_FALSE(view::choiceAt(planes, fromAbove, {103, 97}, 2));
    CHECK_FALSE(view::choiceAt(planes, fromAbove, {20, 180}, 4));
    // Inside one beats only near the side of another.
    CHECK(view::choiceAt(planes, fromAbove, {130, 92}, 4) == 0u);
}

TEST_CASE("Where squares overlap on the screen, the nearest along the line of sight wins",
          "[view][choices]") {
    // Two squares lying flat, one above the other, seen from above.
    std::vector<view::ViewChoice> stack{{"low", "", {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 0, 40, false, {}},
                                        {"high", "", {0, 0, 20}, {1, 0, 0}, {0, 1, 0}, 0, 40, false, {}}};
    const view::Ray down{{20, 20, 100}, {0, 0, -1}};
    CHECK(view::choiceAt(stack, fromAbove, {120, 80}, 4, down) == 1u);  // the high one is nearer
    const view::Ray up{{20, 20, -100}, {0, 0, 1}};
    CHECK(view::choiceAt(stack, fromAbove, {120, 80}, 4, up) == 0u);  // from below, the low one is
    CHECK(view::choiceAt(stack, fromAbove, {120, 80}, 4) == 0u);      // no line of sight: the first
}
