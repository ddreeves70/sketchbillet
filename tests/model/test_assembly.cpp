// Tests for assembly documents: making one, putting parts in it, and changing
// its components, with undo and redo; and building each part once.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "FakeKernel.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;
using sb::test::FakeKernel;

namespace {

// A part with one box in it.
std::unique_ptr<Document> wheel(std::string name = "Wheel") {
    auto part = std::make_unique<Document>(DocKind::Part, std::move(name));
    REQUIRE(part->execute(std::make_unique<AddFeatureCommand>(
                              std::make_unique<BoxFeature>("Box1", kernel::Vec3{}, kernel::Vec3{2, 3, 4})))
                .empty());
    return part;
}

void makeAssembly(Document& doc) {
    REQUIRE(doc.execute(std::make_unique<MakeAssemblyCommand>()).empty());
}

Diagnostics run(Document& doc, const std::string& name, const Args& args = {}) {
    auto made = CommandRegistry::builtin().create(name, args, doc);
    if (!made.ok()) return made.issues;
    return doc.execute(std::move(made.command));
}

std::vector<std::string> names(const Document& doc) {
    std::vector<std::string> out;
    for (const auto& c : doc.components()) out.push_back(c.name);
    return out;
}

}  // namespace

TEST_CASE("a new, empty design becomes an assembly; one with steps can't", "[model][assembly]") {
    Document car(DocKind::Part, "Car");
    REQUIRE(car.execute(std::make_unique<MakeAssemblyCommand>()).empty());
    CHECK(car.kind() == DocKind::Assembly);
    CHECK(car.undoLabel() == "Make assembly");
    REQUIRE(car.undo());
    CHECK(car.kind() == DocKind::Part);
    REQUIRE(car.redo());
    CHECK(car.kind() == DocKind::Assembly);
    auto again = car.execute(std::make_unique<MakeAssemblyCommand>());
    REQUIRE(again.size() == 1);
    CHECK(again[0].code == "assembly.already");

    auto tag = wheel("Tag");
    const auto problems = tag->execute(std::make_unique<MakeAssemblyCommand>());
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].code == "assembly.not_empty");
    CHECK(problems[0].message.find("insert Tag into it") != std::string::npos);
    CHECK(tag->kind() == DocKind::Part);
}

TEST_CASE("inserting a part puts it in once, with components in a row; the first is grounded",
          "[model][assembly]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    auto part = wheel();
    const DocId wheelId = part->id();
    kernel::Placement at;
    at.origin = {10, 0, 0};
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(std::move(part), 4, at, kernel::Vec3{30, 0, 0}))
                .empty());
    CHECK(car.undoLabel() == "Insert 4 of Wheel");
    REQUIRE(car.parts().size() == 1);
    CHECK(car.findPart(wheelId)->name() == "Wheel");
    CHECK(car.findPart("Wheel") == car.findPart(wheelId));
    CHECK(names(car) == std::vector<std::string>{"Wheel 1", "Wheel 2", "Wheel 3", "Wheel 4"});
    CHECK(car.components()[0].grounded);  // the first in an empty assembly
    CHECK_FALSE(car.components()[1].grounded);
    CHECK(car.components()[3].placement.origin.x == Approx(100));
    for (const auto& c : car.components()) CHECK(c.part == wheelId);

    // The same part again only adds components.
    auto same = std::make_unique<Document>(wheelId, DocKind::Part, "Wheel");
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(std::move(same), 1)).empty());
    CHECK(car.parts().size() == 1);
    CHECK(car.components().size() == 5);
    CHECK_FALSE(car.components()[4].grounded);  // the assembly wasn't empty

    // Undo takes them out; redo puts the same ones back.
    const ComponentId first = car.components()[0].id;
    REQUIRE(car.undo());
    REQUIRE(car.undo());
    CHECK(car.components().empty());
    CHECK(car.parts().empty());
    REQUIRE(car.redo());
    CHECK(car.parts().size() == 1);
    CHECK(car.components()[0].id == first);
    CHECK(car.findPart(wheelId)->featureCount() == 1);
}

TEST_CASE("what can't go in an assembly says why", "[model][assembly]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    // Not into a part.
    auto tag = wheel("Tag");
    auto problems = tag->execute(std::make_unique<InsertPartCommand>(wheel(), 1));
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].code == "assembly.not_assembly");
    // Not an assembly inside an assembly, yet.
    auto truck = std::make_unique<Document>(DocKind::Part, "Truck");
    REQUIRE(truck->execute(std::make_unique<MakeAssemblyCommand>()).empty());
    problems = car.execute(std::make_unique<InsertPartCommand>(std::move(truck), 1));
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].code == "assembly.sub");
    // Not itself.
    problems = car.execute(
        std::make_unique<InsertPartCommand>(std::make_unique<Document>(car.id(), DocKind::Part, "Car"), 1));
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].code == "assembly.itself");
    // From 1 to 100 at once.
    for (int n : {0, 101}) {
        problems = car.execute(std::make_unique<InsertPartCommand>(wheel(), n));
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].code == "assembly.count");
    }
    // Facing a way that can be told.
    kernel::Placement flat{{0, 0, 0}, {0, 0, 1}, {0, 0, 1}};
    problems = car.execute(std::make_unique<InsertPartCommand>(wheel(), 1, flat));
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].code == "assembly.placement");
    CHECK(car.components().empty());
    CHECK(car.parts().empty());
    CHECK(car.undoLabel() == "Make assembly");  // nothing else happened
}

