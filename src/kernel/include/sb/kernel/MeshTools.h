/// @file
/// Tidying triangle meshes: joining the copies of a point that sit on the edges
/// between faces, so a mesh of a solid is one closed skin.

#pragma once

#include "sb/kernel/Types.h"

namespace sb::kernel {

/// Joins vertices at exactly the same place into one, and drops triangles that
/// collapse to a line or a point as a result.
///
/// tessellate() meshes each face on its own, so a point on an edge between two
/// faces appears once in each. 3MF files and print checks need the faces to
/// share it, so that every edge of a closed solid belongs to exactly two
/// triangles. The order of the remaining vertices and triangles is kept.
/// @param mesh The mesh to tidy (positions in mm).
/// @return The same surface, with each point listed once.
///
/// @code
/// Mesh skin = weld(kernel.tessellate(shape, MeshQuality{}));
/// @endcode
Mesh weld(const Mesh& mesh);

}  // namespace sb::kernel
