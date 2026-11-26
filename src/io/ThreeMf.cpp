#include "sb/io/ThreeMf.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
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
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
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
    " <Default Extension=\"rels\" "
    "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
    " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
    "</Types>\n";

constexpr const char* kRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
    " <Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
    "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n"
    "</Relationships>\n";

// The 3MF transform of a placement: the turned x, y and z axes, then where the
// origin goes (3MF multiplies a row of x, y, z, 1 by it).
std::string transformText(const kernel::Placement& p) {
    const auto& x = p.xAxis;
    const auto& z = p.zAxis;
    const auto y = kernel::yAxisOf(p);
    std::string out;
    for (const double v : {x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z, p.origin.x, p.origin.y, p.origin.z}) {
        if (!out.empty()) out += ' ';
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof buf, std::abs(v) < 1e-12 ? 0.0 : v);
        out.append(buf, r.ptr);
    }
    return out;
}

bool isIdentity(const kernel::Placement& p) {
    return p.origin.x == 0 && p.origin.y == 0 && p.origin.z == 0 && p.xAxis.x == 1 && p.xAxis.y == 0 &&
           p.xAxis.z == 0 && p.zAxis.x == 0 && p.zAxis.y == 0 && p.zAxis.z == 1;
}

std::string model(const std::vector<MeshObject>& objects, const std::vector<ThreeMfPart>& parts,
                  const std::vector<ThreeMfItem>& items, const std::string& title) {
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
        x += "   <base name=\"" + escape(o.material.name) + "\" displaycolor=\"" +
             hexColor(o.material.color) + "\"/>\n";
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
            x += "     <triangle v1=\"" + std::to_string(t[0]) + "\" v2=\"" + std::to_string(t[1]) +
                 "\" v3=\"" + std::to_string(t[2]) + "\"/>\n";
        }
        x += "    </triangles>\n   </mesh>\n  </object>\n";
    }
    // A part with more than one mesh: an object made of them. Its id follows the meshes'.
    std::vector<std::size_t> partId(parts.size());
    std::size_t next = objects.size() + 2;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto& p = parts[i];
        if (p.meshes.size() == 1) {
            partId[i] = p.meshes.front() + 2;
            continue;
        }
        partId[i] = next++;
        x += "  <object id=\"" + std::to_string(partId[i]) + "\" type=\"model\" name=\"" + escape(p.name) +
             "\">\n   <components>\n";
        for (const auto m : p.meshes) x += "    <component objectid=\"" + std::to_string(m + 2) + "\"/>\n";
        x += "   </components>\n  </object>\n";
    }
    x += " </resources>\n <build>\n";
    for (const auto& item : items) {
        x += "  <item objectid=\"" + std::to_string(partId[item.part]) + "\"";
        if (!isIdentity(item.at)) x += " transform=\"" + transformText(item.at) + "\"";
        x += "/>\n";
    }
    x += " </build>\n</model>\n";
    return x;
}

// Each mesh its own part, once, where it is: a design's bodies.
void onePartEach(const std::vector<MeshObject>& objects, std::vector<ThreeMfPart>& parts,
                 std::vector<ThreeMfItem>& items) {
    for (std::size_t i = 0; i < objects.size(); ++i) {
        parts.push_back({objects[i].name, {i}});
        items.push_back({i, {}});
    }
}

}  // namespace

Diagnostics write3mfBytes(const std::vector<MeshObject>& objects, const std::string& title, Bytes& out) {
    std::vector<ThreeMfPart> parts;
    std::vector<ThreeMfItem> items;
    onePartEach(objects, parts, items);
    return write3mfBytes(objects, parts, items, title, out);
}

Diagnostics write3mfBytes(const std::vector<MeshObject>& objects, const std::vector<ThreeMfPart>& parts,
                          const std::vector<ThreeMfItem>& items, const std::string& title, Bytes& out) {
    if (objects.empty() || items.empty())
        return {{"export.nothing",
                 Severity::Error,
                 "There's nothing to put in the 3MF file. Make a shape first.",
                 {}}};
    zip::Entries entries;
    entries.emplace_back("[Content_Types].xml", kContentTypes);
    entries.emplace_back("_rels/.rels", kRels);
    entries.emplace_back("3D/3dmodel.model", model(objects, parts, items, title));
    return zip::pack(
        entries, out,
        {"export.3mf.failed", Severity::Error, "SketchBillet couldn't put the part into a 3MF file.", {}});
}

namespace {

Diagnostics writeBytes(const Bytes& bytes, const std::filesystem::path& path) {
    return zip::writeAtomically(bytes, path,
                                {"export.cant_write",
                                 Severity::Error,
                                 "SketchBillet couldn't write " + path.filename().string() +
                                     ". Check that the folder exists, that you're allowed to save there, and "
                                     "that the disk isn't full.",
                                 {}});
}

}  // namespace

