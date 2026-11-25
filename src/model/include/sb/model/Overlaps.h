/// @file
/// Checking an assembly for components that overlap: two parts that would have
/// to be in the same place at once, like an axle thicker than its wheel's hole.

#pragma once

#include <cstddef>
#include <vector>

#include "sb/core/Diagnostic.h"
#include "sb/kernel/IKernel.h"
#include "sb/kernel/Types.h"
#include "sb/model/Assembly.h"

namespace sb::model {

class Document;
class RebuildReport;

/// The smallest overlap that counts (mm^3). Faces that only touch, and the
/// kernel's rounding, share less than this.
inline constexpr double kOverlapVolume = 0.001;

/// Two components that overlap.
struct Overlap {
    ComponentId a{};          ///< The first, in the assembly's order.
    ComponentId b{};          ///< The second.
    double volume = 0;        ///< How much space they share (mm^3).
    kernel::Point3 center{};  ///< The middle of the space they share, in the assembly (mm).
    /// The space they share, in the assembly, to show in the view. Each of a's
    /// bodies' overlap with b's, for parts with more than one body.
    std::vector<kernel::ShapeHandle> shapes;
};

/// What the overlap check found.
struct OverlapCheck {
    std::vector<Overlap> overlaps;  ///< Each pair that overlaps, in the assembly's order.
    /// Each press fit that overlaps, as it's meant to (Document::pressFits()).
    std::vector<Overlap> pressFits;
    /// How many pairs of components were close enough to need a check: their
    /// boxes meet.
    std::size_t pairsChecked = 0;
    /// What to tell the student: assembly.no_overlaps (Info) when none overlap
    /// (but for press fits); otherwise an assembly.overlap (Warning) for each pair,
    /// and assembly.overlap_failed (Warning) for a pair the kernel couldn't check.
    /// Each press fit gets assembly.press_fit (Info) when it overlaps, as meant,
    /// or assembly.press_fit_loose (Warning) when it doesn't.
    Diagnostics issues;

    /// Says whether no components overlap.
    /// @return True when nothing is a warning or an error.
    bool clear() const;
};

/// Checks every pair of an assembly's components for overlaps, where each one
/// is placed. Only pairs whose boxes meet are checked, with the kernel's Boolean
/// common, so it's quick for parts far apart. Faces that only touch don't count.
/// Press fits (Document::pressFits()) are meant to overlap: they're listed
/// apart, and it's when one doesn't overlap that it says so.
/// @param assembly The assembly.
/// @param report Its last rebuild: where each component is placed, and its part's bodies.
/// @param kernel The kernel that built it.
/// @return The pairs that overlap, and the messages.
///
/// @code
/// auto check = model::checkOverlaps(assembly, report, kernel);
/// for (const auto& issue : check.issues) std::puts(issue.message.c_str());
/// @endcode
OverlapCheck checkOverlaps(const Document& assembly, const RebuildReport& report, kernel::IKernel& kernel);

}  // namespace sb::model
