#pragma once

/// @file
/// Where a part sits in an assembly: an origin and directions, like a sketch
/// plane, and the plain maths of moving points with it. No OCCT.

#include <optional>

#include "sb/kernel/Types.h"

namespace sb::kernel {

/// Where something sits and which way it faces: its own origin, x direction and
/// z direction, in the coordinates around it. Its y direction is z × x. A part
/// placed with the default placement sits exactly where it was made.
///
/// @code
/// Placement wheel{{40, -32, 12}, {1, 0, 0}, {0, -1, 0}};  // 40 mm along x, its z pointing along -y
/// const Point3 hub = apply(wheel, {0, 0, 5});             // a point of the wheel, in the assembly: (40,
/// -27, 12) const Motion move = toMotion(wheel);                     // the same move, for
/// IKernel::transform()
/// @endcode
struct Placement {
    Point3 origin{};      ///< Where its own origin is (mm).
    Vec3 xAxis{1, 0, 0};  ///< Its x direction (a unit vector).
    Vec3 zAxis{0, 0, 1};  ///< Its z direction (a unit vector, square to xAxis).
};

/// Gives a placement's y direction.
/// @param p The placement.
/// @return z × x (a unit vector when x and z are).
Vec3 yAxisOf(const Placement& p);

/// Makes a placement usable: its directions made unit length, and x made square
/// to z (keeping z, and x's direction as near as it can).
/// @param p The placement.
/// @return The tidied placement, or nothing if a direction is zero or x is
///         along z, so there's no way to tell which way it faces.
std::optional<Placement> tidyPlacement(const Placement& p);

/// Moves a point by a placement: from the placed thing's own coordinates to the
/// coordinates around it.
/// @param p The placement.
/// @param point A point, in the placed thing's own coordinates (mm).
/// @return The same point around it (mm).
Point3 apply(const Placement& p, const Point3& point);

/// Turns a direction by a placement (no sliding).
/// @param p The placement.
/// @param direction A direction, in the placed thing's own coordinates.
/// @return The same direction around it.
Vec3 applyDirection(const Placement& p, const Vec3& direction);

/// Places one placement inside another: a thing placed by `inner` inside
/// something placed by `outer`.
/// @param outer The outer placement.
/// @param inner The inner one, in the outer's own coordinates.
/// @return One placement that does both: apply(compose(outer, inner), q) is
///         apply(outer, apply(inner, q)).
Placement compose(const Placement& outer, const Placement& inner);

/// Gives the placement that undoes another.
/// @param p The placement.
/// @return Its inverse: compose(p, inverse(p)) is the default placement.
Placement inverse(const Placement& p);

/// Gives the rigid move that takes a thing from where it was made to its
/// placement, for IKernel::transform().
/// @param p The placement (with unit, square directions; see tidyPlacement()).
/// @return The move: a turn about a line through the origin, then a slide to p.origin.
Motion toMotion(const Placement& p);

/// Gives the placement a rigid move puts a thing at, from where it was made.
/// @param m The move.
/// @return The placement: where the move takes the origin and the x and z directions.
Placement fromMotion(const Motion& m);

/// Says whether two placements are the same, within a tolerance.
/// @param a One placement.
/// @param b The other.
/// @param tolerance How far apart their origins (mm) and directions may be.
/// @return True if they match.
bool samePlacement(const Placement& a, const Placement& b, double tolerance = 1e-9);

}  // namespace sb::kernel
