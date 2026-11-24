#pragma once

/// @file
/// Dragging along a joint: how changing one revolute or slider joint's value
/// moves the components it carries, worked out without building anything.

#include <optional>
#include <vector>

#include "sb/core/Id.h"
#include "sb/kernel/Types.h"
#include "sb/model/Document.h"

namespace sb::model {

/// How a joint's components move when its value changes (see dragJoint()).
struct JointDrag {
    /// The value it changes to, kept within the joint's lowest and highest: a
    /// revolute joint's turn (radians) or a slider's distance (mm).
    double value = 0;
    /// The components that move: the one the joint places, and every one placed
    /// from it, along later joints. All move together, the same way.
    std::vector<ComponentId> components;
    /// How they move, from where the rebuild placed them: turned about the
    /// joint's line, or slid along it (mm, radians).
    kernel::Motion motion{};
};

/// Works out how a joint's components move when its value changes: a hinged lid
/// turns about its hinge, a drawer slides along its runners, and whatever sits
/// on them goes with them. Only the joint's value changes; the chain of joints
/// is walked again from the last rebuild, so it's instant. Set the joint's value
/// with SetJointCommand to make the change.
///
/// @code
/// if (auto drag = dragJoint(assembly, report, hinge, 0.5))  // 0.5 radians
///     for (const auto& c : drag->components) preview(c, drag->motion);
/// @endcode
///
/// @param assembly The assembly.
/// @param report Its last rebuild.
/// @param joint A revolute or slider joint.
/// @param value The value to change to: radians for a revolute joint, mm for a
///        slider. It's kept within the joint's lowest and highest.
/// @return How they move, or nullopt if the joint can't be dragged: it isn't in
///         the assembly, it's rigid or off, or it placed nothing in that rebuild
///         (an origin couldn't be found, or it closes a loop).
std::optional<JointDrag> dragJoint(const Document& assembly, const RebuildReport& report,
                                   const JointId& joint, double value);

}  // namespace sb::model
