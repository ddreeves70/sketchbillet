// Tests for linked parts: an assembly's parts that come from their own .sb
// files. Fingerprints, link.json, and what opening does when a file changed,
// moved, was renamed or is gone; and the commands that link and unlink parts.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>

#include "sb/io/LinkedParts.h"
#include "sb/io/PartCommands.h"
#include "sb/io/SketchBilletFile.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"

using namespace sb;
namespace fs = std::filesystem;

namespace {

io::Bytes bytesOf(const std::string& text) {
    return io::Bytes(text.begin(), text.end());
}

// A fresh, empty folder for one test, removed afterwards.
struct Folder {
    fs::path path;
    Folder() {
        // A new id each time: tests run in parallel, each in its own process.
        path = fs::temp_directory_path() / ("sb_linked_" + DocId::generate().toString());
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Folder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;
};

model::CommandRegistry& registry() {
    static model::CommandRegistry r = [] {
        model::CommandRegistry made = model::CommandRegistry::builtin();
        io::addPartCommands(made);
        return made;
    }();
    return r;
}

Diagnostics run(model::Document& doc, const std::string& name, const model::Args& args = {}) {
    auto made = registry().create(name, args, doc);
    if (!made.ok()) return made.issues;
    return doc.execute(std::move(made.command));
}

std::string utf8(const fs::path& p) {
    const auto text = p.generic_u8string();
    return std::string(text.begin(), text.end());
}

// Saves a part with `boxes` boxes in it, as wheel.sb's do.
void savePart(model::Document& part, int boxes, const fs::path& path) {
    while (static_cast<int>(part.featureCount()) < boxes) {
        const auto n = std::to_string(part.featureCount() + 1);
        REQUIRE(part.execute(std::make_unique<model::AddFeatureCommand>(std::make_unique<model::BoxFeature>(
                                 "Box" + n, kernel::Vec3{}, kernel::Vec3{10, 10, 10})))
                    .empty());
    }
    fs::create_directories(path.parent_path());
    REQUIRE(io::saveFile(part, path).empty());
}

// Car, in `dir`, with two Wheels linked to dir/parts/wheel.sb (one box).
struct Setup {
    Folder folder;
    model::Document wheel{model::DocKind::Part, "Wheel"};
    fs::path car;
    fs::path wheelFile;
    Setup() {
        wheelFile = folder.path / "parts" / "wheel.sb";
        car = folder.path / "car.sb";
        savePart(wheel, 1, wheelFile);
        model::Document doc(model::DocKind::Part, "Car");
        REQUIRE(run(doc, "assembly.new").empty());
        REQUIRE(run(doc, "component.insert",
                    {{"file", utf8(wheelFile)}, {"count", std::int64_t{2}}, {"step", kernel::Vec3{30, 0, 0}}})
                    .empty());
        REQUIRE(io::saveFile(doc, car).empty());
    }
};

}  // namespace

TEST_CASE("SHA-256 gives the standard fingerprints", "[io][link]") {
    CHECK(io::sha256Hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(io::sha256Hex(bytesOf("abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 bytes: the padding needs a second block.
    CHECK(io::sha256Hex(bytesOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(io::sha256Hex(io::Bytes(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("link times are UTC to the second, and shown as dates", "[io][link]") {
    const auto now = io::utcNow();
    REQUIRE(now.size() == 20);
    CHECK(now[4] == '-');
    CHECK(now[10] == 'T');
    CHECK(now.back() == 'Z');
    CHECK(io::linkDate("2026-11-13T23:40:00Z") == "Nov 13, 2026");
    CHECK(io::linkDate("2026-01-02T00:00:00Z") == "Jan 2, 2026");
    CHECK(io::linkDate("someday") == "someday");
}

TEST_CASE("a linked part's path is saved relative to the assembly, and opens as a full path", "[io][link]") {
    Setup s;
    auto opened = io::openFile(s.car);
    REQUIRE(opened.ok());
    CHECK(opened.notes.empty());  // the file is as it was
    const auto& car = *opened.document;
    CHECK_FALSE(car.isModified());
    REQUIRE(car.parts().size() == 1);
    const auto* link = car.partLink(car.parts()[0]->id());
    REQUIRE(link);
    CHECK(fs::path(link->path) == s.wheelFile.lexically_normal());
    CHECK(link->found);
    io::Bytes wheelBytes;
    REQUIRE(io::readFileBytes(s.wheelFile, wheelBytes));
    CHECK(link->sha256 == io::sha256Hex(wheelBytes));

    // In the file: parts/<id>/link.json, with the path relative to car.sb.
    io::Bytes first;
    REQUIRE(io::writeBytes(car, first, s.folder.path).empty());
    // Read back without a folder, the path is as written.
    auto written = io::readBytes(first, "car.sb");
    REQUIRE(written.ok());
    CHECK(written.document->partLink(written.document->parts()[0]->id())->path == "parts/wheel.sb");
    // Moving the whole folder keeps it working: the path is relative.
    Folder elsewhere;
    fs::rename(s.folder.path / "parts", elsewhere.path / "parts");
    fs::rename(s.car, elsewhere.path / "car.sb");
    auto moved = io::openFile(elsewhere.path / "car.sb");
    REQUIRE(moved.ok());
    CHECK(moved.notes.empty());
    const auto* there = moved.document->partLink(moved.document->parts()[0]->id());
    REQUIRE(there);
    CHECK(fs::path(there->path) == (elsewhere.path / "parts" / "wheel.sb").lexically_normal());
    // And it saves the same bytes again.
    io::Bytes second;
    REQUIRE(io::writeBytes(*moved.document, second, elsewhere.path).empty());
    CHECK(second == first);
}

TEST_CASE("a linked file that changed updates the part on opening, as one undo step", "[io][link]") {
    Setup s;
    savePart(s.wheel, 2, s.wheelFile);  // Wheel gets a second box
    auto opened = io::openFile(s.car);
    REQUIRE(opened.ok());
    auto& car = *opened.document;
    const auto id = car.parts()[0]->id();
    CHECK(car.findPart(id)->featureCount() == 2);
    REQUIRE(opened.notes.size() == 1);
    CHECK(opened.notes[0].severity == Severity::Info);
    CHECK(opened.notes[0].code == "part.updated");
    CHECK(opened.notes[0].message == "Wheel changed in wheel.sb, so Car uses the new Wheel.");
    CHECK(car.isModified());
    CHECK(car.undoLabel() == "Update Wheel");
    CHECK(car.components().size() == 2);
    // Undo goes back to the old Wheel.
    REQUIRE(car.undo());
    CHECK(car.findPart(id)->featureCount() == 1);
    REQUIRE(car.redo());
    // Saved, it opens with nothing to do.
    REQUIRE(io::saveFile(car, s.car).empty());
    auto again = io::openFile(s.car);
    REQUIRE(again.ok());
    CHECK(again.notes.empty());
    CHECK(again.document->findPart(id)->featureCount() == 2);
}

TEST_CASE("a linked file that moved or was renamed is found, and the link remembers where", "[io][link]") {
    Setup s;
    SECTION("moved next to the assembly: found by name") {
        fs::rename(s.wheelFile, s.folder.path / "wheel.sb");
        auto opened = io::openFile(s.car);
        REQUIRE(opened.ok());
        REQUIRE(opened.notes.size() == 1);
        CHECK(opened.notes[0].code == "part.moved");
        CHECK(opened.notes[0].message ==
              "Car found wheel.sb in the same folder as Car, and links to it there now.");
        const auto& car = *opened.document;
        CHECK(fs::path(car.partLink(car.parts()[0]->id())->path) ==
              (s.folder.path / "wheel.sb").lexically_normal());
        CHECK(car.undoLabel() == "Update Wheel");
    }
    SECTION("renamed into another folder: found by its design id") {
        fs::create_directories(s.folder.path / "shop");
        fs::rename(s.wheelFile, s.folder.path / "shop" / "tire.sb");
        auto opened = io::openFile(s.car);
        REQUIRE(opened.ok());
        REQUIRE(opened.notes.size() == 1);
        CHECK(opened.notes[0].message == "Car found tire.sb in the shop folder, and links to it there now.");
        const auto& car = *opened.document;
        CHECK(fs::path(car.partLink(car.parts()[0]->id())->path) ==
              (s.folder.path / "shop" / "tire.sb").lexically_normal());
    }
    SECTION("another design with the same name doesn't count") {
        fs::remove(s.wheelFile);
        model::Document other(model::DocKind::Part, "Wheel");
        savePart(other, 1, s.wheelFile);
        auto opened = io::openFile(s.car);
        REQUIRE(opened.ok());
        REQUIRE(opened.notes.size() == 1);
        CHECK(opened.notes[0].code == "part.not_found");
    }
}

TEST_CASE("a linked file that's gone leaves the saved copy, marked; part.relink links it again",
          "[io][link]") {
    Setup s;
    Folder away;
    fs::rename(s.wheelFile, away.path / "w.sb");
    auto opened = io::openFile(s.car);
    REQUIRE(opened.ok());
    auto& car = *opened.document;
    const auto id = car.parts()[0]->id();
    CHECK(car.findPart(id)->featureCount() == 1);  // the copy
    CHECK_FALSE(car.partLink(id)->found);
    CHECK_FALSE(car.isModified());  // nothing changed
    REQUIRE(opened.notes.size() == 1);
    CHECK(opened.notes[0].severity == Severity::Warning);
    const auto date = io::linkDate(car.partLink(id)->updated);
    CHECK(opened.notes[0].message ==
          "wheel.sb can't be found, so Car uses the copy of Wheel saved on " + date +
              ". Put wheel.sb back, or link Wheel to it where it is now with part.relink part=\"Wheel\" "
              "file=<where it is>.");
    // Still gone: part.update says so.
    auto issues = run(car, "part.update");
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "part.not_found");

    // Find Linked File, with a changed Wheel: it's used, and linked there.
    savePart(s.wheel, 3, away.path / "w.sb");
    REQUIRE(run(car, "part.relink", {{"part", std::string("Wheel")}, {"file", utf8(away.path / "w.sb")}})
                .empty());
    CHECK(car.undoLabel() == "Link Wheel to w.sb");
    CHECK(car.findPart(id)->featureCount() == 3);
    CHECK(car.partLink(id)->found);
    CHECK(fs::path(car.partLink(id)->path) == (away.path / "w.sb").lexically_normal());
    REQUIRE(car.undo());
    CHECK_FALSE(car.partLink(id)->found);

    // A file with another design can't stand in for it.
    model::Document other(model::DocKind::Part, "Tire");
    savePart(other, 1, away.path / "tire.sb");
    issues = run(car, "part.relink", {{"part", std::string("Wheel")}, {"file", utf8(away.path / "tire.sb")}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "part.other_design");
    CHECK(issues[0].message ==
          "tire.sb holds Tire, a different design from Wheel, so it can't take its place. Pick the file "
          "Wheel was "
          "saved in.");
}

TEST_CASE("component.insert links to the file, or keeps only a copy with link=false", "[io][link]") {
    Setup s;
    auto opened = io::openFile(s.car);
    REQUIRE(opened.ok());
    auto& car = *opened.document;
    model::Document body(model::DocKind::Part, "Body");
    savePart(body, 1, s.folder.path / "body.sb");
    REQUIRE(
        run(car, "component.insert", {{"file", utf8(s.folder.path / "body.sb")}, {"link", false}}).empty());
    const auto* inside = car.findPart(std::string_view("Body"));
    REQUIRE(inside);
    CHECK_FALSE(car.partLink(inside->id()));
    CHECK(car.partLink(car.findPart(std::string_view("Wheel"))->id()));

    // Saved: only the linked part has a link.json.
    REQUIRE(io::saveFile(car, s.car).empty());
    io::Bytes bytes;
    REQUIRE(io::readFileBytes(s.car, bytes));
    const std::string text(bytes.begin(), bytes.end());
    CHECK(text.find("parts/" + inside->id().toString() + "/link.json") == std::string::npos);
    CHECK(text.find("parts/" + car.findPart(std::string_view("Wheel"))->id().toString() + "/link.json") !=
          std::string::npos);
}

TEST_CASE("part.update updates from files that changed, or says there's nothing to do", "[io][link]") {
    Setup s;
    auto opened = io::openFile(s.car);
    REQUIRE(opened.ok());
    auto& car = *opened.document;
    auto issues = run(car, "part.update");
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "part.up_to_date");
    CHECK(issues[0].message ==
          "Every linked part is the same as its file already, so there's nothing to update.");
    issues = run(car, "part.update", {{"part", std::string("Wheel")}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].message == "Wheel is the same as its file already, so there's nothing to update.");

    savePart(s.wheel, 2, s.wheelFile);  // changed while Car is open
    REQUIRE(run(car, "part.update").empty());
    CHECK(car.undoLabel() == "Update Wheel");
    CHECK(car.parts()[0]->featureCount() == 2);
}

TEST_CASE("part.make_inside and part.save_as switch a part between inside and linked", "[io][link]") {
    Setup s;
    auto opened = io::openFile(s.car);
    REQUIRE(opened.ok());
    auto& car = *opened.document;
    const auto id = car.parts()[0]->id();
    REQUIRE(run(car, "part.make_inside", {{"part", std::string("Wheel")}}).empty());
    CHECK_FALSE(car.partLink(id));
    // An inside part has no file to update from.
    auto issues = run(car, "part.update", {{"part", std::string("Wheel")}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "part.not_linked");

    // Save it as a file of its own: it's linked to that.
    const auto file = s.folder.path / "new-wheel.sb";
    REQUIRE(run(car, "part.save_as", {{"part", std::string("Wheel")}, {"file", utf8(file)}}).empty());
    CHECK(car.undoLabel() == "Save Wheel as new-wheel.sb");
    REQUIRE(car.partLink(id));
    CHECK(fs::path(car.partLink(id)->path) == file.lexically_normal());
    auto saved = io::openFile(file);
    REQUIRE(saved.ok());
    CHECK(saved.document->id() == id);  // the same design
    CHECK(saved.document->featureCount() == 1);
    io::Bytes bytes;
    REQUIRE(io::readFileBytes(file, bytes));
    CHECK(car.partLink(id)->sha256 == io::sha256Hex(bytes));

    // Not over a file that's there.
    issues = run(car, "part.save_as", {{"part", std::string("Wheel")}, {"file", utf8(s.wheelFile)}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "part.file_there");
    CHECK(issues[0].message ==
          "wheel.sb is there already. Pick another name, or link Wheel to it with part.relink.");
}
