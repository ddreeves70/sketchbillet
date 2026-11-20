#pragma once

/// @file
/// Commands that change an assembly: making one, putting parts in it, and its
/// components (see docs/design/assemblies.md). Each is one undo step.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sb/model/Command.h"
#include "sb/model/Document.h"

namespace sb::model {

/// Makes a new, empty design an assembly: a design made of parts placed
/// together, instead of steps. A design with steps (or parameters) can't become
/// one.
///
/// @code
/// Document car(DocKind::Part, "Car");
/// car.execute(std::make_unique<MakeAssemblyCommand>());
/// car.execute(std::make_unique<InsertPartCommand>(std::move(wheel), 4, Placement{}, Vec3{30, 0, 0}));
/// @endcode
class MakeAssemblyCommand final : public Command {
public:
    std::string label() const override { return "Make assembly"; }
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;
};

/// Puts a part in an assembly as an inside part, with one or more components of
/// it, in a row. A part the assembly already has (the same design id) isn't
/// added again: only its new components are. The first component put in an
/// empty assembly is grounded.
class InsertPartCommand final : public Command {
public:
    /// Prepares the change.
    /// @param part The part: a design of the kind Part. The assembly keeps it.
    /// @param count How many components of it, 1 to 100.
    /// @param at Where the first one goes.
    /// @param step How far each one is from the one before (mm).
    InsertPartCommand(std::unique_ptr<Document> part, int count, kernel::Placement at = {},
                      kernel::Vec3 step = {});
    ~InsertPartCommand() override;

    std::string label() const override;
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

    /// Gives the components it added, after apply().
    /// @return Their ids, in order.
    const std::vector<ComponentId>& added() const { return ids_; }

private:
    std::unique_ptr<Document> part_;  // kept here while it isn't in the assembly
    DocId partId_;
    std::string partName_;
    int count_;
    kernel::Placement at_;
    kernel::Vec3 step_;
    bool addedPart_ = false;
    std::vector<ComponentId> ids_;  // made on the first apply, the same on redo
};

/// Puts a part in an assembly with no components of it, as opening a file does
/// before its components (AddComponentCommand).
class AddPartCommand final : public Command {
public:
    /// Prepares the change.
    /// @param part The part: a design of the kind Part, not in the assembly yet.
    explicit AddPartCommand(std::unique_ptr<Document> part);
    ~AddPartCommand() override;

    std::string label() const override;
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

private:
    std::unique_ptr<Document> part_;
    DocId id_;
    std::string name_;
};

/// Adds one component exactly as given (its id, name, part, placement and
/// whether it's grounded), as opening a file does.
class AddComponentCommand final : public Command {
public:
    /// Prepares the change.
    /// @param component The component: its part must be in the assembly, and its
    ///        id and name not used by another component.
    explicit AddComponentCommand(Component component);

    std::string label() const override { return "Add " + component_.name; }
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

private:
    Component component_;
};

/// Adds more components of a part the assembly already has, in a row.
class AddComponentsCommand final : public Command {
public:
    /// Prepares the change.
    /// @param part The part's design id (Document::findPart()).
    /// @param count How many, 1 to 100.
    /// @param at Where the first one goes.
    /// @param step How far each one is from the one before (mm).
    AddComponentsCommand(DocId part, int count, kernel::Placement at = {}, kernel::Vec3 step = {});

    std::string label() const override;
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

    /// Gives the components it added, after apply().
    /// @return Their ids, in order.
    const std::vector<ComponentId>& added() const { return ids_; }

private:
    DocId part_;
    std::string partName_;
    int count_;
    kernel::Placement at_;
    kernel::Vec3 step_;
    std::vector<ComponentId> ids_;
};

/// Fixes a component where it is (grounded), or lets it go.
class GroundComponentCommand final : public Command {
public:
    /// Prepares the change.
    /// @param id The component.
    /// @param grounded True to fix it where it is, false to let it go.
    GroundComponentCommand(ComponentId id, bool grounded);

    std::string label() const override;
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

private:
    ComponentId id_;
    bool grounded_;
    bool old_ = false;
    std::string name_;
};

/// Puts a component somewhere else, facing another way.
class PlaceComponentCommand final : public Command {
public:
    /// Prepares the change.
    /// @param id The component.
    /// @param placement Where it goes (see kernel::tidyPlacement(): its directions
    ///        are made unit and square).
    PlaceComponentCommand(ComponentId id, kernel::Placement placement);

    std::string label() const override;
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

private:
    ComponentId id_;
    kernel::Placement placement_;
    kernel::Placement old_{};
    std::string name_;
};

/// Renames a component.
class RenameComponentCommand final : public Command {
public:
    /// Prepares the change.
    /// @param id The component.
    /// @param name Its new name: not empty, and no other component's.
    RenameComponentCommand(ComponentId id, std::string name);

    std::string label() const override;
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

private:
    ComponentId id_;
    std::string name_;
    std::string old_;
};

/// Takes a component out of an assembly. Its part goes too when no other
/// component places it.
class DeleteComponentCommand final : public Command {
public:
    /// Prepares the change.
    /// @param id The component.
    explicit DeleteComponentCommand(ComponentId id);
    ~DeleteComponentCommand() override;

    std::string label() const override;
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

private:
    ComponentId id_;
    std::optional<Component> removed_;
    std::size_t index_ = 0;
    std::unique_ptr<Document> part_;  // its part, if it went too
    std::size_t partIndex_ = 0;
};

}  // namespace sb::model
