#include "sb/kernel/MeshTools.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

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

namespace sb::kernel {

namespace {

using V = std::array<double, 3>;

V sub(const V& a, const V& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V cross(const V& a, const V& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A tree of boxes round the triangles, so a line only tests the triangles near it.
struct Bvh {
    struct Node {
        V lo, hi;
        std::uint32_t first = 0, count = 0;  // a leaf's triangles, in `order`
        std::uint32_t left = 0, right = 0;   // an inner node's children
    };
    std::vector<Node> nodes;
    std::vector<std::uint32_t> order;
    const std::vector<std::array<V, 3>>* tris = nullptr;

    explicit Bvh(const std::vector<std::array<V, 3>>& t) : tris(&t) {
        order.resize(t.size());
        for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        if (!t.empty()) build(0, static_cast<std::uint32_t>(t.size()));
    }

    std::uint32_t build(std::uint32_t first, std::uint32_t count) {
        Node n;
        n.lo = {1e300, 1e300, 1e300};
        n.hi = {-1e300, -1e300, -1e300};
        for (std::uint32_t i = first; i < first + count; ++i)
            for (const auto& p : (*tris)[order[i]])
                for (int k = 0; k < 3; ++k) {
                    n.lo[k] = std::min(n.lo[k], p[k]);
                    n.hi[k] = std::max(n.hi[k], p[k]);
                }
        const auto index = static_cast<std::uint32_t>(nodes.size());
        nodes.push_back(n);
        if (count <= 4) {
            nodes[index].first = first;
            nodes[index].count = count;
            return index;
        }
        // Split at the middle triangle along the box's longest side.
        int axis = 0;
        for (int k = 1; k < 3; ++k)
            if (n.hi[k] - n.lo[k] > n.hi[axis] - n.lo[axis]) axis = k;
        const auto mid = first + count / 2;
        auto centre = [&](std::uint32_t t) {
            const auto& tri = (*tris)[t];
            return tri[0][axis] + tri[1][axis] + tri[2][axis];
        };
        std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count,
                         [&](std::uint32_t a, std::uint32_t b) { return centre(a) < centre(b) || (centre(a) == centre(b) && a < b); });
        const auto left = build(first, mid - first);
        const auto right = build(mid, first + count - mid);
        nodes[index].left = left;
        nodes[index].right = right;
        return index;
    }

    // Does the line from o along d (at distances up to `far`) pass through the box?
    static bool meets(const Node& n, const V& o, const V& inv, double far) {
        double lo = 0, hi = far;
        for (int k = 0; k < 3; ++k) {
            double a = (n.lo[k] - o[k]) * inv[k], b = (n.hi[k] - o[k]) * inv[k];
            if (a > b) std::swap(a, b);
            lo = std::max(lo, a);
            hi = std::min(hi, b);
            if (lo > hi) return false;
        }
        return true;
    }

    // The nearest triangle (other than `skip`) the line from o along d passes
    // through, from inside the solid (the triangle faces the same way as d).
    double nearest(const V& o, const V& d, std::uint32_t skip) const {
        if (nodes.empty()) return std::numeric_limits<double>::infinity();
        const V inv{1 / d[0], 1 / d[1], 1 / d[2]};
        double best = std::numeric_limits<double>::infinity();
        std::vector<std::uint32_t> stack{0};
        while (!stack.empty()) {
            const Node& n = nodes[stack.back()];
            stack.pop_back();
            if (!meets(n, o, inv, best)) continue;
            if (n.count == 0) {
                stack.push_back(n.left);
                stack.push_back(n.right);
                continue;
            }
            for (std::uint32_t i = n.first; i < n.first + n.count; ++i) {
                const auto t = order[i];
                if (t == skip) continue;
                const auto& tri = (*tris)[t];
                // Möller-Trumbore, counting a line through an edge or corner as a hit.
                const V e1 = sub(tri[1], tri[0]), e2 = sub(tri[2], tri[0]);
                const V p = cross(d, e2);
                const double det = dot(e1, p);
                if (det >= -1e-15) continue;  // edge-on, or facing back at the line: not a way out
                const double f = 1 / det;
                const V s = sub(o, tri[0]);
                const double u = f * dot(s, p);
                if (u < -1e-9 || u > 1 + 1e-9) continue;
                const V q = cross(s, e1);
                const double v = f * dot(d, q);
                if (v < -1e-9 || u + v > 1 + 1e-9) continue;
                const double dist = f * dot(e2, q);
                if (dist > 1e-9 && dist < best) best = dist;
            }
        }
        return best;
    }
};

}  // namespace

MeshEdges checkEdges(const Mesh& mesh) {
    // How many times each edge runs each way.
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> runs;
    for (const auto& t : mesh.triangles)
        for (int i = 0; i < 3; ++i) ++runs[{t[i], t[(i + 1) % 3]}];
    MeshEdges out;
    for (const auto& [e, n] : runs) {
        if (e.first > e.second && runs.count({e.second, e.first})) continue;  // counted from the other way
        const auto back = runs.find({e.second, e.first});
        const int other = back == runs.end() ? 0 : back->second;
        if (n == 1 && other == 1) continue;
        if (n + other == 1) ++out.open;
        else ++out.crowded;
    }
    return out;
}

std::vector<double> wallThickness(const Mesh& mesh) {
    std::vector<std::array<V, 3>> tris;
    tris.reserve(mesh.triangles.size());
    for (const auto& t : mesh.triangles) {
        std::array<V, 3> tri;
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) tri[i][k] = mesh.vertices[t[i]][k];
        tris.push_back(tri);
    }
    const Bvh bvh(tris);
    std::vector<double> out(tris.size(), std::numeric_limits<double>::infinity());
    for (std::uint32_t i = 0; i < tris.size(); ++i) {
        const auto& tri = tris[i];
        const V n = cross(sub(tri[1], tri[0]), sub(tri[2], tri[0]));
        const double len = std::sqrt(dot(n, n));
        if (len < 1e-12) continue;
        const V in{-n[0] / len, -n[1] / len, -n[2] / len};
        const V mid{(tri[0][0] + tri[1][0] + tri[2][0]) / 3, (tri[0][1] + tri[1][1] + tri[2][1]) / 3,
                    (tri[0][2] + tri[1][2] + tri[2][2]) / 3};
        out[i] = bvh.nearest(mid, in, i);
    }
    return out;
}

}  // namespace sb::kernel
