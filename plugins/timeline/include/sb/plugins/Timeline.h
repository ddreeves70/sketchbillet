#pragma once

/// @file
/// sb.timeline: the built-in plugin with the timeline bar.

#include "sb/plugin/PluginHost.h"

namespace sb::plugins {

/// Gives the sb.timeline plugin: a bar along the bottom of the window with the
/// active design's steps in order, as icons, and the rollback marker between them.
/// Click a step to pick it; drag it to move it in the order (timeline.move); drag
/// the marker, or use the buttons at the left, to roll back and forward
/// (timeline.rollback). Steps after the marker are faded: they aren't built.
/// @return Its manifest and register function, for PluginHost::addBuiltin().
plugin::BuiltinPlugin timelinePlugin();

}  // namespace sb::plugins
