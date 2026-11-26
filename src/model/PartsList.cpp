#include "sb/model/PartsList.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "sb/model/Document.h"
#include "sb/model/Material.h"

namespace sb::model {

std::vector<PartsListRow> partsList(const Document& assembly, const RebuildReport& report,
                                    const kernel::IKernel& kernel) {
    std::vector<PartsListRow> out;
    if (assembly.kind() != DocKind::Assembly) return out;
    for (const auto* part : assembly.parts()) {
        PartsListRow row;
        row.item = static_cast<int>(out.size()) + 1;
        row.part = part->id();
        row.name = part->name();
        row.count = static_cast<int>(std::count_if(assembly.components().begin(), assembly.components().end(),
                                                   [&](const Component& c) { return c.part == part->id(); }));
        if (const auto* link = assembly.partLink(part->id())) {
            row.linked = true;
            row.file = link->path.substr(link->path.find_last_of('/') + 1);  // its name
        }
        if (const auto* built = report.part(part->id())) {
            row.built = built->ok();
            std::vector<std::string> materials;
            for (const auto& body : built->bodies()) {
                const auto* r = built->result(body);
                if (!r || !r->shape.valid()) continue;
                const auto& material = part->materialOf(body);
                const auto mass = bodyMass(kernel, r->shape, material);
                ++row.bodies;
                row.volumeEach += mass.volume;
                row.gramsEach += mass.grams;
                row.costEach += mass.cost;
                // Each material once, in the order the bodies are.
                if (std::find(materials.begin(), materials.end(), material.name) == materials.end()) {
                    materials.push_back(material.name);
                    row.material += (row.material.empty() ? "" : ", ") + material.name;
                }
            }
        }
        row.gramsAll = row.gramsEach * row.count;
        out.push_back(std::move(row));
    }
    return out;
}

namespace {

// One CSV field: quoted when it has a comma, a quote or a line break.
std::string field(const std::string& text) {
    if (text.find_first_of(",\"\n") == std::string::npos) return text;
    std::string out = "\"";
    for (char c : text) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

std::string number(double v, int decimals) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

}  // namespace

std::string partsListCsv(const std::vector<PartsListRow>& rows, LengthUnit unit) {
    const double perMm = toUnit(1.0, unit);
    const double cube = perMm * perMm * perMm;
    std::string out = "Item,Part,Count,Material,Mass each (g),Mass in all (g),Volume each (" +
                      std::string(unitSymbol(unit)) + "^3),Kept,File\n";
    for (const auto& r : rows) {
        out += std::to_string(r.item) + "," + field(r.name) + "," + std::to_string(r.count) + "," +
               field(r.material) + "," + number(r.gramsEach, 2) + "," + number(r.gramsAll, 2) + "," +
               number(r.volumeEach * cube, unit == LengthUnit::Millimeter ? 2 : 4) + "," +
               (r.linked ? "linked" : "inside") + "," + field(r.file) + "\n";
    }
    return out;
}

}  // namespace sb::model
