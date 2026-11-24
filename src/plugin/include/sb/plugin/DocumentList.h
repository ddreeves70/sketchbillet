#pragma once

/// @file
/// The list of open designs, and the one path for changing them.

#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "sb/core/Diagnostic.h"
#include "sb/core/Id.h"
#include "sb/kernel/IKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Document.h"
#include "sb/plugin/EventBus.h"
#include "sb/plugin/Events.h"

namespace sb::plugin {

/// The designs open in SketchBillet.
///
/// Plugins read designs here and change them only through execute(), run(),
/// undo() and redo(). After every change the list sends DocumentChanged, rebuilds
/// the design (when it has a kernel) and sends RebuildFinished, so every panel
/// and view hears about it no matter which plugin made the change.
///
/// While the student edits one of an assembly's inside parts (Edit Part, see
/// editPart()), that part is the active design: find(), execute(), run(),
/// undo(), redo() and lastRebuild() take its id too, and its changes are steps
/// of the assembly. The assembly is still the open file (activeDesign()).
///
/// @code
/// DocId id = docs.add(std::make_unique<model::Document>());
/// docs.run(id, "feature.add_box", {{"size", kernel::Vec3{10, 20, 30}}});
/// const model::RebuildReport* built = docs.lastRebuild(id);
/// @endcode
class DocumentList {
public:
    /// Makes an empty list.
    /// @param bus Where change events are sent. Must outlive the list.
    /// @param commands The commands run() can find by name. Must outlive the list.
    /// @param kernel The kernel used to rebuild designs after each change; null
    ///        means designs are not rebuilt and RebuildFinished is not sent.
    /// @param solver The solver for Sketch steps; null means Sketch steps fail to rebuild.
    DocumentList(EventBus& bus, const model::CommandRegistry& commands, kernel::IKernel* kernel = nullptr,
                 sketch::ISketchSolver* solver = nullptr);

    /// Closes every design.
    ~DocumentList();

    DocumentList(const DocumentList&) = delete;
    DocumentList& operator=(const DocumentList&) = delete;

    /// Adds a new or opened design. Sends DocumentOpened, then rebuilds it. The
    /// first design added becomes the active one.
    /// @param doc The design. The list takes ownership.
    /// @return The design's id.
    DocId add(std::unique_ptr<model::Document> doc);

    /// Closes a design. Sends DocumentClosed; if it was active, the next open
    /// design (or none) becomes active.
    /// @param id The design to close.
    /// @return False if no open design has that id.
    bool close(const DocId& id);

    /// Lists the open designs.
    /// @return Their ids, in the order they were added.
    std::vector<DocId> ids() const;

    /// Finds an open design, or a part being edited in one, for reading.
    /// @param id The design, or the part.
    /// @return It, or nullptr if it is not open (or not being edited).
    const model::Document* find(const DocId& id) const;

    /// Gives the design the student is working on: the part being edited, while
    /// they edit a part of the active assembly (editPart()), or else the active
    /// design.
    /// @return Its id, or a nil id if nothing is open.
    DocId active() const;

    /// Gives the open file the student is working on: the active assembly even
    /// while they edit one of its parts. Save and close this one.
    /// @return Its id, or a nil id if nothing is open.
    DocId activeDesign() const { return active_; }

    /// Makes another open design the active one. Sends ActiveDocumentChanged.
    /// @param id The design (or a part being edited in it: its assembly).
    /// @return False if it is not open.
    bool setActive(const DocId& id);

    /// Starts editing one of an open assembly's inside parts, in the assembly
    /// (Edit Part): the part becomes the active design (see active()), and its
    /// changes are steps of the assembly. Sends ActiveDocumentChanged if the
    /// assembly is the active design. Editing another part first finishes the one
    /// before.
    /// @param assembly The assembly.
    /// @param component The component whose part to edit: the viewer shows the
    ///        part where it is, with the others around it.
    /// @return Nothing on success; otherwise why not (a linked part is edited in
    ///         its own file), and nothing changed.
    Diagnostics editPart(const DocId& assembly, const model::ComponentId& component);

    /// Stops editing a part (Done), so the assembly is the active design again.
    /// Sends ActiveDocumentChanged if it's the active design. It also stops by
    /// itself when the part or the component goes (undo, say) or the part becomes
    /// linked.
    /// @param assembly The assembly.
    /// @return False if no part of it was being edited.
    bool finishEditing(const DocId& assembly);

