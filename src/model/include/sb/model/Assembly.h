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

/// Where one of an assembly's parts comes from when it's linked to its own `.sb`
/// file, like wheel.sb, so changing the file changes the assembly. The assembly
/// still keeps a copy of the part as last used, so it opens even when the file
/// can't be found. A part without a link is an inside part: made in the
/// assembly, and kept only there.
struct PartLink {
    /// The part's file, as UTF-8 with forward slashes. While the assembly is
    /// open it's a full path; in the assembly's file it's written relative to
    /// the assembly, like "parts/wheel.sb".
    std::string path;
    /// The fingerprint of the file as last used: SHA-256 of its bytes, as 64
    /// lowercase hex digits. A file with another fingerprint has changed.
    std::string sha256;
    /// When the assembly last took its copy from the file, in UTC, like
    /// "2026-11-13T23:40:00Z".
    std::string updated;
    /// False when the file couldn't be found as the assembly opened, so it uses
    /// its saved copy. Not saved in the file.
    bool found = true;
};

}  // namespace sb::model
