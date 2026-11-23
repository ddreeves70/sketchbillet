#pragma once

/// @file
/// The Command interface and the editor that commands use to change a Document.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "sb/core/Diagnostic.h"
#include "sb/core/Id.h"
#include "sb/core/Units.h"
#include "sb/model/Assembly.h"
#include "sb/model/Feature.h"
#include "sb/model/Folder.h"
#include "sb/model/Material.h"
#include "sb/model/Parameters.h"

namespace sb::model {

class Document;
class Command;
enum class DocKind;

/// Write access to a Document, given only to Commands.
///
/// Commands receive a DocumentEditor from Document::execute(), Document::undo()
/// and Document::redo(); nothing else can create one. This enforces the rule
/// that every change goes through the command stack, so undo always works.
class DocumentEditor {
public:
    /// Gives read access to the document being changed.
    /// @return The document.
    const Document& document() const { return doc_; }

    /// Finds a feature to change in place.
    /// @param id The feature to find.
    /// @return The feature, or nullptr if it is not in the document.
    Feature* find(const FeatureId& id);

    /// Inserts a feature.
    /// @param index Position in the feature list; past the end means append.
    /// @param feature The feature to insert. The document takes ownership.
    void insert(std::size_t index, std::unique_ptr<Feature> feature);

    /// Removes a feature.
    /// @param id The feature to remove.
    /// @return The removed feature (the caller now owns it), or nullptr if not found.
    std::unique_ptr<Feature> remove(const FeatureId& id);

    /// Puts a new feature in at the rollback marker, and moves the marker past it,
    /// so it's built and the steps after the marker still aren't. Not rolled back,
    /// it goes at the end.
    /// @param feature The feature to add.
    /// @return Where it went.
    std::size_t insertAtMarker(std::unique_ptr<Feature> feature);

    /// Moves the rollback marker.
    /// @param index How many steps come before it; featureCount() or more puts it at the end.
    void setMarker(std::size_t index);

    /// Swaps a feature for another with the same id, at the same position.
    /// @param f The new version. Its id must already be in the document.
    /// @return The old version, or nullptr if no feature has that id.
    std::unique_ptr<Feature> replace(std::unique_ptr<Feature> f);

    /// Gives the design's own materials to change in place.
    /// @return The list (see Document::customMaterials()).
    std::vector<Material>& materials();

    /// Gives the design's folders to change in place, as commands left them
    /// (with steps that were deleted still listed; see Document::folders()).
    /// @return The list.
    std::vector<Folder>& folders();

    /// Gives the design's parameters to change in place.
    /// @return The table (see Document::parameters()).
    ParameterTable& parameters();

    /// Changes the unit the design shows its lengths in (see Document::lengthUnit()).
    /// @param unit The unit.
    void setLengthUnit(LengthUnit unit);

    /// Makes the design a part or an assembly (MakeAssemblyCommand).
    /// @param kind The kind.
    void setKind(DocKind kind);

    /// Gives an assembly's components to change in place.
    /// @return The list (see Document::components()).
    std::vector<Component>& components();

    /// Gives an assembly's joints to change in place.
    /// @return The list (see Document::joints()).
    std::vector<Joint>& joints();

    /// Adds an inside part to an assembly.
    /// @param part The part. The assembly takes ownership.
    /// @param index Where it goes in the list of parts; past the end means last.
    void addPart(std::unique_ptr<Document> part, std::size_t index = static_cast<std::size_t>(-1));

    /// Takes an inside part out of an assembly.
    /// @param id The part's design id.
    /// @return The part (the caller owns it now) and where it was, or nullptr if it isn't there.
    std::pair<std::unique_ptr<Document>, std::size_t> removePart(const DocId& id);

    /// Puts a new design in place of one of an assembly's parts with the same
    /// design id, keeping its place in the list.
    /// @param part The new design. The assembly takes ownership.
    /// @return The design it replaced (the caller owns it now), or nullptr (and
    ///         @p part is dropped) if no part has its id.
    std::unique_ptr<Document> replacePart(std::unique_ptr<Document> part);

    /// Links one of an assembly's parts to its own file, or makes it an inside part.
    /// @param part The part's design id.
    /// @param link The link, or nullopt for an inside part.
    /// @return The link it had before, or nullopt if it had none.
    std::optional<PartLink> setPartLink(const DocId& part, std::optional<PartLink> link);

    /// Applies a command to one of an assembly's parts, as a change to the
    /// assembly: what EditPartCommand does. The part's rollback marker is kept
    /// right, as Document::execute() keeps the design's.
    /// @param part The part's design id.
    /// @param command The change to make in the part.
    /// @param markerBefore Set to where the part's rollback marker was, for revertOnPart().
    /// @return The command's problems (then the part is unchanged), or one if
    ///         the assembly has no such part.
    Diagnostics applyToPart(const DocId& part, Command& command, std::optional<FeatureId>& markerBefore);

    /// Takes back a command applyToPart() made.
    /// @param part The part's design id.
    /// @param command The change applyToPart() made.
    /// @param markerBefore Where the part's rollback marker was before it.
    void revertOnPart(const DocId& part, Command& command, const std::optional<FeatureId>& markerBefore);

private:
    friend class Document;
    explicit DocumentEditor(Document& doc) : doc_(doc) {}
    Document& doc_;
};

/// One undoable change to a Document.
///
/// Contract:
///  - apply() either makes the whole change and returns no errors, or returns
///    errors and leaves the document exactly as it was.
///  - revert() undoes a successful apply(); redo calls apply() again.
///  - Ids stay the same across undo and redo.
class Command {
public:
    /// Destroys the command.
    virtual ~Command() = default;

    /// Gives the text shown in Edit > Undo.
    /// @return A short label, for example "Add Box1".
    virtual std::string label() const = 0;

    /// Makes the change.
    /// @param editor Write access to the document.
    /// @return No diagnostics on success. On failure, the errors, and the document is unchanged.
    virtual Diagnostics apply(DocumentEditor& editor) = 0;

    /// Undoes a successful apply().
    /// @param editor Write access to the document.
    virtual void revert(DocumentEditor& editor) = 0;

    /// Lets a burst of small changes (dragging a value) become one undo step.
    ///
    /// Called on the newest command in history after @p next has been applied.
    /// @param next The command that was just applied.
    /// @return True to absorb @p next into this command; false to keep them separate (the default).
    virtual bool mergeWith(const Command& next) {
        (void)next;
        return false;
    }
};

}  // namespace sb::model