    /// Gives the part being edited in an assembly.
    /// @param assembly The assembly.
    /// @return The part's id, or a nil id if none is.
    DocId editedPart(const DocId& assembly) const;

    /// Gives the component that was picked to edit its part (see editPart()).
    /// @param assembly The assembly.
    /// @return The component, or a nil id if no part is being edited.
    model::ComponentId editedComponent(const DocId& assembly) const;

    /// Finds the open assembly a part is being edited in.
    /// @param part The part.
    /// @return The assembly, or a nil id if the part isn't being edited.
    DocId assemblyOf(const DocId& part) const;

    /// Runs a command on a design. On success sends DocumentChanged and rebuilds.
    /// For a part being edited, the command is a step of its assembly
    /// (model::EditPartCommand), and the events name the assembly.
    /// @param id The design, or a part being edited.
    /// @param command The command. The design takes ownership.
    /// @return No diagnostics on success; otherwise why it failed, and nothing changed.
    Diagnostics execute(const DocId& id, std::unique_ptr<model::Command> command);

    /// Runs a command by name, for menus, scripts and the assistant.
    /// @param id The design, or a part being edited.
    /// @param name The command name, e.g. "feature.add_box".
    /// @param args Its arguments.
    /// @return No diagnostics on success; otherwise every problem found.
    Diagnostics run(const DocId& id, std::string_view name, const model::Args& args);

    /// Undoes the newest step of a design. On success sends DocumentChanged and rebuilds.
    /// @param id The design (for a part being edited: its assembly's newest step).
    /// @return False if it is not open or has nothing to undo.
    bool undo(const DocId& id);

    /// Redoes the newest undone step of a design. On success sends DocumentChanged and rebuilds.
    /// @param id The design (for a part being edited: its assembly's).
    /// @return False if it is not open or has nothing to redo.
    bool redo(const DocId& id);

    /// Records that a design was just saved, so it shows no unsaved changes.
    /// @param id The design (for a part being edited: its assembly).
    /// @return False if it is not open.
    bool markSaved(const DocId& id);

    /// Gives the result of a design's latest rebuild.
    /// @param id The design, or a part being edited (its build, from its assembly's).
    /// @return The report, or nullptr if the design is not open or there is no kernel.
    const model::RebuildReport* lastRebuild(const DocId& id) const;

    /// Gives the kernel designs are rebuilt with, for questions about shapes
    /// such as their volume (see model::bodyMass()).
    /// @return The kernel, or nullptr if there is none (designs aren't rebuilt).
    const kernel::IKernel* kernel() const { return kernel_; }
    /// Gives the kernel designs are rebuilt with, to make new shapes from theirs,
    /// such as where two parts overlap (see model::checkOverlaps()). Shapes it
    /// makes change no design.
    /// @return The kernel, or nullptr if there is none (designs aren't rebuilt).
    kernel::IKernel* kernel() { return kernel_; }

    /// Gives the solver Sketch steps are solved with, for live work on a sketch
    /// such as dragging part of it (sketch::ISketchSolver::drag()). Change the
    /// sketch itself only with a command (model::EditSketchCommand).
    /// @return The solver, or nullptr if there is none.
    sketch::ISketchSolver* solver() const { return solver_; }

private:
    struct Entry {
        std::unique_ptr<model::Document> doc;
        std::optional<model::RebuildReport> rebuild;
        DocId editing;                 // the part being edited in it, or nil
        model::ComponentId component;  // and the component picked to edit it
    };
    Entry* entry(const DocId& id);
    Entry* owner(const DocId& id);  // the entry for a design, or for a part edited in one
    const Entry* owner(const DocId& id) const;
    void checkEditing(Entry& e);  // stops editing a part that's gone
    void changed(Entry& e, ChangeKind kind, std::string label);
    void rebuild(Entry& e);

    EventBus& bus_;
    const model::CommandRegistry& commands_;
    kernel::IKernel* kernel_;
    sketch::ISketchSolver* solver_;
    std::vector<DocId> order_;
    std::map<DocId, Entry> docs_;
    DocId active_;
};

}  // namespace sb::plugin
