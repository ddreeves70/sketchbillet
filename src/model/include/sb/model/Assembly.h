#pragma once

/// @file
/// Components: parts placed in an assembly (see docs/design/assemblies.md).

#include <optional>
#include <string>
#include <vector>

#include "sb/core/Id.h"
#include "sb/kernel/Placement.h"
#include "sb/model/Naming.h"

namespace sb::model {

/// Identifies a component of an assembly: one part placed there.
using ComponentId = InstanceId;

/// One part placed in an assembly. Four wheels are four components of one part,
/// Wheel, named Wheel 1 to Wheel 4. A component places the whole part: every
/// finished body in it.
struct Component {
    ComponentId id{};  ///< Its UUID, which never changes.
    std::string name;  ///< Like "Wheel 1": the part's name and a number. Students can rename it.
    DocId part{};      ///< Which part: one of the assembly's parts (Document::findPart()).
    /// Where it sits in the assembly: where the part's origin goes, and which
    /// way its x and z face.
    kernel::Placement placement{};
    /// True if it's fixed where it is. The first component put in an assembly is.
    bool grounded = false;
};

/// Where one of an assembly's parts comes from when it's linked to its own `.sb`
/// file, like wheel.sb, so changing the file changes the assembly. The assembly
/// still keeps a copy of the part as last used, so it opens even when the file
/// can't be found. A part without a link is an inside part: made in the
/// assembly, and kept only there.
struct PartLink {
    /// The part's file, as UTF-8 with forward slashes. While the assembly is
    /// open it's a full path; in the assembly's file it's written relative to
    /// the assembly, like "parts/wheel.sb".
    std::string path;
    /// The fingerprint of the file as last used: SHA-256 of its bytes, as 64
    /// lowercase hex digits. A file with another fingerprint has changed.
    std::string sha256;
    /// When the assembly last took its copy from the file, in UTC, like
    /// "2026-11-13T23:40:00Z".
    std::string updated;
    /// False when the file couldn't be found as the assembly opened, so it uses
    /// its saved copy. Not saved in the file.
    bool found = true;
};

/// What a joint lets its two components still do (see docs/design/assemblies.md, "Joints").
enum class JointKind {
    Rigid,     ///< Nothing: they're fixed together.
    Revolute,  ///< Turn about the joint's z, like a hinge. Its value is an angle.
    Slider,    ///< Slide along the joint's z. Its value is a distance.
};

/// Where on a straight edge a joint origin is.
enum class OriginAt {
    Middle,  ///< The edge's middle; for anything else, its center.
    Start,   ///< A straight edge's first end.
    End,     ///< A straight edge's other end.
};

/// One side of a joint: a point and a direction (the joint's z) on a
/// component, picked on its part's geometry. The point and direction come from
/// what's picked:
///
/// | Picked | Point | Direction |
/// | --- | --- | --- |
/// | A round edge | The circle's center | Along the circle's axis |
/// | A flat face | The face's center | Straight out of the face |
/// | A round face | The middle of its axis | Along the axis |
/// | A straight edge | Its middle, or an end (`at`) | Along the edge |
/// | A corner | The corner | The part's z |
/// | Nothing | The part's origin | The part's z |
struct JointOrigin {
    ComponentId component{};  ///< The component.
    /// The body of the component's part the element is on; nil for the part's
    /// own origin.
    FeatureId body{};
    /// The face, edge or vertex picked on that body, found again by name after
    /// the part changes (like a fillet's edges); none for the part's origin.
    std::optional<GeometryRef> element;
    OriginAt at = OriginAt::Middle;  ///< Where on a straight edge.
};

/// A joint: it places its second component (b) against its first (a), so their
/// origins meet with their directions pointing at each other, like a peg dropped
/// into a hole, and says how b can still move. If a isn't placed and b is (b
/// grounded, say), it places a against b instead.
struct Joint {
    JointId id{};                       ///< Its UUID, which never changes.
    std::string name;                   ///< Like "Joint1".
    JointKind kind = JointKind::Rigid;  ///< How it still moves.
    JointOrigin a;                      ///< The side that stays.
    JointOrigin b;                      ///< The side that moves to meet it.
    bool flip = false;                  ///< True to turn b the other way round: the directions the same way.
    double angle = 0;                   ///< How far b is turned about the joint's z (radians).
    double offset = 0;                  ///< The gap left along the joint's z (mm).
    /// A revolute joint's turn (radians) or a slider's distance (mm), on top of
    /// `angle` and `offset`; 0 for a rigid joint.
    double value = 0;
    double rest = 0;                ///< Where Reset Joints puts the value back to, in its units.
    std::optional<double> lowest;   ///< The value's lowest, if it has one.
    std::optional<double> highest;  ///< The value's highest, if it has one.
    bool off = false;               ///< True to turn it off: it places nothing.
};

/// Gives a joint kind's name, as commands and files spell it.
/// @param kind The kind.
/// @return "rigid", "revolute" or "slider".
const char* jointKindName(JointKind kind);

/// One step of an exploded view: some components slid along a direction.
struct ExplodeStep {
    ExplodeStepId id{};                     ///< Stays the same for as long as it's there.
    std::vector<ComponentId> components{};  ///< The components it slides.
    kernel::Vec3 direction{0, 0, 1};        ///< Which way (it needn't be a unit vector).
    double distance = 0;                    ///< How far (mm).
};

/// An exploded view: the components pulled apart, step by step, to show how
/// they go together, for pictures and instructions. It never changes the
/// joints, or where the components are: it's only a way to look at them.
struct ExplodedView {
    ExplodedViewId id{};               ///< Stays the same for as long as it's there.
    std::string name;                  ///< Its name, like "Exploded 1".
    std::vector<ExplodeStep> steps{};  ///< Its steps, in order; a component in two moves by both.
};

/// Works out how far an exploded view moves a component, step by step: where
/// it is after each step that slides it, from where it's placed. A line through
/// them is the thin line back to where it came from.
/// @param view The exploded view.
/// @param component The component.
/// @return How far it has moved (mm, along x, y and z): {0, 0, 0} first, then
///         after each step that slides it. Just {0, 0, 0} if none does.
std::vector<kernel::Vec3> explodePath(const ExplodedView& view, const ComponentId& component);

/// A press fit: two components meant to overlap a little, like a pin pushed
/// into a hole a bit smaller than it, so it holds tight. Check for overlaps
/// leaves them out (see checkOverlaps()).
struct PressFit {
    PressFitId id{};  ///< Stays the same for as long as it's there.
    ComponentId a{};  ///< One of the two.
    ComponentId b{};  ///< The other.
};

}  // namespace sb::model
