/// @file
/// Measuring a design: the size of one face, edge or point, or the distance and
/// angle between two.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "sb/core/Diagnostic.h"
#include "sb/core/Id.h"
#include "sb/core/Units.h"
#include "sb/kernel/IKernel.h"
#include "sb/kernel/Types.h"
#include "sb/model/Assembly.h"
#include "sb/model/Naming.h"

namespace sb::model {

class Document;
class RebuildReport;

/// One thing to measure: a face, edge or point of a step's body. In an
/// assembly, a component's: a step of its part, where the component is placed.
struct MeasurePick {
    FeatureId body{};   ///< The step whose body it's on (in the component's part, in an assembly).
    GeometryRef ref{};  ///< The face, edge or point, by name; ref.kind says which.
    /// In an assembly, the component it's on; nil in a part.
    ComponentId component{};
};

/// What was measured of one pick.
struct MeasuredElement {
    /// The name of the step whose body it's on; in an assembly, the component's
    /// name and then the step's, like "Wheel 1, Cylinder1".
    std::string body;
    kernel::TopoKind kind = kernel::TopoKind::Face;  ///< Face, edge or point.
    /// What it is, its size and where it is: in an assembly, where its component
    /// is placed.
    kernel::ElementInfo info;
};

/// What measure() found.
struct Measurement {
    std::vector<MeasuredElement> elements;  ///< The picks, in order: one or two.
    /// The shortest distance between the two picks (mm); 0 if they touch.
    std::optional<double> distance;
    kernel::Point3 from{};  ///< Where the distance starts: the nearest point on the first pick (mm).
    kernel::Point3 to{};    ///< Where it ends: the nearest point on the second pick (mm).
    /// The angle between two flat faces, two straight edges, or one of each
    /// (radians, 0 to pi/2): 0 when they're parallel.
    std::optional<double> angle;
    /// How far apart two parallel things are, straight across (mm): two flat faces,
    /// straight edges, or a flat face and a straight edge.
    std::optional<double> apart;
    /// How far apart the centers of two round edges (whole circles) are (mm):
    /// straight across, when they share a direction, like two holes side by side.
    std::optional<double> centers;
    Diagnostics issues;  ///< Why something couldn't be measured.

    /// Says whether everything asked for was measured.
    /// @return True if there are no errors.
    bool ok() const;
};

/// Measures one face, edge or point (its size), or two (the distance between
/// them, and the angle when they're flat or straight). In an assembly, each pick
/// is on a component, where it's placed, so it measures between components: the
/// gap between two parts, or how far a wheel is from the body.
/// @code
/// const auto m = measure(doc, report, kernel, {{tag, top}, {tag, bottom}});
/// for (const auto& line : describe(m, doc.lengthUnit())) std::cout << line << "\n";
/// @endcode
/// @param doc The design.
/// @param report Its last rebuild.
/// @param kernel The kernel it was built with; in an assembly, it moves each
///        component's body to where the component is placed.
/// @param picks One or two picks.
/// @return What was found, or errors: measure.count for no picks or more than two,
///     measure.no_body for a step with no body, measure.missing for a pick that
///     isn't on its body any more, measure.no_component for a pick in an assembly
///     on no component (or one that isn't there).
Measurement measure(const Document& doc, const RebuildReport& report, kernel::IKernel& kernel,
                    const std::vector<MeasurePick>& picks);

/// Writes what was measured for the student, one line each, in the design's unit:
/// "Plate, flat face: area 1800.00 mm^2", "Distance: 4 mm (x 0, y 0, z 4 mm)". A
/// distance across parallel things is only given when it differs from the
/// shortest distance.
/// @param m What measure() found.
/// @param unit The unit to give lengths in.
/// @return The lines; none if nothing was measured.
std::vector<std::string> describe(const Measurement& m, LengthUnit unit);

}  // namespace sb::model
