/// @file
/// The Shell step: a body hollowed out to walls of a thickness, with some faces
/// left open, like a box with no lid.
#pragma once

#include <string>
#include <vector>

#include "sb/model/Feature.h"
#include "sb/model/Naming.h"

namespace sb::model {

/// A Shell step: a body hollowed out, leaving walls of a thickness, with the
/// picked faces open. The outside stays the same size. Use it for enclosures,
/// boxes, cups and trays: model the solid outside, then shell it.
///
/// Its faces are named like this: the outside faces keep their names (an open
/// face's rim keeps that face's name), and each inside wall is
/// `<step>/inside:<name>`, after the outside face it's the inside of.
///
/// @code
/// // A 2 mm thick tray: Box1 with its top open.
/// auto tray = std::make_unique<ShellFeature>("Shell1", boxId, std::vector<GeometryRef>{top}, 2.0);
/// @endcode
class ShellFeature final : public Feature {
public:
    /// Makes a Shell step with a new id.
    /// @param name The name shown in the feature tree.
    /// @param body The step whose body it hollows.
    /// @param faces The faces to leave open, as references on that body: at least one.
    /// @param thickness How thick the walls are (mm).
    ShellFeature(std::string name, FeatureId body, std::vector<GeometryRef> faces, double thickness);

    /// Makes a Shell step with a given id, for loading a saved design.
    /// @param id The id it was saved with.
    /// @param name The name shown in the feature tree.
    /// @param body The step whose body it hollows.
    /// @param faces The faces to leave open.
    /// @param thickness How thick the walls are (mm).
    ShellFeature(FeatureId id, std::string name, FeatureId body, std::vector<GeometryRef> faces,
                 double thickness);

    /// Gives the body it hollows.
    /// @return The step that made the body.
    const FeatureId& body() const { return body_; }

    /// Gives the faces it leaves open.
    /// @return Their references, in the order they were picked.
    const std::vector<GeometryRef>& faces() const { return faces_; }

    /// Changes the faces it leaves open. Use on a copy, or from a Command.
    /// @param faces The faces, as references on the body.
    void setFaces(std::vector<GeometryRef> faces) { faces_ = std::move(faces); }

    /// Gives how thick the walls are.
    /// @return The thickness (mm).
    double thickness() const { return thickness_; }

    /// Changes how thick the walls are. Use on a copy, or from a Command.
    /// @param mm The thickness (mm).
    void setThickness(double mm) { thickness_ = mm; }

    FeatureType type() const override { return FeatureType::Shell; }
    std::unique_ptr<Feature> clone() const override { return std::make_unique<ShellFeature>(*this); }
    /// thickness (mm, more than 0).
    /// @return The numbers.
    std::vector<StepNumber> numbers() const override;
    std::optional<double> number(std::string_view key) const override;
    bool setNumber(std::string_view key, double value) override;
    std::vector<FeatureId> dependencies() const override { return {body_}; }
    void remap(const std::map<FeatureId, FeatureId>& map, bool renameRefs) override;
    Built build(kernel::IKernel& kernel, const ResultLookup& earlier) const override;

private:
    FeatureId body_;
    std::vector<GeometryRef> faces_;
    double thickness_;
};

}  // namespace sb::model
