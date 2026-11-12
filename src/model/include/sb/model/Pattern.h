/// @file
/// The Pattern step: copies of what steps did (holes, joins, cuts), or of a
/// whole body, in a row or grid, or round a circle.
#pragma once

#include <string>
#include <vector>

#include "sb/model/Feature.h"

namespace sb::model {

/// Where a Pattern's copies go.
struct PatternLayout {
    /// The shape of the pattern.
    enum class Kind {
        Linear,    ///< In a row along a direction, or a grid along two.
        Circular,  ///< Round an axis.
    };
    Kind kind = Kind::Linear;  ///< Row or circle.

    // Linear
    kernel::Vec3 direction{1, 0, 0};   ///< Linear: which way the row goes (any length but 0).
    int count = 3;                     ///< How many in the row (or round the circle), the first one included.
    double spacing = 10;               ///< Linear: how far apart, along direction (mm).
    kernel::Vec3 direction2{0, 1, 0};  ///< Linear: the second direction, for a grid.
    int count2 = 1;                    ///< Linear: how many rows along direction2; 1 for just one row.
    double spacing2 = 10;              ///< Linear: how far apart the rows are (mm).

    // Circular
    kernel::Point3 center{};           ///< Circular: a point on the axis (mm).
    kernel::Vec3 axis{0, 0, 1};        ///< Circular: the axis's direction (any length but 0).
    double angle = 6.283185307179586;  ///< Circular: the angle they spread over (radians); a full turn spaces
                                       ///< them evenly all the way round.
};

/// Gives where each copy goes, the first one (which stays where it is) left
/// out: count x count2 - 1 for a row or grid, count - 1 round a circle.
/// @param layout The layout.
/// @return One motion per copy, in order (along the row first, then the rows).
std::vector<kernel::Motion> patternMotions(const PatternLayout& layout);

/// A Pattern step: copies, in a row or grid or round a circle, of what some
/// steps did to a body (a Hole's holes, or an Extrude's or Revolve's join or
/// cut: a ring of bolt holes, a row of pegs), or of a whole body.
///
/// The first copy is the original; the step adds the rest. A pattern of steps
/// changes the body those steps made (or the body that body became); a pattern
/// of a body makes one body of all the copies, joined where they touch.
///
/// @code
/// // Six copies of Hole1 round the z axis.
/// PatternLayout ring;
/// ring.kind = PatternLayout::Kind::Circular;
/// ring.count = 6;
/// auto bolts = std::make_unique<PatternFeature>("Pattern1", plateId, std::vector{holeId}, ring);
/// @endcode
class PatternFeature final : public Feature {
public:
    /// Makes a Pattern step with a new id.
    /// @param name The name shown in the feature tree.
    /// @param body The body it changes (a pattern of steps), or copies (a pattern of a body).
    /// @param steps The steps whose holes, joins or cuts it repeats; empty to copy the body itself.
    /// @param layout Where the copies go.
    PatternFeature(std::string name, FeatureId body, std::vector<FeatureId> steps, PatternLayout layout);

    /// Makes a Pattern step with a given id, for loading a saved design.
    /// @param id The id it was saved with.
    /// @param name The name shown in the feature tree.
    /// @param body The body it changes, or copies.
    /// @param steps The steps it repeats; empty to copy the body.
    /// @param layout Where the copies go.
    PatternFeature(FeatureId id, std::string name, FeatureId body, std::vector<FeatureId> steps,
                   PatternLayout layout);

    /// Gives the body it changes or copies.
    /// @return The step that made the body.
    const FeatureId& body() const { return body_; }

    /// Gives the steps it repeats.
    /// @return Their ids, in order; empty for a pattern of the body.
    const std::vector<FeatureId>& steps() const { return steps_; }

    /// Gives where the copies go.
    /// @return The layout.
    const PatternLayout& layout() const { return layout_; }

    /// Changes where the copies go. Use on a copy, or from a Command.
    /// @param layout The layout.
    void setLayout(PatternLayout layout) { layout_ = layout; }

    FeatureType type() const override { return FeatureType::Pattern; }
    std::unique_ptr<Feature> clone() const override { return std::make_unique<PatternFeature>(*this); }
    /// count (a whole number, 1 or more) and, for a row, spacing (mm), count2
    /// and spacing2 (mm); for a circle, angle (radians).
    /// @return The numbers.
    std::vector<StepNumber> numbers() const override;
    std::optional<double> number(std::string_view key) const override;
    bool setNumber(std::string_view key, double value) override;
    std::vector<FeatureId> dependencies() const override;
    std::vector<FeatureId> referenceInputs() const override;
    void remap(const std::map<FeatureId, FeatureId>& map, bool renameRefs) override;
    Built build(kernel::IKernel& kernel, const ResultLookup& earlier) const override;

private:
    FeatureId body_;
    std::vector<FeatureId> steps_;
    PatternLayout layout_;
};

}  // namespace sb::model
