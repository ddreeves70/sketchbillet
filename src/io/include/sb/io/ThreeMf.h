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
#include "sb/kernel/Placement.h"
#include "sb/kernel/Types.h"
#include "sb/model/Document.h"
#include "sb/model/Material.h"

namespace sb::io {

/// One part to put in a 3MF file.
struct MeshObject {
    std::string name;   ///< The part's name, shown in the slicer's list of objects.
    kernel::Mesh mesh;  ///< Its triangles (mm), facing out. Points shared by faces are joined when written.
    model::Material material;  ///< What it's made of: its name and color go in the file.
};

/// A part made of one or more meshes (one per body), listed in the slicer as one
/// object (see write3mfBytes() with parts and items).
struct ThreeMfPart {
    std::string name;                 ///< The part's name, shown in the slicer's list of objects.
    std::vector<std::size_t> meshes;  ///< Its meshes: indices into the MeshObject list.
};

/// One copy of a part on the build plate.
struct ThreeMfItem {
    std::size_t part = 0;    ///< Which part: an index into the ThreeMfPart list.
    kernel::Placement at{};  ///< Where it is, and which way it's turned (mm).
};

/// Packs parts into 3MF bytes, each part's meshes once, and each copy of a part
/// as a build item with its own placement: four wheels share one mesh. A part
/// with more than one mesh is one object made of them (3MF components).
/// @param objects The meshes, each with its name and material.
/// @param parts The parts, each made of some of the meshes, in the order the slicer lists them.
/// @param items Each copy of a part, and where it is.
/// @param title The design's name, written as the file's title.
/// @param out The finished file's bytes.
/// @return No diagnostics on success; otherwise why it couldn't be packed.
Diagnostics write3mfBytes(const std::vector<MeshObject>& objects, const std::vector<ThreeMfPart>& parts,
                          const std::vector<ThreeMfItem>& items, const std::string& title, Bytes& out);

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
/// after the step, and made of the step's material, in the body's color
/// (Document::colorOf()).
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
                      const kernel::IKernel& kernel, const FeatureId& step,
                      const std::filesystem::path& path);

/// How to put an assembly in a 3MF file (see exportAssembly3mf()).
enum class AssemblyLayout {
    Assembled,  ///< Each component where it is in the assembly: to see it, or print it in one go.
    ToPrint,    ///< Each part once, as many times as components use it, side by side on the plate.
};

/// Writes an assembly to a 3MF file: each part's bodies meshed once (as finely
/// as STL export), in their materials and colors, and a copy for each component.
/// Assembled puts each copy where its component is; ToPrint lays the copies out
/// in rows on the build plate, 5 mm apart, one row per part, each copy as it's
/// modeled, its lowest point on the plate: the slicer gets four wheels to print.
/// @param assembly The assembly.
/// @param report Its last rebuild.
/// @param kernel The kernel that built it, to mesh the bodies.
/// @param layout Assembled, or ToPrint.
/// @param path Where to write, usually ending in .3mf.
/// @return No diagnostics on success; otherwise why the file couldn't be written.
///
/// @code
/// io::exportAssembly3mf(car, report, kernel, io::AssemblyLayout::ToPrint, "car-parts.3mf");
/// @endcode
Diagnostics exportAssembly3mf(const model::Document& assembly, const model::RebuildReport& report,
                              const kernel::IKernel& kernel, AssemblyLayout layout,
                              const std::filesystem::path& path);

}  // namespace sb::io
