#pragma once

/// @file
/// Commands that read parts from .sb files, for the command registry: they're
/// here because sb_io reads the files (sb_model can't).

#include "sb/model/CommandRegistry.h"

namespace sb::io {

/// Adds the commands that read or write parts' files to a registry, as the
/// sb.modeling plugin does with the other modeling commands:
/// - `component.insert`: puts a part from a `.sb` file into an assembly, with one
///   or more components of it, linked to the file (or, with link=false, as an
///   inside part).
/// - `part.update`: updates linked parts from files that changed (updateLinkedParts()).
/// - `part.relink`: links a part to its file where it is now.
/// - `part.save_as`: saves a part to its own file, and links it.
/// @code
/// model::CommandRegistry r;
/// io::addPartCommands(r);
/// // component.insert file=wheel.sb count=4 step=30,0,0
/// @endcode
/// @param registry The registry to add it to.
void addPartCommands(model::CommandRegistry& registry);

}  // namespace sb::io
