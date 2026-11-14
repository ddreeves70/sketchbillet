#include "sb/io/SketchBilletFile.h"
#include "sb/io/Steps.h"

#include "Zip.h"

#include <zip.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "sb/core/Version.h"
#include "sb/model/Commands.h"
#include "sb/model/ConstructionPlane.h"
#include "sb/model/Holes.h"
#include "sb/model/Import.h"
#include "sb/model/Mirror.h"
#include "sb/model/Pattern.h"
#include "sb/model/Shell.h"

// Format (schema 1), as described in docs/file-format.md:
//
//   manifest.json        format, schema, app version, units, document id/kind/name, file list
//   timeline.json        feature order, suppressed flags and the rollback marker
//   features/<id>.json   one file per feature: id, type, name, inputs, params
//   materials.json       the design's own materials (only if it has any)
//   folders.json         the feature tree's folders (only if it has any)
//   parameters.json      named values like wall = 3 mm (only if it has any)
//
// Lengths are millimeters. JSON is written with sorted keys and two-space indents,
// and every zip entry gets the same fixed timestamp, so saving the same design
// twice gives the same bytes (good for Git diffs and tests).

namespace sb::io {

namespace {

using model::Document;
using model::Feature;
using model::FeatureType;
using nlohmann::json;

constexpr std::uint64_t kMaxEntryBytes = 64ull * 1024 * 1024;  // refuse absurd entries (zip bombs)

// ---------------------------------------------------------------------------
// Messages (plain words for students; technical detail goes in `detail`)
// ---------------------------------------------------------------------------

Diagnostic problem(std::string code, std::string message, std::string detail = {}) {
    return {std::move(code), Severity::Error, std::move(message), std::move(detail)};
}

Diagnostic notSketchBillet(std::string_view name, std::string detail) {
    return problem(
        "file.not_sketchbillet",
        std::string(name) +
            " isn't a SketchBillet design, so SketchBillet can't open it. SketchBillet designs end in .sb.",
        std::move(detail));
}

Diagnostic damaged(std::string_view name, std::string detail) {
    return problem(
        "file.damaged",
        std::string(name) +
            " is damaged, so SketchBillet can't open it. If you have an older copy, try opening that one.",
        std::move(detail));
}

// ---------------------------------------------------------------------------
// libzip helpers
// ---------------------------------------------------------------------------

struct ZipCloser {
    void operator()(zip_t* z) const { zip_discard(z); }
};
struct SourceFreer {
    void operator()(zip_source_t* s) const { zip_source_free(s); }
};
using ZipPtr = std::unique_ptr<zip_t, ZipCloser>;
using SourcePtr = std::unique_ptr<zip_source_t, SourceFreer>;

std::string zipErrorText(zip_error_t* err) {
    std::string text = zip_error_strerror(err);
    zip_error_fini(err);
    return text;
}

// Reads one entry as text. Returns nullopt if it is missing or too large.
std::optional<std::string> readEntry(zip_t* za, const std::string& name, std::string& why) {
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat(za, name.c_str(), 0, &st) != 0) {
        why = "missing " + name;
        return std::nullopt;
    }
    if (!(st.valid & ZIP_STAT_SIZE) || st.size > kMaxEntryBytes) {
        why = name + " is too large";
        return std::nullopt;
    }
    zip_file_t* f = zip_fopen(za, name.c_str(), 0);
    if (!f) {
        why = "cannot open " + name + ": " + zip_strerror(za);
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(st.size), '\0');
    const zip_int64_t got = zip_fread(f, text.data(), st.size);
    zip_fclose(f);
    if (got < 0 || static_cast<zip_uint64_t>(got) != st.size) {
        why = "cannot read " + name;
        return std::nullopt;
    }
    return text;
}

// ---------------------------------------------------------------------------
// JSON <-> values
// ---------------------------------------------------------------------------

json vec3(const kernel::Vec3& v) {
    return json::array({v.x, v.y, v.z});
}

std::optional<kernel::Vec3> readVec3(const json& j) {
    if (!j.is_array() || j.size() != 3) return std::nullopt;
    for (const auto& c : j) {
        if (!c.is_number() || !std::isfinite(c.get<double>())) return std::nullopt;
    }
    return kernel::Vec3{j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

std::optional<double> readNumber(const json& j) {
    if (!j.is_number()) return std::nullopt;
    const double d = j.get<double>();
    return std::isfinite(d) ? std::optional<double>(d) : std::nullopt;
}

json vec2(const sketch::Point2& p) {
    return json::array({p.x, p.y});
}

std::optional<sketch::Point2> readVec2(const json& j) {
    if (!j.is_array() || j.size() != 2) return std::nullopt;
    for (const auto& c : j) {
        if (!c.is_number() || !std::isfinite(c.get<double>())) return std::nullopt;
    }
    return sketch::Point2{j[0].get<double>(), j[1].get<double>()};
}

const char* planeName(model::SketchPlane p) {
    switch (p) {
        case model::SketchPlane::XY:
            return "xy";
        case model::SketchPlane::XZ:
            return "xz";
        case model::SketchPlane::YZ:
            return "yz";
    }
    return "xy";
}

std::optional<model::SketchPlane> readPlane(const json& j) {
    if (!j.is_string()) return std::nullopt;
    const auto s = j.get<std::string>();
    if (s == "xy") return model::SketchPlane::XY;
    if (s == "xz") return model::SketchPlane::XZ;
    if (s == "yz") return model::SketchPlane::YZ;
    return std::nullopt;
}

json sketchToJson(const sketch::Sketch& s) {
    json entities = json::array();
    for (const auto& e : s.entities) {
        json j{{"id", e.id.toString()}, {"name", e.name}};
        if (e.construction) j["construction"] = true;  // left out when false
        std::visit(
            [&j](const auto& shape) {
                using S = std::decay_t<decltype(shape)>;
                if constexpr (std::is_same_v<S, sketch::PointShape>) {
                    j["type"] = "point";
                    j["at"] = vec2(shape.at);
                } else if constexpr (std::is_same_v<S, sketch::LineShape>) {
                    j["type"] = "line";
                    j["start"] = vec2(shape.start);
                    j["end"] = vec2(shape.end);
                } else if constexpr (std::is_same_v<S, sketch::CircleShape>) {
                    j["type"] = "circle";
                    j["center"] = vec2(shape.center);
                    j["radius"] = shape.radius;
                } else if constexpr (std::is_same_v<S, sketch::SplineShape>) {
                    j["type"] = "spline";
                    json poles = json::array();
                    for (const auto& p : shape.poles) poles.push_back(vec2(p));
                    j["poles"] = poles;
                    if (!shape.weights.empty()) j["weights"] = shape.weights;  // left out when all 1
                    j["degree"] = shape.degree;
                    if (shape.closed) j["closed"] = true;  // left out when false
                    if (shape.source) {                    // only for one imported from a file
                        j["source"] = {{"file", shape.source->file},
                                       {"through", shape.source->through},
                                       {"unit", std::string(unitSymbol(shape.source->unit))}};
                    }
                } else {
                    j["type"] = "arc";
                    j["center"] = vec2(shape.center);
                    j["radius"] = shape.radius;
                    j["start_angle"] = shape.startAngle;
                    j["end_angle"] = shape.endAngle;
                }
            },
            e.shape);
        entities.push_back(j);
    }
    json constraints = json::array();
    for (const auto& c : s.constraints) {
        json j{{"id", c.id.toString()}, {"name", c.name}, {"kind", std::string(sketch::kindKey(c.kind))}};
        if (!c.points.empty()) {
            json points = json::array();
            for (const auto& p : c.points) {
                json pj{{"entity", p.entity.toString()}};
                if (p.role != sketch::PointRole::Self) pj["role"] = std::string(sketch::roleKey(p.role));
                if (p.role == sketch::PointRole::Pole) pj["index"] = p.index;  // from 0
                points.push_back(pj);
            }
            j["points"] = points;
        }
        if (!c.entities.empty()) {
            json ids = json::array();
            for (const auto& id : c.entities) ids.push_back(id.toString());
            j["entities"] = ids;
        }
        if (sketch::isDimension(c.kind)) j["value"] = c.value;
        // Only dimensions with a formula get this, so other sketches save exactly
        // as before. "value" is still what it works out to, for an older SketchBillet.
        if (sketch::isDimension(c.kind) && !c.expression.empty()) j["expression"] = c.expression;
        if (c.kind == sketch::ConstraintKind::Fix) j["at"] = vec2(c.at);
        constraints.push_back(j);
    }
    return {{"entities", entities}, {"constraints", constraints}};
}

// Reads a sketch; on failure sets `why` and returns nothing.
std::optional<sketch::Sketch> sketchFromJson(const json& j, std::string& why) {
    sketch::Sketch s;
    const json entities = j.value("entities", json::array());
    const json constraints = j.value("constraints", json::array());
    if (!entities.is_array() || !constraints.is_array()) {
        why = "sketch has no entity or constraint list";
        return std::nullopt;
    }
    for (const auto& ej : entities) {
        const auto id = ej.is_object() ? EntityId::parse(ej.value("id", "")) : std::nullopt;
        if (!id || !ej.contains("name") || !ej["name"].is_string() || !ej.contains("type") ||
            !ej["type"].is_string()) {
            why = "a sketch entity is missing its id, name or type";
            return std::nullopt;
        }
        sketch::Entity e{*id, ej["name"].get<std::string>(), {}, ej.value("construction", false)};
        const auto type = ej["type"].get<std::string>();
        auto bad = [&](const char* what) {
            why = "sketch entity " + e.name + ": bad or missing " + what;
            return std::nullopt;
        };
        if (type == "point") {
            const auto at = readVec2(ej.value("at", json()));
            if (!at) return bad("at");
            e.shape = sketch::PointShape{*at};
        } else if (type == "line") {
            const auto a = readVec2(ej.value("start", json()));
            const auto b = readVec2(ej.value("end", json()));
            if (!a || !b) return bad("start or end");
            e.shape = sketch::LineShape{*a, *b};
        } else if (type == "circle" || type == "arc") {
            const auto c = readVec2(ej.value("center", json()));
            const auto r = readNumber(ej.value("radius", json()));
            if (!c || !r) return bad("center or radius");
            if (type == "circle") {
                e.shape = sketch::CircleShape{*c, *r};
            } else {
                const auto a0 = readNumber(ej.value("start_angle", json()));
                const auto a1 = readNumber(ej.value("end_angle", json()));
                if (!a0 || !a1) return bad("start_angle or end_angle");
                e.shape = sketch::ArcShape{*c, *r, *a0, *a1};
            }
        } else if (type == "spline") {
            sketch::SplineShape sp;
            const json poles = ej.value("poles", json());
            if (!poles.is_array()) return bad("poles");
            for (const auto& pj : poles) {
                const auto p = readVec2(pj);
                if (!p) return bad("poles");
                sp.poles.push_back(*p);
            }
            const json weights = ej.value("weights", json::array());
            if (!weights.is_array()) return bad("weights");
            for (const auto& wj : weights) {
                const auto w = readNumber(wj);
                if (!w) return bad("weights");
                sp.weights.push_back(*w);
            }
            const json degree = ej.value("degree", json());
            if (!degree.is_number_integer()) return bad("degree");
            sp.degree = degree.get<int>();
            sp.closed = ej.value("closed", false);
            if (ej.contains("source")) {
                const json& src = ej["source"];
                if (!src.is_object() || !src.contains("file") || !src["file"].is_string())
                    return bad("source");
                sketch::SplineSource source;
                source.file = src["file"].get<std::string>();
                source.through = src.value("through", true);
                // A unit SketchBillet doesn't know reads as mm, as the document's does.
                source.unit = parseLengthUnit(src.value("unit", "mm")).value_or(LengthUnit::Millimeter);
                sp.source = std::move(source);
            }
            e.shape = std::move(sp);
        } else {
            return bad("type");
        }
        s.entities.push_back(std::move(e));
    }
    for (const auto& cj : constraints) {
        const auto id = cj.is_object() ? ConstraintId::parse(cj.value("id", "")) : std::nullopt;
        const auto kind = cj.is_object() && cj.contains("kind") && cj["kind"].is_string()
                              ? sketch::kindFromKey(cj["kind"].get<std::string>())
                              : std::nullopt;
        if (!id || !kind || !cj.contains("name") || !cj["name"].is_string()) {
            why = "a sketch constraint is missing its id, name or kind";
            return std::nullopt;
        }
        sketch::Constraint c{*id, cj["name"].get<std::string>(), *kind, {}, {}, 0, {}};
        for (const auto& pj : cj.value("points", json::array())) {
            const auto entity = pj.is_object() ? EntityId::parse(pj.value("entity", "")) : std::nullopt;
            const auto role = pj.is_object() ? sketch::roleFromKey(pj.value("role", "")) : std::nullopt;
            const json index = pj.is_object() ? pj.value("index", json(0)) : json();
            if (!entity || !role || !index.is_number_integer()) {
                why = "sketch constraint " + c.name + ": bad point";
                return std::nullopt;
            }
            // value_or, not *role: GCC's -O3 can't see the check above and warns
            // that the optional may be uninitialized.
            const sketch::PointRole r = role.value_or(sketch::PointRole::Self);
            c.points.push_back({*entity, r, r == sketch::PointRole::Pole ? index.get<int>() : 0});
        }
        for (const auto& ij : cj.value("entities", json::array())) {
            const auto entity = ij.is_string() ? EntityId::parse(ij.get<std::string>()) : std::nullopt;
            if (!entity) {
                why = "sketch constraint " + c.name + ": bad entity id";
                return std::nullopt;
            }
            c.entities.push_back(*entity);
        }
        if (sketch::isDimension(c.kind)) {
            const auto v = readNumber(cj.value("value", json()));
            if (!v) {
                why = "sketch constraint " + c.name + ": bad or missing value";
                return std::nullopt;
            }
            c.value = *v;
            if (cj.contains("expression")) {
                if (!cj["expression"].is_string()) {
                    why = "sketch constraint " + c.name + ": bad expression";
                    return std::nullopt;
                }
                c.expression = cj["expression"].get<std::string>();
            }
        }
        if (c.kind == sketch::ConstraintKind::Fix) {
            const auto at = readVec2(cj.value("at", json()));
            if (!at) {
                why = "sketch constraint " + c.name + ": bad or missing at";
                return std::nullopt;
            }
            c.at = *at;
        }
        s.constraints.push_back(std::move(c));
    }
    // Every reference must point at something in the sketch, with the right kinds.
    if (const auto problems = sketch::check(s); !problems.empty()) {
        why = "sketch: " + problems.front().message;
        return std::nullopt;
    }
    return s;
}

const char* kindName(model::DocKind k) {
    return k == model::DocKind::Assembly ? "assembly" : "part";
}

const char* opName(kernel::BoolOp op) {
    switch (op) {
        case kernel::BoolOp::Union:
            return "union";
        case kernel::BoolOp::Cut:
            return "cut";
        case kernel::BoolOp::Intersect:
            return "intersect";
    }
    return "union";
}

// An Extrude's side, as saved: "one", "flip" or "both".
const char* sideName(model::ExtrudeSide side) {
    switch (side) {
        case model::ExtrudeSide::One:
            return "one";
        case model::ExtrudeSide::Flip:
            return "flip";
        case model::ExtrudeSide::Both:
            return "both";
    }
    return "one";
}

std::optional<model::ExtrudeSide> readSide(const json& j) {
    if (!j.is_string()) return std::nullopt;
    const auto s = j.get<std::string>();
    if (s == "one") return model::ExtrudeSide::One;
    if (s == "flip") return model::ExtrudeSide::Flip;
    if (s == "both") return model::ExtrudeSide::Both;
    return std::nullopt;
}

std::optional<kernel::BoolOp> readOp(const json& j) {
    if (!j.is_string()) return std::nullopt;
    const auto s = j.get<std::string>();
    if (s == "union") return kernel::BoolOp::Union;
    if (s == "cut") return kernel::BoolOp::Cut;
    if (s == "intersect") return kernel::BoolOp::Intersect;
    return std::nullopt;
}

// What an element of a body lies on, as saved: "plane", "line", ...
const char* geomName(kernel::GeomKind g) {
    switch (g) {
        case kernel::GeomKind::Plane:
            return "plane";
        case kernel::GeomKind::Cylinder:
            return "cylinder";
        case kernel::GeomKind::Cone:
            return "cone";
        case kernel::GeomKind::Sphere:
            return "sphere";
        case kernel::GeomKind::Torus:
            return "torus";
        case kernel::GeomKind::Line:
            return "line";
        case kernel::GeomKind::Circle:
            return "circle";
        case kernel::GeomKind::Ellipse:
            return "ellipse";
        case kernel::GeomKind::Spline:
            return "spline";
        case kernel::GeomKind::Point:
            return "point";
        case kernel::GeomKind::Other:
            return "other";
    }
    return "other";
}

kernel::GeomKind readGeom(const std::string& s) {
    for (const auto g :
         {kernel::GeomKind::Plane, kernel::GeomKind::Cylinder, kernel::GeomKind::Cone,
          kernel::GeomKind::Sphere, kernel::GeomKind::Torus, kernel::GeomKind::Line, kernel::GeomKind::Circle,
          kernel::GeomKind::Ellipse, kernel::GeomKind::Spline, kernel::GeomKind::Point}) {
        if (s == geomName(g)) return g;
    }
    return kernel::GeomKind::Other;  // a kind from a newer SketchBillet only weakens matching
}

const char* topoName(kernel::TopoKind k) {
    switch (k) {
        case kernel::TopoKind::Face:
            return "face";
        case kernel::TopoKind::Edge:
            return "edge";
        case kernel::TopoKind::Vertex:
            return "vertex";
    }
    return "edge";
}

// A reference to a face, edge or vertex: its name, and what it looked like.
json refToJson(const model::GeometryRef& r) {
    return {{"kind", topoName(r.kind)},
            {"name", r.name},
            {"geom", geomName(r.signature.geom)},
            {"center", vec3(r.signature.center)},
            {"direction", vec3(r.signature.direction)},
            {"size", r.signature.size},
            {"radius", r.signature.radius}};
}

std::optional<model::GeometryRef> refFromJson(const json& j) {
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) return std::nullopt;
    model::GeometryRef r;
    const std::string kind = j.value("kind", "");
    if (kind == "face") {
        r.kind = kernel::TopoKind::Face;
    } else if (kind == "edge") {
        r.kind = kernel::TopoKind::Edge;
    } else if (kind == "vertex") {
        r.kind = kernel::TopoKind::Vertex;
    } else {
        return std::nullopt;
    }
    r.name = j["name"].get<std::string>();
    r.signature.geom = readGeom(j.value("geom", ""));
    r.signature.center = readVec3(j.value("center", json())).value_or(kernel::Vec3{});
    r.signature.direction = readVec3(j.value("direction", json())).value_or(kernel::Vec3{});
    r.signature.size = readNumber(j.value("size", json(0))).value_or(0);
    r.signature.radius = readNumber(j.value("radius", json(0))).value_or(0);
    return r;
}

// Bytes as base64 text (RFC 4648), and back: for binary files on the clipboard.
std::string toBase64(std::string_view bytes) {
    static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::size_t n = std::min<std::size_t>(3, bytes.size() - i);
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 3; ++k)
            v = v << 8 | (k < n ? static_cast<unsigned char>(bytes[i + k]) : 0u);
        for (std::size_t k = 0; k < 4; ++k) out += k <= n ? kDigits[(v >> (18 - 6 * k)) & 63] : '=';
    }
    return out;
}
std::optional<std::string> fromBase64(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        std::uint32_t v = 0;
        int pad = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=' && i + 4 == text.size() && k >= 2) {
                ++pad;
                v <<= 6;
                continue;
            }
            const int d = digit(c);
            if (d < 0 || pad) return std::nullopt;
            v = v << 6 | static_cast<std::uint32_t>(d);
        }
        for (int k = 0; k < 3 - pad; ++k) out += static_cast<char>((v >> (16 - 8 * k)) & 0xff);
    }
    return out;
}

