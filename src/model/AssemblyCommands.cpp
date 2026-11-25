#include "sb/model/AssemblyCommands.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>

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

const Document* partNamed(const Document& doc, const std::string& name, Diagnostic& problem) {
    if (const auto* part = doc.findPart(std::string_view(name))) return part;
    if (auto d = needAssembly(doc)) {
        problem = *d;
        return nullptr;
    }
    std::string names;
    for (const auto* p : doc.parts()) names += (names.empty() ? "" : ", ") + p->name();
    problem = error("assembly.no_part",
                    doc.name() + " has no part called " + name + "." +
                        (names.empty() ? std::string(" Put a part in with component.new or component.insert.")
                                       : " Its parts are: " + names + "."));
    return nullptr;
}

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
                                     kernel::Vec3 step, std::optional<PartLink> link)
    : part_(std::move(part)), count_(count), at_(at), step_(step), link_(std::move(link)) {
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
        link_ = editor.setPartLink(partId_, std::move(link_));
    } else {
        partName_ = doc.findPart(partId_)->name();
    }
    addCopies(editor, partId_, partName_, count_, at_, step_, ids_);
    return {};
}

void InsertPartCommand::revert(DocumentEditor& editor) {
    removeCopies(editor, ids_);
    if (addedPart_) {
        link_ = editor.setPartLink(partId_, std::move(link_));
        part_ = editor.removePart(partId_).first;
    }
}

// --- AddPartCommand ----------------------------------------------------------

AddPartCommand::AddPartCommand(std::unique_ptr<Document> part, std::optional<PartLink> link)
    : part_(std::move(part)), link_(std::move(link)) {
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
    link_ = editor.setPartLink(id_, std::move(link_));
    return {};
}

void AddPartCommand::revert(DocumentEditor& editor) {
    link_ = editor.setPartLink(id_, std::move(link_));
    part_ = editor.removePart(id_).first;
}

// --- UpdatePartsCommand ------------------------------------------------------

UpdatePartsCommand::UpdatePartsCommand(std::vector<PartUpdate> changes, std::string label)
    : changes_(std::move(changes)), label_(std::move(label)) {}

UpdatePartsCommand::~UpdatePartsCommand() = default;

Diagnostics UpdatePartsCommand::apply(DocumentEditor& editor) {
    const Document& doc = editor.document();
    if (auto d = needAssembly(doc)) return {*d};
    if (changes_.empty()) return {error("assembly.no_change", "There were no parts to change.")};
    for (const auto& c : changes_) {
        const auto* part = doc.findPart(c.part);
        if (!part) return {error("assembly.no_part", "That part isn't in " + doc.name() + " any more.")};
        if (c.design && c.design->id() != c.part) {
            return {error("part.other_design", c.design->name() + " is a different design from " +
                                                   part->name() +
                                                   ", so it can't take its place. Pick the file " +
                                                   part->name() + " was saved in.")};
        }
        if (c.design && c.design->kind() == DocKind::Assembly) {
            return {error("assembly.sub", c.design->name() +
                                              " is an assembly. Assemblies inside assemblies come in a later "
                                              "version.")};
        }
    }
    swap(editor);
    return {};
}

void UpdatePartsCommand::revert(DocumentEditor& editor) {
    swap(editor);
}

