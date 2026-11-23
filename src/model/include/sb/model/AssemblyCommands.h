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

/// Puts a part in an assembly, with one or more components of it, in a row: as
/// an inside part, or linked to its file. A part the assembly already has (the
/// same design id) isn't added again: only its new components are, and it stays
/// linked or inside as it was. The first component put in an empty assembly is
/// grounded.
class InsertPartCommand final : public Command {
public:
    /// Prepares the change.
    /// @param part The part: a design of the kind Part. The assembly keeps it.
    /// @param count How many components of it, 1 to 100.
    /// @param at Where the first one goes.
    /// @param step How far each one is from the one before (mm).
    /// @param link Its file, for a linked part; nullopt for an inside part.
    InsertPartCommand(std::unique_ptr<Document> part, int count, kernel::Placement at = {},
                      kernel::Vec3 step = {}, std::optional<PartLink> link = std::nullopt);
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
    std::optional<PartLink> link_;  // the new link while it isn't applied; the old one while it is
    bool addedPart_ = false;
    std::vector<ComponentId> ids_;  // made on the first apply, the same on redo
};

/// Puts a part in an assembly with no components of it, as opening a file does
/// before its components (AddComponentCommand).
class AddPartCommand final : public Command {
public:
    /// Prepares the change.
    /// @param part The part: a design of the kind Part, not in the assembly yet.
    /// @param link Its file, for a linked part; nullopt for an inside part.
    explicit AddPartCommand(std::unique_ptr<Document> part, std::optional<PartLink> link = std::nullopt);
    ~AddPartCommand() override;

    std::string label() const override;
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

private:
    std::unique_ptr<Document> part_;
    DocId id_;
    std::string name_;
    std::optional<PartLink> link_;  // the new link while it isn't applied; the old one while it is
};

/// Finds one of an assembly's parts by name, for a command that names it.
/// @param doc The assembly.
/// @param name The part's name, like Wheel.
/// @param problem Set to why there's no such part (in plain words, with the
///        assembly's part names), when there isn't.
/// @return The part, or nullptr.
const Document* partNamed(const Document& doc, const std::string& name, Diagnostic& problem);

/// One change to one of an assembly's parts, for UpdatePartsCommand.
struct PartUpdate {
    DocId part{};                      ///< Which part, by its design id.
    std::unique_ptr<Document> design;  ///< Its new design (with the same id); null to keep the one it has.
    std::optional<PartLink> link;      ///< Its link from now on; nullopt makes it an inside part.
};

/// Changes where some of an assembly's parts come from, as one undo step:
/// updating linked parts from their files, linking a part to a file found
/// somewhere else, or making a linked part an inside part. The components stay
/// as they are. Undo puts back the parts and links they had.
///
/// @code
/// std::vector<PartUpdate> changes;
/// changes.push_back({wheelId, std::move(newWheel), PartLink{"/class/wheel.sb", fingerprint, now}});
/// car.execute(std::make_unique<UpdatePartsCommand>(std::move(changes), "Update Wheel"));
/// @endcode
class UpdatePartsCommand final : public Command {
public:
    /// Prepares the change.
    /// @param changes The parts to change. Each must be one of the assembly's parts,
    ///        and a new design must be a part with the same design id.
    /// @param label What undo says, like "Update Wheel".
    UpdatePartsCommand(std::vector<PartUpdate> changes, std::string label);
    ~UpdatePartsCommand() override;

    std::string label() const override { return label_; }
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

private:
    void swap(DocumentEditor& editor);  // apply and revert each swap the parts and links
    std::vector<PartUpdate> changes_;
    std::string label_;
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
/// component places it, and so do its joints.
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
    std::optional<PartLink> link_;                       // and that part's link, if it was linked
    std::vector<std::pair<std::size_t, Joint>> joints_;  // its joints, which went too, and where they were
};

