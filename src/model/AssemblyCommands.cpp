#include "sb/model/AssemblyCommands.h"

#include <algorithm>

namespace sb::model {

namespace {

Diagnostic error(std::string code, std::string message) {
    return {std::move(code), Severity::Error, std::move(message), {}};
}

constexpr int kMostComponents = 100;  // at once

// Components only belong to assemblies.
std::optional<Diagnostic> needAssembly(const Document& doc) {
    if (doc.kind() == DocKind::Assembly) return std::nullopt;
    return error("assembly.not_assembly",
                 doc.name() +
                     " is a part, not an assembly, so it has no components. Start an "
                     "assembly in a new design with assembly.new.");
}

std::optional<Diagnostic> checkCount(int count) {
    if (count >= 1 && count <= kMostComponents) return std::nullopt;
    return error("assembly.count", "Put in from 1 to " + std::to_string(kMostComponents) +
                                       " at a time, not " + std::to_string(count) + ".");
}

kernel::Placement shifted(const kernel::Placement& at, const kernel::Vec3& step, int i) {
    kernel::Placement p = at;
    p.origin = {at.origin.x + step.x * i, at.origin.y + step.y * i, at.origin.z + step.z * i};
    return p;
}

std::optional<Diagnostic> checkPlacement(kernel::Placement& p) {
    const auto tidy = kernel::tidyPlacement(p);
    if (!tidy) {
        return error("assembly.placement",
                     "A component needs an x direction and a z direction that aren't along each other "
                     "(and aren't 0), like x=1,0,0 and z=0,0,1.");
    }
    p = *tidy;
    return std::nullopt;
}

// Adds `count` components of a part, in a row; the first one grounded if the
// assembly was empty. `ids` are made the first time and kept for redo.
void addCopies(DocumentEditor& editor, const DocId& part, const std::string& name, int count,
               const kernel::Placement& at, const kernel::Vec3& step, std::vector<ComponentId>& ids) {
    if (ids.empty()) {
        for (int i = 0; i < count; ++i) ids.push_back(ComponentId::generate());
    }
    auto& components = editor.components();
    const bool first = components.empty();
    for (int i = 0; i < count; ++i) {
        Component c;
        c.id = ids[static_cast<std::size_t>(i)];
        c.name = editor.document().uniqueComponentName(name);
        c.part = part;
        c.placement = shifted(at, step, i);
        c.grounded = first && i == 0;
        components.push_back(std::move(c));
    }
}

void removeCopies(DocumentEditor& editor, const std::vector<ComponentId>& ids) {
    std::erase_if(editor.components(),
                  [&](const Component& c) { return std::find(ids.begin(), ids.end(), c.id) != ids.end(); });
}

std::string insertLabel(const std::string& name, int count) {
    return count == 1 ? "Insert " + name : "Insert " + std::to_string(count) + " of " + name;
}

}  // namespace

// --- MakeAssemblyCommand -----------------------------------------------------

Diagnostics MakeAssemblyCommand::apply(DocumentEditor& editor) {
    const Document& doc = editor.document();
    if (doc.kind() == DocKind::Assembly) {
        return {error("assembly.already", doc.name() + " is an assembly already.")};
    }
    if (doc.featureCount() > 0 || !doc.parameters().empty()) {
        return {error("assembly.not_empty",
                      doc.name() +
                          " has steps, so it can't become an assembly. Start the assembly in a new design, "
                          "and insert " +
                          doc.name() + " into it as a part.")};
    }
    editor.setKind(DocKind::Assembly);
    return {};
}

void MakeAssemblyCommand::revert(DocumentEditor& editor) {
    editor.setKind(DocKind::Part);
}

// --- InsertPartCommand -------------------------------------------------------

InsertPartCommand::InsertPartCommand(std::unique_ptr<Document> part, int count, kernel::Placement at,
                                     kernel::Vec3 step)
    : part_(std::move(part)), count_(count), at_(at), step_(step) {
    if (part_) {
        partId_ = part_->id();
        partName_ = part_->name();
    }
}

InsertPartCommand::~InsertPartCommand() = default;

std::string InsertPartCommand::label() const {
    return insertLabel(partName_, count_);
}

Diagnostics InsertPartCommand::apply(DocumentEditor& editor) {
    const Document& doc = editor.document();
    if (auto d = needAssembly(doc)) return {*d};
    const bool there = doc.findPart(partId_) != nullptr;
    if (!part_ && !there) return {error("assembly.no_part", "There was no part to put in.")};
    if (part_ && part_->kind() == DocKind::Assembly) {
        return {error("assembly.sub",
                      partName_ + " is an assembly. Assemblies inside assemblies come in a later version: "
                                  "insert its parts one at a time for now.")};
    }
    if (partId_ == doc.id()) {
        return {error("assembly.itself", doc.name() + " can't use itself as a part.")};
    }
    if (auto d = checkCount(count_)) return {*d};
    if (auto d = checkPlacement(at_)) return {*d};
    // A part the assembly has already (the same design) only gets more components.
    addedPart_ = !there;
    if (addedPart_) {
        editor.addPart(std::move(part_));
    } else {
        partName_ = doc.findPart(partId_)->name();
    }
    addCopies(editor, partId_, partName_, count_, at_, step_, ids_);
    return {};
}

void InsertPartCommand::revert(DocumentEditor& editor) {
    removeCopies(editor, ids_);
    if (addedPart_) part_ = editor.removePart(partId_).first;
}

// --- AddPartCommand ----------------------------------------------------------

AddPartCommand::AddPartCommand(std::unique_ptr<Document> part) : part_(std::move(part)) {
    if (part_) {
        id_ = part_->id();
        name_ = part_->name();
    }
}

AddPartCommand::~AddPartCommand() = default;

std::string AddPartCommand::label() const {
    return "Add part " + name_;
}

Diagnostics AddPartCommand::apply(DocumentEditor& editor) {
    const Document& doc = editor.document();
    if (auto d = needAssembly(doc)) return {*d};
    if (!part_) return {error("assembly.no_part", "There was no part to put in.")};
    if (part_->kind() == DocKind::Assembly) {
        return {error("assembly.sub",
                      name_ + " is an assembly. Assemblies inside assemblies come in a later version: "
                              "insert its parts one at a time for now.")};
    }
    if (doc.findPart(id_) || id_ == doc.id()) {
        return {error("assembly.part_there", doc.name() + " has the part " + name_ + " already.")};
    }
    editor.addPart(std::move(part_));
    return {};
}

void AddPartCommand::revert(DocumentEditor& editor) {
    part_ = editor.removePart(id_).first;
}

// --- AddComponentCommand -----------------------------------------------------

AddComponentCommand::AddComponentCommand(Component component) : component_(std::move(component)) {}

Diagnostics AddComponentCommand::apply(DocumentEditor& editor) {
    const Document& doc = editor.document();
    if (auto d = needAssembly(doc)) return {*d};
    if (!doc.findPart(component_.part)) {
        return {error("assembly.no_part", component_.name + "'s part isn't in " + doc.name() + ".")};
    }
    if (component_.id.isNil() || doc.findComponent(component_.id) ||
        doc.findComponent(std::string_view(component_.name)) || component_.name.empty()) {
        return {error("assembly.name_taken",
                      "Another component is called " + component_.name + " already, or has the same id.")};
    }
    if (auto d = checkPlacement(component_.placement)) return {*d};
    editor.components().push_back(component_);
    return {};
}

void AddComponentCommand::revert(DocumentEditor& editor) {
    removeCopies(editor, {component_.id});
}

// --- AddComponentsCommand ----------------------------------------------------

AddComponentsCommand::AddComponentsCommand(DocId part, int count, kernel::Placement at, kernel::Vec3 step)
    : part_(part), count_(count), at_(at), step_(step) {}

std::string AddComponentsCommand::label() const {
    return insertLabel(partName_, count_);
}

Diagnostics AddComponentsCommand::apply(DocumentEditor& editor) {
    const Document& doc = editor.document();
    if (auto d = needAssembly(doc)) return {*d};
    const Document* part = doc.findPart(part_);
    if (!part) return {error("assembly.no_part", "That part isn't in " + doc.name() + " anymore.")};
    if (auto d = checkCount(count_)) return {*d};
    if (auto d = checkPlacement(at_)) return {*d};
    partName_ = part->name();
    addCopies(editor, part_, partName_, count_, at_, step_, ids_);
    return {};
}

void AddComponentsCommand::revert(DocumentEditor& editor) {
    removeCopies(editor, ids_);
}

// --- GroundComponentCommand --------------------------------------------------

GroundComponentCommand::GroundComponentCommand(ComponentId id, bool grounded)
    : id_(id), grounded_(grounded) {}

std::string GroundComponentCommand::label() const {
    return (grounded_ ? "Ground " : "Let go of ") + name_;
}

Diagnostics GroundComponentCommand::apply(DocumentEditor& editor) {
    if (auto d = needAssembly(editor.document())) return {*d};
    for (auto& c : editor.components()) {
        if (c.id != id_) continue;
        name_ = c.name;
        old_ = c.grounded;
        c.grounded = grounded_;
        return {};
    }
    return {error("assembly.no_component", "That component isn't in the assembly anymore.")};
}

void GroundComponentCommand::revert(DocumentEditor& editor) {
    for (auto& c : editor.components()) {
        if (c.id == id_) c.grounded = old_;
    }
}

// --- PlaceComponentCommand ---------------------------------------------------

PlaceComponentCommand::PlaceComponentCommand(ComponentId id, kernel::Placement placement)
    : id_(id), placement_(placement) {}

std::string PlaceComponentCommand::label() const {
    return "Move " + name_;
}

Diagnostics PlaceComponentCommand::apply(DocumentEditor& editor) {
    if (auto d = needAssembly(editor.document())) return {*d};
    if (auto d = checkPlacement(placement_)) return {*d};
    for (auto& c : editor.components()) {
        if (c.id != id_) continue;
        name_ = c.name;
        old_ = c.placement;
        c.placement = placement_;
        return {};
    }
    return {error("assembly.no_component", "That component isn't in the assembly anymore.")};
}

void PlaceComponentCommand::revert(DocumentEditor& editor) {
    for (auto& c : editor.components()) {
        if (c.id == id_) c.placement = old_;
    }
}

// --- RenameComponentCommand --------------------------------------------------

RenameComponentCommand::RenameComponentCommand(ComponentId id, std::string name)
    : id_(id), name_(std::move(name)) {}

std::string RenameComponentCommand::label() const {
    return "Rename " + old_ + " to " + name_;
}

Diagnostics RenameComponentCommand::apply(DocumentEditor& editor) {
    const Document& doc = editor.document();
    if (auto d = needAssembly(doc)) return {*d};
    if (name_.find_first_not_of(" \t") == std::string::npos) {
        return {error("assembly.name_empty", "A component needs a name. Type one, like Front wheel.")};
    }
    if (const auto* other = doc.findComponent(std::string_view(name_)); other && other->id != id_) {
        return {error("assembly.name_taken", "Another component is called " + name_ +
                                                 " already. Pick a name no other component has.")};
    }
    for (auto& c : editor.components()) {
        if (c.id != id_) continue;
        old_ = c.name;
        c.name = name_;
        return {};
    }
    return {error("assembly.no_component", "That component isn't in the assembly anymore.")};
}

void RenameComponentCommand::revert(DocumentEditor& editor) {
    for (auto& c : editor.components()) {
        if (c.id == id_) c.name = old_;
    }
}

// --- DeleteComponentCommand --------------------------------------------------

DeleteComponentCommand::DeleteComponentCommand(ComponentId id) : id_(id) {}

DeleteComponentCommand::~DeleteComponentCommand() = default;

std::string DeleteComponentCommand::label() const {
    return "Delete " + (removed_ ? removed_->name : std::string("component"));
}

Diagnostics DeleteComponentCommand::apply(DocumentEditor& editor) {
    if (auto d = needAssembly(editor.document())) return {*d};
    auto& components = editor.components();
    const auto it =
        std::find_if(components.begin(), components.end(), [&](const Component& c) { return c.id == id_; });
    if (it == components.end()) {
        return {error("assembly.no_component", "That component isn't in the assembly anymore.")};
    }
    removed_ = *it;
    index_ = static_cast<std::size_t>(it - components.begin());
    components.erase(it);
    // Its part goes too when nothing else places it.
    const bool used = std::any_of(components.begin(), components.end(),
                                  [&](const Component& c) { return c.part == removed_->part; });
    if (!used) {
        auto [part, at] = editor.removePart(removed_->part);
        part_ = std::move(part);
        partIndex_ = at;
    }
    return {};
}

void DeleteComponentCommand::revert(DocumentEditor& editor) {
    if (!removed_) return;
    if (part_) editor.addPart(std::move(part_), partIndex_);
    auto& components = editor.components();
    components.insert(components.begin() + static_cast<std::ptrdiff_t>(std::min(index_, components.size())),
                      *removed_);
}

}  // namespace sb::model
