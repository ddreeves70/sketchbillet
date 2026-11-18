#pragma once

/// @file
/// Choices: flat squares the student clicks to pick one, like the three base
/// planes offered when starting a sketch. Plain data and the maths for drawing
/// and picking them; no Qt or OCCT.

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "sb/kernel/Types.h"
#include "sb/view/Handles.h"
#include "sb/view/Scene.h"

namespace sb::view {

/// A flat square in the view that the student can click, like the ground (XY)
/// when starting a sketch. It reaches from `from` to `to` along both of its
/// directions, so a set of them meeting at one point (the three base planes)
/// leaves a gap along the lines where they meet, and each is easy to click.
struct ViewChoice {
    std::string id{};             ///< Says which was clicked, like "xy".
    std::string label{};          ///< The words written on it, like "XY (the ground)".
    kernel::Vec3 origin{};        ///< Where its directions start, in mm.
    kernel::Vec3 xAxis{1, 0, 0};  ///< One direction along it (a unit vector).
    kernel::Vec3 yAxis{0, 1, 0};  ///< The other direction along it (a unit vector, square to xAxis).
    double from = 10;             ///< Where it starts along both directions, in mm or pixels (see `pixels`).
    double to = 100;              ///< Where it ends along both directions, in mm or pixels.
    /// True if `from` and `to` are in screen pixels: the view keeps the square the
    /// same size on the screen however far it zooms. The maths below always takes mm.
    bool pixels = true;
    Rgb color{};  ///< Its color.
};

/// Works out a choice's four corners, in order around it.
/// @param choice The choice, with `from` and `to` in mm.
/// @return The corners, in mm.
std::array<kernel::Vec3, 4> choiceCorners(const ViewChoice& choice);

/// Works out where a choice's words start: inside it, near the middle of its far side.
/// @param choice The choice, with `from` and `to` in mm.
/// @return The point, in mm.
kernel::Vec3 choiceLabelAt(const ViewChoice& choice);

/// Finds which choice is under the mouse: one whose square the mouse is inside,
/// or, for one seen nearly edge on, within `reach` pixels of its sides. When the
/// mouse is inside more than one, the nearest along the line of sight wins.
/// @param choices The choices, with `from` and `to` in mm.
/// @param project Where points are drawn on the screen.
/// @param mouse Where the mouse is.
/// @param reach How close the mouse must be to a side, in pixels.
/// @param sight The line of sight through the mouse, to pick the nearest; without
///        it, the first one the mouse is inside wins.
/// @return The choice's index, or nothing if none is there.
std::optional<std::size_t> choiceAt(const std::vector<ViewChoice>& choices, const Projection& project,
                                    ScreenPoint mouse, double reach, const std::optional<Ray>& sight = {});

}  // namespace sb::view
