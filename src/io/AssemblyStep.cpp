#include "sb/io/AssemblyStep.h"

#include <algorithm>
#include <map>

namespace sb::io {

kernel::StepAssembly stepAssemblyOf(const model::Document& assembly, const model::RebuildReport& report) {
    kernel::StepAssembly out;
    out.name = assembly.name();
    std::map<DocId, std::size_t> index;  // each part's place in out.parts
    for (const auto* part : assembly.parts()) {
        const auto* built = report.part(part->id());
        if (!built) continue;
        kernel::StepPart p{part->name(), {}};
        for (const auto& body : built->bodies()) {
            const auto* shape = built->shapeOf(body);
            const auto* feature = part->find(body);
            if (!shape || !feature) continue;
            const auto c = part->colorOf(body);
            p.bodies.push_back({*shape, feature->name(), std::array<std::uint8_t, 3>{c.r, c.g, c.b}});
        }
        if (p.bodies.empty()) continue;  // nothing to export yet
        index[part->id()] = out.parts.size();
        out.parts.push_back(std::move(p));
    }
    for (const auto& c : assembly.components()) {
        const auto it = index.find(c.part);
        if (it == index.end()) continue;
        const auto* at = report.placementOf(c.id);
        out.copies.push_back({it->second, c.name, at ? *at : c.placement});
    }
    return out;
}

Diagnostics exportAssemblyStep(const model::Document& assembly, const model::RebuildReport& report,
                               const kernel::IKernel& kernel, const std::filesystem::path& path) {
    const auto step = stepAssemblyOf(assembly, report);
    if (step.copies.empty())
        return {{"export.nothing",
                 Severity::Error,
                 assembly.name() + " has no parts with a shape to export yet. Add a shape to a part first.",
                 {}}};
    return kernel.exportStepAssembly(step, path);
}

}  // namespace sb::io
