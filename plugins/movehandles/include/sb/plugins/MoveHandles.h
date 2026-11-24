#pragma once

/// @file
/// sb.movehandles: the built-in plugin that puts arrows on the picked body in
/// the 3D view, so the student can drag it to a new place, and a ring or an
/// arrow on an assembly's joint, to turn or slide the component it places.

#include <optional>
#include <vector>

#include "sb/core/Id.h"
#include "sb/core/Units.h"
#include "sb/kernel/Types.h"
#include "sb/plugin/PluginHost.h"
#include "sb/viewer/Viewer.h"

namespace sb::plugins {

/// Gives the sb.movehandles plugin. When one body is picked, it shows a red, a
/// green and a blue arrow (x, y and z) from its middle, and a square for sliding
/// it flat on the ground. Dragging one slides the body (model::SlideBodyCommand),
/// in steps of 1 mm (0.1 in in a design shown in inches or feet), as one undo step.
/// In an assembly, a free component gets the same handles. One a revolute joint
/// places gets a purple ring ("turn") about the joint's line instead, and one a
/// slider places a purple arrow ("slide") along it: dragging turns it in whole
/// degrees, or slides it in the same steps, within the joint's limits, with
/// everything placed from it (model::dragJoint()); dropping sets the joint's value
/// (model::SetJointCommand). A grounded component, or one a rigid joint places,
/// has none.
/// @return Its manifest and register function, for PluginHost::addBuiltin().
plugin::BuiltinPlugin moveHandlesPlugin();

/// How long the arrows are on the screen, in pixels.
inline constexpr double kArrowPixels = 130;
/// How long each side of the square is on the screen, in pixels.
inline constexpr double kSquarePixels = 32;
/// How big a joint's ring is on the screen: its radius, in pixels.
inline constexpr double kRingPixels = 70;

/// Works out the handles for a body: arrows along x, y and z from the middle of
/// its bounds (kArrowPixels long on the screen, whatever the zoom), and a square
/// on the ground plane there (kSquarePixels on each side).
/// @param bounds The body's bounds (mm).
/// @return Four handles: "x", "y", "z" and "xy".
std::vector<viewer::Handle> moveHandles(const kernel::Box3& bounds);

/// Rounds a drag to the steps a body slides in: 1 mm, or 0.1 in for a design
/// shown in inches or feet.
/// @param by How far the handle was dragged, along x, y and z (mm).
/// @param unit The unit the design shows lengths in.
/// @return The rounded distance (mm).
kernel::Vec3 snapSlide(const kernel::Vec3& by, LengthUnit unit);

}  // namespace sb::plugins
