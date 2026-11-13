#include "sb/io/ThreeMf.h"

#include <charconv>
#include <cstdio>
#include <string>

#include "Zip.h"
#include "sb/core/Version.h"
#include "sb/kernel/MeshTools.h"

// A 3MF file (3MF core specification 1.3) is a zip holding:
//
//   [Content_Types].xml   what kind of file each ending is
//   _rels/.rels           where the model is
//   3D/3dmodel.model      the model: units, metadata, materials, meshes, build plate
//
// Each part is an <object> with its own mesh; one <basematerials> group lists
// each part's material name and color, and each object points at its entry.

namespace sb::io {

namespace {

std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // XML 1.0 can't hold most control characters at all.
                if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n' || c == '\r') out += c;
        }
    }
    return out;
}

// The shortest text that reads back as the same float: exact, and the same everywhere.
void number(std::string& out, float v) {
    if (v == 0.0f) v = 0.0f;  // no "-0"
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
}

std::string hexColor(const model::Color& c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

constexpr const char* kContentTypes =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
    " <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
    " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
    "</Types>\n";

constexpr const char* kRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
    " <Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
    "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n"
    "</Relationships>\n";

std::string model(const std::vector<MeshObject>& objects, const std::string& title) {
    std::string x;
    x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    x += "<model unit=\"millimeter\" xml:lang=\"en-US\" "
         "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n";
    x += " <metadata name=\"Title\">" + escape(title) + "</metadata>\n";
    x += " <metadata name=\"Application\">SketchBillet " + std::string(version()) + "</metadata>\n";
    x += " <resources>\n";
    // id 1: the materials, one entry per part, in order.
    x += "  <basematerials id=\"1\">\n";
    for (const auto& o : objects)
        x += "   <base name=\"" + escape(o.material.name) + "\" displaycolor=\"" + hexColor(o.material.color) + "\"/>\n";
    x += "  </basematerials>\n";
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto& o = objects[i];
        const kernel::Mesh mesh = kernel::weld(o.mesh);
        x += "  <object id=\"" + std::to_string(i + 2) + "\" type=\"model\" name=\"" + escape(o.name) +
             "\" pid=\"1\" pindex=\"" + std::to_string(i) + "\">\n";
        x += "   <mesh>\n    <vertices>\n";
        for (const auto& v : mesh.vertices) {
            x += "     <vertex x=\"";
            number(x, v[0]);
            x += "\" y=\"";
            number(x, v[1]);
            x += "\" z=\"";
            number(x, v[2]);
            x += "\"/>\n";
        }
        x += "    </vertices>\n    <triangles>\n";
        for (const auto& t : mesh.triangles) {
            x += "     <triangle v1=\"" + std::to_string(t[0]) + "\" v2=\"" + std::to_string(t[1]) + "\" v3=\"" +
                 std::to_string(t[2]) + "\"/>\n";
        }
        x += "    </triangles>\n   </mesh>\n  </object>\n";
    }
    x += " </resources>\n <build>\n";
    for (std::size_t i = 0; i < objects.size(); ++i) x += "  <item objectid=\"" + std::to_string(i + 2) + "\"/>\n";
    x += " </build>\n</model>\n";
    return x;
}

}  // namespace

Diagnostics write3mfBytes(const std::vector<MeshObject>& objects, const std::string& title, Bytes& out) {
    if (objects.empty())
        return {{"export.nothing", Severity::Error, "There's nothing to put in the 3MF file. Make a shape first.", {}}};
    zip::Entries entries;
    entries.emplace_back("[Content_Types].xml", kContentTypes);
    entries.emplace_back("_rels/.rels", kRels);
    entries.emplace_back("3D/3dmodel.model", model(objects, title));
    return zip::pack(entries, out,
                     {"export.3mf.failed", Severity::Error, "SketchBillet couldn't put the part into a 3MF file.", {}});
}

Diagnostics write3mf(const std::vector<MeshObject>& objects, const std::string& title,
                     const std::filesystem::path& path) {
    Bytes bytes;
    if (auto issues = write3mfBytes(objects, title, bytes); !issues.empty()) return issues;
    return zip::writeAtomically(bytes, path,
                                {"export.cant_write", Severity::Error,
                                 "SketchBillet couldn't write " + path.filename().string() +
                                     ". Check that the folder exists, that you're allowed to save there, and "
                                     "that the disk isn't full.",
                                 {}});
}

Diagnostics export3mf(const model::Document& doc, const model::RebuildReport& report,
                      const kernel::IKernel& kernel, const FeatureId& step, const std::filesystem::path& path) {
    const auto* shape = report.shapeOf(step);
    const auto* feature = doc.find(step);
    if (!shape || !feature)
        return {{"export.nothing", Severity::Error, "That step didn't build, so there's nothing to export.", {}}};
    MeshObject part{feature->name(), kernel.tessellate(*shape, kernel::MeshQuality{}), doc.materialOf(step)};
    if (part.mesh.triangles.empty())
        return {{"export.nothing", Severity::Error,
                 feature->name() + " has no surface to export. Pick a step that makes a solid shape.", {}}};
    return write3mf({std::move(part)}, doc.name(), path);
}

}  // namespace sb::io
