/// @file
/// Reading triangle meshes from STL and 3MF files, for importing parts made in
/// other programs. Standard library only: 3MF's zip is unpacked by SketchBillet itself.

#pragma once

#include <cstddef>
#include <string_view>

#include "sb/core/Diagnostic.h"
#include "sb/kernel/Types.h"

namespace sb::kernel {

/// What readStl() or read3mf() read.
struct MeshFile {
    Mesh mesh;           ///< The triangles, in mm; points at the same place are listed once (see weld()).
    Diagnostics issues;  ///< Why it couldn't be read; empty if it was.

    /// Checks whether it was read.
    /// @return True if there are no problems.
    bool ok() const { return issues.empty(); }
};

/// Reads an STL file, binary or text. STL files have no unit: the numbers are
/// taken as `unit` mm each, so 25.4 reads a file made in inches.
/// @code
/// auto read = readStl(fileBytes, 1.0);
/// if (read.ok()) auto solid = kernel.solidFromMesh(read.mesh);
/// @endcode
/// @param bytes The whole file.
/// @param mmPerUnit How many mm each of its numbers is: 1 for mm, 25.4 for inches.
/// @return The mesh, or import.stl.unreadable (not an STL file) or
///     import.mesh.empty (no triangles).
MeshFile readStl(std::string_view bytes, double mmPerUnit = 1.0);

/// Reads a 3MF file: every object its build places, with their placements, in
/// mm (3MF files say their unit).
/// @param bytes The whole file.
/// @return The mesh, or import.3mf.unreadable (not a 3MF file SketchBillet can
///     read, with why) or import.mesh.empty.
MeshFile read3mf(std::string_view bytes);

/// The most triangles a mesh can have to be made into a solid: more than this
/// makes a part too slow to work with.
inline constexpr std::size_t kMaxImportTriangles = 100000;

}  // namespace sb::kernel
