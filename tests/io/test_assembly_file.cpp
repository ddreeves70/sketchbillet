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

TEST_CASE("a damaged link.json says what's wrong", "[io][assembly][link]") {
    auto doc = std::make_unique<model::Document>(model::DocKind::Part, "Car");
    REQUIRE(doc->execute(std::make_unique<model::MakeAssemblyCommand>()).empty());
    REQUIRE(doc->execute(std::make_unique<model::InsertPartCommand>(
                             boxPart("Wheel"), 1, kernel::Placement{}, kernel::Vec3{},
                             model::PartLink{"parts/wheel.sb", std::string(64, '0'), "2026-11-13T23:40:00Z"}))
                .empty());
    io::Bytes bytes;
    REQUIRE(io::writeBytes(*doc, bytes).empty());
    const auto good = readZip(bytes);
    const std::string link = "parts/" + doc->parts()[0]->id().toString() + "/link.json";
    REQUIRE(good.count(link));
    CHECK(good.at(link).find(R"("path": "parts/wheel.sb")") != std::string::npos);
    // It opens, linked, when nothing is wrong (without a folder, the path stays as it is).
    auto r = io::readBytes(bytes, "car.sb");
    REQUIRE(r.ok());
    REQUIRE(r.document->partLink(r.document->parts()[0]->id()));
    CHECK(r.document->partLink(r.document->parts()[0]->id())->path == "parts/wheel.sb");

    auto broken = [&](auto change) {
        auto entries = good;
        change(entries[link]);
        return io::readBytes(makeZip(entries), "car.sb");
    };
    r = broken([&](std::string& text) {
        const std::string id = doc->parts()[0]->id().toString();
        text.replace(text.find(id), id.size(), DocId::generate().toString());
    });
    REQUIRE_FALSE(r.ok());
    CHECK(r.issues[0].code == "file.damaged");
    CHECK(r.issues[0].detail.find("link.json is for another design") != std::string::npos);
    r = broken([](std::string& text) { text.replace(text.find(R"("path")"), 6, R"("road")"); });
    REQUIRE_FALSE(r.ok());
    CHECK(r.issues[0].detail.find("link.json is missing its path or documentId") != std::string::npos);
    r = broken([](std::string& text) { text = "{not json"; });
    REQUIRE_FALSE(r.ok());
    CHECK(r.issues[0].code == "file.damaged");
}

// A schema-2 file with a linked part, checked into the repo. Its linked file
// (parts/Wheel.sb next to it) is left out on purpose: it opens from its copy,
// with the part marked. Every future version must still open it. Never regenerate it.
TEST_CASE("the checked-in schema 2 car with a linked part still opens", "[io][assembly][link][compat]") {
    auto r = io::openFile(std::filesystem::path(SB_TEST_FILES_DIR) / "schema2-linked-car.sb");
    REQUIRE(r.ok());
    const auto& doc = *r.document;
    CHECK(doc.id().toString() == "e8a2dbfe-053b-48ee-bee3-ebf74535b40b");
    REQUIRE(doc.parts().size() == 2);
    const auto* body = doc.findPart(std::string_view("Body"));
    const auto* wheel = doc.findPart(std::string_view("Wheel"));
    REQUIRE(body);
    REQUIRE(wheel);
    CHECK(wheel->id().toString() == "07c45ad1-0eee-4bfe-b317-11a66c5767e1");
    CHECK_FALSE(doc.partLink(body->id()));  // an inside part
    const auto* link = doc.partLink(wheel->id());
    REQUIRE(link);
    CHECK(std::filesystem::path(link->path) ==
          (std::filesystem::absolute(SB_TEST_FILES_DIR) / "parts" / "Wheel.sb").lexically_normal());
    CHECK(link->sha256 == "07775b71b71d3648493210cf095cc858ab512f99de1f4580cebad134a4203805");
    CHECK(link->updated == "2026-11-20T16:32:32Z");
    CHECK_FALSE(link->found);
    CHECK_FALSE(doc.isModified());
    REQUIRE(r.notes.size() == 1);
    CHECK(r.notes[0].code == "part.not_found");
    CHECK(r.notes[0].message.find("the copy of Wheel saved on Nov 20, 2026") != std::string::npos);
    CHECK(doc.components().size() == 3);

    test::FakeKernel kernel;
    CHECK(doc.regenerate(kernel).ok());
}

