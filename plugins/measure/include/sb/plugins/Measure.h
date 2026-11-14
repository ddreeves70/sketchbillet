#pragma once

/// @file
/// sb.measure: the built-in plugin with the Measure tool.

#include <vector>

#include "sb/core/Units.h"
#include "sb/model/Measure.h"
#include "sb/plugin/Overlays.h"
#include "sb/plugin/PluginHost.h"

namespace sb::plugins {

/// Gives the sb.measure plugin: Tools > Measure (Ctrl+Shift+I) switches the
/// viewers to picking faces, edges and points (plugin::PickMode::Measure). The
/// Measure panel shows the size of the one picked, or the distance and angle
/// between two (model::measure()), and every viewer draws a line between the
/// nearest points, with the distance written at its middle.
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

}  // namespace sb::plugins
