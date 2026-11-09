/// @file
/// The Plane step: a construction plane to sketch on, moved out from a base
/// plane, a flat face or another plane, and turned at an angle.
#pragma once

#include <optional>
#include <string>

#include "sb/model/Feature.h"
#include "sb/model/Naming.h"

namespace sb::model {

/// What a Plane step starts from.
struct PlaneFrom {
    /// Which kind of plane.
    enum class Kind {
        Base,   ///< A base plane: XY, XZ or YZ.
        Face,   ///< A flat face of a body.
        Plane,  ///< Another Plane step.
    };
    Kind kind = Kind::Base;              ///< Which kind.
    SketchPlane base = SketchPlane::XY;  ///< For Base: which one.
    SketchFace face{};                   ///< For Face: the body and its face.
    FeatureId plane{};                   ///< For Plane: the Plane step.
};

/// The line a Plane step turns about, in the plane it starts from.
struct PlaneHinge {
    /// Which line.
    enum class Kind {
        X,     ///< The starting plane's x direction, through its origin.
        Y,     ///< The starting plane's y direction, through its origin.
        Edge,  ///< A straight edge of the face's body, lying in the face (only when starting from a face).
    };
    Kind kind = Kind::X;  ///< Which line.
    GeometryRef edge{};   ///< For Edge: the edge, on the face's body.
};

/// A Plane step: a construction plane, for sketching where there's no face to
/// sketch on, like 20 mm above the ground or tilted 30 degrees. It isn't a body;
/// its rebuild result is the plane (FeatureResult::plane). It starts from a base
/// plane, a flat face or another Plane step, turns by angle() about a hinge line
/// in it, then moves offset() along the way it then faces.
///
/// @code
/// // A plane 25 mm above the ground.
/// auto above = std::make_unique<PlaneFeature>("Plane1", PlaneFrom{});
/// above->setOffset(25);
/// @endcode
class PlaneFeature final : public Feature {
public:
    /// Makes a Plane step with a new id: at its starting plane, not moved or turned.
    /// @param name The name shown in the feature tree.
    /// @param from What it starts from.
    PlaneFeature(std::string name, PlaneFrom from);

    /// Makes a Plane step with a given id, for loading a saved design.
    /// @param id The id it was saved with.
    /// @param name The name shown in the feature tree.
    /// @param from What it starts from.
    PlaneFeature(FeatureId id, std::string name, PlaneFrom from);

    /// Gives what it starts from.
    /// @return The starting plane.
    const PlaneFrom& from() const { return from_; }

    /// Changes what it starts from. Use on a copy, or from a Command.
    /// @param from The starting plane.
    void setFrom(PlaneFrom from) { from_ = std::move(from); }

    /// Gives how far it's moved along the way it faces (after turning).
    /// @return The distance (mm); below 0 moves it the other way.
    double offset() const { return offset_; }

    /// Changes how far it's moved. Use on a copy, or from a Command.
    /// @param mm The distance (mm).
    void setOffset(double mm) { offset_ = mm; }

    /// Gives how far it's turned about its hinge.
    /// @return The angle (radians), the right-hand way about the hinge's direction.
    double angle() const { return angle_; }

    /// Changes how far it's turned. Use on a copy, or from a Command.
    /// @param a The angle (radians).
    void setAngle(double a) { angle_ = a; }

    /// Gives the line it turns about.
    /// @return The hinge.
    const PlaneHinge& hinge() const { return hinge_; }

    /// Changes the line it turns about. Use on a copy, or from a Command.
    /// @param h The hinge.
    void setHinge(PlaneHinge h) { hinge_ = std::move(h); }

    /// Works out where the plane is, from the results of the steps before it.
    /// @param earlier The results so far.
    /// @param why Set to why not, when it can't be worked out.
    /// @return The plane's origin and directions (x and y in it), or nothing.
    std::optional<PlaneAxes> where(const ResultLookup& earlier, Diagnostic& why) const;

    FeatureType type() const override { return FeatureType::Plane; }
    std::unique_ptr<Feature> clone() const override { return std::make_unique<PlaneFeature>(*this); }
    /// offset (mm) and angle (radians).
    /// @return The numbers.
    std::vector<StepNumber> numbers() const override;
    std::optional<double> number(std::string_view key) const override;
    bool setNumber(std::string_view key, double value) override;
    std::vector<FeatureId> dependencies() const override;
    std::vector<FeatureId> referenceInputs() const override { return dependencies(); }
    void remapInputs(const std::map<FeatureId, FeatureId>& map) override;
    /// A Plane isn't built by the kernel: Document::regenerate() works it out with
    /// where() instead. This returns an error if it's called.
    /// @param kernel Not used.
    /// @param earlier Not used.
    /// @return An error.
    Built build(kernel::IKernel& kernel, const ResultLookup& earlier) const override;

private:
    PlaneFrom from_;
    double offset_ = 0;
    double angle_ = 0;
    PlaneHinge hinge_;
};

}  // namespace sb::model