// Where an Import step's file is saved in the design: imports/<step id>.step.
std::string importPath(const model::ImportFeature& f) {
    return "imports/" + f.id().toString() + "." + std::string(model::formatName(f.format()));
}

json featureToJson(const Feature& f) {
    json j{{"id", f.id().toString()}, {"name", f.name()}};
    if (!f.material().empty()) j["material"] = f.material();  // left out when not picked
    switch (f.type()) {
        case FeatureType::Box: {
            const auto& b = static_cast<const model::BoxFeature&>(f);
            j["type"] = "Box";
            j["params"] = {{"corner", vec3(b.corner())}, {"size", vec3(b.size())}};
            break;
        }
        case FeatureType::Cylinder: {
            const auto& c = static_cast<const model::CylinderFeature&>(f);
            j["type"] = "Cylinder";
            j["params"] = {{"base", vec3(c.base())},
                           {"axis", vec3(c.axis())},
                           {"radius", c.radius()},
                           {"height", c.height()}};
            break;
        }
        case FeatureType::Boolean: {
            const auto& b = static_cast<const model::BooleanFeature&>(f);
            j["type"] = "Boolean";
            j["inputs"] = {{"a", b.a().toString()}, {"b", b.b().toString()}};
            j["params"] = {{"operation", opName(b.op())}};
            break;
        }
        case FeatureType::Sketch: {
            const auto& sk = static_cast<const model::SketchFeature&>(f);
            j["type"] = "Sketch";
            j["params"] = sketchToJson(sk.sketch());
            j["params"]["plane"] = planeName(sk.plane());
            if (sk.face()) {
                // A sketch on a face: the body, and the face by name (see "Edge references").
                j["inputs"] = {{"body", sk.face()->body.toString()}};
                j["params"]["face"] = refToJson(sk.face()->face);
            }
            if (!sk.onPlane().isNil()) j["inputs"] = {{"on", sk.onPlane().toString()}};  // a Plane step
            break;
        }
        case FeatureType::Plane: {
            const auto& p = static_cast<const model::PlaneFeature&>(f);
            j["type"] = "Plane";
            const auto& from = p.from();
            json params = {{"offset", p.offset()}, {"angle", p.angle()}};
            switch (from.kind) {
                case model::PlaneFrom::Kind::Base:
                    params["from"] = planeName(from.base);
                    break;
                case model::PlaneFrom::Kind::Face:
                    params["from"] = "face";
                    params["face"] = refToJson(from.face.face);
                    j["inputs"] = {{"body", from.face.body.toString()}};
                    break;
                case model::PlaneFrom::Kind::Plane:
                    params["from"] = "plane";
                    j["inputs"] = {{"plane", from.plane.toString()}};
                    break;
            }
            const auto& h = p.hinge();
            params["hinge"] = h.kind == model::PlaneHinge::Kind::X   ? json("x")
                              : h.kind == model::PlaneHinge::Kind::Y ? json("y")
                                                                     : json({{"edge", refToJson(h.edge)}});
            j["params"] = params;
            break;
        }
        case FeatureType::Move: {
            const auto& m = static_cast<const model::MoveFeature&>(f);
            j["type"] = "Move";
            j["inputs"] = {{"body", m.body().toString()}};
            j["params"] = {{"offset", vec3(m.motion().offset)},
                           {"center", vec3(m.motion().center)},
                           {"axis", vec3(m.motion().axis)},
                           {"angle", m.motion().angle}};
            // A linked copy keeps its body (an older SketchBillet moves the body instead).
            if (m.copy()) j["params"]["copy"] = true;
            break;
        }
        case FeatureType::Extrude: {
            const auto& e = static_cast<const model::ExtrudeFeature&>(f);
            j["type"] = "Extrude";
            j["inputs"] = {{"sketch", e.sketch().toString()}};
            j["params"] = {{"distance", e.distance()}, {"side", sideName(e.side())}};
            if (e.operation() != model::ExtrudeOp::NewBody) {
                // Joined onto or cut from a body (an older SketchBillet makes a new body).
                j["params"]["operation"] = e.operation() == model::ExtrudeOp::Join ? "join" : "cut";
                j["inputs"]["body"] = e.body().toString();
            }
            break;
        }
        case FeatureType::Import: {
            // The file itself is saved next to this one (see importPath()); the
            // clipboard carries it in "content" instead (see stepsToText()).
            const auto& im = static_cast<const model::ImportFeature&>(f);
            j["type"] = "Import";
            j["params"] = {{"format", std::string(model::formatName(im.format()))},
                           {"source", im.source()},
                           {"data", importPath(im)}};
            // An STL file's unit, when it isn't mm (STL files don't say).
            if (im.format() == model::ImportFormat::Stl && im.unit() != LengthUnit::Millimeter)
                j["params"]["unit"] = std::string(unitSymbol(im.unit()));
            break;
        }
        case FeatureType::Shell: {
            const auto& sh = static_cast<const model::ShellFeature&>(f);
            j["type"] = "Shell";
            j["inputs"] = {{"body", sh.body().toString()}};
            json faces = json::array();
            for (const auto& r : sh.faces()) faces.push_back(refToJson(r));
            j["params"] = {{"thickness", sh.thickness()}, {"faces", faces}};
            break;
        }
        case FeatureType::Mirror: {
            const auto& m = static_cast<const model::MirrorFeature&>(f);
            j["type"] = "Mirror";
            json steps = json::array();
            for (const auto& s : m.steps()) steps.push_back(s.toString());
            j["inputs"] = {{"body", m.body().toString()}, {"steps", steps}};
            const auto& p = m.plane();
            if (p.kind == model::MirrorPlane::Kind::Face) {
                j["params"] = {{"plane", "face"}, {"face", refToJson(p.face)}};
            } else {
                j["params"] = {{"plane", p.kind == model::MirrorPlane::Kind::YZ   ? "yz"
                                         : p.kind == model::MirrorPlane::Kind::XZ ? "xz"
                                                                                  : "xy"},
                               {"offset", p.offset}};
            }
            break;
        }
        case FeatureType::Pattern: {
            const auto& p = static_cast<const model::PatternFeature&>(f);
            const auto& l = p.layout();
            j["type"] = "Pattern";
            json steps = json::array();
            for (const auto& s : p.steps()) steps.push_back(s.toString());
            j["inputs"] = {{"body", p.body().toString()}, {"steps", steps}};
            if (l.kind == model::PatternLayout::Kind::Linear) {
                j["params"] = {
                    {"kind", "linear"},      {"count", l.count},   {"direction", vec3(l.direction)},
                    {"spacing", l.spacing},  {"count2", l.count2}, {"direction2", vec3(l.direction2)},
                    {"spacing2", l.spacing2}};
            } else {
                j["params"] = {{"kind", "circular"},
                               {"count", l.count},
                               {"center", vec3(l.center)},
                               {"axis", vec3(l.axis)},
                               {"angle", l.angle}};
            }
            break;
        }
        case FeatureType::Hole: {
            const auto& h = static_cast<const model::HoleFeature&>(f);
            j["type"] = "Hole";
            j["inputs"] = {{"body", h.body().toString()}};
            const auto& at = h.placement();
            json params = {{"diameter", h.diameter()},
                           {"kind", h.kind() == model::HoleKind::Counterbore   ? "counterbore"
                                    : h.kind() == model::HoleKind::Countersink ? "countersink"
                                                                               : "simple"},
                           {"through", h.through()},
                           {"depth", h.depth()},
                           {"counterbore_diameter", h.counterboreDiameter()},
                           {"counterbore_depth", h.counterboreDepth()},
                           {"countersink_diameter", h.countersinkDiameter()},
                           {"countersink_angle", h.countersinkAngle()},
                           {"flip", h.flipped()}};
            if (!h.size().empty()) {
                params["size"] = h.size();
                params["fit"] = h.fit() == model::HoleFit::Tap ? "tap" : "clearance";
            }
            if (at.kind == model::HolePlacement::Kind::SketchPoints) {
                params["placement"] = "points";
                j["inputs"]["sketch"] = at.sketch.toString();
            } else {
                params["placement"] = "edges";
                params["edge_a"] = refToJson(at.edgeA);
                params["edge_b"] = refToJson(at.edgeB);
                params["distance_a"] = at.distanceA;
                params["distance_b"] = at.distanceB;
            }
            j["params"] = params;
            break;
        }
        case FeatureType::Revolve: {
            const auto& r = static_cast<const model::RevolveFeature&>(f);
            j["type"] = "Revolve";
            j["inputs"] = {{"sketch", r.sketch().toString()}};
            json axis = r.axis().kind == model::RevolveAxis::Kind::X ? json("x")
                        : r.axis().kind == model::RevolveAxis::Kind::Y
                            ? json("y")
                            : json({{"line", r.axis().line.toString()}});
            j["params"] = {{"angle", r.angle()}, {"axis", axis}, {"side", sideName(r.side())}};
            if (r.operation() != model::ExtrudeOp::NewBody) {
                j["params"]["operation"] = r.operation() == model::ExtrudeOp::Join ? "join" : "cut";
                j["inputs"]["body"] = r.body().toString();
            }
            break;
        }
        case FeatureType::Fillet:
        case FeatureType::Chamfer: {
            const auto& e = static_cast<const model::EdgeFeature&>(f);
            const bool fillet = f.type() == FeatureType::Fillet;
            j["type"] = fillet ? "Fillet" : "Chamfer";
            j["inputs"] = {{"body", e.body().toString()}};
            json edges = json::array();
            for (const auto& r : e.edges()) edges.push_back(refToJson(r));
            j["params"] = {{fillet ? "radius" : "distance", e.size()}, {"edges", edges}};
            break;
        }
    }
    if (!f.formulas().empty()) {
        // Only steps with numbers worked out from formulas get this, so other
        // steps save exactly as before. "params" still has the numbers.
        json formulas = json::object();
        for (const auto& [key, text] : f.formulas()) formulas[key] = text;
        j["formulas"] = formulas;
    }
    return j;
}

