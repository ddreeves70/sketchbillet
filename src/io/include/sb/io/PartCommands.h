#pragma once

/// @file
/// Commands that read parts from .sb files, for the command registry: they're
/// here because sb_io reads the files (sb_model can't).

#include "sb/model/CommandRegistry.h"

namespace sb::io {

/// Adds `component.insert` to a registry: puts a part from a `.sb` file into an
/// assembly, as an inside part (a copy: a later change to the file doesn't
/// change the assembly), with one or more components of it. The sb.modeling
/// plugin adds it with the other modeling commands.
/// @code
/// model::CommandRegistry r;
/// io::addPartCommands(r);
/// // component.insert file=wheel.sb count=4 step=30,0,0
/// @endcode
/// @param registry The registry to add it to.
void addPartCommands(model::CommandRegistry& registry);

}  // namespace sb::io
