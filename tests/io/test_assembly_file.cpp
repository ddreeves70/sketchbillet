// Tests for assembly files (schema 2): what's in them, saving and opening, and
// what a damaged one says.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <zip.h>

#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "../model/FakeKernel.h"
#include "sb/core/Version.h"
#include "sb/io/SketchBilletFile.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"

using namespace sb;
using Catch::Approx;

namespace {

std::unique_ptr<model::Document> boxPart(std::string name) {
    auto part = std::make_unique<model::Document>(model::DocKind::Part, std::move(name));
    REQUIRE(part->execute(std::make_unique<model::AddFeatureCommand>(std::make_unique<model::BoxFeature>(
                              "Box1", kernel::Vec3{}, kernel::Vec3{20, 10, 5})))
                .empty());
    return part;
}

// Car: a Body and four Wheels, one of them moved and turned, one let go.
std::unique_ptr<model::Document> car() {
    auto doc = std::make_unique<model::Document>(model::DocKind::Part, "Car");
    REQUIRE(doc->execute(std::make_unique<model::MakeAssemblyCommand>()).empty());
    REQUIRE(doc->execute(std::make_unique<model::InsertPartCommand>(boxPart("Body"), 1)).empty());
    kernel::Placement at;
    at.origin = {0, -15, 0};
    REQUIRE(doc->execute(std::make_unique<model::InsertPartCommand>(boxPart("Wheel"), 4, at,
                                                                    kernel::Vec3{25, 0, 0}))
                .empty());
    const auto* w2 = doc->findComponent("Wheel 2");
    REQUIRE(doc->execute(std::make_unique<model::PlaceComponentCommand>(
                             w2->id, kernel::Placement{{40, -32, 12}, {1, 0, 0}, {0, -1, 0}}))
                .empty());
    return doc;
}

std::map<std::string, std::string> readZip(const io::Bytes& bytes) {
    std::map<std::string, std::string> out;
    zip_error_t err;
    zip_error_init(&err);
    zip_source_t* src = zip_source_buffer_create(bytes.data(), bytes.size(), 0, &err);
    zip_t* za = zip_open_from_source(src, ZIP_RDONLY, &err);
    if (!za) {
        zip_source_free(src);
        return out;
    }
    for (zip_int64_t i = 0; i < zip_get_num_entries(za, 0); ++i) {
        zip_stat_t st;
        zip_stat_index(za, static_cast<zip_uint64_t>(i), 0, &st);
        std::string text(static_cast<std::size_t>(st.size), '\0');
        zip_file_t* f = zip_fopen_index(za, static_cast<zip_uint64_t>(i), 0);
        zip_fread(f, text.data(), st.size);
        zip_fclose(f);
        out[st.name] = text;
    }
    zip_close(za);
    return out;
}

io::Bytes makeZip(const std::map<std::string, std::string>& entries) {
    zip_error_t err;
    zip_error_init(&err);
    zip_source_t* src = zip_source_buffer_create(nullptr, 0, 0, &err);
    zip_t* za = zip_open_from_source(src, ZIP_TRUNCATE, &err);
    zip_source_keep(src);
    for (const auto& [name, text] : entries) {
        zip_file_add(za, name.c_str(), zip_source_buffer(za, text.data(), text.size(), 0), ZIP_FL_OVERWRITE);
    }
    zip_close(za);
    zip_source_open(src);
    zip_source_seek(src, 0, SEEK_END);
    const auto size = zip_source_tell(src);
    zip_source_seek(src, 0, SEEK_SET);
    io::Bytes out(static_cast<std::size_t>(size));
    zip_source_read(src, out.data(), out.size());
    zip_source_close(src);
    zip_source_free(src);
    return out;
}

}  // namespace

TEST_CASE("an assembly is saved as schema 2, its parts inside as parts' files", "[io][assembly]") {
    const auto doc = car();
    io::Bytes bytes;
    REQUIRE(io::writeBytes(*doc, bytes).empty());
    const auto entries = readZip(bytes);
    const auto& manifest = entries.at("manifest.json");
    CHECK(manifest.find(R"("schema": 2)") != std::string::npos);
    CHECK(manifest.find(R"("kind": "assembly")") != std::string::npos);
    CHECK(entries.count("components.json"));
    CHECK_FALSE(entries.count("timeline.json"));  // an assembly has no steps of its own
    for (const auto* part : doc->parts()) {
        const std::string folder = "parts/" + part->id().toString() + "/";
        REQUIRE(entries.count(folder + "manifest.json"));
        // Each part is written as schema 1, the same as its own .sb file would be.
        CHECK(entries.at(folder + "manifest.json").find(R"("schema": 1)") != std::string::npos);
        CHECK(entries.at(folder + "manifest.json").find(R"("kind": "part")") != std::string::npos);
        CHECK(entries.count(folder + "timeline.json"));
        CHECK(manifest.find(folder + "manifest.json") != std::string::npos);
    }
    const auto& components = entries.at("components.json");
    CHECK(components.find(R"("name": "Wheel 2")") != std::string::npos);
    CHECK(components.find(R"("grounded": true)") != std::string::npos);
}

