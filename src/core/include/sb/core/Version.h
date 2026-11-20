#pragma once

/// @file
/// Application and file-format versions.

#include <string_view>

namespace sb {

/// Gives the application version.
/// @return The version as text, for example "0.0.1".
std::string_view version();

/// The newest .sb file schema this build reads and writes: 2, for assemblies.
/// Files with a higher number were saved by a newer SketchBillet and are refused
/// with a message to update. See docs/file-format.md.
inline constexpr int kFileSchemaVersion = 2;

/// The schema a part is written with, alone or inside an assembly: 1, so an older
/// SketchBillet still opens every part. Assemblies are written as
/// kFileSchemaVersion.
inline constexpr int kPartSchemaVersion = 1;

}  // namespace sb
