#pragma once

// The Sketch panel: while a sketch is edited, every constraint and dimension in
// it, with each dimension's number ready to type in.

#include <QPointer>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "sb/core/Id.h"
#include "sb/model/CommandRegistry.h"
#include "sb/plugin/PluginContext.h"
#include "sb/sketch/Sketch.h"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace sb::plugins::sketcher {

// A spline picked in the viewer, or one of its control points (from 0).
struct SplinePick {
    EntityId spline;
    std::optional<int> pole;
};

// What the panel asks the sketcher.
struct PanelHooks {
    std::function<std::optional<FeatureId>()> editing;        // the sketch being edited
    std::function<std::vector<ConstraintId>()> picked;        // constraints picked in the viewer
    std::function<void(const ConstraintId&)> pick;            // pick one (from a click on its row)
    std::function<ConstraintId()> takeJustAdded;              // a new dimension to put the cursor in
    std::function<std::optional<SplinePick>()> pickedSpline;  // the one spline (or control point) picked
    std::function<void(SplinePick)> pickSpline;               // pick a spline, or one of its control points
};

// Changes a dimension of a sketch to what the student typed: a plain number is
// in the design's unit (degrees for an angle), like "45"; anything else is a
// formula, like "45 mm" or "wall * 2". Shows what went wrong, if anything.
// Returns false if nothing changed (the same number, or a problem).
bool typeDimension(plugin::PluginContext& ctx, const FeatureId& sketch, const std::string& name,
                   const std::string& text);

// What a dimension's box shows to start with: its number in the design's unit,
// like "60 mm", or "30 deg" for an angle.
std::string dimensionBoxText(plugin::PluginContext& ctx, const sketch::Constraint& c);

class SketchPanel final : public QWidget {
public:
    SketchPanel(plugin::PluginContext& ctx, PanelHooks hooks, QWidget* parent = nullptr);

    // Shows the edited sketch's constraints again (rebuilding the list only if
    // they changed, so a number being typed isn't lost).
    void refresh();

private:
    void rebuild();
    void setValue(const std::string& name, double value);
    void setFormula(const std::string& name, const std::string& formula);
    void showSpline(const sketch::Sketch& sketch);
    // Runs a spline command on the edited sketch; shows its problems. True if it worked.
    bool runSpline(const std::string& command, model::Args args);

    plugin::PluginContext& ctx_;
    PanelHooks hooks_;
    QLabel* status_ = nullptr;
    QTableWidget* table_ = nullptr;
    // The picked spline's settings.
    QWidget* splineBox_ = nullptr;
    QLabel* splineTitle_ = nullptr;
    QSpinBox* degree_ = nullptr;
    QCheckBox* closed_ = nullptr;
    QLabel* weightLabel_ = nullptr;
    QDoubleSpinBox* weight_ = nullptr;
    QPushButton* addPole_ = nullptr;
    QPushButton* removePole_ = nullptr;
    QLabel* source_ = nullptr;          // "From wave.csv", for an imported spline
    QPushButton* reload_ = nullptr;     // Reload from file
    std::optional<SplinePick> spline_;  // what the box is showing
    std::string shown_;                 // what the list shows now, to tell when it needs making again
    std::vector<ConstraintId> rows_;
    bool updating_ = false;
};

}  // namespace sb::plugins::sketcher