TEST_CASE("components are grounded, moved, renamed and deleted, each one undo step", "[model][assembly]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    REQUIRE(run(car, "component.new", {{"name", std::string("Body")}}).empty());
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(wheel(), 2)).empty());
    CHECK(names(car) == std::vector<std::string>{"Body 1", "Wheel 1", "Wheel 2"});
    CHECK(car.findPart("Body")->featureCount() == 0);  // a new part is empty

    REQUIRE(run(car, "component.ground", {{"component", std::string("Wheel 1")}}).empty());
    CHECK(car.undoLabel() == "Ground Wheel 1");
    CHECK(car.findComponent("Wheel 1")->grounded);
    REQUIRE(
        run(car, "component.ground", {{"component", std::string("Wheel 1")}, {"grounded", false}}).empty());
    CHECK(car.undoLabel() == "Let go of Wheel 1");
    CHECK_FALSE(car.findComponent("Wheel 1")->grounded);

    // Moved and turned: the directions are tidied, the old place comes back on undo.
    REQUIRE(run(car, "component.place",
                {{"component", std::string("Wheel 2")},
                 {"origin", kernel::Vec3{40, -32, 12}},
                 {"z", kernel::Vec3{0, -3, 0}}})
                .empty());
    CHECK(car.undoLabel() == "Move Wheel 2");
    const auto& moved = car.findComponent("Wheel 2")->placement;
    CHECK(moved.origin.y == Approx(-32));
    CHECK(moved.zAxis.y == Approx(-1));
    CHECK(moved.xAxis.x == Approx(1));
    REQUIRE(car.undo());
    CHECK(car.findComponent("Wheel 2")->placement.origin.y == Approx(0));

    REQUIRE(
        run(car, "component.rename", {{"component", std::string("Wheel 2")}, {"name", std::string("Back")}})
            .empty());
    CHECK(car.undoLabel() == "Rename Wheel 2 to Back");
    auto problems =
        run(car, "component.rename", {{"component", std::string("Back")}, {"name", std::string("Body 1")}});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].code == "assembly.name_taken");

    // Deleting the last component of a part takes the part out too; undo brings both back.
    REQUIRE(run(car, "component.delete", {{"component", std::string("Body 1")}}).empty());
    CHECK(car.undoLabel() == "Delete Body 1");
    CHECK_FALSE(car.findPart("Body"));
    REQUIRE(run(car, "component.delete", {{"component", std::string("Back")}}).empty());
    CHECK(car.findPart("Wheel"));  // Wheel 1 still uses it
    REQUIRE(car.undo());
    REQUIRE(car.undo());
    CHECK(names(car) == std::vector<std::string>{"Body 1", "Wheel 1", "Back"});
    CHECK(car.parts().size() == 2);
    CHECK(car.parts()[0]->name() == "Body");  // in its old place
}

TEST_CASE("components and parts are found by name, or the problem lists what there is", "[model][assembly]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    auto problems = run(car, "component.ground", {{"component", std::string("Wheel 1")}});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.find("It has none yet") != std::string::npos);
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(wheel(), 2)).empty());
    problems = run(car, "component.ground", {{"component", std::string("Wheel 9")}});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.find("Its components are: Wheel 1, Wheel 2.") != std::string::npos);
    problems = run(car, "component.add", {{"part", std::string("Axle")}});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.find("Its parts are: Wheel.") != std::string::npos);
    REQUIRE(run(car, "component.add", {{"part", std::string("Wheel")}, {"count", std::int64_t{2}}}).empty());
    CHECK(car.undoLabel() == "Insert 2 of Wheel");
    CHECK(car.components().size() == 4);
    problems = run(car, "component.new", {{"name", std::string("Wheel")}});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].code == "assembly.part_taken");
    REQUIRE(run(car, "component.new").empty());
    CHECK(car.findPart("Part1"));
}

TEST_CASE("an assembly's steps are in its parts; a part has no components", "[model][assembly]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    auto problems = run(car, "feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].code == "assembly.steps");
    CHECK(problems[0].message.find("Car is an assembly") != std::string::npos);
    Document tag(DocKind::Part, "Tag");
    problems = run(tag, "component.new");
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].code == "assembly.not_assembly");
    problems = run(tag, "component.delete", {{"component", std::string("Wheel 1")}});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].code == "assembly.not_assembly");
}

TEST_CASE("rebuilding an assembly builds each part once, and again only when it changed",
          "[model][assembly]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(wheel(), 4)).empty());
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(wheel("Axle"), 2)).empty());
    FakeKernel kernel;
    const auto first = car.regenerate(kernel);
    CHECK(kernel.calls == 2);  // one box per part, not per component
    CHECK(first.ok());
    const auto* wheelBuilt = first.part(car.findPart("Wheel")->id());
    REQUIRE(wheelBuilt);
    CHECK(wheelBuilt->bodies().size() == 1);
    CHECK_FALSE(first.part(DocId::generate()));
    // Nothing changed: nothing built again.
    const auto second = car.regenerate(kernel, nullptr, &first);
    CHECK(kernel.calls == 2);
    CHECK(second.part(car.findPart("Axle")->id())->reused().size() == 1);
}