std::unique_ptr<Feature> stepFromJson(const json& j, const FeatureId& id, std::string& why,
                                      std::string& unknownType);

// Builds a feature from its JSON, with the formulas its numbers come from. On
// failure sets `why` (technical) and, for an unknown type, `unknownType`.
std::unique_ptr<Feature> featureFromJson(const json& j, const FeatureId& id, std::string& why,
                                         std::string& unknownType) {
    auto f = stepFromJson(j, id, why, unknownType);
    if (!f || !j.contains("formulas")) return f;
    const auto& formulas = j["formulas"];
    if (!formulas.is_object()) {
        why = "step " + f->name() + ": formulas isn't a list of names and formulas";
        return nullptr;
    }
    for (const auto& [key, text] : formulas.items()) {
        // A key this SketchBillet doesn't know (from a newer SketchBillet) is kept, so saving doesn't lose
        // it.
        if (!text.is_string()) {
            why = "step " + f->name() + ": the formula for " + key + " isn't text";
            return nullptr;
        }
        f->setFormula(key, text.get<std::string>());
    }
    return f;
}

// Builds a feature from its JSON, without its formulas (see featureFromJson()).
std::unique_ptr<Feature> stepFromJson(const json& j, const FeatureId& id, std::string& why,
                                      std::string& unknownType) {
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string() || !j.contains("name") ||
        !j["name"].is_string()) {
        why = "feature " + id.toString() + " has no type or name";
        return nullptr;
    }
    const auto type = j["type"].get<std::string>();
    const auto name = j["name"].get<std::string>();
    const json params = j.value("params", json::object());
    const json inputs = j.value("inputs", json::object());
    auto bad = [&](const char* what) {
        why = "step " + name + ": bad or missing " + what;
        return nullptr;
    };

    if (type == "Box") {
        const auto corner = readVec3(params.value("corner", json()));
        const auto size = readVec3(params.value("size", json()));
        if (!corner) return bad("corner");
        if (!size) return bad("size");
        return std::make_unique<model::BoxFeature>(id, name, *corner, *size);
    }
    if (type == "Cylinder") {
        const auto base = readVec3(params.value("base", json()));
        const auto axis = readVec3(params.value("axis", json()));
        const auto radius = readNumber(params.value("radius", json()));
        const auto height = readNumber(params.value("height", json()));
        if (!base) return bad("base");
        if (!axis) return bad("axis");
        if (!radius) return bad("radius");
        if (!height) return bad("height");
        return std::make_unique<model::CylinderFeature>(id, name, *base, *axis, *radius, *height);
    }
    if (type == "Boolean") {
        const auto op = readOp(params.value("operation", json()));
        const auto a = inputs.contains("a") && inputs["a"].is_string()
                           ? FeatureId::parse(inputs["a"].get<std::string>())
                           : std::nullopt;
        const auto b = inputs.contains("b") && inputs["b"].is_string()
                           ? FeatureId::parse(inputs["b"].get<std::string>())
                           : std::nullopt;
        if (!op) return bad("operation");
        if (!a) return bad("input a");
        if (!b) return bad("input b");
        return std::make_unique<model::BooleanFeature>(id, name, *op, *a, *b);
    }
    if (type == "Sketch") {
        const auto plane = readPlane(params.value("plane", json()));
        if (!plane) return bad("plane");
        auto s = sketchFromJson(params, why);
        if (!s) {
            why = "step " + name + ": " + why;
            return nullptr;
        }
        auto step = std::make_unique<model::SketchFeature>(id, name, *plane, std::move(*s));
        if (params.contains("face")) {
            const auto body = inputs.contains("body") && inputs["body"].is_string()
                                  ? FeatureId::parse(inputs["body"].get<std::string>())
                                  : std::nullopt;
            const auto face = refFromJson(params["face"]);
            if (!body) return bad("input body");
            if (!face) return bad("face");
            step->setFace(model::SketchFace{*body, *face});
        }
        if (inputs.contains("on")) {
            const auto on =
                inputs["on"].is_string() ? FeatureId::parse(inputs["on"].get<std::string>()) : std::nullopt;
            if (!on) return bad("input on");
            step->setOnPlane(*on);
        }
        return step;
    }
    if (type == "Plane") {
        model::PlaneFrom from;
        const json f = params.value("from", json());
        if (f == "face") {
            const auto body = inputs.contains("body") && inputs["body"].is_string()
                                  ? FeatureId::parse(inputs["body"].get<std::string>())
                                  : std::nullopt;
            const auto face = refFromJson(params.value("face", json()));
            if (!body) return bad("input body");
            if (!face) return bad("face");
            from.kind = model::PlaneFrom::Kind::Face;
            from.face = {*body, *face};
        } else if (f == "plane") {
            const auto plane = inputs.contains("plane") && inputs["plane"].is_string()
                                   ? FeatureId::parse(inputs["plane"].get<std::string>())
                                   : std::nullopt;
            if (!plane) return bad("input plane");
            from.kind = model::PlaneFrom::Kind::Plane;
            from.plane = *plane;
        } else {
            const auto base = readPlane(f);
            if (!base) return bad("from");
            from.base = *base;
        }
        const auto offset = readNumber(params.value("offset", json()));
        const auto angle = readNumber(params.value("angle", json()));
        if (!offset) return bad("offset");
        if (!angle) return bad("angle");
        model::PlaneHinge hinge;
        const json h = params.value("hinge", json("x"));
        if (h == "y") {
            hinge.kind = model::PlaneHinge::Kind::Y;
        } else if (h.is_object() && h.contains("edge")) {
            const auto edge = refFromJson(h["edge"]);
            if (!edge) return bad("hinge");
            hinge = {model::PlaneHinge::Kind::Edge, *edge};
        } else if (h != "x") {
            return bad("hinge");
        }
        auto step = std::make_unique<model::PlaneFeature>(id, name, from);
        step->setOffset(*offset);
        step->setAngle(*angle);
        step->setHinge(hinge);
        return step;
    }
    if (type == "Move") {
        const auto body = inputs.contains("body") && inputs["body"].is_string()
                              ? FeatureId::parse(inputs["body"].get<std::string>())
                              : std::nullopt;
        const auto offset = readVec3(params.value("offset", json()));
        const auto center = readVec3(params.value("center", json()));
        const auto axis = readVec3(params.value("axis", json()));
        const auto angle = readNumber(params.value("angle", json()));
        if (!body) return bad("input body");
        if (!offset) return bad("offset");
        if (!center) return bad("center");
        if (!axis) return bad("axis");
        if (!angle) return bad("angle");
        auto move = std::make_unique<model::MoveFeature>(id, name, *body,
                                                         kernel::Motion{*offset, *center, *axis, *angle});
        if (params.contains("copy")) {
            if (!params["copy"].is_boolean()) return bad("copy");
            move->setCopy(params["copy"].get<bool>());
        }
        return move;
    }
    if (type == "Extrude") {
        const auto sketch = inputs.contains("sketch") && inputs["sketch"].is_string()
                                ? FeatureId::parse(inputs["sketch"].get<std::string>())
                                : std::nullopt;
        const auto distance = readNumber(params.value("distance", json()));
        const auto side = readSide(params.value("side", json()));
        if (!sketch) return bad("input sketch");
        if (!distance) return bad("distance");
        if (!side) return bad("side");
        auto step = std::make_unique<model::ExtrudeFeature>(id, name, *sketch, *distance, *side);
        const std::string op = params.value("operation", std::string("new"));
        if (op == "join" || op == "cut") {
            const auto body = inputs.contains("body") && inputs["body"].is_string()
                                  ? FeatureId::parse(inputs["body"].get<std::string>())
                                  : std::nullopt;
            if (!body) return bad("input body");
            step->setOperation(op == "join" ? model::ExtrudeOp::Join : model::ExtrudeOp::Cut, *body);
        } else if (op != "new") {
            return bad("operation");
        }
        return step;
    }
    if (type == "Import") {
        const std::string format = params.value("format", "");
        const auto kind = format == "step"  ? std::optional(model::ImportFormat::Step)
                          : format == "stl" ? std::optional(model::ImportFormat::Stl)
                          : format == "3mf" ? std::optional(model::ImportFormat::ThreeMf)
                                            : std::nullopt;
        if (!kind) return bad("format");
        const json source = params.value("source", json(""));
        if (!source.is_string()) return bad("source");
        // The contents come from the file named in "data" (read by readBytes()),
        // or, on the clipboard, from "content" (text) or "content_base64" (STL and 3MF files).
        std::shared_ptr<const std::string> content;
        if (params.contains("content")) {
            if (!params["content"].is_string()) return bad("content");
            content = std::make_shared<const std::string>(params["content"].get<std::string>());
        } else if (params.contains("content_base64")) {
            const auto bytes = params["content_base64"].is_string()
                                   ? fromBase64(params["content_base64"].get<std::string>())
                                   : std::nullopt;
            if (!bytes) return bad("content_base64");
            content = std::make_shared<const std::string>(*bytes);
        }
        auto step = std::make_unique<model::ImportFeature>(id, name, *kind, std::move(content),
                                                           source.get<std::string>());
        if (params.contains("unit")) {
            const auto unit = params["unit"].is_string() ? parseLengthUnit(params["unit"].get<std::string>())
                                                         : std::nullopt;
            if (!unit) return bad("unit");
            step->setUnit(*unit);
        }
        return step;
    }
    if (type == "Shell") {
        const auto body = inputs.contains("body") && inputs["body"].is_string()
                              ? FeatureId::parse(inputs["body"].get<std::string>())
                              : std::nullopt;
        if (!body) return bad("input body");
        const auto thickness = readNumber(params.value("thickness", json()));
        if (!thickness) return bad("thickness");
        const json list = params.value("faces", json());
        if (!list.is_array()) return bad("faces");
        std::vector<model::GeometryRef> faces;
        for (const auto& r : list) {
            const auto ref = refFromJson(r);
            if (!ref) return bad("faces");
            faces.push_back(*ref);
        }
        return std::make_unique<model::ShellFeature>(id, name, *body, std::move(faces), *thickness);
    }
    if (type == "Mirror") {
        const auto body = inputs.contains("body") && inputs["body"].is_string()
                              ? FeatureId::parse(inputs["body"].get<std::string>())
                              : std::nullopt;
        if (!body) return bad("input body");
        std::vector<FeatureId> steps;
        const json list = inputs.value("steps", json::array());
        if (!list.is_array()) return bad("input steps");
        for (const auto& s : list) {
            const auto step = s.is_string() ? FeatureId::parse(s.get<std::string>()) : std::nullopt;
            if (!step) return bad("input steps");
            steps.push_back(*step);
        }
        model::MirrorPlane plane;
        const std::string kind = params.value("plane", std::string());
        if (kind == "face") {
            const auto face = refFromJson(params.value("face", json()));
            if (!face) return bad("face");
            plane.kind = model::MirrorPlane::Kind::Face;
            plane.face = *face;
        } else if (kind == "yz" || kind == "xz" || kind == "xy") {
            const auto offset = readNumber(params.value("offset", json()));
            if (!offset) return bad("offset");
            plane.kind = kind == "yz"   ? model::MirrorPlane::Kind::YZ
                         : kind == "xz" ? model::MirrorPlane::Kind::XZ
                                        : model::MirrorPlane::Kind::XY;
            plane.offset = *offset;
        } else {
            return bad("plane");
        }
        return std::make_unique<model::MirrorFeature>(id, name, *body, std::move(steps), plane);
    }
    if (type == "Pattern") {
        const auto body = inputs.contains("body") && inputs["body"].is_string()
                              ? FeatureId::parse(inputs["body"].get<std::string>())
                              : std::nullopt;
        if (!body) return bad("input body");
        std::vector<FeatureId> steps;
        const json list = inputs.value("steps", json::array());
        if (!list.is_array()) return bad("input steps");
        for (const auto& s : list) {
            const auto step = s.is_string() ? FeatureId::parse(s.get<std::string>()) : std::nullopt;
            if (!step) return bad("input steps");
            steps.push_back(*step);
        }
        model::PatternLayout l;
        const std::string kind = params.value("kind", std::string());
        const json count = params.value("count", json());
        if (!count.is_number_integer()) return bad("count");
        l.count = count.get<int>();
        if (kind == "linear") {
            const auto direction = readVec3(params.value("direction", json()));
            const auto spacing = readNumber(params.value("spacing", json()));
            const auto direction2 = readVec3(params.value("direction2", json()));
            const auto spacing2 = readNumber(params.value("spacing2", json()));
            const json count2 = params.value("count2", json());
            if (!direction) return bad("direction");
            if (!spacing) return bad("spacing");
            if (!direction2) return bad("direction2");
            if (!spacing2) return bad("spacing2");
            if (!count2.is_number_integer()) return bad("count2");
            l.direction = *direction;
            l.spacing = *spacing;
            l.direction2 = *direction2;
            l.spacing2 = *spacing2;
            l.count2 = count2.get<int>();
        } else if (kind == "circular") {
            l.kind = model::PatternLayout::Kind::Circular;
            const auto center = readVec3(params.value("center", json()));
            const auto axis = readVec3(params.value("axis", json()));
            const auto angle = readNumber(params.value("angle", json()));
            if (!center) return bad("center");
            if (!axis) return bad("axis");
            if (!angle) return bad("angle");
            l.center = *center;
            l.axis = *axis;
            l.angle = *angle;
        } else {
            return bad("kind");
        }
        return std::make_unique<model::PatternFeature>(id, name, *body, std::move(steps), l);
    }
    if (type == "Hole") {
        const auto body = inputs.contains("body") && inputs["body"].is_string()
                              ? FeatureId::parse(inputs["body"].get<std::string>())
                              : std::nullopt;
        if (!body) return bad("input body");
        model::HolePlacement at;
        const std::string placement = params.value("placement", std::string());
        if (placement == "points") {
            const auto sketch = inputs.contains("sketch") && inputs["sketch"].is_string()
                                    ? FeatureId::parse(inputs["sketch"].get<std::string>())
                                    : std::nullopt;
            if (!sketch) return bad("input sketch");
            at.sketch = *sketch;
        } else if (placement == "edges") {
            at.kind = model::HolePlacement::Kind::FromEdges;
            const auto a = refFromJson(params.value("edge_a", json()));
            const auto b = refFromJson(params.value("edge_b", json()));
            const auto da = readNumber(params.value("distance_a", json()));
            const auto db = readNumber(params.value("distance_b", json()));
            if (!a) return bad("edge_a");
            if (!b) return bad("edge_b");
            if (!da) return bad("distance_a");
            if (!db) return bad("distance_b");
            at.edgeA = *a;
            at.edgeB = *b;
            at.distanceA = *da;
            at.distanceB = *db;
        } else {
            return bad("placement");
        }
        std::optional<double> nums[6];
        const char* keys[6] = {
            "diameter",         "depth", "counterbore_diameter", "counterbore_depth", "countersink_diameter",
            "countersink_angle"};
        for (int k = 0; k < 6; ++k) {
            nums[k] = readNumber(params.value(keys[k], json()));
            if (!nums[k]) return bad(keys[k]);
        }
        const json through = params.value("through", json());
        const json flip = params.value("flip", json(false));
        if (!through.is_boolean()) return bad("through");
        if (!flip.is_boolean()) return bad("flip");
        const std::string kind = params.value("kind", std::string());
        if (kind != "simple" && kind != "counterbore" && kind != "countersink") return bad("kind");
        auto step = std::make_unique<model::HoleFeature>(id, name, *body, at, *nums[0]);
        step->setKind(kind == "counterbore"   ? model::HoleKind::Counterbore
                      : kind == "countersink" ? model::HoleKind::Countersink
                                              : model::HoleKind::Simple);
        step->setDepth(through.get<bool>(), *nums[1]);
        step->setCounterbore(*nums[2], *nums[3]);
        step->setCountersink(*nums[4], *nums[5]);
        // The screw size is only a name: the saved numbers are what it drills.
        if (params.contains("size") && params["size"].is_string()) {
            step->setSizeName(params["size"].get<std::string>(),
                              params.value("fit", std::string("clearance")) == "tap"
                                  ? model::HoleFit::Tap
                                  : model::HoleFit::Clearance);
        }
        step->setFlipped(flip.get<bool>());
        return step;
    }
    if (type == "Revolve") {
        const auto sketch = inputs.contains("sketch") && inputs["sketch"].is_string()
                                ? FeatureId::parse(inputs["sketch"].get<std::string>())
                                : std::nullopt;
        const auto angle = readNumber(params.value("angle", json()));
        const auto side = readSide(params.value("side", json()));
        if (!sketch) return bad("input sketch");
        if (!angle) return bad("angle");
        if (!side) return bad("side");
        // "x", "y", or {"line": <entity id>}.
        const json a = params.value("axis", json());
        model::RevolveAxis axis;
        if (a == "x") {
            axis.kind = model::RevolveAxis::Kind::X;
        } else if (a == "y") {
            axis.kind = model::RevolveAxis::Kind::Y;
        } else if (a.is_object() && a.contains("line") && a["line"].is_string()) {
            const auto line = EntityId::parse(a["line"].get<std::string>());
            if (!line) return bad("axis");
            axis = {model::RevolveAxis::Kind::Line, *line};
        } else {
            return bad("axis");
        }
        auto step = std::make_unique<model::RevolveFeature>(id, name, *sketch, axis, *angle);
        step->setSide(*side);
        const std::string op = params.value("operation", std::string("new"));
        if (op == "join" || op == "cut") {
            const auto body = inputs.contains("body") && inputs["body"].is_string()
                                  ? FeatureId::parse(inputs["body"].get<std::string>())
                                  : std::nullopt;
            if (!body) return bad("input body");
            step->setOperation(op == "join" ? model::ExtrudeOp::Join : model::ExtrudeOp::Cut, *body);
        } else if (op != "new") {
            return bad("operation");
        }
        return step;
    }
    if (type == "Fillet" || type == "Chamfer") {
        const bool fillet = type == "Fillet";
        const auto body = inputs.contains("body") && inputs["body"].is_string()
                              ? FeatureId::parse(inputs["body"].get<std::string>())
                              : std::nullopt;
        const auto size = readNumber(params.value(fillet ? "radius" : "distance", json()));
        if (!body) return bad("input body");
        if (!size) return bad(fillet ? "radius" : "distance");
        const json edgeList = params.value("edges", json());
        if (!edgeList.is_array()) return bad("edges");
        std::vector<model::GeometryRef> edges;
        for (const auto& e : edgeList) {
            auto r = refFromJson(e);
            if (!r) return bad("edges");
            edges.push_back(std::move(*r));
        }
        if (fillet) return std::make_unique<model::FilletFeature>(id, name, *body, std::move(edges), *size);
        return std::make_unique<model::ChamferFeature>(id, name, *body, std::move(edges), *size);
    }
    unknownType = type;
    why = "step " + name + " has unknown type " + type;
    return nullptr;
}