Diagnostics write3mf(const std::vector<MeshObject>& objects, const std::string& title,
                     const std::filesystem::path& path) {
    Bytes bytes;
    if (auto issues = write3mfBytes(objects, title, bytes); !issues.empty()) return issues;
    return writeBytes(bytes, path);
}

Diagnostics export3mf(const model::Document& doc, const model::RebuildReport& report,
                      const kernel::IKernel& kernel, const FeatureId& step,
                      const std::filesystem::path& path) {
    const auto* shape = report.shapeOf(step);
    const auto* feature = doc.find(step);
    if (!shape || !feature)
        return {
            {"export.nothing", Severity::Error, "That step didn't build, so there's nothing to export.", {}}};
    MeshObject part{feature->name(), kernel.tessellate(*shape, kernel::MeshQuality{}), doc.materialOf(step)};
    part.material.color = doc.colorOf(step);  // the body's own color, if it has one
    if (part.mesh.triangles.empty())
        return {{"export.nothing",
                 Severity::Error,
                 feature->name() + " has no surface to export. Pick a step that makes a solid shape.",
                 {}}};
    return write3mf({std::move(part)}, doc.name(), path);
}

Diagnostics exportAssembly3mf(const model::Document& assembly, const model::RebuildReport& report,
                              const kernel::IKernel& kernel, AssemblyLayout layout,
                              const std::filesystem::path& path) {
    std::vector<MeshObject> objects;
    std::vector<ThreeMfPart> parts;
    std::vector<kernel::Box3> boxes;  // each part's, in its own place
    std::vector<std::size_t> partOf(assembly.parts().size(), SIZE_MAX);
    for (std::size_t k = 0; k < assembly.parts().size(); ++k) {
        const auto* part = assembly.parts()[k];
        const auto* built = report.part(part->id());
        if (!built) continue;
        ThreeMfPart p{part->name(), {}};
        kernel::Box3 box{};
        for (const auto& body : built->bodies()) {
            const auto* shape = built->shapeOf(body);
            const auto* feature = part->find(body);
            if (!shape || !feature) continue;
            MeshObject m{built->bodies().size() == 1 ? part->name() : part->name() + " " + feature->name(),
                         kernel.tessellate(*shape, kernel::MeshQuality{}), part->materialOf(body)};
            m.material.color = part->colorOf(body);
            if (m.mesh.triangles.empty()) continue;
            const auto b = kernel.bounds(*shape);
            if (p.meshes.empty()) box = b;
            box.min = {std::min(box.min.x, b.min.x), std::min(box.min.y, b.min.y),
                       std::min(box.min.z, b.min.z)};
            box.max = {std::max(box.max.x, b.max.x), std::max(box.max.y, b.max.y),
                       std::max(box.max.z, b.max.z)};
            p.meshes.push_back(objects.size());
            objects.push_back(std::move(m));
        }
        if (p.meshes.empty()) continue;  // nothing to print yet
        partOf[k] = parts.size();
        parts.push_back(std::move(p));
        boxes.push_back(box);
    }

    std::vector<ThreeMfItem> items;
    if (layout == AssemblyLayout::Assembled) {
        for (const auto& c : assembly.components()) {
            const auto& all = assembly.parts();
            const auto it =
                std::find_if(all.begin(), all.end(), [&](const auto* p) { return p->id() == c.part; });
            if (it == all.end() || partOf[static_cast<std::size_t>(it - all.begin())] == SIZE_MAX) continue;
            const auto* at = report.placementOf(c.id);
            items.push_back({partOf[static_cast<std::size_t>(it - all.begin())], at ? *at : c.placement});
        }
    } else {
        // A row for each part, 5 mm apart, as many copies as components use it.
        constexpr double gap = 5;
        double y = 0;
        for (std::size_t k = 0; k < assembly.parts().size(); ++k) {
            if (partOf[k] == SIZE_MAX) continue;
            const auto* part = assembly.parts()[k];
            const auto count = std::count_if(assembly.components().begin(), assembly.components().end(),
                                             [&](const model::Component& c) { return c.part == part->id(); });
            const auto& box = boxes[partOf[k]];
            double x = 0;
            for (long n = 0; n < count; ++n) {
                kernel::Placement at;
                at.origin = {x - box.min.x, y - box.min.y, -box.min.z};
                items.push_back({partOf[k], at});
                x += box.max.x - box.min.x + gap;
            }
            y += box.max.y - box.min.y + gap;
        }
    }
    if (items.empty())
        return {{"export.nothing",
                 Severity::Error,
                 assembly.name() + " has no parts with a shape to export yet. Add a shape to a part first.",
                 {}}};
    Bytes bytes;
    if (auto issues = write3mfBytes(objects, parts, items, assembly.name(), bytes); !issues.empty())
        return issues;
    return writeBytes(bytes, path);
}

}  // namespace sb::io
