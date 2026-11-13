#include "sb/model/PrintCheck.h"

#include <cmath>

#include "sb/model/Document.h"

namespace sb::model {

bool PrintCheckResult::ready() const {
    for (const auto& i : issues)
        if (i.severity != Severity::Info) return false;
    return true;
}

namespace {

std::string pointText(const kernel::Vec3& p, LengthUnit unit) {
    // "x 12, y 3.5, z 4 mm": one unit at the end.
    auto n = [&](double v) {
        std::string t = lengthText(v, unit);
        return t.substr(0, t.rfind(' '));
    };
    return "x " + n(p.x) + ", y " + n(p.y) + ", z " + n(p.z) + " " + std::string(unitSymbol(unit));
}

}  // namespace

PrintCheckResult checkPrint(const kernel::Mesh& mesh, const std::string& name, const PrintCheckOptions& options,
                            LengthUnit unit) {
    PrintCheckResult out;
    const kernel::Mesh skin = kernel::weld(mesh);
    if (skin.triangles.empty()) {
        out.issues.push_back({"print.nothing", Severity::Error,
                              name + " has no surface to print. Pick a step that makes a solid shape.", {}});
        return out;
    }
    out.edges = kernel::checkEdges(skin);
    if (!out.edges.closed()) {
        const int gaps = out.edges.open + out.edges.crowded;
        out.issues.push_back(
            {"print.open", Severity::Error,
             name + " has " + std::to_string(gaps) + (gaps == 1 ? " place" : " places") +
                 " where its surface isn't closed, so a slicer can't tell what's inside it. This happens when "
                 "a step makes a surface instead of a solid, or two bodies only touch along an edge. Check the "
                 "steps that made it, and join touching bodies so they overlap by at least 0.1 mm.",
             {}});
        return out;  // thickness needs a closed skin
    }

    const auto thick = kernel::wallThickness(skin);
    for (std::size_t i = 0; i < thick.size(); ++i) {
        const auto& t = skin.triangles[i];
        const auto& a = skin.vertices[t[0]];
        const auto& b = skin.vertices[t[1]];
        const auto& c = skin.vertices[t[2]];
        if (thick[i] < out.thinnest) {
            out.thinnest = thick[i];
            out.thinnestAt = {(a[0] + b[0] + c[0]) / 3.0, (a[1] + b[1] + c[1]) / 3.0, (a[2] + b[2] + c[2]) / 3.0};
        }
        if (thick[i] < options.minWall) {
            const double u[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
            const double v[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
            const double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
            out.thinArea += std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) / 2;
        }
    }

    const std::string least = lengthText(options.minWall, unit);
    if (out.thinnest < options.minWall) {
        out.issues.push_back({"print.thin", Severity::Warning,
                              name + " has walls thinner than " + least + ": the thinnest is " +
                                  lengthText(out.thinnest, unit) + " thick, near " +
                                  pointText(out.thinnestAt, unit) +
                                  ". Most printers can't print walls that thin well, so they may come out "
                                  "weak, with holes, or not at all. Make them at least " +
                                  least + " thick.",
                              {}});
        return out;
    }
    out.issues.push_back(
        {"print.ready", Severity::Info,
         name + " is ready to print: its surface is closed all round, and its thinnest wall is " +
             (std::isinf(out.thinnest) ? std::string("too thick to measure") : lengthText(out.thinnest, unit)) +
             " (" + least + " or more prints well).",
         {}});
    return out;
}

PrintCheckResult checkPrint(const Document& doc, const RebuildReport& report, const kernel::IKernel& kernel,
                            const FeatureId& step, const PrintCheckOptions& options) {
    const auto* feature = doc.find(step);
    const auto* shape = report.shapeOf(step);
    const std::string name = feature ? feature->name() : "That step";
    if (!feature || !shape) {
        PrintCheckResult out;
        out.issues.push_back({"print.nothing", Severity::Error,
                              name + " didn't build, so there's nothing to check. Fix it first.", {}});
        return out;
    }
    auto invalid = kernel.validate(*shape);
    if (!invalid.empty()) {
        PrintCheckResult out;
        out.issues.push_back({"print.invalid", Severity::Error,
                              name + "'s shape has problems, so it may not print as it looks: " +
                                  invalid.front().message,
                              {}});
        return out;
    }
    return checkPrint(kernel.tessellate(*shape, kernel::MeshQuality{}), name, options, doc.lengthUnit());
}

}  // namespace sb::model
