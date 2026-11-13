#include "sb/kernel/MeshTools.h"

#include <array>
#include <map>

namespace sb::kernel {

Mesh weld(const Mesh& mesh) {
    Mesh out;
    out.vertices.reserve(mesh.vertices.size());
    out.triangles.reserve(mesh.triangles.size());
    // Where each position went: exact matches only, since faces meshed on their
    // own share the very same points along their edges.
    std::map<std::array<float, 3>, std::uint32_t> seen;
    std::vector<std::uint32_t> index(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        auto [it, added] = seen.try_emplace(mesh.vertices[i], static_cast<std::uint32_t>(out.vertices.size()));
        if (added) out.vertices.push_back(mesh.vertices[i]);
        index[i] = it->second;
    }
    for (const auto& t : mesh.triangles) {
        const std::array<std::uint32_t, 3> w{index[t[0]], index[t[1]], index[t[2]]};
        if (w[0] == w[1] || w[1] == w[2] || w[0] == w[2]) continue;  // collapsed
        out.triangles.push_back(w);
    }
    return out;
}

}  // namespace sb::kernel
