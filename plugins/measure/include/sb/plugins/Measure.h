#pragma once

/// @file
/// sb.measure: the built-in plugin with the Measure tool, and the assembly
/// checks (Check for Overlaps and Balance Point).

#include <string>
#include <utility>
#include <vector>

#include "sb/core/Units.h"
#include "sb/kernel/IKernel.h"
#include "sb/model/AssemblyMass.h"
#include "sb/model/Measure.h"
#include "sb/model/Overlaps.h"
#include "sb/plugin/Overlays.h"
#include "sb/plugin/PluginHost.h"

namespace sb::plugins {

/// Gives the sb.measure plugin: Inspect > Measure (Ctrl+Shift+I) switches the
/// viewers to picking faces, edges and points (plugin::PickMode::Measure). The
/// Measure panel shows the size of the one picked, or the distance and angle
/// between two (model::measure()), and every viewer draws a line between the
/// nearest points, with the distance written at its middle.
///
/// In an assembly, Check for Overlaps on the Assemble tab (model::checkOverlaps())
/// shows where parts overlap in red, and Balance Point
/// (model::assemblyMass()) a dot where the assembly balances; the Checks panel
/// says what they found. They're done again after each rebuild while they're on.
/// @return Its manifest and register function, for PluginHost::addBuiltin().
plugin::BuiltinPlugin measurePlugin();

/// The overlays sb.measure draws for a measurement.
struct MeasureOverlays {
    plugin::Overlay line;   ///< The line between the nearest points (hidden when they touch).
    plugin::Overlay ends;   ///< A dot at each end of the line.
    plugin::Overlay label;  ///< The distance, written at the line's middle.
};

/// Works out what sb.measure draws for a measurement: nothing visible unless it
/// has a distance more than 0.
/// @param m What model::measure() found.
/// @param unit The unit to write the distance in.
/// @return The three overlays; hidden ones have `visible` false.
MeasureOverlays measureOverlays(const model::Measurement& m, LengthUnit unit);

/// Works out what Check for Overlaps draws: where each pair overlaps, as red,
/// see-through triangles over the parts.
/// @param check What model::checkOverlaps() found.
/// @param kernel The kernel that made the shapes, to cut them into triangles;
///        nullptr draws nothing.
/// @return The overlay; `visible` is false when nothing overlaps.
plugin::Overlay overlapOverlay(const model::OverlapCheck& check, const kernel::IKernel* kernel);

/// Works out what Balance Point draws: a dark dot on a white one, at the
/// balance point.
/// @param mass What model::assemblyMass() found.
/// @param show False to hide them.
/// @return The white dot and the dark one; hidden when not shown or there's no mass.
std::pair<plugin::Overlay, plugin::Overlay> balanceOverlays(const model::AssemblyMass& mass, bool show);

/// Writes what the checks found, for the Checks panel: each overlap (or that
/// none overlap), then the mass, weight and balance point.
/// @param overlaps What model::checkOverlaps() found.
/// @param mass What model::assemblyMass() found.
/// @param overlapsOn Whether Check for Overlaps is on.
/// @param balanceOn Whether Balance Point is on.
/// @param unit The unit to write lengths in.
/// @return The lines, with an empty line between the two checks.
std::vector<std::string> checksText(const model::OverlapCheck& overlaps, const model::AssemblyMass& mass,
                                    bool overlapsOn, bool balanceOn, LengthUnit unit);

}  // namespace sb::plugins
