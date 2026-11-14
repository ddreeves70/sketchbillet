#include "sb/kernel/MeshFiles.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Unzip.h"
#include "sb/kernel/MeshTools.h"

namespace sb::kernel {

namespace {

Diagnostic problem(std::string code, std::string message, std::string detail = {}) {
    return {std::move(code), Severity::Error, std::move(message), std::move(detail)};
}

MeshFile failed(std::string code, std::string message, std::string detail = {}) {
    MeshFile out;
    out.issues.push_back(problem(std::move(code), std::move(message), std::move(detail)));
    return out;
}

// A number like "-1.5e3", read the same whatever language the computer uses
// (strtod reads "1,5" in some). Nothing if `text` isn't one number.
std::optional<double> number(std::string_view text) {
    std::size_t i = 0;
    auto at = [&](std::size_t k) { return k < text.size() ? text[k] : '\0'; };
    bool negative = false;
    if (at(i) == '+' || at(i) == '-') negative = at(i++) == '-';
    long double mantissa = 0;
    int exponent = 0, digits = 0;
    while (at(i) >= '0' && at(i) <= '9') {
        mantissa = mantissa * 10 + (at(i++) - '0');
        ++digits;
    }
    if (at(i) == '.') {
        ++i;
        while (at(i) >= '0' && at(i) <= '9') {
            mantissa = mantissa * 10 + (at(i++) - '0');
            --exponent;
            ++digits;
        }
    }
    if (digits == 0) return std::nullopt;
    if (at(i) == 'e' || at(i) == 'E') {
        ++i;
        bool down = false;
        if (at(i) == '+' || at(i) == '-') down = at(i++) == '-';
        int e = 0, edigits = 0;
        while (at(i) >= '0' && at(i) <= '9' && e < 10000) {
            e = e * 10 + (at(i++) - '0');
            ++edigits;
        }
        if (edigits == 0) return std::nullopt;
        exponent += down ? -e : e;
    }
    if (i != text.size()) return std::nullopt;
    const long double v = mantissa * std::pow(10.0L, static_cast<long double>(exponent));
    if (!std::isfinite(static_cast<double>(v))) return std::nullopt;
    return static_cast<double>(negative ? -v : v);
}

// A float stored little-endian, as STL files keep them, on any computer.
float littleFloat(const unsigned char* p) {
    const std::uint32_t bits = static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
                               static_cast<std::uint32_t>(p[2]) << 16 |
                               static_cast<std::uint32_t>(p[3]) << 24;
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

MeshFile tooBig(std::size_t triangles, const char* kind) {
    return failed(
        "import.mesh.too_big",
        "This " + std::string(kind) + " file has " + std::to_string(triangles) +
            " triangles, and SketchBillet can make a solid of up to " + std::to_string(kMaxImportTriangles) +
            ". Export it with fewer triangles (a coarser or \"low\" resolution), or simplify it first "
            "in a mesh program.");
}

// Joins points at the same place, and checks something is left.
MeshFile finish(Mesh mesh, const char* kind) {
    if (mesh.triangles.size() > kMaxImportTriangles) return tooBig(mesh.triangles.size(), kind);
    MeshFile out;
    out.mesh = weld(mesh);
    if (out.mesh.triangles.empty()) {
        return failed("import.mesh.empty", "This " + std::string(kind) +
                                               " file has no triangles in it. Check it's the right file, and "
                                               "export the part again from the other program.");
    }
    return out;
}

// ---------------------------------------------------------------------------
// STL
// ---------------------------------------------------------------------------

MeshFile readBinaryStl(std::string_view bytes, double scale) {
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::uint32_t count = static_cast<std::uint32_t>(p[80]) | static_cast<std::uint32_t>(p[81]) << 8 |
                                static_cast<std::uint32_t>(p[82]) << 16 |
                                static_cast<std::uint32_t>(p[83]) << 24;
    Mesh mesh;
    mesh.vertices.reserve(static_cast<std::size_t>(count) * 3);
    mesh.triangles.reserve(count);
    for (std::uint32_t t = 0; t < count; ++t) {
        const unsigned char* tri = p + 84 + static_cast<std::size_t>(t) * 50 + 12;  // after the normal
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        for (int v = 0; v < 3; ++v) {
            std::array<float, 3> xyz{};
            for (int k = 0; k < 3; ++k) {
                const float f = littleFloat(tri + v * 12 + k * 4);
                if (!std::isfinite(f))
                    return failed("import.stl.unreadable",
                                  "This STL file has a point that isn't a number. Export it again from the "
                                  "other program.");
                xyz[static_cast<std::size_t>(k)] = static_cast<float>(f * scale);
            }
            mesh.vertices.push_back(xyz);
        }
        mesh.triangles.push_back({first, first + 1, first + 2});
    }
    return finish(std::move(mesh), "STL");
}

MeshFile readTextStl(std::string_view text, double scale) {
    Mesh mesh;
    std::vector<std::array<float, 3>> facet;
    std::size_t i = 0;
    auto word = [&]() -> std::string_view {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        const std::size_t from = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        return text.substr(from, i - from);
    };
    const auto bad = [] {
        return failed("import.stl.unreadable",
                      "This isn't an STL file SketchBillet can read. Export the part from the other program "
                      "again, as STL.");
    };
    if (word() != "solid") return bad();
    for (auto w = word(); !w.empty(); w = word()) {
        if (w == "vertex") {
            std::array<float, 3> xyz{};
            for (int k = 0; k < 3; ++k) {
                const auto v = number(word());
                if (!v) return bad();
                xyz[static_cast<std::size_t>(k)] = static_cast<float>(*v * scale);
            }
            facet.push_back(xyz);
        } else if (w == "endloop") {
            if (facet.size() != 3) return bad();  // only triangles
            const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.insert(mesh.vertices.end(), facet.begin(), facet.end());
            mesh.triangles.push_back({first, first + 1, first + 2});
            facet.clear();
            if (mesh.triangles.size() > kMaxImportTriangles) return finish(std::move(mesh), "STL");
        }
    }
    return finish(std::move(mesh), "STL");
}

// ---------------------------------------------------------------------------
// 3MF: a zip with an XML model in it.
// ---------------------------------------------------------------------------

// One XML tag: <name a="1" b='2'>, </name> or <name/>.
struct Tag {
    std::string_view name;  // without any "prefix:"
    bool closing = false;   // </name>
    bool empty = false;     // <name/>
    std::vector<std::pair<std::string_view, std::string_view>> attributes;

    std::optional<std::string_view> attribute(std::string_view key) const {
        for (const auto& [k, v] : attributes)
            if (k == key) return v;
        return std::nullopt;
    }
};

// Reads tags one after another, skipping text, comments and declarations.
class Tags {
public:
    explicit Tags(std::string_view xml) : xml_(xml) {}

    // The next tag; nothing at the end. Sets `damaged` if a tag isn't finished.
    std::optional<Tag> next() {
        for (;;) {
            const auto lt = xml_.find('<', pos_);
            if (lt == std::string_view::npos) return std::nullopt;
            pos_ = lt + 1;
            if (xml_.compare(pos_, 3, "!--") == 0) {
                if (!skipPast("-->")) return std::nullopt;
                continue;
            }
            if (xml_.compare(pos_, 8, "![CDATA[") == 0) {
                if (!skipPast("]]>")) return std::nullopt;
                continue;
            }
            if (pos_ < xml_.size() && (xml_[pos_] == '?' || xml_[pos_] == '!')) {
                if (!skipPast(">")) return std::nullopt;
                continue;
            }
            Tag t;
            if (pos_ < xml_.size() && xml_[pos_] == '/') {
                t.closing = true;
                ++pos_;
            }
            const auto nameFrom = pos_;
            while (pos_ < xml_.size() && !std::isspace(static_cast<unsigned char>(xml_[pos_])) &&
                   xml_[pos_] != '/' && xml_[pos_] != '>')
                ++pos_;
            t.name = xml_.substr(nameFrom, pos_ - nameFrom);
            if (const auto colon = t.name.find(':'); colon != std::string_view::npos)
                t.name = t.name.substr(colon + 1);
            for (;;) {
                while (pos_ < xml_.size() && std::isspace(static_cast<unsigned char>(xml_[pos_]))) ++pos_;
                if (pos_ >= xml_.size()) {
                    damaged = true;
                    return std::nullopt;
                }
                if (xml_[pos_] == '>') {
                    ++pos_;
                    return t;
                }
                if (xml_.compare(pos_, 2, "/>") == 0) {
                    pos_ += 2;
                    t.empty = true;
                    return t;
                }
                const auto keyFrom = pos_;
                while (pos_ < xml_.size() && xml_[pos_] != '=' &&
                       !std::isspace(static_cast<unsigned char>(xml_[pos_])) && xml_[pos_] != '>')
                    ++pos_;
                auto key = xml_.substr(keyFrom, pos_ - keyFrom);
                while (pos_ < xml_.size() && std::isspace(static_cast<unsigned char>(xml_[pos_]))) ++pos_;
                if (pos_ >= xml_.size() || xml_[pos_] != '=') {
                    damaged = true;
                    return std::nullopt;
                }
                ++pos_;
                while (pos_ < xml_.size() && std::isspace(static_cast<unsigned char>(xml_[pos_]))) ++pos_;
                if (pos_ >= xml_.size() || (xml_[pos_] != '"' && xml_[pos_] != '\'')) {
                    damaged = true;
                    return std::nullopt;
                }
                const char quote = xml_[pos_++];
                const auto end = xml_.find(quote, pos_);
                if (end == std::string_view::npos) {
                    damaged = true;
                    return std::nullopt;
                }
                // A namespaced attribute (like p:path) is left out, so it isn't mistaken for a core one.
                if (key.find(':') == std::string_view::npos)
                    t.attributes.emplace_back(key, xml_.substr(pos_, end - pos_));
                pos_ = end + 1;
            }
        }
    }

    bool damaged = false;

private:
    bool skipPast(std::string_view end) {
        const auto at = xml_.find(end, pos_);
        if (at == std::string_view::npos) {
            damaged = true;
            return false;
        }
        pos_ = at + end.size();
        return true;
    }

    std::string_view xml_;
    std::size_t pos_ = 0;
};

// A 3MF placement: x' = x*m[0] + y*m[3] + z*m[6] + m[9], and so on.
using Transform = std::array<double, 12>;
constexpr Transform kIdentity{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

std::optional<Transform> transformFrom(std::optional<std::string_view> text) {
    if (!text) return kIdentity;
    Transform m{};
    std::size_t i = 0, k = 0;
    while (k < 12) {
        while (i < text->size() && std::isspace(static_cast<unsigned char>((*text)[i]))) ++i;
        const auto from = i;
        while (i < text->size() && !std::isspace(static_cast<unsigned char>((*text)[i]))) ++i;
        const auto v = number(text->substr(from, i - from));
        if (!v) return std::nullopt;
        m[k++] = *v;
    }
    return m;
}

// `a` then `b`: a point placed by `a`, then by `b`.
Transform then(const Transform& a, const Transform& b) {
    Transform out{};
    for (int r = 0; r < 4; ++r) {      // rows of a (the last is the shift)
        for (int c = 0; c < 3; ++c) {  // columns
            double v = 0;
            for (int k = 0; k < 3; ++k)
                v += a[static_cast<std::size_t>(r * 3 + k)] * b[static_cast<std::size_t>(k * 3 + c)];
            if (r == 3) v += b[static_cast<std::size_t>(9 + c)];
            out[static_cast<std::size_t>(r * 3 + c)] = v;
        }
    }
    return out;
}

struct Object {
    std::vector<std::array<double, 3>> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<std::pair<std::string, Transform>> components;  // object id, placement
};

double unitScale(std::string_view unit) {
    if (unit == "micron") return 0.001;
    if (unit == "centimeter") return 10;
    if (unit == "inch") return 25.4;
    if (unit == "foot") return 304.8;
    if (unit == "meter") return 1000;
    return 1;  // millimeter, the default
}

}  // namespace

MeshFile readStl(std::string_view bytes, double mmPerUnit) {
    // Binary: an 80-byte header, a count, then 50 bytes a triangle. Text files
    // start with "solid", but so do some binary ones, so the size decides.
    if (bytes.size() >= 84) {
        const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
        const std::uint64_t count =
            static_cast<std::uint64_t>(p[80]) | static_cast<std::uint64_t>(p[81]) << 8 |
            static_cast<std::uint64_t>(p[82]) << 16 | static_cast<std::uint64_t>(p[83]) << 24;
        if (84 + count * 50 == bytes.size()) {
            if (count > kMaxImportTriangles) return tooBig(static_cast<std::size_t>(count), "STL");
            return readBinaryStl(bytes, mmPerUnit);
        }
    }
    return readTextStl(bytes, mmPerUnit);
}

MeshFile read3mf(std::string_view bytes) {
    const auto unreadable = [](std::string why) {
        return failed(
            "import.3mf.unreadable",
            "This isn't a 3MF file SketchBillet can read. Export the part from the other program again, "
            "as 3MF or STL.",
            std::move(why));
    };
    std::string why;
    const auto entries = unzip::list(bytes, why);
    if (entries.empty()) return unreadable(why);
    constexpr std::size_t kLimit = 512ull * 1024 * 1024;  // unpacked: refuse zip bombs
    auto find = [&](std::string_view name) -> const unzip::Entry* {
        if (!name.empty() && name.front() == '/') name.remove_prefix(1);
        for (const auto& e : entries)
            if (e.name == name) return &e;
        return nullptr;
    };
    // The model's part: named in _rels/.rels, usually 3D/3dmodel.model.
    std::string modelName = "3D/3dmodel.model";
    if (const auto* rels = find("_rels/.rels")) {
        if (auto text = unzip::read(bytes, *rels, kLimit, why)) {
            Tags tags(*text);
            while (auto t = tags.next()) {
                const auto type = t->attribute("Type");
                const auto target = t->attribute("Target");
                if (t->name == "Relationship" && type && target && type->ends_with("/3dmodel")) {
                    modelName = std::string(*target);
                    break;
                }
            }
        }
    }
    const auto* model = find(modelName);
    if (!model) return unreadable("no " + modelName + " in it");
    const auto xml = unzip::read(bytes, *model, kLimit, why);
    if (!xml) return unreadable(why);

    double scale = 1;
    std::map<std::string, Object> objects;
    std::vector<std::pair<std::string, Transform>> build;  // what's placed, and where
    Object* current = nullptr;
    Tags tags(*xml);
    while (auto t = tags.next()) {
        if (t->closing) {
            if (t->name == "object") current = nullptr;
            continue;
        }
        if (t->name == "model") {
            scale = unitScale(t->attribute("unit").value_or("millimeter"));
        } else if (t->name == "object") {
            const auto id = t->attribute("id");
            if (!id) return unreadable("an object with no id");
            current = &objects[std::string(*id)];
            if (t->empty) current = nullptr;
        } else if (t->name == "vertex" && current) {
            const auto x = number(t->attribute("x").value_or("")), y = number(t->attribute("y").value_or("")),
                       z = number(t->attribute("z").value_or(""));
            if (!x || !y || !z) return unreadable("a vertex that isn't three numbers");
            current->vertices.push_back({*x, *y, *z});
        } else if (t->name == "triangle" && current) {
            std::array<std::uint32_t, 3> tri{};
            int k = 0;
            for (const char* key : {"v1", "v2", "v3"}) {
                const auto v = number(t->attribute(key).value_or(""));
                if (!v || *v < 0 || *v != std::floor(*v))
                    return unreadable("a triangle that isn't three corners");
                tri[static_cast<std::size_t>(k++)] = static_cast<std::uint32_t>(*v);
            }
            current->triangles.push_back(tri);
        } else if (t->name == "component" && current) {
            const auto id = t->attribute("objectid");
            const auto place = transformFrom(t->attribute("transform"));
            if (!id || !place) return unreadable("a component with no object or a bad transform");
            current->components.emplace_back(std::string(*id), *place);
        } else if (t->name == "item") {
            const auto id = t->attribute("objectid");
            const auto place = transformFrom(t->attribute("transform"));
            if (!id || !place) return unreadable("a build item with no object or a bad transform");
            build.emplace_back(std::string(*id), *place);
        }
    }
    if (tags.damaged) return unreadable("its model isn't finished XML");

    // Everything the build places, with its components, into one mesh in mm.
    Mesh mesh;
    std::string missing;
    std::size_t triangles = 0;
    std::function<bool(const std::string&, const Transform&, int)> place =
        [&](const std::string& id, const Transform& m, int depth) {
            const auto it = objects.find(id);
            if (it == objects.end()) {
                missing = id;
                return false;
            }
            if (depth > 16) return false;  // components inside themselves
            const Object& o = it->second;
            triangles += o.triangles.size();
            if (triangles > kMaxImportTriangles) return true;  // finish() says so
            const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
            for (const auto& v : o.vertices) {
                const double x = v[0] * m[0] + v[1] * m[3] + v[2] * m[6] + m[9];
                const double y = v[0] * m[1] + v[1] * m[4] + v[2] * m[7] + m[10];
                const double z = v[0] * m[2] + v[1] * m[5] + v[2] * m[8] + m[11];
                mesh.vertices.push_back({static_cast<float>(x * scale), static_cast<float>(y * scale),
                                         static_cast<float>(z * scale)});
            }
            for (const auto& tri : o.triangles) {
                for (const auto corner : tri)
                    if (corner >= o.vertices.size()) return false;
                mesh.triangles.push_back({first + tri[0], first + tri[1], first + tri[2]});
            }
            for (const auto& [child, cm] : o.components)
                if (!place(child, then(cm, m), depth + 1)) return false;
            return true;
        };
    for (const auto& [id, m] : build) {
        if (!place(id, m, 0)) {
            return unreadable(missing.empty()
                                  ? "an object with a triangle corner it doesn't have, or that holds itself"
                                  : "object " + missing + " isn't in the model (it may be in another file)");
        }
    }
    if (triangles > kMaxImportTriangles) return tooBig(triangles, "3MF");
    return finish(std::move(mesh), "3MF");
}

}  // namespace sb::kernel
