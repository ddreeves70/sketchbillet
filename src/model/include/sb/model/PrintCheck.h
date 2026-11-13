/// @file
/// Checking a part before 3D printing: that its skin is closed all round, so a
/// slicer can tell inside from outside, and that no wall is too thin to print.

#pragma once

#include <limits>
#include <string>

#include "sb/core/Diagnostic.h"
#include "sb/core/Id.h"
#include "sb/core/Units.h"
#include "sb/kernel/IKernel.h"
#include "sb/kernel/MeshTools.h"
#include "sb/kernel/Types.h"

namespace sb::model {

class Document;
class RebuildReport;

/// What the print check looks for.
struct PrintCheckOptions {
    /// The thinnest wall that prints well (mm). 0.8 mm is two lines from a
    /// 0.4 mm nozzle, the most common size.
    double minWall = 0.8;
};

/// What the print check found.
struct PrintCheckResult {
    kernel::MeshEdges edges;  ///< Gaps and crowded edges in the part's skin.
    /// The thinnest wall (mm), or infinity if nothing could be measured.
    double thinnest = std::numeric_limits<double>::infinity();
    kernel::Vec3 thinnestAt;  ///< Where the thinnest wall is: a point on its surface (mm).
    double thinArea = 0;      ///< How much of the surface is on walls thinner than the minimum (mm^2).
    /// What to tell the student: print.ready (Info) when all is well; otherwise
    /// print.open (Error), print.thin (Warning), print.invalid (Error) or print.nothing (Error).
    Diagnostics issues;

    /// Says whether the part is ready to print.
    /// @return True when nothing is an error or a warning.
    bool ready() const;
};

/// Checks a mesh of a part for printing.
/// @param mesh The part's mesh (mm), facing out. It's welded first (kernel::weld()).
/// @param name The part's name, for the messages.
/// @param options What to look for.
/// @param unit The unit to give lengths in.
/// @return What was found, with the messages.
PrintCheckResult checkPrint(const kernel::Mesh& mesh, const std::string& name, const PrintCheckOptions& options,
                            LengthUnit unit = LengthUnit::Millimeter);

/// Checks one step's body for printing: meshed as finely as STL and 3MF
/// export, so it checks what would be printed.
/// @param doc The design: its unit is used for lengths in the messages.
/// @param report The design's last rebuild.
/// @param kernel The kernel that built it.
/// @param step The step whose body to check.
/// @param options What to look for.
/// @return What was found, with the messages.
///
/// @code
/// auto result = model::checkPrint(doc, report, kernel, report.bodies().front(), {});
/// for (const auto& issue : result.issues) std::puts(issue.message.c_str());
/// @endcode
PrintCheckResult checkPrint(const Document& doc, const RebuildReport& report, const kernel::IKernel& kernel,
                            const FeatureId& step, const PrintCheckOptions& options);

}  // namespace sb::model