json materialToJson(const model::Material& m) {
    return {{"id", m.key},
            {"name", m.name},
            {"density", m.density},
            {"color", model::colorText(m.color)},
            {"youngs_modulus", m.youngsModulus},
            {"poissons_ratio", m.poissonsRatio},
            {"yield_strength", m.yieldStrength},
            {"cost_per_kg", m.costPerKg}};
}

std::optional<model::Material> materialFromJson(const json& j) {
    if (!j.is_object()) return std::nullopt;
    model::Material m;
    const auto key = MaterialId::parse(j.value("id", ""));
    const auto color = model::parseColor(j.value("color", ""));
    const auto density = readNumber(j.value("density", json()));
    if (!key || !color || !density || !j.contains("name") || !j["name"].is_string()) return std::nullopt;
    m.key = key->toString();
    m.name = j["name"].get<std::string>();
    m.density = *density;
    m.color = *color;
    m.youngsModulus = readNumber(j.value("youngs_modulus", json(0))).value_or(0);
    m.poissonsRatio = readNumber(j.value("poissons_ratio", json(0))).value_or(0);
    m.yieldStrength = readNumber(j.value("yield_strength", json(0))).value_or(0);
    m.costPerKg = readNumber(j.value("cost_per_kg", json(0))).value_or(0);
    return m;
}

