#pragma once

// Which constraints and dimensions fit what's picked in a sketch, and the
// words and places of the labels that show them on the drawing.

#include <optional>
#include <string>
#include <vector>

#include "sb/core/Units.h"
#include "sb/sketch/Sketch.h"

namespace sb::plugins::sketcher {

// What's picked, sorted: points (ends, centers, control points, point
// entities), lines, round things (circles and arcs) and whole splines.
struct Picks {
    std::vector<sketch::PointRef> points;
    std::vector<EntityId> lines;
    std::vector<EntityId> rounds;
    std::vector<EntityId> splines{};
    std::size_t other = 0;  // picked things no rule takes (constraint labels)
    std::size_t count() const {
        return points.size() + lines.size() + rounds.size() + splines.size() + other;
    }
};

// The sketch toolbar's constraint and dimension buttons.
enum class Rule {
    Coincident,
    Horizontal,
    Vertical,
    Parallel,
    Perpendicular,
    Equal,
    Tangent,
    Smooth,  // a spline joined to a curve: tangent, and bending equally (G2)
    On,
    Midpoint,
    Symmetric,  // two points mirrored across a line
    Fix,
    Dimension,            // length, radius, diameter, distance or angle: whichever fits
    HorizontalDimension,  // how far apart in x
    VerticalDimension,    // how far apart in y
};

// A button's words, tip, and what to pick for it (said when it doesn't fit).
struct RuleInfo {
    const char* id;     // the action id after "sb.sketcher.", e.g. "horizontal"
    const char* text;   // e.g. "Horizontal"
    const char* tip;    // one sentence, starting with what it does, ending with what to pick
    const char* needs;  // e.g. "one or more lines"
};

const std::vector<Rule>& allRules();
RuleInfo ruleInfo(Rule rule);

// The constraints a button adds for what's picked, measured from the sketch as
// it is (dimensions start at today's size, so nothing moves). Ids and names are
// left empty. Empty if the button doesn't fit what's picked.
std::vector<sketch::Constraint> plan(Rule rule, const Picks& picks, const sketch::Sketch& sketch);

// Whether two constraints say the same thing (same kind, on the same parts).
bool sameConstraint(const sketch::Constraint& a, const sketch::Constraint& b);

// What a constraint's label says on the drawing: "H", "40 mm", "R4 mm", "30°";
// empty for one that isn't labeled (Coincident: its dot shows it). Lengths are
// in the design's unit ("1.575 in").
std::string labelText(const sketch::Constraint& c, LengthUnit unit = LengthUnit::Millimeter);

// Where a label goes, in sketch mm, and which way to move it if another label is
// already there (a unit vector, away from the middle of the sketch).
struct LabelPlace {
    sketch::Point2 at;
    sketch::Point2 away;
};

// Where a constraint's label goes: `gap` mm beside what it's on, on the side away
// from `middle` (the middle of the sketch), so labels sit outside the shape.
// Empty for a constraint without a label.
std::optional<LabelPlace> labelPlace(const sketch::Constraint& c, const sketch::Sketch& sketch, double gap,
                                     sketch::Point2 middle);

// A dimension drawn as on a drawing (plugin::OverlayShape::Dimension), in sketch
// mm: what it measures from and to, a point its line passes through, the
// direction it measures along (zero: straight from one point to the other), and
// where its number goes (for clicking it).
struct DimensionPlace {
    sketch::Point2 from, to, through;
    sketch::Point2 along{};
    bool arrowAtFrom = true;
    bool arrowAtTo = true;
    bool textAtEnd = false;  // a leader (radius, diameter): the number past its `to` end
    sketch::Point2 textAt;
};

// Where a dimension is drawn: lengths and distances get a line `2 * gap` mm out
// from what they measure, on the side away from `middle`; a radius or diameter
// gets a leader from the edge outwards. Empty for constraints drawn as labels
// (an angle, H, V, ...).
std::optional<DimensionPlace> dimensionPlace(const sketch::Constraint& c, const sketch::Sketch& sketch,
                                             double gap, sketch::Point2 middle);

// Where a dimension's line runs, in sketch mm: level with what it measures from
// and to, through its `through` point.
std::pair<sketch::Point2, sketch::Point2> dimensionLine(const DimensionPlace& d);

// Moves a dimension's line (and its number) `by` mm further out, away from what it measures.
void pushOut(DimensionPlace& d, double by);

// Whether two dimensions' lines would be drawn on top of each other: side by side
// closer than `near` mm, along the same direction, and overlapping.
bool dimensionsClash(const DimensionPlace& a, const DimensionPlace& b, double near);

// What a dimension's number says: like labelText(), without the "x" or "y" of a
// horizontal or vertical distance (its line shows which way), and never negative.
std::string dimensionText(const sketch::Constraint& c, LengthUnit unit = LengthUnit::Millimeter);

// Name base for a new constraint, e.g. "HorizontalDistance".
std::string constraintBase(sketch::ConstraintKind kind);

}  // namespace sb::plugins::sketcher
