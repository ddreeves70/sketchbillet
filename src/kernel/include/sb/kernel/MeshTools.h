/// @file
/// Tidying and measuring triangle meshes: joining the copies of a point that
/// sit on the edges between faces, checking the skin is closed, and measuring
/// how thick the solid inside is.

#pragma once

#include <vector>

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

/// What's wrong with a mesh's edges, if anything. In a closed skin around a
/// solid, every edge belongs to exactly two triangles that run it opposite ways.
struct MeshEdges {
    int open = 0;     ///< Edges with no triangle on one side: gaps in the skin.
    int crowded = 0;  ///< Edges shared by more than two triangles, or by two that face the same way.

    /// Says whether the skin is closed, with nothing crowded.
    /// @return True when there are no open or crowded edges.
    bool closed() const { return open == 0 && crowded == 0; }
};

/// Counts a mesh's open and crowded edges. Weld the mesh first (weld()), or
/// every edge between two faces counts as open.
/// @param mesh A welded mesh.
/// @return The counts.
MeshEdges checkEdges(const Mesh& mesh);

/// How thick a solid is at each triangle: the distance (mm) from the
/// triangle's middle, straight in, to where that line comes out of the solid
/// again. For a flat wall, that's the wall's thickness.
///
/// The mesh must be a closed skin facing out, like weld(tessellate(...)). A
/// triangle whose line in never comes out (a gap in the skin, or a triangle of
/// no size) gets infinity.
/// @param mesh A welded, closed mesh (mm).
/// @return One thickness per triangle (mm), in the mesh's triangle order.
std::vector<double> wallThickness(const Mesh& mesh);

}  // namespace sb::kernel
