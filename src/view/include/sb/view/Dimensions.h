#pragma once

/// @file
/// Dimensions drawn as on a drawing: extension lines, a line with arrowheads,
/// and the number. The maths for turning a Dimension overlay into lines and
/// words at the view's zoom; no Qt or OCCT.

#include <vector>

#include "sb/kernel/Types.h"
#include "sb/view/Scene.h"

namespace sb::view {

/// Sizes in a dimension's drawing, in screen pixels, so it looks the same at any zoom.
struct DimensionSizes {
    double arrowLength = 9;     ///< From an arrowhead's tip to the back of it.
    double arrowWidth = 3;      ///< From an arrowhead's middle out to each side.
    double gap = 3;             ///< Between what's measured and the start of an extension line.
    double overshoot = 4;       ///< How far extension lines go past the dimension's line.
    double textGap = 4;         ///< Between its number and the line around or beside it.
    double letterWidth = 0.62;  ///< A letter's width, as a part of its height (bold letters).
};

/// Which way the screen's right and up point, in the design (unit vectors), so
/// a dimension's number gets the room it takes on the screen.
struct ScreenAxes {
    kernel::Vec3 right{1, 0, 0};  ///< Across the screen, to the right.
    kernel::Vec3 up{0, 1, 0};     ///< Up the screen.
};

/// A dimension, worked out for drawing.
struct DimensionDrawing {
    /// True when its number is beside its line instead of on it (a line going up
    /// the screen, or one too short for the number to fit in a gap in it).
    bool textBeside = false;
    std::vector<kernel::Vec3>
        lines{};              ///< Straight lines, in pairs, in mm: extension lines, its line, arrowheads.
    kernel::Vec3 textAt{};    ///< The middle of its number, in mm.
    kernel::Vec3 lineFrom{};  ///< Where its line starts (level with `from`), in mm.
    kernel::Vec3 lineTo{};    ///< Where its line ends (level with `to`), in mm.
};

/// Works out how to draw a Dimension overlay.
///
/// Its line runs along `along` (or from one point to the other) through the
/// third point. Extension lines go square to it, from just off each measured
/// point to just past its line; there are none where the line passes through
/// the points. Arrowheads sit at each end of the line, pointing out. The number
/// goes in a gap in the middle of the line when the line runs across the screen
/// and is long enough; otherwise beside its middle, on the outside. For a leader
/// it goes past the `to` end.
///
/// @code
/// ViewOverlay d;
/// d.shape = OverlayShape::Dimension;
/// d.points = {{0, 0, 0}, {90, 0, 0}, {0, -15, 0}};  // 90 mm along x, its line 15 mm below
/// d.texts = {"90.00 mm"};
/// const auto drawn = dimensionDrawing(d, 0.25);      // 0.25 mm per pixel
/// @endcode
///
/// @param overlay The dimension: `points` are where it measures from and to, and
///        a point its line passes through; anything else draws nothing.
/// @param mmPerPixel How many mm one screen pixel covers where it's drawn.
/// @param screen Which way the screen's right and up point.
/// @param sizes Its sizes in pixels.
/// @return Its lines and where its number goes.
DimensionDrawing dimensionDrawing(const ViewOverlay& overlay, double mmPerPixel,
                                  const ScreenAxes& screen = {}, const DimensionSizes& sizes = {});

/// Turns Dimension overlays into the Lines and boxed Labels that draw them, in
/// the same order; other overlays are kept as they are.
/// @param overlays The overlays.
/// @param mmPerPixel How many mm one screen pixel covers, for each overlay's sizes.
/// @param screen Which way the screen's right and up point.
/// @return The overlays to draw.
std::vector<ViewOverlay> expandDimensions(const std::vector<ViewOverlay>& overlays, double mmPerPixel,
                                          const ScreenAxes& screen = {});

}  // namespace sb::view
