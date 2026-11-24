#include "sb/model/AssemblyMass.h"

#include <map>
#include <vector>

#include "sb/kernel/Placement.h"
#include "sb/model/Document.h"
#include "sb/model/Material.h"

namespace sb::model {

AssemblyMass assemblyMass(const Document& assembly, const RebuildReport& report,
                          const kernel::IKernel& kernel) {
    // Each part's bodies, worked out once, in the part's own place.
    std::map<DocId, std::vector<BodyMass>> parts;
    for (const auto* part : assembly.parts()) {
        auto& bodies = parts[part->id()];
        const auto* built = report.part(part->id());
        if (!built) continue;
        for (const auto& body : built->bodies()) {
            const auto* r = built->result(body);
            if (!r || !r->shape.valid()) continue;
            bodies.push_back(bodyMass(kernel, r->shape, part->materialOf(body)));
        }
    }

    AssemblyMass out;
    kernel::Point3 weighted{};
    for (const auto& c : assembly.components()) {
        const auto found = parts.find(c.part);
        if (found == parts.end() || found->second.empty()) {
            ++out.missing;
            continue;
        }
        const auto* where = report.placementOf(c.id);
        const auto& placement = where ? *where : c.placement;
        for (const auto& b : found->second) {
            out.volume += b.volume;
            out.grams += b.grams;
            out.newtons += b.newtons;
            out.cost += b.cost;
            const auto at = kernel::apply(placement, b.center);
            weighted.x += at.x * b.grams;
            weighted.y += at.y * b.grams;
            weighted.z += at.z * b.grams;
        }
    }
    if (out.grams > 0) out.center = {weighted.x / out.grams, weighted.y / out.grams, weighted.z / out.grams};
    return out;
}

}  // namespace sb::model
