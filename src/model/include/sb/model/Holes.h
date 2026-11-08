/// @file
/// The Hole step: round holes drilled into a body, simple, counterbored or
/// countersunk, at a sketch's points or a distance from two edges; and standard
/// metric screw sizes for them.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sb/model/Feature.h"
#include "sb/model/Naming.h"

namespace sb::model {

/// What shape a hole is at the top.
enum class HoleKind {
    Simple,       ///< The same diameter all the way down.
    Counterbore,  ///< A wider, flat-bottomed pocket at the top, for a screw head to sit below the face.
    Countersink,  ///< A cone at the top, for a flat (countersunk) screw head to sit flush.
};

/// What a hole's screw size gives: a hole the screw slides through, or a smaller
/// one it cuts its own thread into (or that's tapped).
enum class HoleFit {
    Clearance,  ///< The screw slides through.
    Tap,        ///< The screw's thread grips: the tap-drill size.
};

/// Hole sizes for one metric screw, in mm.
struct ScrewSize {
    std::string name;                ///< The screw, like "M3".
    double clearance = 0;            ///< A hole it slides through (normal fit).
    double tap = 0;                  ///< A hole to tap, or for the screw to cut its own thread in.
    double counterboreDiameter = 0;  ///< A counterbore for a socket head screw: its head + 1 mm.
    double counterboreDepth = 0;     ///< That counterbore's depth: the head's height + 0.5 mm.
    /// A countersink for a flat head (ISO 10642, 90 degrees) screw: the head's
    /// widest; 0 when there's no such screw this small.
    double countersinkDiameter = 0;
};

/// The screw sizes SketchBillet knows, smallest first: M2, M2.5, M3, M4, M5, M6
/// and M8.
/// @return The sizes.
const std::vector<ScrewSize>& screwSizes();

/// Looks up a screw size by name.
/// @param name Like "M3" (or "m3").
/// @return Its sizes, or nullptr if SketchBillet doesn't know it.
const ScrewSize* findScrewSize(std::string_view name);

/// Where a Hole step drills.
struct HolePlacement {
    /// How the holes are placed.
    enum class Kind {
        /// At every point of a sketch (not helper points), into the face the
        /// sketch is on (or out of the sketch's back, for a sketch on a base plane).
        SketchPoints,
        /// One hole on the flat face two straight edges border, a distance in
        /// from each edge.
        FromEdges,
    };
    Kind kind = Kind::SketchPoints;  ///< How.
    FeatureId sketch{};              ///< For SketchPoints: the Sketch step.
    GeometryRef edgeA{};             ///< For FromEdges: the first edge.
    GeometryRef edgeB{};             ///< For FromEdges: the second edge, not parallel to the first.
    double distanceA = 0;            ///< For FromEdges: how far in from edgeA (mm).
    double distanceB = 0;            ///< For FromEdges: how far in from edgeB (mm).
};

/// A Hole step: round holes drilled into a body, all the same size and kind.
/// Each is simple, counterbored or countersunk, and goes all the way through or
/// to a depth (with a flat bottom). They're placed at a sketch's points, or one
/// hole a distance in from two edges.
///
/// @code
/// // An M3 clearance hole, 10 mm in from two edges, all the way through.
/// HolePlacement at{HolePlacement::Kind::FromEdges, {}, edgeA, edgeB, 10, 10};
/// auto hole = std::make_unique<HoleFeature>("Hole1", blockId, at, 3.4);
/// @endcode
class HoleFeature final : public Feature {
public:
    /// Makes a Hole step with a new id: simple, all the way through.
    /// @param name The name shown in the feature tree.
    /// @param body The step whose body it drills.
    /// @param placement Where.
    /// @param diameter The hole's diameter (mm).
    HoleFeature(std::string name, FeatureId body, HolePlacement placement, double diameter);

    /// Makes a Hole step with a given id, for loading a saved design.
    /// @param id The id it was saved with.
    /// @param name The name shown in the feature tree.
    /// @param body The step whose body it drills.
    /// @param placement Where.
    /// @param diameter The hole's diameter (mm).
    HoleFeature(FeatureId id, std::string name, FeatureId body, HolePlacement placement, double diameter);

    /// Gives the body it drills.
    /// @return The step that made the body.
    const FeatureId& body() const { return body_; }

    /// Gives where it drills.
    /// @return The placement.
    const HolePlacement& placement() const { return placement_; }

    /// Changes where it drills. Use on a copy, or from a Command.
    /// @param p The placement.
    void setPlacement(HolePlacement p) { placement_ = std::move(p); }

    /// Gives the shape at the top.
    /// @return Simple, Counterbore or Countersink.
    HoleKind kind() const { return kind_; }

    /// Changes the shape at the top. Use on a copy, or from a Command.
    /// @param k The kind.
    void setKind(HoleKind k) { kind_ = k; }

