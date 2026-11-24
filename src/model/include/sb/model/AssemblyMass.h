/// @file
/// An assembly's mass, weight and balance point (center of gravity), from each
/// part's material and where each component is placed.

#pragma once

#include <cstddef>

#include "sb/kernel/IKernel.h"
#include "sb/kernel/Types.h"

namespace sb::model {

class Document;
class RebuildReport;

/// How heavy an assembly is, and where it balances.
struct AssemblyMass {
    double volume = 0;   ///< All the components' bodies together (mm^3).
    double grams = 0;    ///< Mass (g).
    double newtons = 0;  ///< Weight on Earth (N).
    double cost = 0;     ///< Material cost, for the materials with a price (in their money).
    /// The balance point (center of gravity), in the assembly (mm): where the
    /// whole assembly would balance on a pin. The origin when it has no mass.
    kernel::Point3 center{};
    /// How many components have no bodies to count yet: an empty part, or one
    /// that didn't build. The numbers leave them out.
    std::size_t missing = 0;
};

/// Works out an assembly's mass, weight, cost and balance point: each part's
/// bodies once, from their materials, then for every component that uses it,
/// where it's placed.
/// @param assembly The assembly.
/// @param report Its last rebuild: each part's bodies, and where each component is placed.
/// @param kernel The kernel that built it.
/// @return The totals.
///
/// @code
/// const auto mass = model::assemblyMass(car, report, kernel);
/// std::printf("%.2f g, balances at z %.1f mm\n", mass.grams, mass.center.z);
/// @endcode
AssemblyMass assemblyMass(const Document& assembly, const RebuildReport& report,
                          const kernel::IKernel& kernel);

}  // namespace sb::model
