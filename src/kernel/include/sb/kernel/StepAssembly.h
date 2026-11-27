#pragma once

/// @file
/// An assembly to write as one STEP file (see IKernel::exportStepAssembly()):
/// its parts once each, and a copy of a part for each component. No OCCT.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sb/kernel/Placement.h"
#include "sb/kernel/Types.h"

namespace sb::kernel {

/// One body of a part, as written to a STEP file.
struct StepBody {
    ShapeHandle shape;  ///< The body, where it is in its part (mm).
    std::string name;   ///< Its name, like "Tyre".
    std::optional<std::array<std::uint8_t, 3>>
        color;  ///< Its color (red, green, blue, 0 to 255), if it has one.
};

/// A part, written once however many components use it.
struct StepPart {
    std::string name;  ///< Its name, like "Wheel": the STEP product's name.
    std::vector<StepBody>
        bodies;  ///< Its bodies. One body is the part itself; more make a small assembly of them.
};

/// A copy of a part: one component of the assembly.
struct StepCopy {
    std::size_t part = 0;  ///< Which part: an index into StepAssembly::parts.
    std::string name;      ///< The component's name, like "Wheel 2".
    Placement at{};        ///< Where it is in the assembly, and which way it faces (mm).
};

/// An assembly to write as a STEP file: other CAD programs open it as an
/// assembly of parts, each part's shape stored once.
///
/// @code
/// StepAssembly car{"Car", {{"Wheel", {{wheel, "Wheel", std::nullopt}}}}, {}};
/// car.copies.push_back({0, "Wheel 1", {{0, -20, 0}}});
/// car.copies.push_back({0, "Wheel 2", {{25, -20, 0}}});
/// kernel.exportStepAssembly(car, "car.step");
/// @endcode
struct StepAssembly {
    std::string name;              ///< The assembly's name: the top STEP product's name.
    std::vector<StepPart> parts;   ///< Its parts, each once.
    std::vector<StepCopy> copies;  ///< Its components, each a copy of a part.
};

}  // namespace sb::kernel