std::string featurePath(const FeatureId& id) {
    return "features/" + id.toString() + ".json";
}

std::string dump(const json& j) {
    return j.dump(2) + "\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

Diagnostics writeBytes(const Document& doc, Bytes& out) {
    // Build every entry's text first.
    std::deque<std::pair<std::string, std::string>> entries;  // deque: stable addresses for libzip
    json timeline = json::array();
    json files = json::array({"timeline.json"});
    if (!doc.customMaterials().empty()) {
        // Only designs with materials of their own get this file, so other designs
        // save exactly as before.
        json materials = json::array();
        for (const auto& m : doc.customMaterials()) materials.push_back(materialToJson(m));
        entries.emplace_back("materials.json", dump(json{{"materials", materials}}));
        files.push_back("materials.json");
    }
    if (!doc.parameters().empty()) {
        // Only designs with parameters get this file, so other designs save exactly as before.
        json list = json::array();
        for (const auto& p : doc.parameters().parameters()) {
            json pj{{"id", p.id.toString()}, {"name", p.name}, {"expression", p.expression}};
            if (!p.note.empty()) pj["note"] = p.note;
            list.push_back(std::move(pj));
        }
        entries.emplace_back("parameters.json", dump(json{{"parameters", list}}));
        files.push_back("parameters.json");
    }
    for (std::size_t i = 0; i < doc.featureCount(); ++i) {
        const Feature& f = doc.featureAt(i);
        timeline.push_back({{"id", f.id().toString()}, {"suppressed", f.suppressed()}});
        entries.emplace_back(featurePath(f.id()), dump(featureToJson(f)));
        files.push_back(featurePath(f.id()));
        if (f.type() == FeatureType::Import) {
            // The imported file, as it was: only designs with imports have these.
            const auto& im = static_cast<const model::ImportFeature&>(f);
            entries.emplace_back(importPath(im), im.content());
            files.push_back(importPath(im));
        }
    }
    if (const auto folders = doc.folders(); !folders.empty()) {
        // Only designs with folders get this file, so other designs save exactly as before.
        json list = json::array();
        for (const auto& folder : folders) {
            json steps = json::array();
            for (const auto& id : folder.steps) steps.push_back(id.toString());
            list.push_back({{"id", folder.id.toString()}, {"name", folder.name}, {"steps", steps}});
        }
        entries.emplace_back("folders.json", dump(json{{"folders", list}}));
        files.push_back("folders.json");
    }
    json document{{"id", doc.id().toString()}, {"kind", kindName(doc.kind())}, {"name", doc.name()}};
    if (doc.lengthUnit() != LengthUnit::Millimeter) {
        // The unit the design shows lengths in. Only designs that changed it get
        // this, so other designs save exactly as before. (The numbers in the file
        // are still in mm: see "units".)
        document["lengthUnit"] = std::string(unitSymbol(doc.lengthUnit()));
    }
    const json manifest{
        {"format", "sketchbillet"},      {"schema", kFileSchemaVersion},
        {"app", std::string(version())}, {"units", {{"length", "mm"}, {"angle", "rad"}}},
        {"document", document},          {"files", files},
    };
    json timelineJson{{"features", timeline}};
    // Only a rolled-back design says where the marker is, so others save exactly as before.
    if (const auto marker = doc.marker())
        timelineJson["rollback"] = marker->isNil() ? "start" : marker->toString();
    entries.emplace_front("timeline.json", dump(timelineJson));
    entries.emplace_front("manifest.json", dump(manifest));

    return zip::pack(entries, out,
                     problem("file.cant_write", "SketchBillet couldn't put the design into a file."));
}

Diagnostics saveFile(Document& doc, const std::filesystem::path& path) {
    auto issues = writeFile(doc, path);
    if (issues.empty()) doc.markSaved();
    return issues;
}

Diagnostics writeFile(const Document& doc, const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    auto cantWrite = [&](std::string detail) {
        return Diagnostics{problem("file.cant_write",
                                   "SketchBillet couldn't save " + name +
                                       ". Check that the folder exists, that you're allowed to save there, "
                                       "and that the disk isn't full.",
                                   std::move(detail))};
    };

    Bytes bytes;
    if (auto issues = writeBytes(doc, bytes); !issues.empty()) return issues;

    // Written next to the target, then moved into place, so a failed save never
    // leaves a half-written design behind.
    return zip::writeAtomically(bytes, path, cantWrite("").front());
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

OpenResult readBytes(const Bytes& data, std::string_view displayName) {
    OpenResult result;
    auto fail = [&](Diagnostic d) {
        result.issues.push_back(std::move(d));
        return std::move(result);
    };

    zip_error_t err;
    zip_error_init(&err);
    zip_source_t* raw = zip_source_buffer_create(data.data(), data.size(), 0, &err);
    if (!raw) return fail(notSketchBillet(displayName, zipErrorText(&err)));
    ZipPtr za(zip_open_from_source(raw, ZIP_RDONLY, &err));
    if (!za) {
        zip_source_free(raw);
        return fail(notSketchBillet(displayName, zipErrorText(&err)));
    }

    std::string why;
    auto readJson = [&](const std::string& entry) -> std::optional<json> {
        auto text = readEntry(za.get(), entry, why);
        if (!text) return std::nullopt;
        json j = json::parse(*text, nullptr, false);
        if (j.is_discarded()) {
            why = entry + " is not valid JSON";
            return std::nullopt;
        }
        return std::optional<json>(std::in_place,
                                   std::move(j));  // not "return j": ambiguous with nlohmann 3.12
    };

    // Manifest: is this a SketchBillet file, and can this version read it?
    const auto manifest = readJson("manifest.json");
    if (!manifest) {
        return fail(why.rfind("missing", 0) == 0 ? notSketchBillet(displayName, why)
                                                 : damaged(displayName, why));
    }
    const std::string format = manifest->is_object() ? manifest->value("format", "") : std::string();
    if (format != "sketchbillet") {
        return fail(notSketchBillet(displayName, "manifest format is not \"sketchbillet\""));
    }
    const auto schema = manifest->value("schema", -1);
    if (schema > kFileSchemaVersion) {
        return fail(
            problem("file.too_new",
                    std::string(displayName) +
                        " was saved by a newer version of SketchBillet. Update SketchBillet to open it.",
                    "file schema " + std::to_string(schema) + ", this SketchBillet reads up to schema " +
                        std::to_string(kFileSchemaVersion)));
    }
    if (schema < 1) return fail(damaged(displayName, "bad schema number"));

    const json docInfo = manifest->value("document", json::object());
    const auto docId = DocId::parse(docInfo.value("id", ""));
    if (!docId) return fail(damaged(displayName, "manifest has no document id"));
    const auto kind =
        docInfo.value("kind", "part") == "assembly" ? model::DocKind::Assembly : model::DocKind::Part;
    auto doc = std::make_unique<Document>(*docId, kind, docInfo.value("name", std::string(displayName)));
    if (docInfo.contains("lengthUnit")) {
        // A unit this SketchBillet doesn't know (from a newer SketchBillet) shows lengths in mm:
        // it changes nothing's size, so the design still opens.
        const auto& u = docInfo["lengthUnit"];
        if (const auto unit = u.is_string() ? parseLengthUnit(u.get<std::string>()) : std::nullopt) {
            doc->execute(std::make_unique<model::SetUnitsCommand>(*unit));
        }
    }

    // Parameters, before the steps that use them, as they were saved: one with a
    // problem stays, so the student can see it and fix it.
    // (A design without parameters has no parameters.json.)
    if (const auto parameters = readJson("parameters.json"); parameters) {
        const json list = parameters->value("parameters", json());
        if (!list.is_array()) return fail(damaged(displayName, "parameters.json has no parameter list"));
        std::vector<model::Parameter> read;
        for (const auto& pj : list) {
            const auto id = pj.is_object() ? ParamId::parse(pj.value("id", "")) : std::nullopt;
            if (!id || !pj.contains("name") || !pj["name"].is_string() || !pj.contains("expression") ||
                !pj["expression"].is_string()) {
                return fail(damaged(displayName,
                                    "a parameter in parameters.json is missing its id, name or expression"));
            }
            model::Parameter p{*id, pj["name"].get<std::string>(), pj["expression"].get<std::string>(), {}};
            if (pj.contains("note") && pj["note"].is_string()) p.note = pj["note"].get<std::string>();
            read.push_back(std::move(p));
        }
        if (auto issues = doc->execute(std::make_unique<model::ReplaceParametersCommand>(std::move(read)));
            !issues.empty()) {
            return fail(damaged(displayName, "parameters: " + issues.front().message));
        }
    } else if (why.rfind("missing", 0) != 0) {
        return fail(damaged(displayName, why));  // there, but not readable
    }

    // The design's own materials, before the steps that use them.
    // (A design without materials of its own has no materials.json.)
    if (const auto materials = readJson("materials.json"); materials) {
        const json list = materials->value("materials", json());
        if (!list.is_array()) return fail(damaged(displayName, "materials.json has no material list"));
        for (const auto& mj : list) {
            auto m = materialFromJson(mj);
            if (!m)
                return fail(damaged(displayName,
                                    "a material in materials.json is missing its id, name, "
                                    "density or color"));
            const auto name = m->name;
            if (auto issues = doc->execute(std::make_unique<model::DefineMaterialCommand>(std::move(*m)));
                !issues.empty()) {
                return fail(damaged(displayName, "material " + name + ": " + issues.front().code));
            }
        }
    } else if (why.rfind("missing", 0) != 0) {
        return fail(damaged(displayName, why));  // there, but not readable
    }

    // Timeline: the features in order.
    const auto timeline = readJson("timeline.json");
    if (!timeline) return fail(damaged(displayName, why));
    const json order = timeline->value("features", json());
    if (!order.is_array()) return fail(damaged(displayName, "timeline has no feature list"));

    for (const auto& entry : order) {
        const auto id = entry.is_object() ? FeatureId::parse(entry.value("id", "")) : std::nullopt;
        if (!id) return fail(damaged(displayName, "timeline entry without a valid id"));
        const auto fj = readJson(featurePath(*id));
        if (!fj) return fail(damaged(displayName, why));

        std::string unknownType;
        auto feature = featureFromJson(*fj, *id, why, unknownType);
        if (!feature) {
            if (!unknownType.empty()) {
                return fail(problem("file.unknown_step",
                                    std::string(displayName) + " has a step, " + fj->value("name", "?") +
                                        ", of a kind this version of SketchBillet doesn't know (" +
                                        unknownType + "). It may need a newer SketchBillet or a plugin.",
                                    why));
            }
            return fail(damaged(displayName, why));
        }
        if (feature->type() == FeatureType::Import) {
            // Its file, saved next to it.
            auto& im = static_cast<model::ImportFeature&>(*feature);
            const json where = fj->value("params", json::object()).value("data", json());
            const auto text =
                where.is_string() ? readEntry(za.get(), where.get<std::string>(), why) : std::nullopt;
            if (!text) return fail(damaged(displayName, "step " + im.name() + ": " + why));
            im.setContent(im.format(), std::make_shared<const std::string>(*text), im.source());
        }
        feature->setSuppressed(entry.value("suppressed", false));
        if (fj->contains("material") && (*fj)["material"].is_string()) {
            // A key this SketchBillet doesn't know (a material from a newer SketchBillet) is kept,
            // and the body counts as the default material until it's changed.
            feature->setMaterial((*fj)["material"].get<std::string>());
        }

        // Add through the normal command, so the same checks run as when a
        // student builds the design (unique names, inputs come first).
        const auto name = feature->name();
        auto issues = doc->execute(std::make_unique<model::AddFeatureCommand>(std::move(feature)));
        if (!issues.empty()) {
            return fail(damaged(displayName, "step " + name + ": " + issues.front().code));
        }
    }

    // The rollback marker, after the steps, through the same command a student uses.
    // (A design that isn't rolled back has none.)
    if (timeline->contains("rollback")) {
        const json& marker = (*timeline)["rollback"];
        const std::string text = marker.is_string() ? marker.get<std::string>() : std::string();
        std::optional<std::size_t> index;
        if (text == "start") {
            index = 0;
        } else if (const auto id = FeatureId::parse(text); id && doc->indexOf(*id)) {
            index = *doc->indexOf(*id) + 1;
        }
        if (!index) return fail(damaged(displayName, "the rollback marker is after a step that isn't there"));
        if (*index < doc->featureCount()) doc->execute(std::make_unique<model::RollbackCommand>(*index));
    }

    // Folders, after the steps they hold, through the same command a student uses.
    // (A design without folders has no folders.json.)
    if (const auto folders = readJson("folders.json"); folders) {
        const json list = folders->value("folders", json());
        if (!list.is_array()) return fail(damaged(displayName, "folders.json has no folder list"));
        for (const auto& fj : list) {
            const auto id = fj.is_object() ? FolderId::parse(fj.value("id", "")) : std::nullopt;
            const json steps = fj.is_object() ? fj.value("steps", json()) : json();
            if (!id || !fj.contains("name") || !fj["name"].is_string() || !steps.is_array()) {
                return fail(
                    damaged(displayName, "a folder in folders.json is missing its id, name or steps"));
            }
            std::vector<FeatureId> ids;
            for (const auto& s : steps) {
                const auto step = s.is_string() ? FeatureId::parse(s.get<std::string>()) : std::nullopt;
                if (!step) return fail(damaged(displayName, "a folder lists a step without a valid id"));
                ids.push_back(*step);
            }
            const std::string name = fj["name"].get<std::string>();
            if (auto issues =
                    doc->execute(std::make_unique<model::AddFolderCommand>(std::move(ids), name, *id));
                !issues.empty()) {
                return fail(damaged(displayName, "folder " + name + ": " + issues.front().code));
            }
        }
    } else if (why.rfind("missing", 0) != 0) {
        return fail(damaged(displayName, why));  // there, but not readable
    }

    doc->clearHistory();  // opening a file is not an undoable step
    doc->markSaved();
    result.document = std::move(doc);
    return result;
}

OpenResult openFile(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        OpenResult r;
        r.issues.push_back(
            problem("file.cant_read",
                    "SketchBillet couldn't open " + name +
                        ". Check that the file is still there and that you're allowed to open it.",
                    path.string()));
        return r;
    }
    Bytes data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return readBytes(data, name);
}

// ---------------------------------------------------------------------------
// Steps as text (the clipboard)
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kStepsFormat = "sketchbillet-steps";
}  // namespace