TEST_CASE("an assembly opens as it was saved, and saves the same bytes again", "[io][assembly]") {
    const auto doc = car();
    io::Bytes bytes;
    REQUIRE(io::writeBytes(*doc, bytes).empty());
    auto opened = io::readBytes(bytes, "car.sb");
    REQUIRE(opened.ok());
    const auto& back = *opened.document;
    CHECK(back.kind() == model::DocKind::Assembly);
    CHECK(back.id() == doc->id());
    CHECK(back.name() == "Car");
    REQUIRE(back.components().size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        const auto& a = doc->components()[i];
        const auto& b = back.components()[i];
        CHECK(a.id == b.id);
        CHECK(a.name == b.name);
        CHECK(a.part == b.part);
        CHECK(a.grounded == b.grounded);
        CHECK(kernel::samePlacement(a.placement, b.placement));
    }
    REQUIRE(back.parts().size() == 2);
    CHECK(back.parts()[0]->name() == "Body");
    CHECK(back.parts()[1]->name() == "Wheel");
    CHECK(back.parts()[1]->featureCount() == 1);
    CHECK(back.findComponent("Wheel 2")->placement.zAxis.y == Approx(-1));
    CHECK_FALSE(back.canUndo());  // opening isn't a step to undo
    CHECK_FALSE(back.isModified());
    io::Bytes again;
    REQUIRE(io::writeBytes(back, again).empty());
    CHECK(again == bytes);
}

TEST_CASE("a part is still saved as schema 1", "[io][assembly]") {
    const auto part = boxPart("Tag");
    io::Bytes bytes;
    REQUIRE(io::writeBytes(*part, bytes).empty());
    CHECK(readZip(bytes).at("manifest.json").find(R"("schema": 1)") != std::string::npos);
    CHECK(kPartSchemaVersion == 1);
    CHECK(kFileSchemaVersion == 2);
}

TEST_CASE("a damaged assembly says what's wrong", "[io][assembly]") {
    const auto doc = car();
    io::Bytes bytes;
    REQUIRE(io::writeBytes(*doc, bytes).empty());
    const auto good = readZip(bytes);
    auto broken = [&](auto change) {
        auto entries = good;
        change(entries);
        return io::readBytes(makeZip(entries), "car.sb");
    };
    // No components.json.
    auto r = broken([](auto& e) { e.erase("components.json"); });
    REQUIRE_FALSE(r.ok());
    CHECK(r.issues[0].code == "file.damaged");
    // A part's folder gone: it says which.
    const std::string bodyFolder = "parts/" + doc->parts()[0]->id().toString() + "/";
    r = broken([&](auto& e) {
        std::erase_if(e, [&](const auto& kv) { return kv.first.rfind(bodyFolder, 0) == 0; });
    });
    REQUIRE_FALSE(r.ok());
    CHECK(r.issues[0].code == "file.damaged");
    CHECK(r.issues[0].detail.find("the part in " + bodyFolder + " is missing") != std::string::npos);
    // A component whose part isn't one of the assembly's.
    r = broken([&](auto& e) {
        auto& text = e["components.json"];
        const std::string id = doc->parts()[0]->id().toString();
        text.replace(text.find(id), id.size(), DocId::generate().toString());
    });
    REQUIRE_FALSE(r.ok());
    CHECK(r.issues[0].code == "file.damaged");
    CHECK(r.issues[0].detail.find("component Body 1") != std::string::npos);
    // A component with no placement.
    r = broken([](auto& e) {
        auto& text = e["components.json"];
        text.replace(text.find(R"("placement")"), 11, R"("place_x")");
    });
    REQUIRE_FALSE(r.ok());
    CHECK(r.issues[0].detail.find("missing its id, name, part or placement") != std::string::npos);
    // An assembly inside an assembly's part folder.
    r = broken([&](auto& e) {
        auto& text = e[bodyFolder + "manifest.json"];
        text.replace(text.find(R"("kind": "part")"), 14, R"("kind": "assembly")");
    });
    REQUIRE_FALSE(r.ok());
    CHECK(r.issues[0].detail.find("a part in it is an assembly") != std::string::npos);
}

// A schema-2 file checked into the repo. Every future version of SketchBillet must still
// open it (through a migration if the schema changes). Never regenerate it.
TEST_CASE("the checked-in schema 2 car still opens", "[io][assembly][compat]") {
    auto r = io::openFile(std::filesystem::path(SB_TEST_FILES_DIR) / "schema2-car.sb");
    REQUIRE(r.ok());
    const auto& doc = *r.document;
    CHECK(doc.kind() == model::DocKind::Assembly);
    CHECK(doc.id().toString() == "e096625e-fa78-49d1-8f6c-d422facb835a");
    CHECK(doc.name() == "Car");
    REQUIRE(doc.parts().size() == 2);
    CHECK(doc.parts()[0]->name() == "Body");
    CHECK(doc.parts()[1]->name() == "Wheel");
    REQUIRE(doc.components().size() == 3);

    const auto* body = doc.findComponent("Body 1");
    const auto* wheel1 = doc.findComponent("Wheel 1");
    const auto* wheel2 = doc.findComponent("Wheel 2");
    REQUIRE(body);
    REQUIRE(wheel1);
    REQUIRE(wheel2);
    CHECK(body->grounded);
    CHECK(wheel1->grounded);
    CHECK_FALSE(wheel2->grounded);
    CHECK(body->part == doc.parts()[0]->id());
    CHECK(wheel2->part == doc.parts()[1]->id());
    CHECK(wheel1->placement.origin.x == Approx(10));
    CHECK(wheel1->placement.origin.y == Approx(-6));
    CHECK(wheel2->placement.origin.z == Approx(12));
    CHECK(wheel2->placement.zAxis.y == Approx(-1));

    test::FakeKernel kernel;
    const auto report = doc.regenerate(kernel);
    CHECK(report.ok());
}
