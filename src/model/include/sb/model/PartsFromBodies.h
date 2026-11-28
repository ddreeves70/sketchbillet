/// @file
/// Make Parts from Bodies: turns a part's bodies into the parts of a new
/// assembly, each placed where it was, ready to join.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sb/core/Diagnostic.h"
#include "sb/core/Id.h"
#include "sb/kernel/Placement.h"
#include "sb/model/Command.h"
#include "sb/model/Document.h"

namespace sb::model {

/// One part Make Parts from Bodies makes, and where its components go.
struct BodyPart {
    /// The new part: a copy of every step that makes its body (as Copy and Paste
    /// copies them), with the design's parameters, own materials and unit. A
    /// body the steps only look at (one a sketch is drawn on) comes too, kept
    /// for reference (Feature::reference()), so it has no mass and isn't exported.
    std::unique_ptr<Document> part;
    /// Where each of its components goes, in the assembly (mm): the body where it
    /// was, then each of its linked copies where the copy is.
    std::vector<kernel::Placement> at;
    /// The steps of the design it was made from: the body, then its linked copies.
    std::vector<FeatureId> bodies;
};

/// What partsFromBodies() gives.
struct PartsFromBodies {
    std::vector<BodyPart> parts;  ///< The parts, in the order of their bodies in the design.
    Diagnostics problems;         ///< Why no parts could be made, in plain words; empty if they were.
};

/// Lists the steps that make a design's finished bodies, worked out from its
/// steps without building them: each step that's on and before the rollback
/// marker, isn't a sketch or a plane, isn't kept for reference, and isn't used up
/// by a later step. A step that doesn't build is listed too (its part shows why).
/// @param design A part.
/// @return The steps, in design order.
std::vector<FeatureId> finishedBodySteps(const Document& design);

/// Makes the parts for Make Parts from Bodies: one part for each body, named
/// after the body's step, holding a copy of every step that makes it. A body and
/// its linked copies (Move steps with MoveFeature::copy()) make one part, with a
/// component for each, placed where each copy is: four wheels made with Linked
/// Copy give Wheel x4. The design isn't changed.
/// @code
/// auto made = model::partsFromBodies(robot);
/// Document assembly(DocKind::Part, "Robot assembly");
/// assembly.execute(std::make_unique<model::MakePartsFromBodiesCommand>(std::move(made.parts)));
/// @endcode
/// @param design The part whose bodies to use.
/// @param bodies The bodies to make parts of (their steps). Leave it empty for
///        every finished body (finishedBodySteps()).
/// @return The parts, or why there are none.
PartsFromBodies partsFromBodies(const Document& design, const std::vector<FeatureId>& bodies = {});

/// Makes a new, empty design an assembly of the parts partsFromBodies() made, as
/// one undo step: each part inside the assembly, with a component where each of
/// its bodies was. The first component is grounded. Then join them with joints.
class MakePartsFromBodiesCommand final : public Command {
public:
    /// Prepares the change.
    /// @param parts The parts, from partsFromBodies(). The assembly keeps them.
    explicit MakePartsFromBodiesCommand(std::vector<BodyPart> parts);
    ~MakePartsFromBodiesCommand() override;

    std::string label() const override { return "Make parts from bodies"; }
    Diagnostics apply(DocumentEditor& editor) override;
    void revert(DocumentEditor& editor) override;

private:
    std::vector<std::unique_ptr<Command>>
        steps_;                // make it an assembly, then put in each part and its copies
    std::size_t applied_ = 0;  // how many of steps_ apply() did
};

}  // namespace sb::model