std::string stepsToText(const Document& doc, const std::vector<FeatureId>& ids) {
    // copySteps() finds every step they use, in design order; it gives them new
    // ids, which is harmless here (stepsFromText() gives new ones again).
    const auto steps = model::copySteps(doc, ids);
    if (steps.empty()) return {};
    json list = json::array();
    std::vector<std::string> used;
    for (const auto& f : steps) {
        json j = featureToJson(*f);
        if (f->suppressed()) j["suppressed"] = true;
        if (f->type() == FeatureType::Import) {
            // The clipboard is one piece of text: the imported file goes in it.
            const auto& im = static_cast<const model::ImportFeature&>(*f);
            j["params"].erase("data");
            if (im.format() == model::ImportFormat::Step)
                j["params"]["content"] = im.content();  // text
            else
                j["params"]["content_base64"] = toBase64(im.content());  // STL and 3MF files can be binary
        }
        list.push_back(j);
        if (!f->material().empty()) used.push_back(f->material());
    }
    json materials = json::array();
    for (const auto& m : doc.customMaterials()) {
        if (std::find(used.begin(), used.end(), m.key) != used.end()) materials.push_back(materialToJson(m));
    }
    return dump(json{{"format", kStepsFormat}, {"version", 1}, {"steps", list}, {"materials", materials}});
}

