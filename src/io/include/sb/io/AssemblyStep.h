/// @file
/// Export an assembly to STEP, for other CAD programs: its parts once each, and
/// a named, placed copy for each component, so it opens as an assembly.

#pragma once

#include <filesystem>

#include "sb/core/Diagnostic.h"
#include "sb/kernel/IKernel.h"
#include "sb/kernel/StepAssembly.h"
#include "sb/model/Document.h"

namespace sb::io {

/// Gathers what an assembly's STEP file holds: each part with a body, once,
/// with its bodies' names and colors, and a copy of it for each component,
/// where the last rebuild placed it. A part with no body yet is left out.
/// @param assembly The assembly.
/// @param report Its last rebuild.
/// @return The parts and their copies, ready for IKernel::exportStepAssembly().
kernel::StepAssembly stepAssemblyOf(const model::Document& assembly, const model::RebuildReport& report);

/// Writes an assembly to a STEP file (AP214, in mm) that other CAD programs open
/// as an assembly: each part once, by name, with its bodies' colors, and each
/// component a named copy of its part, where it is in the assembly.
/// @param assembly The assembly.
/// @param report Its last rebuild.
/// @param kernel The kernel that built it, to write the file.
/// @param path Where to write, usually ending in .step or .stp.
/// @return No diagnostics on success; otherwise why the file couldn't be written
///     (export.nothing if no part has a body yet).
///
/// @code
/// io::exportAssemblyStep(car, car.regenerate(kernel), kernel, "car.step");
/// @endcode
Diagnostics exportAssemblyStep(const model::Document& assembly, const model::RebuildReport& report,
                               const kernel::IKernel& kernel, const std::filesystem::path& path);

}  // namespace sb::io
