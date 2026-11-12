#include "sb/model/Document.h"
#include "sb/model/ConstructionPlane.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <limits>
#include <set>
#include <utility>

#include "sb/model/Formulas.h"

namespace sb::model {

// ---------------------------------------------------------------------------
// DocumentEditor
// ---------------------------------------------------------------------------

namespace {

// Revisions count up across every document, so a number never stands for two
// versions of a step, even in two designs open at once.
std::atomic<std::uint64_t> lastRevision{0};

}  // namespace

// Anything that hands out a step to change gives it a new revision, so the next
// rebuild builds it again (see Document::regenerate).
Feature* DocumentEditor::find(const FeatureId& id) {
    for (auto& f : doc_.features_) {
        if (f->id() == id) {
            doc_.touch(id);
            return f.get();
        }
    }
    return nullptr;
}

void DocumentEditor::insert(std::size_t index, std::unique_ptr<Feature> feature) {
    index = std::min(index, doc_.features_.size());
    doc_.touch(feature->id());
    doc_.features_.insert(doc_.features_.begin() + static_cast<std::ptrdiff_t>(index), std::move(feature));
}

std::unique_ptr<Feature> DocumentEditor::remove(const FeatureId& id) {
    auto& fs = doc_.features_;
    auto it = std::find_if(fs.begin(), fs.end(), [&](const auto& f) { return f->id() == id; });
    if (it == fs.end()) return nullptr;
    // The marker was just after this step: it stays where it is, after the one before.
    if (doc_.marker_ && *doc_.marker_ == id) {
        doc_.marker_ = it == fs.begin() ? FeatureId{} : (*std::prev(it))->id();
    }
    auto out = std::move(*it);
    fs.erase(it);
    return out;
}

std::size_t DocumentEditor::insertAtMarker(std::unique_ptr<Feature> feature) {
    const std::size_t at = doc_.markerIndex();
    const bool rolledBack = doc_.rolledBack();
    const FeatureId id = feature->id();
    insert(at, std::move(feature));
    if (rolledBack) doc_.marker_ = id;
    return at;
}

void DocumentEditor::setMarker(std::size_t index) {
    const auto& fs = doc_.features_;
    if (index >= fs.size()) {
        doc_.marker_.reset();
    } else {
        doc_.marker_ = index == 0 ? FeatureId{} : fs[index - 1]->id();
    }
}

std::unique_ptr<Feature> DocumentEditor::replace(std::unique_ptr<Feature> f) {
    for (auto& slot : doc_.features_) {
        if (slot->id() == f->id()) {
            doc_.touch(f->id());
            std::swap(slot, f);
            return f;  // now holds the old feature
        }
    }
    return nullptr;
}

std::vector<Material>& DocumentEditor::materials() {
    return doc_.materials_;
}

ParameterTable& DocumentEditor::parameters() {
    return doc_.parameters_;
}

std::vector<Folder>& DocumentEditor::folders() {
    return doc_.folders_;
}

void DocumentEditor::setLengthUnit(LengthUnit unit) {
    doc_.lengthUnit_ = unit;
}

// ---------------------------------------------------------------------------
// RebuildReport
// ---------------------------------------------------------------------------

const FeatureResult* RebuildReport::result(const FeatureId& id) const {
    auto it = results_.find(id);
    return it == results_.end() ? nullptr : &it->second;
}

const kernel::ShapeHandle* RebuildReport::shapeOf(const FeatureId& id) const {
    const auto* r = result(id);
    // A sketch comes out Ok with no shape: it isn't a body.
    return (r && r->status == FeatureStatus::Ok && r->shape.valid()) ? &r->shape : nullptr;
}

bool RebuildReport::ok() const {
    return std::all_of(results_.begin(), results_.end(), [](const auto& kv) {
        return kv.second.status == FeatureStatus::Ok || kv.second.status == FeatureStatus::Suppressed ||
               kv.second.status == FeatureStatus::RolledBack;
    });
}

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

void Document::touch(const FeatureId& id) {
    revisions_[id] = ++lastRevision;
}

std::uint64_t Document::revision(const FeatureId& id) const {
    if (!find(id)) return 0;
    auto it = revisions_.find(id);
    return it == revisions_.end() ? 0 : it->second;
}

Document::Document(DocKind kind, std::string name) : Document(DocId::generate(), kind, std::move(name)) {}

Document::Document(DocId id, DocKind kind, std::string name) : id_(id), kind_(kind), name_(std::move(name)) {}

Document::~Document() = default;

const Feature* Document::find(const FeatureId& id) const {
    for (const auto& f : features_) {
        if (f->id() == id) return f.get();
    }
    return nullptr;
}

std::optional<std::size_t> Document::indexOf(const FeatureId& id) const {
    for (std::size_t i = 0; i < features_.size(); ++i) {
        if (features_[i]->id() == id) return i;
    }
    return std::nullopt;
}

std::vector<FeatureId> Document::dependents(const FeatureId& id) const {
    std::vector<FeatureId> out;
    for (const auto& f : features_) {
        const auto deps = f->dependencies();
        if (std::find(deps.begin(), deps.end(), id) != deps.end()) out.push_back(f->id());
    }
    return out;
}

std::size_t Document::markerIndex() const {
    if (!marker_) return features_.size();
    if (marker_->isNil()) return 0;
    const auto at = indexOf(*marker_);
    return at ? *at + 1 : features_.size();
}

bool Document::afterMarker(const FeatureId& id) const {
    const auto at = indexOf(id);
    return at && *at >= markerIndex();
}

std::optional<FeatureId> Document::marker() const {
    return rolledBack() ? marker_ : std::nullopt;
}

void Document::tidyMarker() {
    if (marker_ && markerIndex() >= features_.size()) marker_.reset();
}

std::string Document::uniqueName(const std::string& base) const {
    for (int n = 1;; ++n) {
        std::string candidate = base + std::to_string(n);
        const bool taken = std::any_of(features_.begin(), features_.end(),
                                       [&](const auto& f) { return f->name() == candidate; });
        if (!taken) return candidate;
    }
}

// ---------------------------------------------------------------------------
// Folders
// ---------------------------------------------------------------------------

void Document::updateFolders() {
    // Each folder with only the steps still here, in design order; then the
    // folders in the order of their first step.
    std::vector<std::pair<std::size_t, Folder>> listed;
    for (const auto& folder : folders_) {
        std::vector<std::pair<std::size_t, FeatureId>> here;
        for (const auto& id : folder.steps) {
            if (auto at = indexOf(id)) here.emplace_back(*at, id);
        }
        if (here.empty()) continue;
        std::sort(here.begin(), here.end());
        Folder shown{folder.id, folder.name, {}};
        for (const auto& [at, id] : here) shown.steps.push_back(id);
        listed.emplace_back(here.front().first, std::move(shown));
    }
    std::stable_sort(listed.begin(), listed.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    shown_.clear();
    for (auto& [at, folder] : listed) shown_.push_back(std::move(folder));
}

std::vector<Folder> Document::folders() const {
    return shown_;
}

const Folder* Document::findFolder(const FolderId& id) const {
    for (const auto& f : shown_) {
        if (f.id == id) return &f;
    }
    return nullptr;
}

const Folder* Document::findFolder(std::string_view name) const {
    for (const auto& f : shown_) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

FolderId Document::folderOf(const FeatureId& id) const {
    for (const auto& f : shown_) {
        if (std::find(f.steps.begin(), f.steps.end(), id) != f.steps.end()) return f.id;
    }
    return {};
}

std::string Document::uniqueFolderName(const std::string& base) const {
    for (int n = 1;; ++n) {
        std::string candidate = base + std::to_string(n);
        if (!findFolder(std::string_view(candidate))) return candidate;
    }
}

Diagnostics Document::execute(std::unique_ptr<Command> command) {
    if (!command) return {{"command.missing", Severity::Error, "There was nothing to do.", {}}};

    DocumentEditor editor(*this);
    const auto markerBefore = marker_;
    auto issues = command->apply(editor);
    updateFolders();
    if (!issues.empty()) {
        marker_ = markerBefore;
        return issues;  // contract: the document is unchanged
    }
    tidyMarker();

    undone_.clear();
    const std::uint64_t serial = nextSerial_++;
    if (!done_.empty() && done_.back().command->mergeWith(*command)) {
        done_.back().serial = serial;  // merged: same undo step, new state
    } else {
        done_.push_back({std::move(command), serial, markerBefore});
        if (done_.size() > undoLimit_) done_.erase(done_.begin());
    }
    return {};
}

bool Document::undo() {
    if (done_.empty()) return false;
    DocumentEditor editor(*this);
    done_.back().command->revert(editor);
    marker_ = done_.back().markerBefore;
    updateFolders();
    undone_.push_back(std::move(done_.back()));
    done_.pop_back();
    return true;
}

bool Document::redo() {
    if (undone_.empty()) return false;
    DocumentEditor editor(*this);
    auto issues = undone_.back().command->apply(editor);
    updateFolders();
    tidyMarker();
    if (!issues.empty()) return false;  // should not happen: it applied cleanly before
    done_.push_back(std::move(undone_.back()));
    undone_.pop_back();
    return true;
}

std::string Document::undoLabel() const {
    return done_.empty() ? "" : done_.back().command->label();
}

std::string Document::redoLabel() const {
    return undone_.empty() ? "" : undone_.back().command->label();
}

void Document::clearHistory() {
    // The current state becomes the base state: serial 0. A document that was
    // saved in this state stays unmodified.
    const bool wasSaved = !isModified();
    done_.clear();
    undone_.clear();
    savedSerial_ = wasSaved ? 0 : std::numeric_limits<std::uint64_t>::max();
}

void Document::setUndoLimit(std::size_t limit) {
    undoLimit_ = std::max<std::size_t>(limit, 1);
    while (done_.size() > undoLimit_) done_.erase(done_.begin());
}

void Document::markSaved() {
    savedSerial_ = currentSerial();
}

bool Document::isModified() const {
    return currentSerial() != savedSerial_;
}

namespace {

// Where a sketch's plane is in this rebuild: its base plane, or its face as the
// body is now. A face that can't be found, or isn't flat, stops the sketch.
struct SketchWhere {
    std::optional<PlaneAxes> axes;  // empty if it can't be worked out
    Diagnostic why;                 // then, why
};

SketchWhere sketchAxes(const SketchFeature& step, const RebuildReport& report) {
    if (!step.onPlane().isNil()) {
        const auto* p = report.result(step.onPlane());
        if (p && p->status == FeatureStatus::Ok && p->plane) return {p->plane, {}};
        return {std::nullopt,
                Diagnostic{"sketch.plane_missing",
                           Severity::Error,
                           step.name() + " is on a plane that isn't there. Fix the plane, or put the sketch "
                                         "somewhere else.",
                           {}}};
    }
    if (!step.face()) return {planeAxes(step.plane()), {}};
    const auto& on = *step.face();
    const auto* body = report.result(on.body);
    const auto pickAgain = " Pick a flat face for it again: pick the sketch, then Sketch > Move to a face.";
    if (!body || !body->names) {
        return {std::nullopt,
                Diagnostic{"sketch.face_missing",
                           Severity::Error,
                           step.name() + " is on a face of a body that isn't there." + pickAgain,
                           {}}};
    }
    const auto found = resolve(on.face, *body->names);
    if (!found.found()) {
        return {std::nullopt,
                Diagnostic{"sketch.face_missing",
                           Severity::Error,
                           step.name() +
                               (found.status == ResolveStatus::Ambiguous
                                    ? " can't tell which face it's on: an earlier change split it into faces "
                                      "that look alike."
                                    : " can't find the face it's on: an earlier change took it away.") +
                               pickAgain,
                           {}}};
    }
    const auto& info = body->names->elements.faces.at(static_cast<std::size_t>(found.index));
    const auto axes = faceAxes(info);
    if (!axes) {
        return {std::nullopt,
                Diagnostic{"sketch.face_curved",
                           Severity::Error,
                           step.name() + " is on a curved face, and a sketch needs a flat one." + pickAgain,
                           {}}};
    }
    return {axes, {}};
}

}  // namespace

RebuildReport Document::regenerate(kernel::IKernel& kernel, sketch::ISketchSolver* solver,
                                   const RebuildReport* previous) const {
    RebuildReport report;
    report.kernel_ = &kernel;
    report.solver_ = solver;
    report.unit_ = lengthUnit_;
    std::set<FeatureId> usedAsInput;
    const ParameterValues values = parameters_.evaluate();  // before any step, as the design says
    const std::size_t builtCount = markerIndex();

    // The previous rebuild is only any use if it was made the same way.
    if (previous &&
        (previous->kernel_ != &kernel || previous->solver_ != solver || previous->unit_ != lengthUnit_))
        previous = nullptr;
    std::set<FeatureId> reused;
    // A step keeps its last result if it hasn't changed since, every step it uses
    // kept theirs, and it got as far as the kernel or the solver last time.
    auto lastResult = [&](const Feature& f) -> const FeatureResult* {
        if (!previous) return nullptr;
        auto rev = previous->revisions_.find(f.id());
        if (rev == previous->revisions_.end() || rev->second != revision(f.id())) return nullptr;
        const auto* last = previous->result(f.id());
        if (!last || (last->status != FeatureStatus::Ok && last->status != FeatureStatus::Failed)) return nullptr;
        for (const auto& dep : f.dependencies())
            if (!reused.count(dep)) return nullptr;
        return last;
    };
    // A sketch's dimensions whose formulas can't be worked out keep the number
    // they last had, and say why.
    auto dimensionIssues = [&](const SketchFeature& step) {
        Diagnostics out;
        for (const auto& c : step.sketch().constraints) {
            if (c.expression.empty() || !sketch::isDimension(c.kind)) continue;
            const std::string what = c.name + " in " + step.name();
            auto worked = evalDimension(c.kind, c.expression, values, what);
            if (worked.ok()) continue;
            out.push_back({"dimension.formula",
                           Severity::Warning,
                           worked.issues.front().message + " It keeps its last value, " +
                               (c.kind == sketch::ConstraintKind::Angle ? quantityText({c.value, kAngle}, lengthUnit_)
                                                                        : lengthText(c.value, lengthUnit_)) +
                               ".",
                           {}});
        }
        return out;
    };

    for (std::size_t i = 0; i < features_.size(); ++i) {
        const auto& f = features_[i];
        FeatureResult r;
        if (i >= builtCount) {
            r.status = FeatureStatus::RolledBack;
        } else if (f->suppressed()) {
            r.status = FeatureStatus::Suppressed;
        } else {
            // A feature is blocked if any input has no shape (failed, blocked,
            // suppressed or missing). Name the first such input for the message.
            std::optional<std::string> blocker;  // the message, once one input is found
            Diagnostics formulaIssues;           // warnings about formulas that don't work
            const auto sketchInputs = f->sketchInputs();
            const auto lookedAt = f->referenceInputs();
            for (const auto& dep : f->dependencies()) {
                // A body only looked at (sketched on) stays a body of the design.
                if (std::find(lookedAt.begin(), lookedAt.end(), dep) == lookedAt.end())
                    usedAsInput.insert(dep);
                if (blocker) continue;
                const Feature* d = find(dep);
                if (std::find(sketchInputs.begin(), sketchInputs.end(), dep) != sketchInputs.end()) {
                    // A sketch input is ready when it solved.
                    const auto* input = report.result(dep);
                    if (input && input->status == FeatureStatus::Ok && input->sketch && input->sketch->ok())
                        continue;
                    if (!d) {
                        blocker =
                            "This step uses a sketch that no longer exists. Change or delete this step.";
                    } else if (d->type() != FeatureType::Sketch) {
                        blocker =
                            "This step uses " + d->name() + ", which isn't a sketch. Pick a sketch instead.";
                    } else if (d->suppressed()) {
                        blocker = "This step uses " + d->name() + ", which is turned off. Turn " + d->name() +
                                  " back on, or change this step.";
                    } else {
                        blocker =
                            "This step uses " + d->name() + ", which didn't solve. Fix the sketch first.";
                    }
                    continue;
                }
                if (report.shapeOf(dep)) continue;
                if (d && d->type() == FeatureType::Plane) {
                    // A plane is ready when it was worked out.
                    const auto* input = report.result(dep);
                    if (input && input->status == FeatureStatus::Ok && input->plane) continue;
                    blocker = "This step uses " + d->name() + ", which " +
                              (d->suppressed() ? "is turned off. Turn it back on, or change this step."
                                               : "has a problem. Fix that plane first.");
                    continue;
                }
                if (!d) {
                    blocker = "This step uses a step that no longer exists. Change or delete this step.";
                } else if (d->type() == FeatureType::Sketch) {
                    blocker = "This step uses " + d->name() +
                              ", which is a sketch, not a body. Pick a body instead.";
                } else if (d->suppressed()) {
                    blocker = "This step uses " + d->name() + ", which is turned off. Turn " + d->name() +
                              " back on, or change this step.";
                } else {
                    blocker = "This step uses " + d->name() + ", which has a problem. Fix that step first.";
                }
            }
            // A number whose formula can't be worked out keeps the value it last
            // had, and says why (a sketch's dimensions are checked below).
            for (const auto& n : f->numbers()) {
                const std::string text = f->formula(n.key);
                if (text.empty()) continue;
                const auto worked = evalNumber(n.dim, n.positive, text, values, n.key + " in " + f->name());
                if (worked.ok()) continue;
                const double last = f->number(n.key).value_or(0);
                formulaIssues.push_back({"dimension.formula",
                                         Severity::Warning,
                                         worked.issues.front().message + " It keeps its last value, " +
                                             (n.dim == kAngle ? quantityText({last, kAngle}, lengthUnit_)
                                                              : lengthText(last, lengthUnit_)) +
                                             ".",
                                         {}});
            }
            std::optional<std::size_t> ownIssues;  // issues before the formula warnings, if not all of them
            if (blocker) {
                r.status = FeatureStatus::Blocked;
                r.issues.push_back({"feature.blocked", Severity::Error, *blocker, {}});
                ownIssues = r.issues.size();
            } else if (const auto* last = lastResult(*f)) {
                // Unchanged: the last result, without its formula warnings, which
                // are worked out again below (a parameter may have changed).
                r = *last;
                r.issues.resize(r.issues.size() - previous->formulaIssues_.at(f->id()));
                ownIssues = r.issues.size();
                if (f->type() == FeatureType::Sketch) {
                    const auto dims = dimensionIssues(static_cast<const SketchFeature&>(*f));
                    r.issues.insert(r.issues.end(), dims.begin(), dims.end());
                }
                reused.insert(f->id());
                report.reused_.push_back(f->id());
            } else if (f->type() == FeatureType::Plane) {
                // Planes are worked out, not built: a plane, no shape, and not a body.
                Diagnostic why;
                if (auto at = static_cast<const PlaneFeature&>(*f).where(report, why)) {
                    r.status = FeatureStatus::Ok;
                    r.plane = *at;
                } else {
                    r.status = FeatureStatus::Failed;
                    r.issues.push_back(why);
                }
            } else if (f->type() == FeatureType::Sketch) {
                // Sketches are solved, not built: no shape, and not a body.
                if (!solver) {
                    r.status = FeatureStatus::Failed;
                    r.issues.push_back({"sketch.no_solver",
                                        Severity::Error,
                                        "This copy of SketchBillet can't solve sketches.",
                                        {}});
                } else if (auto where = sketchAxes(static_cast<const SketchFeature&>(*f), report);
                           !where.axes) {
                    r.status = FeatureStatus::Failed;
                    r.issues.push_back(where.why);
                } else {
                    auto solved = std::make_shared<sketch::SolveResult>(
                        solver->solve(static_cast<const SketchFeature&>(*f).sketch()));
                    r.status = solved->ok() ? FeatureStatus::Ok : FeatureStatus::Failed;
                    r.issues = solved->issues;
                    ownIssues = r.issues.size();
                    const auto dims = dimensionIssues(static_cast<const SketchFeature&>(*f));
                    r.issues.insert(r.issues.end(), dims.begin(), dims.end());
                    r.sketch = std::move(solved);
                    r.plane = *where.axes;
                }
            } else {
                auto built = f->build(kernel, report);
                r.status = built.op.ok() ? FeatureStatus::Ok : FeatureStatus::Failed;
                r.shape = built.op.shape;
                r.issues = std::move(built.op.issues);
                if (r.status == FeatureStatus::Ok) {
                    r.names = std::move(built.names);
                    r.tools = std::move(built.tools);
                }
            }
            const std::size_t own = ownIssues.value_or(r.issues.size());
            r.issues.insert(r.issues.end(), formulaIssues.begin(), formulaIssues.end());
            report.formulaIssues_[f->id()] = r.issues.size() - own;
        }
        report.revisions_[f->id()] = revision(f->id());
        report.order_.push_back(f->id());
        report.results_.emplace(f->id(), std::move(r));
    }

    for (const auto& id : report.order_) {
        if (report.shapeOf(id) && !usedAsInput.count(id)) report.bodies_.push_back(id);
    }
    return report;
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

const Material* Document::findMaterial(std::string_view keyOrName) const {
    auto same = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) ==
                          std::tolower(static_cast<unsigned char>(y));
               });
    };
    for (const auto& m : materials_) {
        if (m.key == keyOrName || same(m.name, keyOrName)) return &m;
    }
    return findBuiltinMaterial(keyOrName);
}

FeatureId Document::materialSource(const FeatureId& id) const {
    // Follow first inputs back. Inputs always come earlier, so this ends; the
    // step limit is a guard against a damaged design.
    const Feature* f = find(id);
    for (std::size_t steps = 0; f && steps <= features_.size(); ++steps) {
        if (!f->material().empty() && findMaterial(f->material())) return f->id();
        // A body made from others (a Boolean's A, a Move's body) is made of what its first input is.
        const auto inputs = f->dependencies();
        if (inputs.empty()) break;
        f = find(inputs.front());
    }
    return {};
}

const Material& Document::materialOf(const FeatureId& id) const {
    const FeatureId source = materialSource(id);
    if (source.isNil()) return defaultMaterial();
    return *findMaterial(find(source)->material());
}

}  // namespace sb::model
