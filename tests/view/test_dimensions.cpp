// Tests for dimensions drawn as on a drawing: where the extension lines, the
// line, the arrowheads and the number go, at a given zoom.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "sb/view/Dimensions.h"

using namespace sb;
using namespace sb::view;
using Catch::Approx;

namespace {

ViewOverlay dimension(kernel::Vec3 from, kernel::Vec3 to, kernel::Vec3 through, std::string text = "90 mm") {
    ViewOverlay o;
    o.shape = OverlayShape::Dimension;
    o.points = {from, to, through};
    o.texts = {std::move(text)};
    o.size = 13;
    return o;
}

double length(kernel::Vec3 a, kernel::Vec3 b) {
    return std::hypot(b.x - a.x, b.y - a.y, b.z - a.z);
}

bool same(kernel::Vec3 a, kernel::Vec3 b) {
    return length(a, b) < 1e-9;
}

// Half the number's width, in pixels: 0.62 of the letters' height per letter.
double halfWide(const std::string& text, double size = 13) {
    return 0.5 * 0.62 * size * static_cast<double>(text.size());
}

}  // namespace

TEST_CASE("a dimension below a line: extension lines, its line with a gap for the number, arrows",
          "[view][dimensions]") {
    // 90 mm along x, its line 15 mm below, at 0.5 mm per pixel, seen from the top.
    const auto d = dimensionDrawing(dimension({0, 0, 0}, {90, 0, 0}, {40, -15, 0}), 0.5);
    CHECK(same(d.lineFrom, {0, -15, 0}));
    CHECK(same(d.lineTo, {90, -15, 0}));
    CHECK(same(d.textAt, {45, -15, 0}));  // in the middle of its line
    CHECK_FALSE(d.textBeside);
    // Two extension lines, its line in two pieces, and two lines per arrowhead.
    REQUIRE(d.lines.size() == 2 * (2 + 2 + 4));
    // Extension lines: from 3 px (1.5 mm) off the point to 4 px (2 mm) past the line.
    CHECK(same(d.lines[0], {0, -1.5, 0}));
    CHECK(same(d.lines[1], {0, -17, 0}));
    CHECK(same(d.lines[2], {90, -1.5, 0}));
    CHECK(same(d.lines[3], {90, -17, 0}));
    // Its line stops short of the number, 4 px clear of it each side.
    const double hole = (halfWide("90 mm") + 4) * 0.5;
    CHECK(same(d.lines[4], {0, -15, 0}));
    CHECK(same(d.lines[5], {45 - hole, -15, 0}));
    CHECK(same(d.lines[6], {45 + hole, -15, 0}));
    CHECK(same(d.lines[7], {90, -15, 0}));
    // The first arrowhead points at its end: 9 px (4.5 mm) long, 3 px (1.5 mm) to each side.
    CHECK(same(d.lines[8], {0, -15, 0}));
    CHECK(d.lines[9].x == Approx(4.5));
    CHECK(std::abs(d.lines[9].y + 15) == Approx(1.5));
    CHECK(same(d.lines[12], {90, -15, 0}));
    CHECK(d.lines[13].x == Approx(85.5));
}

TEST_CASE("arrowheads keep their size on the screen as the view zooms", "[view][dimensions]") {
    for (double mmPerPixel : {0.05, 0.5, 5.0}) {
        // Its line far enough away for extension lines at every zoom (more than 3 px).
        const auto d = dimensionDrawing(dimension({0, 0, 0}, {900, 0, 0}, {0, -400, 0}), mmPerPixel);
        REQUIRE(d.lines.size() >= 12);
        const auto n = d.lines.size();
        CHECK(length(d.lines[n - 4], d.lines[n - 3]) / mmPerPixel == Approx(std::hypot(9.0, 3.0)));
    }
}

TEST_CASE("a line too short for its number in a gap has the number beside it, outside",
          "[view][dimensions]") {
    // 10 mm at 0.5 mm per pixel is 20 px: less than the number and its arrows need.
    const auto d = dimensionDrawing(dimension({0, 0, 0}, {10, 0, 0}, {0, -15, 0}, "10 mm"), 0.5);
    CHECK(d.textBeside);
    // Below its line (outside, away from what it measures), clear of it by 4 px and half the number's height.
    CHECK(d.textAt.x == Approx(5));
    CHECK(d.textAt.y == Approx(-15 - (4 + 6.5) * 0.5));
    REQUIRE(d.lines.size() == 2 * (2 + 1 + 4));  // its line in one piece
}

