#include "sb/model/PartsFromBodies.h"

#include <algorithm>
#include <set>

#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"

namespace sb::model {

namespace {

Diagnostic error(std::string code, std::string message) {
    return Diagnostic{std::move(code), Severity::Error, std::move(message), {}};
}

bool makesNoBody(const Feature& f) {
    return f.type() == FeatureType::Sketch || f.type() == FeatureType::Plane;
}

// The steps a list of steps uses up (not just looks at), as a rebuild counts them:
// only steps that are on use anything up.
template <typename Steps, typename Get>
std::set<FeatureId> usedUp(const Steps& steps, Get get) {
    std::set<FeatureId> used;
    for (const auto& s : steps) {
        const Feature& f = get(s);
        if (f.suppressed()) continue;
        const auto looked = f.referenceInputs();
        for (const auto& dep : f.dependencies())
            if (std::find(looked.begin(), looked.end(), dep) == looked.end()) used.insert(dep);
    }
    return used;
}

}  // namespace

std::vector<FeatureId> finishedBodySteps(const Document& design) {
    std::vector<const Feature*> built;
    for (std::size_t i = 0; i < design.markerIndex(); ++i) built.push_back(&design.featureAt(i));
    const auto used = usedUp(built, [](const Feature* f) -> const Feature& { return *f; });
    std::vector<FeatureId> out;
    for (const Feature* f : built) {
        if (!f->suppressed() && !makesNoBody(*f) && !f->reference() && !used.count(f->id()))
            out.push_back(f->id());
    }
    return out;
}

PartsFromBodies partsFromBodies(const Document& design, const std::vector<FeatureId>& bodies) {
    PartsFromBodies out;
    auto fail = [&](std::string code, std::string message) {
        out.problems.push_back(error(std::move(code), std::move(message)));
        return std::move(out);
    };
    if (design.kind() == DocKind::Assembly) {
        return fail("parts.from_assembly",
                    design.name() +
                        " is an assembly already, so its parts are made. Open a part with "
                        "several bodies to make parts from them.");
    }
    const auto finished = finishedBodySteps(design);
    std::vector<FeatureId> picked = bodies.empty() ? finished : std::vector<FeatureId>{};
    for (const auto& id : bodies) {
        const Feature* f = design.find(id);
        if (!f) return fail("feature.not_found", "That step isn't in the design anymore.");
        if (std::find(finished.begin(), finished.end(), id) == finished.end()) {
            return fail(
                "parts.not_body",
                f->name() + " isn't one of " + design.name() +
                    "'s finished bodies (a later step uses it, or it's off or for reference), so "
                    "it can't be a part. Pick finished bodies, or leave them out to use all of them.");
        }
        if (std::find(picked.begin(), picked.end(), id) == picked.end()) picked.push_back(id);
    }
    if (picked.empty()) {
        return fail("parts.no_bodies",
                    design.name() +
                        " has no finished bodies yet, so there's nothing to make parts of. Add a "
                        "shape, like a Box.");
    }
    // Keep design order, whatever order they were picked in.
    std::sort(picked.begin(), picked.end(),
              [&](const FeatureId& a, const FeatureId& b) { return design.indexOf(a) < design.indexOf(b); });

    // A linked copy of a picked body is another component of that body's part, at
    // the copy's move (and the moves of the copies it's a copy of).
    struct Group {
        FeatureId root;
        std::vector<FeatureId> bodies;
        std::vector<kernel::Placement> at;
    };
    std::vector<Group> groups;
    for (const auto& id : picked) {
        kernel::Placement at{};
        const Feature* f = design.find(id);
        while (f->type() == FeatureType::Move && static_cast<const MoveFeature*>(f)->copy()) {
            const auto& move = static_cast<const MoveFeature&>(*f);
            const Feature* original = design.find(move.body());
            if (!original) break;
            at = kernel::compose(at, kernel::fromMotion(move.motion()));
            f = original;
        }
        const bool grouped =
            f->id() != id && std::find(picked.begin(), picked.end(), f->id()) != picked.end();
        const FeatureId root = grouped ? f->id() : id;
        auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) { return x.root == root; });
        if (g == groups.end()) {
            groups.push_back({root, {}, {}});
            g = std::prev(groups.end());
        }
        if (grouped) {
            g->bodies.push_back(id);
            g->at.push_back(at);
        } else {
            // The body itself goes first, where it was.
            g->bodies.insert(g->bodies.begin(), id);
            g->at.insert(g->at.begin(), kernel::Placement{});
        }
    }
    // Parts in the order of their bodies in the design.
    std::sort(groups.begin(), groups.end(), [&](const Group& a, const Group& b) {
        return design.indexOf(a.root) < design.indexOf(b.root);
    });

    for (auto& g : groups) {
        const std::string name = design.find(g.root)->name();
        auto part = std::make_unique<Document>(DocKind::Part, name);
        if (design.lengthUnit() != LengthUnit::Millimeter)
            part->execute(std::make_unique<SetUnitsCommand>(design.lengthUnit()));
        if (!design.parameters().empty())
            part->execute(std::make_unique<ReplaceParametersCommand>(design.parameters().parameters()));
        auto steps = copySteps(design, {g.root});
        // Another body these steps only look at (a sketch on its face) comes along,
        // kept for reference: so the sketch finds its face, but it doesn't count.
        const auto used =
            usedUp(steps, [](const std::unique_ptr<Feature>& f) -> const Feature& { return *f; });
        for (std::size_t i = 0; i + 1 < steps.size(); ++i) {  // the last copied step is the body
            auto& f = *steps[i];
            if (!f.suppressed() && !makesNoBody(f) && !used.count(f.id())) f.setReference(true);
        }
        auto problems = part->execute(
            std::make_unique<AddFeaturesCommand>(std::move(steps), design.customMaterials(), "Copy " + name));
        if (!problems.empty()) {
            out.problems = std::move(problems);
            out.parts.clear();
            return out;
        }
        part->clearHistory();  // the part starts as it is, like one opened from a file
        out.parts.push_back({std::move(part), std::move(g.at), std::move(g.bodies)});
    }
    return out;
}

MakePartsFromBodiesCommand::MakePartsFromBodiesCommand(std::vector<BodyPart> parts) {
    steps_.push_back(std::make_unique<MakeAssemblyCommand>());
    for (auto& p : parts) {
        if (!p.part || p.at.empty()) continue;
        const DocId id = p.part->id();
        steps_.push_back(std::make_unique<InsertPartCommand>(std::move(p.part), 1, p.at.front()));
        for (std::size_t i = 1; i < p.at.size(); ++i)
            steps_.push_back(std::make_unique<AddComponentsCommand>(id, 1, p.at[i]));
    }
}

MakePartsFromBodiesCommand::~MakePartsFromBodiesCommand() = default;

Diagnostics MakePartsFromBodiesCommand::apply(DocumentEditor& editor) {
    if (steps_.size() < 2) return {error("parts.no_bodies", "There are no bodies to make parts of.")};
    applied_ = 0;
    for (auto& step : steps_) {
        if (auto problems = step->apply(editor); !problems.empty()) {
            revert(editor);  // all or nothing
            return problems;
        }
        ++applied_;
    }
    return {};
}

void MakePartsFromBodiesCommand::revert(DocumentEditor& editor) {
    while (applied_ > 0) steps_[--applied_]->revert(editor);
}

}  // namespace sb::model
