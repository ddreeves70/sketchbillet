#pragma once

/// @file
/// Components: parts placed in an assembly (see docs/design/assemblies.md).

#include <string>

#include "sb/core/Id.h"
#include "sb/kernel/Placement.h"

namespace sb::model {

/// Identifies a component of an assembly: one part placed there.
using ComponentId = InstanceId;

/// One part placed in an assembly. Four wheels are four components of one part,
/// Wheel, named Wheel 1 to Wheel 4. A component places the whole part: every
/// finished body in it.
struct Component {
    ComponentId id{};  ///< Its UUID, which never changes.
    std::string name;  ///< Like "Wheel 1": the part's name and a number. Students can rename it.
    DocId part{};      ///< Which part: one of the assembly's parts (Document::findPart()).
    /// Where it sits in the assembly: where the part's origin goes, and which
    /// way its x and z face.
    kernel::Placement placement{};
    /// True if it's fixed where it is. The first component put in an assembly is.
    bool grounded = false;
};

}  // namespace sb::model
