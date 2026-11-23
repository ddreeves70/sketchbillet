#include "sb/plugin/DocumentList.h"

#include <algorithm>
#include <string>
#include <utility>

#include "sb/model/AssemblyCommands.h"
#include "sb/plugin/Events.h"

namespace sb::plugin {

namespace {

Diagnostic notOpen() {
    return {"document.not_open",
            Severity::Error,
            "That design isn't open any more. Open it again and retry.",
            {}};
}

}  // namespace

DocumentList::DocumentList(EventBus& bus, const model::CommandRegistry& commands, kernel::IKernel* kernel,
                           sketch::ISketchSolver* solver)
    : bus_(bus), commands_(commands), kernel_(kernel), solver_(solver) {}

DocumentList::~DocumentList() = default;

DocumentList::Entry* DocumentList::entry(const DocId& id) {
    auto it = docs_.find(id);
    return it == docs_.end() ? nullptr : &it->second;
}

DocumentList::Entry* DocumentList::owner(const DocId& id) {
    return const_cast<Entry*>(std::as_const(*this).owner(id));
}

const DocumentList::Entry* DocumentList::owner(const DocId& id) const {
    if (id.isNil()) return nullptr;
    if (auto it = docs_.find(id); it != docs_.end()) return &it->second;
    for (const auto& [_, e] : docs_)
        if (e.editing == id) return &e;
    return nullptr;
}

DocId DocumentList::add(std::unique_ptr<model::Document> doc) {
    if (!doc) return {};
    const DocId id = doc->id();
    if (docs_.count(id)) return id;  // already open: the same design, opened twice
    order_.push_back(id);
    docs_[id].doc = std::move(doc);
    bus_.publish(DocumentOpened{id});
    // Look it up again after each event: a listener may have closed it.
    if (auto* e = entry(id)) rebuild(*e);
    if (active_.isNil() && docs_.count(id)) setActive(id);
    return id;
}

bool DocumentList::close(const DocId& id) {
    auto it = docs_.find(id);
    if (it == docs_.end()) return false;
    // Take it out first, so listeners never see a closed design in the list.
    auto closing = std::move(it->second);
    docs_.erase(it);
    order_.erase(std::find(order_.begin(), order_.end(), id));
    bus_.publish(DocumentClosed{id});
    if (active_ == id) {
        active_ = order_.empty() ? DocId{} : order_.front();
        bus_.publish(ActiveDocumentChanged{active()});
    }
    return true;
}

std::vector<DocId> DocumentList::ids() const {
    return order_;
}

const model::Document* DocumentList::find(const DocId& id) const {
    const auto* e = owner(id);
    if (!e) return nullptr;
    return e->doc->id() == id ? e->doc.get() : e->doc->findPart(id);
}

DocId DocumentList::active() const {
    auto it = docs_.find(active_);
    if (it != docs_.end() && !it->second.editing.isNil()) return it->second.editing;
    return active_;
}

bool DocumentList::setActive(const DocId& id) {
    const auto* e = owner(id);
    if (!e) return false;
    const DocId design = e->doc->id();
    if (active_ == design) return true;
    active_ = design;
    bus_.publish(ActiveDocumentChanged{active()});
    return true;
}

Diagnostics DocumentList::editPart(const DocId& assembly, const model::ComponentId& component) {
    auto* e = entry(assembly);
    if (!e) return {notOpen()};
    const auto& doc = *e->doc;
    const auto& components = doc.components();
    const auto c = std::find_if(components.begin(), components.end(),
                                [&](const model::Component& each) { return each.id == component; });
    if (c == components.end()) {
        return {{"assembly.no_component",
                 Severity::Error,
                 "That component isn't in " + doc.name() + " any more. Pick another one and try again.",
                 {}}};
    }
    const auto* part = doc.findPart(c->part);
    if (!part) {
        return {{"part.not_found",
                 Severity::Error,
                 c->name + "'s part isn't in " + doc.name() + ". Pick another component and try again.",
                 {}}};
    }
    if (const auto* link = doc.partLink(c->part)) {
        return {{"part.linked",
                 Severity::Error,
                 part->name() + " is linked to its own file, " + link->path +
                     ", so change it there: Open part opens it beside " + doc.name() +
                     ". Or make it an inside part first (Make part inside).",
                 {}}};
    }
    if (e->editing == c->part && e->component == component) return {};
    e->editing = c->part;
    e->component = component;
    if (active_ == assembly) bus_.publish(ActiveDocumentChanged{active()});
    return {};
}

bool DocumentList::finishEditing(const DocId& assembly) {
    auto* e = entry(assembly);
    if (!e || e->editing.isNil()) return false;
    e->editing = DocId{};
    e->component = model::ComponentId{};
    if (active_ == assembly) bus_.publish(ActiveDocumentChanged{active()});
    return true;
}

DocId DocumentList::editedPart(const DocId& assembly) const {
    auto it = docs_.find(assembly);
    return it == docs_.end() ? DocId{} : it->second.editing;
}

model::ComponentId DocumentList::editedComponent(const DocId& assembly) const {
    auto it = docs_.find(assembly);
    return it == docs_.end() ? model::ComponentId{} : it->second.component;
}

DocId DocumentList::assemblyOf(const DocId& part) const {
    if (part.isNil()) return {};
    for (const auto& [id, e] : docs_)
        if (e.editing == part) return id;
    return {};
}

void DocumentList::checkEditing(Entry& e) {
    if (e.editing.isNil()) return;
    const auto& doc = *e.doc;
    const auto& components = doc.components();
    const bool still = doc.findPart(e.editing) && !doc.partLink(e.editing) &&
                       std::any_of(components.begin(), components.end(), [&](const model::Component& c) {
                           return c.id == e.component && c.part == e.editing;
                       });
    if (!still) finishEditing(doc.id());
}

Diagnostics DocumentList::execute(const DocId& id, std::unique_ptr<model::Command> command) {
    auto* e = owner(id);
    if (!e) return {notOpen()};
    // A change to a part being edited is a step of its assembly.
    const bool inPart = e->doc->id() != id;
    if (inPart) command = std::make_unique<model::EditPartCommand>(id, std::move(command));
    const std::string label = command ? command->label() : std::string();
    auto issues = e->doc->execute(std::move(command));
    // An edit in a part is named once it has run ("Add box in Wheel").
    if (issues.empty()) changed(*e, ChangeKind::Executed, inPart ? e->doc->undoLabel() : label);
    return issues;
}

Diagnostics DocumentList::run(const DocId& id, std::string_view name, const model::Args& args) {
    const auto* doc = find(id);
    if (!doc) return {notOpen()};
    auto made = commands_.create(name, args, *doc);
    if (!made.ok()) return std::move(made.issues);
    return execute(id, std::move(made.command));
}

bool DocumentList::undo(const DocId& id) {
    auto* e = owner(id);
    if (!e) return false;
    const auto label = e->doc->undoLabel();
    if (!e->doc->undo()) return false;
    changed(*e, ChangeKind::Undone, label);
    return true;
}

bool DocumentList::redo(const DocId& id) {
    auto* e = owner(id);
    if (!e) return false;
    const auto label = e->doc->redoLabel();
    if (!e->doc->redo()) return false;
    changed(*e, ChangeKind::Redone, label);
    return true;
}

bool DocumentList::markSaved(const DocId& id) {
    auto* e = owner(id);
    if (!e) return false;
    e->doc->markSaved();
    return true;
}

const model::RebuildReport* DocumentList::lastRebuild(const DocId& id) const {
    const auto* e = owner(id);
    if (!e || !e->rebuild) return nullptr;
    return e->doc->id() == id ? &*e->rebuild : e->rebuild->part(id);
}

void DocumentList::changed(Entry& e, ChangeKind kind, std::string label) {
    const DocId id = e.doc->id();
    // Undo may have taken away the part being edited: stop editing it first, so
    // listeners never see a part that's gone as the active design.
    checkEditing(e);
    bus_.publish(DocumentChanged{id, kind, std::move(label)});
    // A DocumentChanged listener may have closed the design.
    if (auto* still = entry(id)) rebuild(*still);
}

void DocumentList::rebuild(Entry& e) {
    if (!kernel_) return;
    // Only the steps that changed, and the steps that use them, build again.
    e.rebuild = e.doc->regenerate(*kernel_, solver_, e.rebuild ? &*e.rebuild : nullptr);
    bus_.publish(RebuildFinished{e.doc->id(), e.rebuild->ok()});
}

}  // namespace sb::plugin