void UpdatePartsCommand::swap(DocumentEditor& editor) {
    // Each change holds what the assembly doesn't have now: swapping twice puts it back.
    for (auto& c : changes_) {
        if (c.design) c.design = editor.replacePart(std::move(c.design));
        c.link = editor.setPartLink(c.part, std::move(c.link));
    }
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
    // Its joints go with it.
    joints_.clear();
    auto& joints = editor.joints();
    for (std::size_t i = 0; i < joints.size();) {
        if (joints[i].a.component == id_ || joints[i].b.component == id_) {
            joints_.emplace_back(i + joints_.size(), joints[i]);
            joints.erase(joints.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    // And its steps in exploded views.
    views_ = editor.explodedViews();
    for (auto& v : editor.explodedViews()) {
        for (auto& step : v.steps) std::erase(step.components, id_);
    }
    // And its press fits.
    fits_.clear();
    auto& fits = editor.pressFits();
    for (std::size_t i = 0; i < fits.size();) {
        if (fits[i].a == id_ || fits[i].b == id_) {
            fits_.emplace_back(i + fits_.size(), fits[i]);
            fits.erase(fits.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (!used) {
        link_ = editor.setPartLink(removed_->part, std::nullopt);
        auto [part, at] = editor.removePart(removed_->part);
        part_ = std::move(part);
        partIndex_ = at;
    }
    return {};
}

void DeleteComponentCommand::revert(DocumentEditor& editor) {
    if (!removed_) return;
    if (part_) {
        editor.addPart(std::move(part_), partIndex_);
        editor.setPartLink(removed_->part, std::move(link_));
        link_.reset();
    }
    auto& components = editor.components();
    components.insert(components.begin() + static_cast<std::ptrdiff_t>(std::min(index_, components.size())),
                      *removed_);
    auto& joints = editor.joints();
    for (const auto& [at, joint] : joints_) {
        joints.insert(joints.begin() + static_cast<std::ptrdiff_t>(std::min(at, joints.size())), joint);
    }
    joints_.clear();
    editor.explodedViews() = views_;
    views_.clear();
    auto& fits = editor.pressFits();
    for (const auto& [at, fit] : fits_) {
        fits.insert(fits.begin() + static_cast<std::ptrdiff_t>(std::min(at, fits.size())), fit);
    }
    fits_.clear();
}

// ---------------------------------------------------------------------------
// Exploded views
// ---------------------------------------------------------------------------

namespace {

// Gives each step without an id a new one.
void giveStepIds(ExplodedView& view) {
    for (auto& step : view.steps)
        if (step.id.isNil()) step.id = ExplodeStepId::generate();
}

// Checks a view could be in this assembly: a name of its own, and steps that
// slide components that are there, along a direction, by a number.
std::optional<Diagnostic> checkExplodedView(const Document& doc, const ExplodedView& view) {
    if (auto d = needAssembly(doc)) return d;
    if (view.name.empty()) return error("explode.name", "An exploded view needs a name, like Exploded 1.");
    if (const auto* other = doc.findExplodedView(std::string_view(view.name)); other && other->id != view.id)
        return error("explode.name", "There's an exploded view called " + view.name +
                                         " already. Give this one another name.");
    for (std::size_t i = 0; i < view.steps.size(); ++i) {
        const auto& step = view.steps[i];
        const std::string which = "Step " + std::to_string(i + 1) + " of " + view.name;
        for (const auto& c : step.components) {
            if (!doc.findComponent(c))
                return error("assembly.no_component",
                             which + " moves a component that isn't in the assembly anymore.");
        }
        const auto& d = step.direction;
        if (!(std::isfinite(d.x) && std::isfinite(d.y) && std::isfinite(d.z)) ||
            std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) < 1e-9)
            return error("explode.direction", which + " needs a direction to slide in, like 0,0,1 for up.");
        if (!std::isfinite(step.distance))
            return error("explode.distance", which + " needs a distance to slide, in mm, like 20.");
    }
    return std::nullopt;
}

}  // namespace

AddExplodedViewCommand::AddExplodedViewCommand(ExplodedView view) : view_(std::move(view)) {}

Diagnostics AddExplodedViewCommand::apply(DocumentEditor& editor) {
    const auto& doc = editor.document();
    if (view_.name.empty() && doc.kind() == DocKind::Assembly) view_.name = doc.uniqueExplodedViewName();
    if (view_.id.isNil()) view_.id = ExplodedViewId::generate();
    if (doc.findExplodedView(view_.id))
        return {error("explode.exists", "That exploded view is in the assembly already.")};
    giveStepIds(view_);
    if (auto d = checkExplodedView(doc, view_)) return {*d};
    editor.explodedViews().push_back(view_);
    return {};
}

void AddExplodedViewCommand::revert(DocumentEditor& editor) {
    std::erase_if(editor.explodedViews(), [&](const ExplodedView& v) { return v.id == view_.id; });
}

SetExplodedViewCommand::SetExplodedViewCommand(ExplodedView view, bool continuesPrevious)
    : view_(std::move(view)), continues_(continuesPrevious) {}

Diagnostics SetExplodedViewCommand::apply(DocumentEditor& editor) {
    auto& views = editor.explodedViews();
    const auto it =
        std::find_if(views.begin(), views.end(), [&](const ExplodedView& v) { return v.id == view_.id; });
    if (it == views.end())
        return {error("explode.missing", "That exploded view isn't in the assembly anymore.")};
    giveStepIds(view_);
    if (auto d = checkExplodedView(editor.document(), view_)) return {*d};
    old_ = *it;
    *it = view_;
    return {};
}

void SetExplodedViewCommand::revert(DocumentEditor& editor) {
    for (auto& v : editor.explodedViews())
        if (v.id == old_.id) v = old_;
}

bool SetExplodedViewCommand::mergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetExplodedViewCommand*>(&next);
    if (!other || !other->continues_ || other->view_.id != view_.id) return false;
    view_ = other->view_;  // old_ stays: undo goes back to before the drag
    return true;
}

DeleteExplodedViewCommand::DeleteExplodedViewCommand(ExplodedViewId id) : id_(id) {}

std::string DeleteExplodedViewCommand::label() const {
    return "Delete " + (removed_ ? removed_->name : std::string("exploded view"));
}

Diagnostics DeleteExplodedViewCommand::apply(DocumentEditor& editor) {
    if (auto d = needAssembly(editor.document())) return {*d};
    auto& views = editor.explodedViews();
    const auto it =
        std::find_if(views.begin(), views.end(), [&](const ExplodedView& v) { return v.id == id_; });
    if (it == views.end())
        return {error("explode.missing", "That exploded view isn't in the assembly anymore.")};
    removed_ = *it;
    index_ = static_cast<std::size_t>(it - views.begin());
    views.erase(it);
    return {};
}

void DeleteExplodedViewCommand::revert(DocumentEditor& editor) {
    if (!removed_) return;
    auto& views = editor.explodedViews();
    views.insert(views.begin() + static_cast<std::ptrdiff_t>(std::min(index_, views.size())), *removed_);
}

// ---------------------------------------------------------------------------
// Press fits
// ---------------------------------------------------------------------------

AddPressFitCommand::AddPressFitCommand(PressFit fit) : fit_(fit) {}

Diagnostics AddPressFitCommand::apply(DocumentEditor& editor) {
    const auto& doc = editor.document();
    if (auto d = needAssembly(doc)) return {*d};
    const auto* a = doc.findComponent(fit_.a);
    const auto* b = doc.findComponent(fit_.b);
    if (!a || !b) return {error("assembly.no_component", "That component isn't in the assembly anymore.")};
    if (fit_.a == fit_.b) {
        return {error("press_fit.same", "A press fit is between two components: pick two different ones.")};
    }
    if (doc.findPressFit(fit_.a, fit_.b)) {
        return {error("press_fit.exists", a->name + " and " + b->name + " are a press fit already.")};
    }
    if (fit_.id.isNil()) fit_.id = PressFitId::generate();
    editor.pressFits().push_back(fit_);
    return {};
}

void AddPressFitCommand::revert(DocumentEditor& editor) {
    auto& fits = editor.pressFits();
    fits.erase(std::remove_if(fits.begin(), fits.end(), [&](const PressFit& f) { return f.id == fit_.id; }),
               fits.end());
}

DeletePressFitCommand::DeletePressFitCommand(ComponentId a, ComponentId b) : a_(a), b_(b) {}

Diagnostics DeletePressFitCommand::apply(DocumentEditor& editor) {
    if (auto d = needAssembly(editor.document())) return {*d};
    const auto* fit = editor.document().findPressFit(a_, b_);
    if (!fit) {
        return {error("press_fit.missing",
                      "Those two aren't a press fit, so there's nothing to unmark. Pick two that are.")};
    }
    auto& fits = editor.pressFits();
    index_ = static_cast<std::size_t>(fit - fits.data());
    removed_ = *fit;
    fits.erase(fits.begin() + static_cast<std::ptrdiff_t>(index_));
    return {};
}

void DeletePressFitCommand::revert(DocumentEditor& editor) {
    if (!removed_) return;
    auto& fits = editor.pressFits();
    fits.insert(fits.begin() + static_cast<std::ptrdiff_t>(std::min(index_, fits.size())), *removed_);
}

// ---------------------------------------------------------------------------
// EditPartCommand
// ---------------------------------------------------------------------------

EditPartCommand::EditPartCommand(DocId part, std::unique_ptr<Command> change)
    : part_(part), change_(std::move(change)) {}

EditPartCommand::~EditPartCommand() = default;

std::string EditPartCommand::label() const {
    const std::string what = change_ ? change_->label() : std::string("Change");
    return partName_.empty() ? what : what + " in " + partName_;
}

Diagnostics EditPartCommand::apply(DocumentEditor& editor) {
    if (auto d = needAssembly(editor.document())) return {*d};
    if (!change_) return {error("command.missing", "There was nothing to do.")};
    const Document* part = editor.document().findPart(part_);
    if (!part) {
        return {error("part.not_found", "That part isn't in " + editor.document().name() +
                                            " any more. Pick a component and try again.")};
    }
    if (const auto* link = editor.document().partLink(part_)) {
        return {error("part.linked",
                      part->name() + " is linked to " +
                          std::filesystem::path(link->path).filename().string() +
                          ", so change it there: open that file (Open part), change it and save it. Or "
                          "make it an inside part first (Make part inside).")};
    }
    partName_ = part->name();
    return editor.applyToPart(part_, *change_, markerBefore_);
}

void EditPartCommand::revert(DocumentEditor& editor) {
    if (change_) editor.revertOnPart(part_, *change_, markerBefore_);
}

bool EditPartCommand::mergeWith(const Command& next) {
    const auto* other = dynamic_cast<const EditPartCommand*>(&next);
    if (!other || other->part_ != part_ || !change_ || !other->change_) return false;
    return change_->mergeWith(*other->change_);
}

// ---------------------------------------------------------------------------
// Joints
// ---------------------------------------------------------------------------

namespace {

std::string valueText(const Joint& j, double v) {
    char text[48];
    if (j.kind == JointKind::Revolute) {
        std::snprintf(text, sizeof text, "%g\xC2\xB0", std::round(v * 180.0 / std::numbers::pi * 1e6) / 1e6);
    } else {
        std::snprintf(text, sizeof text, "%g mm", std::round(v * 1e6) / 1e6);
    }
    return text;
}

}  // namespace

std::optional<Diagnostic> checkJoint(const Document& assembly, const Joint& j) {
    if (auto d = needAssembly(assembly)) return d;
    const Component* a = assembly.findComponent(j.a.component);
    const Component* b = assembly.findComponent(j.b.component);
    if (!a || !b) return error("assembly.no_component", "A joint's component isn't in the assembly anymore.");
    if (a->id == b->id) {
        return error("joint.same_component", j.name + " joins " + a->name +
                                                 " to itself. Pick a point on another component for its "
                                                 "second side.");
    }
    for (const auto* side : {&j.a, &j.b}) {
        if (side->body.isNil()) continue;
        const Component* c = side == &j.a ? a : b;
        const Document* part = assembly.findPart(c->part);
        if (!part || !part->find(side->body)) {
            return error("joint.no_body",
                         "That body isn't in " + c->name + "'s part. Pick one of its bodies.");
        }
    }
    if (const auto* other = assembly.findJoint(std::string_view(j.name)); other && other->id != j.id) {
        return error("joint.name_taken", "There's a joint called " + j.name + " already. Pick another name.");
    }
    if (j.name.empty()) return error("joint.no_name", "A joint needs a name.");
    if (j.kind == JointKind::Rigid) {
        if (j.value != 0 || j.lowest || j.highest) {
            return error("joint.rigid_value",
                         j.name +
                             " is rigid, so it doesn't move: it has no value or limits. Make it revolute "
                             "(it turns) or a slider (it slides) first.");
        }
        return std::nullopt;
    }
    if (j.lowest && j.highest && *j.lowest > *j.highest) {
        return error("joint.limits", j.name + "'s lowest, " + valueText(j, *j.lowest) +
                                         ", is more than its highest, " + valueText(j, *j.highest) +
                                         ". Swap them.");
    }
    if ((j.lowest && j.value < *j.lowest - 1e-12) || (j.highest && j.value > *j.highest + 1e-12)) {
        return error("joint.out_of_limits",
                     j.name + (j.kind == JointKind::Revolute ? " turns" : " slides") + " from " +
                         (j.lowest ? valueText(j, *j.lowest) : std::string("any")) + " to " +
                         (j.highest ? valueText(j, *j.highest) : std::string("any")) + ", so it can't be " +
                         valueText(j, j.value) + ". Pick a value between them, or change its limits.");
    }
    return std::nullopt;
}

AddJointCommand::AddJointCommand(Joint joint) : joint_(std::move(joint)) {}

Diagnostics AddJointCommand::apply(DocumentEditor& editor) {
    if (joint_.id.isNil()) joint_.id = JointId::generate();
    if (joint_.name.empty()) joint_.name = editor.document().uniqueJointName();
    if (auto d = checkJoint(editor.document(), joint_)) return {*d};
    editor.joints().push_back(joint_);
    return {};
}

void AddJointCommand::revert(DocumentEditor& editor) {
    auto& joints = editor.joints();
    joints.erase(
        std::remove_if(joints.begin(), joints.end(), [&](const Joint& j) { return j.id == joint_.id; }),
        joints.end());
}

SetJointCommand::SetJointCommand(Joint joint, bool continuesPrevious)
    : joint_(std::move(joint)), continues_(continuesPrevious) {}

Diagnostics SetJointCommand::apply(DocumentEditor& editor) {
    auto& joints = editor.joints();
    const auto it =
        std::find_if(joints.begin(), joints.end(), [&](const Joint& j) { return j.id == joint_.id; });
    if (it == joints.end()) return {error("joint.missing", "That joint isn't in the assembly anymore.")};
    if (auto d = checkJoint(editor.document(), joint_)) return {*d};
    old_ = *it;
    *it = joint_;
    return {};
}

void SetJointCommand::revert(DocumentEditor& editor) {
    for (auto& j : editor.joints()) {
        if (j.id == old_.id) j = old_;
    }
}

bool SetJointCommand::mergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetJointCommand*>(&next);
    if (!other || !other->continues_ || other->joint_.id != joint_.id) return false;
    joint_ = other->joint_;  // old_ stays: undo goes back to before the drag
    return true;
}

DeleteJointCommand::DeleteJointCommand(JointId id) : id_(id) {}

std::string DeleteJointCommand::label() const {
    return "Delete " + (removed_ ? removed_->name : std::string("joint"));
}

Diagnostics DeleteJointCommand::apply(DocumentEditor& editor) {
    if (auto d = needAssembly(editor.document())) return {*d};
    auto& joints = editor.joints();
    const auto it = std::find_if(joints.begin(), joints.end(), [&](const Joint& j) { return j.id == id_; });
    if (it == joints.end()) return {error("joint.missing", "That joint isn't in the assembly anymore.")};
    removed_ = *it;
    index_ = static_cast<std::size_t>(it - joints.begin());
    joints.erase(it);
    return {};
}

void DeleteJointCommand::revert(DocumentEditor& editor) {
    if (!removed_) return;
    auto& joints = editor.joints();
    joints.insert(joints.begin() + static_cast<std::ptrdiff_t>(std::min(index_, joints.size())), *removed_);
}

ResetJointsCommand::ResetJointsCommand(std::vector<JointId> joints) : only_(std::move(joints)) {}

Diagnostics ResetJointsCommand::apply(DocumentEditor& editor) {
    if (auto d = needAssembly(editor.document())) return {*d};
    old_.clear();
    for (auto& j : editor.joints()) {
        if (!only_.empty() && std::find(only_.begin(), only_.end(), j.id) == only_.end()) continue;
        if (j.kind == JointKind::Rigid || j.value == j.rest) continue;
        old_.emplace_back(j.id, j.value);
        j.value = j.rest;
    }
    if (old_.empty()) {
        return {
            error("joint.at_rest", editor.document().joints().empty()
                                       ? editor.document().name() + " has no joints to reset."
                                       : "Every joint is at its rest already, so there's nothing to reset.")};
    }
    return {};
}

void ResetJointsCommand::revert(DocumentEditor& editor) {
    for (auto& j : editor.joints()) {
        for (const auto& [id, value] : old_) {
            if (j.id == id) j.value = value;
        }
    }
}

}  // namespace sb::model