TEST_CASE("an assembly's joints are saved in joints.json and open as they were", "[io][assembly][joints]") {
    auto doc = car();
    const auto* body = doc->findComponent("Body 1");
    const auto* wheel = doc->findComponent("Wheel 1");
    const auto bodyBox = doc->findPart(body->part)->featureAt(0).id();
    model::Joint hinge;
    hinge.name = "Axle";
    hinge.kind = model::JointKind::Revolute;
    hinge.a = {body->id, bodyBox, model::GeometryRef{kernel::TopoKind::Edge, "edge between A and B", {}},
               model::OriginAt::End};
    hinge.b = {wheel->id, {}, std::nullopt, model::OriginAt::Middle};  // the wheel's own origin
    hinge.flip = true;
    hinge.angle = 0.25;
    hinge.offset = 1.5;
    hinge.value = 0.5;
    hinge.rest = 0.1;
    hinge.lowest = -1.0;
    hinge.highest = 2.0;
    REQUIRE(doc->execute(std::make_unique<model::AddJointCommand>(hinge)).empty());
    model::Joint slide;
    slide.kind = model::JointKind::Slider;
    slide.a = {body->id, {}, std::nullopt, model::OriginAt::Middle};
    slide.b = {doc->findComponent("Wheel 2")->id, {}, std::nullopt, model::OriginAt::Middle};
    slide.off = true;
    REQUIRE(doc->execute(std::make_unique<model::AddJointCommand>(slide)).empty());

    io::Bytes bytes;
    REQUIRE(io::writeBytes(*doc, bytes).empty());
    const auto entries = readZip(bytes);
    REQUIRE(entries.count("joints.json"));
    CHECK(entries.at("manifest.json").find("\"joints.json\"") != std::string::npos);

    auto opened = io::readBytes(bytes, "car.sb");
    REQUIRE(opened.ok());
    const auto& back = *opened.document;
    REQUIRE(back.joints().size() == 2);
    const auto& j = back.joints()[0];
    CHECK(j.id == doc->joints()[0].id);
    CHECK(j.name == "Axle");
    CHECK(j.kind == model::JointKind::Revolute);
    CHECK(j.a.component == body->id);
    CHECK(j.a.body == bodyBox);
    REQUIRE(j.a.element);
    CHECK(j.a.element->name == "edge between A and B");
    CHECK(j.a.at == model::OriginAt::End);
    CHECK(j.b.component == wheel->id);
    CHECK(j.b.body.isNil());
    CHECK_FALSE(j.b.element);
    CHECK(j.flip);
    CHECK(j.angle == 0.25);
    CHECK(j.offset == 1.5);
    CHECK(j.value == 0.5);
    CHECK(j.rest == 0.1);
    CHECK(j.lowest == -1.0);
    CHECK(j.highest == 2.0);
    CHECK(back.joints()[1].name == "Joint1");
    CHECK(back.joints()[1].kind == model::JointKind::Slider);
    CHECK(back.joints()[1].off);
    CHECK_FALSE(back.joints()[1].lowest);
    CHECK_FALSE(back.canUndo());
    io::Bytes again;
    REQUIRE(io::writeBytes(back, again).empty());
    CHECK(again == bytes);

    // An assembly with no joints has no joints.json.
    io::Bytes plain;
    REQUIRE(io::writeBytes(*car(), plain).empty());
    CHECK_FALSE(readZip(plain).count("joints.json"));

    // A damaged joint says so.
    auto damaged = entries;
    damaged["joints.json"] = R"({"joints": [{"name": "Axle", "kind": "hinge"}]})";
    const auto bad = io::readBytes(makeZip(damaged), "car.sb");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.issues.front().detail.find("a joint in joints.json is missing its id, name, kind or sides") !=
          std::string::npos);
}

TEST_CASE("an assembly's press fits are saved in pressfits.json and open as they were",
          "[io][assembly][press_fit]") {
    auto doc = car();
    const auto body = doc->findComponent("Body 1")->id;
    const auto wheel1 = doc->findComponent("Wheel 1")->id;
    const auto wheel2 = doc->findComponent("Wheel 2")->id;
    REQUIRE(
        doc->execute(std::make_unique<model::AddPressFitCommand>(model::PressFit{{}, wheel1, body})).empty());
    REQUIRE(
        doc->execute(std::make_unique<model::AddPressFitCommand>(model::PressFit{{}, body, wheel2})).empty());

    io::Bytes bytes;
    REQUIRE(io::writeBytes(*doc, bytes).empty());
    const auto entries = readZip(bytes);
    REQUIRE(entries.count("pressfits.json"));
    CHECK(entries.at("manifest.json").find("\"pressfits.json\"") != std::string::npos);

    auto opened = io::readBytes(bytes, "car.sb");
    REQUIRE(opened.ok());
    const auto& back = *opened.document;
    REQUIRE(back.pressFits().size() == 2);
    CHECK(back.pressFits()[0].id == doc->pressFits()[0].id);
    CHECK(back.pressFits()[0].a == wheel1);
    CHECK(back.pressFits()[0].b == body);
    CHECK(back.pressFits()[1].id == doc->pressFits()[1].id);
    CHECK(back.pressFits()[1].b == wheel2);
    CHECK_FALSE(back.canUndo());
    io::Bytes again;
    REQUIRE(io::writeBytes(back, again).empty());
    CHECK(again == bytes);

    // An assembly with no press fits has no pressfits.json.
    io::Bytes plain;
    REQUIRE(io::writeBytes(*car(), plain).empty());
    CHECK_FALSE(readZip(plain).count("pressfits.json"));

    // A damaged press fit says so; one of a component that isn't there too.
    auto damaged = entries;
    damaged["pressfits.json"] = R"({"press_fits": [{"a": "Body 1"}]})";
    auto bad = io::readBytes(makeZip(damaged), "car.sb");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.issues.front().detail.find("a press fit in pressfits.json is missing its id or components") !=
          std::string::npos);
    damaged["pressfits.json"] =
        R"({"press_fits": [{"id": "00000000-0000-4000-8000-000000000001", "a": "00000000-0000-4000-8000-000000000002", "b": ")" +
        body.toString() + R"("}]})";
    bad = io::readBytes(makeZip(damaged), "car.sb");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.issues.front().detail.find("press fit: assembly.no_component") != std::string::npos);
}
