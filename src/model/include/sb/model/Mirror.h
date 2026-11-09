/// @file
/// The Mirror step: a mirror image of what steps did (holes, joins, cuts), or of
/// a whole body, across a plane.
#pragma once

#include <string>
#include <vector>

#include "sb/model/Feature.h"
#include "sb/model/Naming.h"

namespace sb::model {

/// The plane a Mirror reflects across.
struct MirrorPlane {
    /// Which kind of plane.
    enum class Kind {
        YZ,    ///< The YZ plane (facing x), moved offset mm along x: left becomes right.
        XZ,    ///< The XZ plane (facing y), moved offset mm along y: front becomes back.
        XY,    ///< The XY plane (facing z), moved offset mm along z: top becomes bottom.
        Face,  ///< A flat face of the body, like the cut side of half a part.
    };
    Kind kind = Kind::YZ;  ///< Which plane.
    double offset = 0;     ///< For YZ, XZ and XY: how far the plane is from 0 along its normal (mm).
    GeometryRef face{};    ///< For Face: the face, on the Mirror's body.
};

/// A Mirror step: a mirror image, across a plane, of what some steps did to a
/// body (a Hole's holes, an Extrude's or Revolve's join or cut), or of a whole
/// body. Model half a part, mirror it, and the halves stay the same as you
/// change one.
///
/// A mirror of steps changes the body those steps made (or the body that body
/// became). A mirror of a body joins the body and its mirror image into one,
/// joined where they touch.
///
/// @code
/// // Half a bracket, mirrored across its cut face (x = 0) to make the whole.
/// MirrorPlane across;  // the YZ plane
/// auto whole = std::make_unique<MirrorFeature>("Mirror1", halfId, std::vector<FeatureId>{}, across);
/// @endcode
class MirrorFeature final : public Feature {
public:
    /// Makes a Mirror step with a new id.
    /// @param name The name shown in the feature tree.
    /// @param body The body it changes (a mirror of steps), or mirrors (a mirror of a body).
    /// @param steps The steps whose holes, joins or cuts it mirrors; empty to mirror the body itself.
    /// @param plane The plane it reflects across.
    MirrorFeature(std::string name, FeatureId body, std::vector<FeatureId> steps, MirrorPlane plane);

    /// Makes a Mirror step with a given id, for loading a saved design.
    /// @param id The id it was saved with.
    /// @param name The name shown in the feature tree.
    /// @param body The body it changes, or mirrors.
    /// @param steps The steps it mirrors; empty to mirror the body.
    /// @param plane The plane it reflects across.
    MirrorFeature(FeatureId id, std::string name, FeatureId body, std::vector<FeatureId> steps,
                  MirrorPlane plane);

    /// Gives the body it changes or mirrors.
    /// @return The step that made the body.
    const FeatureId& body() const { return body_; }

    /// Gives the steps it mirrors.
    /// @return Their ids, in order; empty for a mirror of the body.
    const std::vector<FeatureId>& steps() const { return steps_; }

    /// Gives the plane it reflects across.
    /// @return The plane.
    const MirrorPlane& plane() const { return plane_; }

    /// Changes the plane it reflects across. Use on a copy, or from a Command.
    /// @param plane The plane.
    void setPlane(MirrorPlane plane) { plane_ = std::move(plane); }

    FeatureType type() const override { return FeatureType::Mirror; }
    std::unique_ptr<Feature> clone() const override { return std::make_unique<MirrorFeature>(*this); }
    /// offset (mm), for a YZ, XZ or XY plane.
    /// @return The numbers.
    std::vector<StepNumber> numbers() const override;
    std::optional<double> number(std::string_view key) const override;
    bool setNumber(std::string_view key, double value) override;
    std::vector<FeatureId> dependencies() const override;
    std::vector<FeatureId> referenceInputs() const override;
    void remapInputs(const std::map<FeatureId, FeatureId>& map) override;
    Built build(kernel::IKernel& kernel, const ResultLookup& earlier) const override;

private:
    FeatureId body_;
    std::vector<FeatureId> steps_;
    MirrorPlane plane_;
};

}  // namespace sb::model