bool looksLikeSteps(std::string_view text) {
    return text.find(std::string("\"") + kStepsFormat + "\"") != std::string_view::npos &&
           text.find_first_not_of(" \t\r\n") != std::string_view::npos &&
           text[text.find_first_not_of(" \t\r\n")] == '{';
}

StepsResult stepsFromText(std::string_view text) {
    StepsResult result;
    auto fail = [&](std::string code, std::string message, std::string detail = {}) {
        result.steps.clear();
        result.materials.clear();
        result.issues.push_back(problem(std::move(code), std::move(message), std::move(detail)));
        return std::move(result);
    };
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("format", "") != kStepsFormat) {
        return fail("paste.not_steps",
                    "What's on the clipboard isn't steps copied from SketchBillet. Copy steps in "
                    "the feature tree first (Edit > Copy), then paste.");
    }
    if (j.value("version", 0) > 1) {
        return fail("paste.too_new",
                    "These steps were copied from a newer SketchBillet. Update SketchBillet to paste them.");
    }
    const json steps = j.value("steps", json());
    if (!steps.is_array() || steps.empty()) return fail("paste.not_steps", "There are no steps to paste.");

    for (const auto& mj : j.value("materials", json::array())) {
        if (auto m = materialFromJson(mj)) result.materials.push_back(*m);
    }
    std::vector<std::unique_ptr<Feature>> read;
    std::vector<FeatureId> seen;
    for (const auto& fj : steps) {
        const auto id = fj.is_object() && fj.contains("id") && fj["id"].is_string()
                            ? FeatureId::parse(fj["id"].get<std::string>())
                            : std::nullopt;
        if (!id)
            return fail("paste.damaged", "The copied steps are damaged. Copy them again.",
                        "a step has no id");
        std::string why, unknownType;
        auto f = featureFromJson(fj, *id, why, unknownType);
        if (!f) {
            if (!unknownType.empty()) {
                return fail("paste.unknown_step",
                            "One of the copied steps is a kind this SketchBillet doesn't know (" +
                                unknownType + "). It may need a newer SketchBillet or a plugin.",
                            why);
            }
            return fail("paste.damaged", "The copied steps are damaged. Copy them again.", why);
        }
        // Every input must be an earlier copied step: pasting never reaches into the design.
        for (const auto& dep : f->dependencies()) {
            if (std::find(seen.begin(), seen.end(), dep) == seen.end()) {
                return fail("paste.damaged", "The copied steps are damaged. Copy them again.",
                            "step " + f->name() + " uses a step that wasn't copied");
            }
        }
        f->setSuppressed(fj.value("suppressed", false));
        if (fj.contains("material") && fj["material"].is_string())
            f->setMaterial(fj["material"].get<std::string>());
        seen.push_back(*id);
        read.push_back(std::move(f));
    }
    result.steps = model::freshIds(std::move(read));
    return result;
}

}  // namespace sb::io