TEST_CASE("a line going up the screen has its number beside it, so it fits", "[view][dimensions]") {
    // 50 mm up y, its line 15 mm to the left, seen from the top.
    auto o = dimension({0, 0, 0}, {0, 50, 0}, {-15, 0, 0}, "50 mm");
    const auto d = dimensionDrawing(o, 0.5);
    CHECK(d.textBeside);
    CHECK(d.textAt.y == Approx(25));
    CHECK(d.textAt.x == Approx(-15 - (4 + halfWide("50 mm")) * 0.5));  // left of its line by half its width
    // Seen with the screen turned a quarter, y runs across: the number goes in a gap.
    const auto turned = dimensionDrawing(o, 0.5, {{0, 1, 0}, {-1, 0, 0}});
    CHECK_FALSE(turned.textBeside);
    CHECK(same(turned.textAt, {-15, 25, 0}));
}

TEST_CASE("a horizontal distance measures along x, whatever the slope between its points",
          "[view][dimensions]") {
    auto o = dimension({0, 0, 0}, {50, 30, 0}, {0, 45, 0});
    o.dimension.along = {1, 0, 0};
    const auto d = dimensionDrawing(o, 0.5);
    CHECK(same(d.lineFrom, {0, 45, 0}));
    CHECK(same(d.lineTo, {50, 45, 0}));
    // Up from each point to the line above them.
    CHECK(d.lines[1].y > 45);
    CHECK(d.lines[3].y > 45);
}

TEST_CASE("a vertical dimension on another plane stays on that plane", "[view][dimensions]") {
    // On the front plane (XZ): 50 mm up z, its line 20 mm to the left (-x), seen from the front.
    auto o = dimension({0, 0, 0}, {0, 0, 50}, {-20, 0, 10});
    o.dimension.normal = {0, -1, 0};
    const auto d = dimensionDrawing(o, 0.5, {{1, 0, 0}, {0, 0, 1}});
    CHECK(same(d.lineFrom, {-20, 0, 0}));
    CHECK(same(d.lineTo, {-20, 0, 50}));
    for (const auto& p : d.lines) CHECK(p.y == Approx(0).margin(1e-12));
    CHECK(d.textAt.y == Approx(0).margin(1e-12));
}

TEST_CASE("a line through its points has no extension lines; a leader puts its number past its end",
          "[view][dimensions]") {
    // A radius: from the circle out to where its number goes, one arrow, at the circle.
    auto o = dimension({10, 0, 0}, {30, 0, 0}, {10, 0, 0}, "R10");
    o.dimension.arrowAtTo = false;
    o.dimension.textAtEnd = true;
    const auto d = dimensionDrawing(o, 0.5);
    REQUIRE(d.lines.size() == 2 * (1 + 2));  // its line and one arrowhead
    CHECK(same(d.lines[2], {10, 0, 0}));     // the arrow's tip on the circle
    // Past the end: 4 px, then half its width.
    CHECK(d.textAt.x == Approx(30 + (4 + halfWide("R10")) * 0.5));
}

TEST_CASE("dimensions become lines and a boxed number; other overlays stay as they are",
          "[view][dimensions]") {
    ViewOverlay grid;
    grid.shape = OverlayShape::Lattice;
    auto empty = dimension({0, 0, 0}, {0, 0, 0}, {0, 0, 0});  // measures nothing: draws nothing
    const auto out =
        expandDimensions({grid, dimension({0, 0, 0}, {90, 0, 0}, {0, -15, 0}, "90.00 mm"), empty}, 0.5);
    REQUIRE(out.size() == 4);
    CHECK(out[0].shape == OverlayShape::Lattice);
    CHECK(out[1].shape == OverlayShape::Lines);
    CHECK(out[1].points.size() == 16);
    CHECK(out[2].shape == OverlayShape::Labels);
    CHECK(out[2].boxed);
    CHECK(out[2].texts == std::vector<std::string>{"90.00 mm"});
    CHECK(same(out[2].points[0], {45, -15, 0}));
    CHECK(out[3].shape == OverlayShape::Lines);  // the empty one: no lines, and no number
    CHECK(out[3].points.empty());
}
