#include "sb/model/Overlaps.h"

#include <algorithm>
#include <string>

#include "sb/core/Units.h"
#include "sb/kernel/Placement.h"
#include "sb/model/Document.h"

namespace sb::model {

bool OverlapCheck::clear() const {
    for (const auto& i : issues)
        if (i.severity != Severity::Info) return false;
    return true;
}

namespace {

std::string pointText(const kernel::Point3& p, LengthUnit unit) {
    // "x 12, y 3.5, z 4 mm": one unit at the end.
    auto n = [&](double v) {
        std::string t = lengthText(v, unit);
        return t.substr(0, t.rfind(' '));
    };
    return "x " + n(p.x) + ", y " + n(p.y) + ", z " + n(p.z) + " " + std::string(unitSymbol(unit));
}

// One body of a component, where the component is placed.
struct PlacedBody {
    kernel::ShapeHandle shape;
    kernel::Box3 box;
};

bool boxesMeet(const kernel::Box3& a, const kernel::Box3& b) {
    constexpr double slack = 1e-6;  // mm: faces that touch still meet
    return a.min.x <= b.max.x + slack && b.min.x <= a.max.x + slack && a.min.y <= b.max.y + slack &&
           b.min.y <= a.max.y + slack && a.min.z <= b.max.z + slack && b.min.z <= a.max.z + slack;
}

kernel::Box3 around(const std::vector<PlacedBody>& bodies) {
    kernel::Box3 out = bodies.front().box;
    for (const auto& b : bodies) {
        out.min = {std::min(out.min.x, b.box.min.x), std::min(out.min.y, b.box.min.y),
                   std::min(out.min.z, b.box.min.z)};
        out.max = {std::max(out.max.x, b.box.max.x), std::max(out.max.y, b.box.max.y),
                   std::max(out.max.z, b.box.max.z)};
    }
    return out;
}

}  // namespace

OverlapCheck checkOverlaps(const Document& assembly, const RebuildReport& report, kernel::IKernel& kernel) {
    OverlapCheck out;
    const auto& components = assembly.components();
    const auto unit = assembly.lengthUnit();

    // Each component's bodies, moved to where it's placed.
    std::vector<std::vector<PlacedBody>> placed(components.size());
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto& c = components[i];
        const auto* built = report.part(c.part);
        const auto* where = report.placementOf(c.id);
        if (!built) continue;
        const auto motion = kernel::toMotion(where ? *where : c.placement);
        for (const auto& body : built->bodies()) {
            const auto* r = built->result(body);
            if (!r || !r->shape.valid()) continue;
            auto moved = kernel.transform(r->shape, motion);
            if (!moved.ok()) continue;
            placed[i].push_back({moved.shape, kernel.bounds(moved.shape)});
        }
    }

    for (std::size_t i = 0; i < components.size(); ++i) {
        if (placed[i].empty()) continue;
        for (std::size_t k = i + 1; k < components.size(); ++k) {
            if (placed[k].empty() || !boxesMeet(around(placed[i]), around(placed[k]))) continue;
            ++out.pairsChecked;
            Overlap overlap{components[i].id, components[k].id, 0, {}, {}};
            kernel::Point3 weighted{};
            bool failed = false;
            for (const auto& a : placed[i]) {
                for (const auto& b : placed[k]) {
                    if (!boxesMeet(a.box, b.box)) continue;
                    auto common = kernel.boolean(kernel::BoolOp::Intersect, a.shape, b.shape);
                    if (!common.ok()) {
                        failed = true;
                        continue;
                    }
                    const auto mass = kernel.massProps(common.shape);
                    if (mass.volume < kOverlapVolume) continue;
                    overlap.volume += mass.volume;
                    weighted.x += mass.centerOfMass.x * mass.volume;
                    weighted.y += mass.centerOfMass.y * mass.volume;
                    weighted.z += mass.centerOfMass.z * mass.volume;
                    overlap.shapes.push_back(common.shape);
                }
            }
            const auto& a = components[i].name;
            const auto& b = components[k].name;
            if (failed) {
                out.issues.push_back({"assembly.overlap_failed",
                                      Severity::Warning,
                                      "Couldn't check whether " + a + " and " + b +
                                          " overlap: their shapes were too hard to compare. Look at where "
                                          "they meet in the 3D view.",
                                      {}});
            }
            if (overlap.volume < kOverlapVolume) continue;
            overlap.center = {weighted.x / overlap.volume, weighted.y / overlap.volume,
                              weighted.z / overlap.volume};
            out.issues.push_back(
                {"assembly.overlap",
                 Severity::Warning,
                 a + " and " + b + " overlap by " + volumeText(overlap.volume, unit) + ", around " +
                     pointText(overlap.center, unit) +
                     ". Parts can't be in the same place, so they won't go together. Move one "
                     "of them, or change one of their parts so they fit.",
                 {}});
            out.overlaps.push_back(std::move(overlap));
        }
    }

    if (out.overlaps.empty() && out.clear()) {
        const auto count = components.size();
        out.issues.insert(
            out.issues.begin(),
            {"assembly.no_overlaps",
             Severity::Info,
             assembly.name() + ": no components overlap (" + std::to_string(count) +
                 (count == 1 ? " component" : " components") + ", " + std::to_string(out.pairsChecked) +
                 (out.pairsChecked == 1 ? " pair" : " pairs") + " close enough to check).",
             {}});
    }
    return out;
}

}  // namespace sb::model
