/// @file
/// Export to 3MF, the 3D printing format slicers like PrusaSlicer, Cura,
/// Bambu Studio and OrcaSlicer open: a zip holding the part's triangles, in
/// millimeters, with its name, material and color.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "sb/core/Diagnostic.h"
#include "sb/io/SketchBilletFile.h"
#include "sb/kernel/IKernel.h"
#include "sb/kernel/Types.h"
#include "sb/model/Document.h"
#include "sb/model/Material.h"

namespace sb::io {

/// One part to put in a 3MF file.
struct MeshObject {
    std::string name;         ///< The part's name, shown in the slicer's list of objects.
    kernel::Mesh mesh;        ///< Its triangles (mm), facing out. Points shared by faces are joined when written.
    model::Material material; ///< What it's made of: its name and color go in the file.
};

/// Packs parts into 3MF bytes. Each part's mesh is welded (kernel::weld()) so
/// every edge belongs to exactly two triangles, as 3MF asks. The same parts
/// always give the same bytes.
/// @param objects The parts, in the order the slicer lists them.
/// @param title The design's name, written as the file's title.
/// @param out The finished file's bytes.
/// @return No diagnostics on success; otherwise why it couldn't be packed.
Diagnostics write3mfBytes(const std::vector<MeshObject>& objects, const std::string& title, Bytes& out);

/// Writes parts to a 3MF file. A file already there is replaced, and a failed
/// write leaves it as it was.
/// @param objects The parts, in the order the slicer lists them.
/// @param title The design's name, written as the file's title.
/// @param path Where to write, usually ending in .3mf.
/// @return No diagnostics on success; otherwise why the file couldn't be written.
Diagnostics write3mf(const std::vector<MeshObject>& objects, const std::string& title,
                     const std::filesystem::path& path);

/// Writes one step's body to a 3MF file: meshed as finely as STL export, named
/// after the step, and made of the step's material.
/// @param doc The design.
/// @param report Its last rebuild.
/// @param kernel The kernel that built it, to mesh the body.
/// @param step The step whose body to write.
/// @param path Where to write, usually ending in .3mf.
/// @return No diagnostics on success; otherwise why the file couldn't be written.
///
/// @code
/// auto report = doc.regenerate(kernel, &solver);
/// auto problems = io::export3mf(doc, report, kernel, report.bodies().front(), "tag.3mf");
/// @endcode
Diagnostics export3mf(const model::Document& doc, const model::RebuildReport& report,
                      const kernel::IKernel& kernel, const FeatureId& step, const std::filesystem::path& path);

}  // namespace sb::io