    /// Gives the hole's diameter.
    /// @return The diameter (mm).
    double diameter() const { return diameter_; }

    /// Changes the hole's diameter; it's then a size of its own, not a screw's.
    /// @param d The diameter (mm).
    void setDiameter(double d) {
        diameter_ = d;
        size_.clear();
    }

    /// Gives whether it goes all the way through the body.
    /// @return True for all the way through; false for depth().
    bool through() const { return through_; }

    /// Gives how deep it goes when it isn't all the way through.
    /// @return The depth from the face to the flat bottom (mm).
    double depth() const { return depth_; }

    /// Makes it go all the way through, or to a depth. Use on a copy, or from a Command.
    /// @param through True for all the way through.
    /// @param depth The depth when it isn't (mm); kept either way.
    void setDepth(bool through, double depth) {
        through_ = through;
        depth_ = depth;
    }

    /// Gives a counterbore's diameter.
    /// @return The diameter (mm), used when kind() is Counterbore.
    double counterboreDiameter() const { return cbDiameter_; }

    /// Gives a counterbore's depth.
    /// @return The depth from the face to its flat floor (mm).
    double counterboreDepth() const { return cbDepth_; }

    /// Changes the counterbore. Use on a copy, or from a Command.
    /// @param diameter Its diameter (mm), more than the hole's.
    /// @param depth Its depth (mm).
    void setCounterbore(double diameter, double depth) {
        cbDiameter_ = diameter;
        cbDepth_ = depth;
        size_.clear();
    }

    /// Gives a countersink's diameter at the face.
    /// @return The diameter (mm), used when kind() is Countersink.
    double countersinkDiameter() const { return csDiameter_; }

    /// Gives a countersink's angle: how wide its cone is, across.
    /// @return The angle (radians); 90 degrees for metric flat head screws.
    double countersinkAngle() const { return csAngle_; }

    /// Changes the countersink. Use on a copy, or from a Command.
    /// @param diameter Its diameter at the face (mm), more than the hole's.
    /// @param angle Its angle across (radians), more than 0 and less than 180 degrees.
    void setCountersink(double diameter, double angle) {
        csDiameter_ = diameter;
        csAngle_ = angle;
        size_.clear();
    }

    /// Gives the screw size its diameters came from.
    /// @return Like "M3", or empty for sizes of its own.
    const std::string& size() const { return size_; }

    /// Gives which of the screw's holes it is.
    /// @return Clearance or Tap.
    HoleFit fit() const { return fit_; }

    /// Sizes it for a screw: the hole (clearance or tap), counterbore and
    /// countersink. Use on a copy, or from a Command.
    /// @param size Like "M3"; must be in screwSizes().
    /// @param fit Clearance or tap.
    /// @return False, changing nothing, if the size isn't known.
    bool setSize(std::string_view size, HoleFit fit);

    /// Says which screw size its numbers came from, without changing them (for
    /// loading a saved design).
    /// @param size Like "M3", or empty for sizes of its own.
    /// @param fit Clearance or tap.
    void setSizeName(std::string size, HoleFit fit) {
        size_ = std::move(size);
        fit_ = fit;
    }

    /// Gives whether it drills the other way: out of the sketch's front instead of
    /// into its back.
    /// @return True if flipped.
    bool flipped() const { return flip_; }

    /// Drills the other way. Use on a copy, or from a Command.
    /// @param flip True to flip.
    void setFlipped(bool flip) { flip_ = flip; }

    FeatureType type() const override { return FeatureType::Hole; }
    std::unique_ptr<Feature> clone() const override { return std::make_unique<HoleFeature>(*this); }
    /// diameter, depth (mm; while not through), counterbore_diameter and
    /// counterbore_depth (for a counterbore), countersink_diameter and
    /// countersink_angle (radians, for a countersink), and distance_a and
    /// distance_b (from two edges).
    /// @return The numbers.
    std::vector<StepNumber> numbers() const override;
    std::optional<double> number(std::string_view key) const override;
    bool setNumber(std::string_view key, double value) override;
    std::vector<FeatureId> dependencies() const override;
    std::vector<FeatureId> sketchInputs() const override;
    void remapInputs(const std::map<FeatureId, FeatureId>& map) override;
    Built build(kernel::IKernel& kernel, const ResultLookup& earlier) const override;

private:
    FeatureId body_;
    HolePlacement placement_;
    HoleKind kind_ = HoleKind::Simple;
    double diameter_;
    bool through_ = true;
    double depth_ = 10;
    double cbDiameter_ = 0;
    double cbDepth_ = 0;
    double csDiameter_ = 0;
    double csAngle_;
    std::string size_;
    HoleFit fit_ = HoleFit::Clearance;
    bool flip_ = false;
};

}  // namespace sb::model