/// Makes a change inside one of an assembly's inside parts, as one step of the
/// assembly: what Edit Part does. The assembly keeps the undo history for its
/// inside parts, so undoing the change works from the assembly. A linked part
/// is changed in its own file instead, so it's refused.
///
/// @code
/// // Car is an assembly with an inside part, Wheel: give the wheel a hub.
/// auto hub = std::make_unique<AddCylinderCommand>(4.0, 10.0);
/// car.execute(std::make_unique<EditPartCommand>(wheelId, std::move(hub)));
/// car.undo();  // takes the hub back off
/// @endcode
class EditPartCommand final : public Command {
public:
    /// Prepares the change.
    /// @param part The inside part's design id.
    /// @param change The change to make in it, made for the part (see
    ///        CommandRegistry::create() with the part as the design).
    EditPartCommand(DocId part, std::unique_ptr<Command> change);
    ~EditPartCommand() override;

    /// Says what the change in the part does, with the part's name once applied,
    /// like "Add cylinder in Wheel".
    /// @return The label.
    std::string label() const override;
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;
    /// Merges the next change into this one when both are in the same part and
    /// the change in it merges (dragging a body, say).
    /// @param next The command that ran next.
    /// @return True if it merged.
    bool mergeWith(const Command& next) override;

    /// Gives the part it changes.
    /// @return The part's design id.
    const DocId& part() const { return part_; }

    /// Gives the change it makes in the part.
    /// @return The change.
    const Command& change() const { return *change_; }

private:
    DocId part_;
    std::unique_ptr<Command> change_;
    std::optional<FeatureId> markerBefore_;  // the part's rollback marker, for revert()
    std::string partName_;                   // for label(), once applied
};

/// Checks a joint before it goes in an assembly: its components are there and
/// aren't the same one, its bodies are in their parts, its name is free, and its
/// value is between its lowest and highest.
/// @param assembly The assembly.
/// @param joint The joint, new or changed.
/// @return Why it can't go in, or nothing if it can.
std::optional<Diagnostic> checkJoint(const Document& assembly, const Joint& joint);

/// Adds a joint to an assembly: it places its second component against its
/// first (see Joint). Where components go is worked out when the assembly
/// rebuilds (RebuildReport::placementOf()).
///
/// @code
/// Joint hinge;
/// hinge.kind = JointKind::Revolute;
/// hinge.a = {box.id, boxBody, boxEdge};
/// hinge.b = {lid.id, lidBody, lidEdge};
/// car.execute(std::make_unique<AddJointCommand>(hinge));
/// @endcode
class AddJointCommand final : public Command {
public:
    /// Prepares the change.
    /// @param joint The joint. A nil id gets a new one; an empty name gets the
    ///        next free one, like "Joint1".
    explicit AddJointCommand(Joint joint);

    std::string label() const override { return "Add " + joint_.name; }
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

    /// Gives the joint as it went in, after apply().
    /// @return The joint, with its id and name.
    const Joint& joint() const { return joint_; }

private:
    Joint joint_;
};

/// Changes a joint: its kind, origins, settings, value or limits, or its name.
/// Changes that follow each other while dragging (`continuesPrevious`) are one
/// undo step.
class SetJointCommand final : public Command {
public:
    /// Prepares the change.
    /// @param joint The joint as it's to be: the same id as the one it changes.
    /// @param continuesPrevious True for the next part of the same drag.
    explicit SetJointCommand(Joint joint, bool continuesPrevious = false);

    std::string label() const override { return "Change " + joint_.name; }
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;
    /// Merges the next change to the same joint, when it continues this one.
    /// @param next The command that ran next.
    /// @return True if it merged.
    bool mergeWith(const Command& next) override;

private:
    Joint joint_;
    Joint old_;
    bool continues_;
};

/// Takes a joint out of an assembly. The component it placed goes back to its
/// own placement (Component::placement): where it was before it was joined.
class DeleteJointCommand final : public Command {
public:
    /// Prepares the change.
    /// @param id The joint.
    explicit DeleteJointCommand(JointId id);

    std::string label() const override;
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

private:
    JointId id_;
    std::optional<Joint> removed_;
    std::size_t index_ = 0;
};

/// Puts joints back to their rest values (Joint::rest): Reset Joints. One undo
/// step for all of them.
class ResetJointsCommand final : public Command {
public:
    /// Prepares the change.
    /// @param joints The joints to reset; empty for every joint of the assembly.
    explicit ResetJointsCommand(std::vector<JointId> joints = {});

    std::string label() const override { return "Reset joints"; }
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

private:
    std::vector<JointId> only_;
    std::vector<std::pair<JointId, double>> old_;  // each joint changed, and its value before
};

}  // namespace sb::model
