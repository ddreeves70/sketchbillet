#pragma once

#include <optional>
#include <string>

#include "sb/kernel/IKernel.h"
#include "sb/model/Document.h"
#include "sb/sketch/Sketch.h"

namespace sb::cli {

// sketchbillet_cli plugins [--plugins <folder>]...
// Loads sb.modeling and every plugin in the folders, and lists each one with
// its state, commands and problems. Returns 1 if any plugin didn't load.
int listPlugins(int argc, char** argv);

// sketchbillet_cli run [--plugins <folder>]... [--open <in.sb>] [--save <out.sb>] <command> [name=value]...
// Runs one command by name (from any loaded plugin) on a new or opened design,
// rebuilds it with OCCT, prints each step, and saves it.
int runCommand(int argc, char** argv);

// sketchbillet_cli help [--plugins <folder>]... [<command>]
// Lists every command that `run` can run, or describes one command and its settings.
int help(int argc, char** argv);

// One line about a solved sketch, like "4 lines, 8 constraints, fully constrained".
std::string sketchSummary(const sketch::Sketch& sketch, int freedom);

// One line about a construction plane, like "plane through 0.00,0.00,20.00 mm, facing 0.00,0.00,1.00".
std::string planeSummary(const model::PlaneAxes& plane);

// Lists a step's numbers that come from formulas, one per line under the step:
// "    distance = wall * 4  (12 mm)".
void printFormulas(const model::Feature& step, LengthUnit unit, const char* indent);

// Lists the design's parameters, if it has any, after its steps: each with its
// formula and, if that isn't just the value, what it works out to.
void printParameters(const model::Document& doc);

// "40, -32, 12 mm": a point (length true, with its unit), or a direction with
// no unit.
std::string pointText(const kernel::Vec3& v, LengthUnit unit, bool length);

// Lists an assembly: its components (each with its part, grounded or free, and
// where it sits; with `directions`, which way its x and z face too), its parts
// (each built once: steps, mass), and the mass of everything together. Returns
// false if a part has a problem.
bool printAssembly(const model::Document& doc, const model::RebuildReport& report, kernel::IKernel& kernel,
                   bool directions);

// A body of a component's part, as `open --edges` and joint sides name it: "Wheel 1"
// (its part's only body) or "Wheel 1:Hub" (a body by its step's name).
struct ComponentBody {
    const model::Component* component = nullptr;
    const model::Document* part = nullptr;
    const model::Feature* body = nullptr;
    const model::ShapeNames* names = nullptr;  // the body's faces, edges and points, built
};

// Finds a component's body, or says why not in `why`.
std::optional<ComponentBody> componentBody(const model::Document& assembly,
                                           const model::RebuildReport& report, const std::string& component,
                                           const std::string& body, std::string& why);

}  // namespace sb::cli
