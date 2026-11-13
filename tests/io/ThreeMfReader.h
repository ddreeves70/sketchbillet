// Reads back what the 3MF writer made, for tests: each object's name, material
// and color, and its welded mesh. Just enough XML reading for our own files.

#pragma once

#include <zip.h>

#include <array>
#include <cstdint>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "sb/io/SketchBilletFile.h"

namespace sb::test {

struct ThreeMfPart {
    std::string name, material, color;
    std::vector<std::array<double, 3>> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;

    // Edges not shared by exactly one other triangle, going the other way. 0 for a closed, outward-facing skin.
    int badEdges() const {
        std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
        for (const auto& t : triangles)
            for (int i = 0; i < 3; ++i) ++edges[{t[i], t[(i + 1) % 3]}];
        int bad = 0;
        for (const auto& [e, n] : edges) {
            auto back = edges.find({e.second, e.first});
            if (n != 1 || back == edges.end() || back->second != 1) ++bad;
        }
        return bad;
    }
    // The volume inside (mm^3), from the triangles.
    double volume() const {
        double v = 0;
        for (const auto& t : triangles) {
            const auto &a = vertices[t[0]], &b = vertices[t[1]], &c = vertices[t[2]];
            v += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
                 a[2] * (b[0] * c[1] - b[1] * c[0]);
        }
        return v / 6;
    }
};

inline std::string entry(const io::Bytes& bytes, const char* name) {
    zip_error_t err;
    zip_error_init(&err);
    zip_source_t* src = zip_source_buffer_create(bytes.data(), bytes.size(), 0, &err);
    zip_t* za = src ? zip_open_from_source(src, ZIP_RDONLY, &err) : nullptr;
    std::string text;
    if (zip_file_t* f = za ? zip_fopen(za, name, 0) : nullptr) {
        char buf[4096];
        zip_int64_t n;
        while ((n = zip_fread(f, buf, sizeof buf)) > 0) text.append(buf, static_cast<std::size_t>(n));
        zip_fclose(f);
    }
    if (za) zip_close(za);
    else if (src) zip_source_free(src);
    zip_error_fini(&err);
    return text;
}

inline std::vector<ThreeMfPart> readParts(const io::Bytes& bytes) {
    const std::string model = entry(bytes, "3D/3dmodel.model");
    std::vector<std::pair<std::string, std::string>> materials;
    const std::regex base(R"re(<base name="([^"]*)" displaycolor="([^"]*)"/>)re");
    for (std::sregex_iterator it(model.begin(), model.end(), base), end; it != end; ++it)
        materials.emplace_back((*it)[1], (*it)[2]);
    std::vector<ThreeMfPart> parts;
    const std::regex object(R"re(<object id="\d+" type="model" name="([^"]*)" pid="1" pindex="(\d+)">([\s\S]*?)</object>)re");
    const std::regex vertex(R"re(<vertex x="([^"]+)" y="([^"]+)" z="([^"]+)"/>)re");
    const std::regex triangle(R"re(<triangle v1="(\d+)" v2="(\d+)" v3="(\d+)"/>)re");
    for (std::sregex_iterator it(model.begin(), model.end(), object), end; it != end; ++it) {
        ThreeMfPart p;
        p.name = (*it)[1];
        const auto& m = materials.at(std::stoul((*it)[2]));
        p.material = m.first;
        p.color = m.second;
        const std::string body = (*it)[3];
        for (std::sregex_iterator v(body.begin(), body.end(), vertex); v != end; ++v)
            p.vertices.push_back({std::stod((*v)[1]), std::stod((*v)[2]), std::stod((*v)[3])});
        for (std::sregex_iterator t(body.begin(), body.end(), triangle); t != end; ++t)
            p.triangles.push_back({static_cast<std::uint32_t>(std::stoul((*t)[1])),
                                   static_cast<std::uint32_t>(std::stoul((*t)[2])),
                                   static_cast<std::uint32_t>(std::stoul((*t)[3]))});
        parts.push_back(std::move(p));
    }
    return parts;
}

}  // namespace sb::test
