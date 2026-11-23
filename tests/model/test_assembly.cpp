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

TEST_CASE("a linked part keeps its link; undo and redo take it out and put it back",
          "[model][assembly][link]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    auto w = wheel();
    const DocId id = w->id();
    const PartLink link{"/class/wheel.sb", std::string(64, 'a'), "2026-11-13T23:40:00Z"};
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(std::move(w), 2, kernel::Placement{},
                                                            kernel::Vec3{}, link))
                .empty());
    REQUIRE(car.partLink(id));
    CHECK(car.partLink(id)->path == "/class/wheel.sb");
    CHECK(car.partLink(id)->found);
    REQUIRE(car.undo());
    CHECK_FALSE(car.partLink(id));
    REQUIRE(car.redo());
    REQUIRE(car.partLink(id));
    CHECK(car.partLink(id)->sha256 == link.sha256);

    // Inserting it again adds components; it stays linked as it was.
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(wheel(), 1)).empty());  // another design
    auto again = std::make_unique<Document>(id, DocKind::Part, "Wheel");
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(std::move(again), 1)).empty());
    REQUIRE(car.partLink(id));

    // Deleting its last components takes the part and its link out; undo puts both back.
    for (const auto* name : {"Wheel 1", "Wheel 2", "Wheel 4"}) {
        REQUIRE(run(car, "component.delete", {{"component", std::string(name)}}).empty());
    }
    CHECK_FALSE(car.findPart(id));
    CHECK_FALSE(car.partLink(id));
    REQUIRE(car.undo());
    REQUIRE(car.partLink(id));
    CHECK(car.partLink(id)->path == "/class/wheel.sb");
}

TEST_CASE("updating a linked part swaps its design and link; the components stay; undo puts it back",
          "[model][assembly][link]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    auto w = wheel();
    const DocId id = w->id();
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(
                            std::move(w), 2, kernel::Placement{}, kernel::Vec3{},
                            PartLink{"/a/wheel.sb", "old", "2026-11-13T00:00:00Z"}))
                .empty());
    const auto components = car.components();

    // The new Wheel: the same design id, with two steps.
    auto changed = std::make_unique<Document>(id, DocKind::Part, "Wheel");
    for (const auto* name : {"Box1", "Box2"}) {
        REQUIRE(changed
                    ->execute(std::make_unique<AddFeatureCommand>(
                        std::make_unique<BoxFeature>(name, kernel::Vec3{}, kernel::Vec3{1, 1, 1})))
                    .empty());
    }
    std::vector<PartUpdate> changes;
    changes.push_back({id, std::move(changed), PartLink{"/b/wheel.sb", "new", "2026-11-20T00:00:00Z"}});
    REQUIRE(car.execute(std::make_unique<UpdatePartsCommand>(std::move(changes), "Update Wheel")).empty());
    CHECK(car.undoLabel() == "Update Wheel");
    CHECK(car.findPart(id)->featureCount() == 2);
    CHECK(car.partLink(id)->path == "/b/wheel.sb");
    CHECK(car.components().size() == components.size());
    CHECK(car.components()[1].name == components[1].name);

    FakeKernel kernel;
    CHECK(car.regenerate(kernel).part(id)->bodies().size() == 2);

    REQUIRE(car.undo());
    CHECK(car.findPart(id)->featureCount() == 1);
    CHECK(car.partLink(id)->path == "/a/wheel.sb");
    CHECK(car.partLink(id)->sha256 == "old");
    CHECK(car.regenerate(kernel).part(id)->bodies().size() == 1);
    REQUIRE(car.redo());
    CHECK(car.findPart(id)->featureCount() == 2);
    CHECK(car.partLink(id)->sha256 == "new");

    // Another design can't take its place, and an assembly can't.
    std::vector<PartUpdate> wrong;
    wrong.push_back({id, wheel("Tire"), std::nullopt});
    auto issues = car.execute(std::make_unique<UpdatePartsCommand>(std::move(wrong), "Update Wheel"));
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "part.other_design");
    CHECK(issues[0].message ==
          "Tire is a different design from Wheel, so it can't take its place. Pick the file "
          "Wheel was saved in.");
    std::vector<PartUpdate> sub;
    sub.push_back({id, std::make_unique<Document>(id, DocKind::Assembly, "Wheel"), std::nullopt});
    issues = car.execute(std::make_unique<UpdatePartsCommand>(std::move(sub), "Update Wheel"));
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "assembly.sub");
    CHECK(car.findPart(id)->featureCount() == 2);  // unchanged
}

TEST_CASE("part.make_inside stops following the file, as one undo step", "[model][assembly][link]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    auto w = wheel();
    const DocId id = w->id();
    REQUIRE(
        car.execute(std::make_unique<InsertPartCommand>(std::move(w), 1, kernel::Placement{}, kernel::Vec3{},
                                                        PartLink{"/a/wheel.sb", "x", "2026-11-13T00:00:00Z"}))
            .empty());
    REQUIRE(run(car, "part.make_inside", {{"part", std::string("Wheel")}}).empty());
    CHECK(car.undoLabel() == "Make Wheel inside");
    CHECK_FALSE(car.partLink(id));
    CHECK(car.findPart(id)->featureCount() == 1);  // its copy, as it was
    REQUIRE(car.undo());
    CHECK(car.partLink(id));
    REQUIRE(car.redo());

    auto issues = run(car, "part.make_inside", {{"part", std::string("Wheel")}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "part.inside_already");
    CHECK(issues[0].message == "Wheel is an inside part already: it isn't linked to a file.");
    issues = run(car, "part.make_inside", {{"part", std::string("Tire")}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].message == "Car has no part called Tire. Its parts are: Wheel.");
}

TEST_CASE("editing an inside part is a step of the assembly: undo and redo work from it",
          "[model][assembly][edit]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(wheel(), 2)).empty());
    const DocId id = car.findPart("Wheel")->id();
    const auto before = car.undoLabel();

    // Give the wheel a second box, from inside the assembly.
    auto made = CommandRegistry::builtin().create("feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}},
                                                  *car.findPart(id));
    REQUIRE(made.ok());
    REQUIRE(car.execute(std::make_unique<EditPartCommand>(id, std::move(made.command))).empty());
    CHECK(car.findPart(id)->featureCount() == 2);
    CHECK(car.undoLabel() == "Add Box2 in Wheel");
    CHECK(car.isModified());
    CHECK(car.components().size() == 2);  // both components place the changed part

    FakeKernel kernel;
    CHECK(car.regenerate(kernel).part(id)->bodies().size() == 2);

    REQUIRE(car.undo());
    CHECK(car.findPart(id)->featureCount() == 1);
    REQUIRE(car.redo());
    CHECK(car.findPart(id)->featureCount() == 2);
}

TEST_CASE("editing a part keeps its rollback marker right, through undo", "[model][assembly][edit]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(wheel(), 1)).empty());
    const DocId id = car.findPart("Wheel")->id();
    REQUIRE(car.execute(std::make_unique<EditPartCommand>(id, std::make_unique<RollbackCommand>(0))).empty());
    CHECK(car.findPart(id)->markerIndex() == 0);
    // A step added while rolled back goes in at the marker, and the marker follows it.
    auto made = CommandRegistry::builtin().create("feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}},
                                                  *car.findPart(id));
    REQUIRE(made.ok());
    REQUIRE(car.execute(std::make_unique<EditPartCommand>(id, std::move(made.command))).empty());
    CHECK(car.findPart(id)->featureCount() == 2);
    CHECK(car.findPart(id)->markerIndex() == 1);
    REQUIRE(car.undo());
    CHECK(car.findPart(id)->markerIndex() == 0);
    REQUIRE(car.undo());
    CHECK(car.findPart(id)->markerIndex() == 1);
    CHECK_FALSE(car.findPart(id)->rolledBack());
}

TEST_CASE("edits to a part that fail, or to a linked part, change nothing and say why",
          "[model][assembly][edit]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(wheel(), 1)).empty());
    PartLink link;
    link.path = "/designs/Axle.sb";
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(wheel("Axle"), 1, kernel::Placement{},
                                                            kernel::Vec3{}, link))
                .empty());
    const DocId wheelId = car.findPart("Wheel")->id();
    const DocId axleId = car.findPart("Axle")->id();
    const auto before = car.undoLabel();

    // A change the part refuses: deleting a step it doesn't have.
    auto bad = car.execute(std::make_unique<EditPartCommand>(
        wheelId, std::make_unique<DeleteFeatureCommand>(FeatureId::generate())));
    CHECK_FALSE(bad.empty());
    CHECK(car.undoLabel() == before);
    CHECK(car.findPart(wheelId)->featureCount() == 1);

    auto made = CommandRegistry::builtin().create("feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}},
                                                  *car.findPart(axleId));
    REQUIRE(made.ok());
    auto linked = car.execute(std::make_unique<EditPartCommand>(axleId, std::move(made.command)));
    REQUIRE(linked.size() == 1);
    CHECK(linked.front().code == "part.linked");
    CHECK(linked.front().message.find("Axle.sb") != std::string::npos);
    CHECK(car.findPart(axleId)->featureCount() == 1);

    auto gone = car.execute(
        std::make_unique<EditPartCommand>(DocId::generate(), std::make_unique<RollbackCommand>(0)));
    REQUIRE(gone.size() == 1);
    CHECK(gone.front().code == "part.not_found");

    Document plain(DocKind::Part, "Plain");
    CHECK_FALSE(
        plain.execute(std::make_unique<EditPartCommand>(wheelId, std::make_unique<RollbackCommand>(0)))
            .empty());
}

TEST_CASE("dragging a body in a part is one undo step of the assembly", "[model][assembly][edit]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(wheel(), 1)).empty());
    const DocId id = car.findPart("Wheel")->id();
    const FeatureId box = car.findPart(id)->featureAt(0).id();
    const auto before = car.undoLabel();
    REQUIRE(car.execute(std::make_unique<EditPartCommand>(
                            id, std::make_unique<SlideBodyCommand>(box, kernel::Vec3{1, 0, 0})))
                .empty());
    REQUIRE(car.execute(std::make_unique<EditPartCommand>(
                            id, std::make_unique<SlideBodyCommand>(box, kernel::Vec3{1, 0, 0}, true)))
                .empty());
    CHECK(car.undoLabel() == "Move Box1 in Wheel");
    REQUIRE(car.undo());
    CHECK(car.undoLabel() == before);  // both slides went back as one step
    CHECK(car.findPart(id)->featureCount() == 1);
}

TEST_CASE("Reset joints puts each hinge and slider back to its rest, as one undo step",
          "[model][assembly][joints]") {
    Document car(DocKind::Part, "Car");
    makeAssembly(car);
    REQUIRE(car.execute(std::make_unique<InsertPartCommand>(wheel(), 3)).empty());
    const auto& c = car.components();
    auto join = [&](const Component& a, const Component& b, JointKind kind, double value, double rest) {
        Joint j;
        j.kind = kind;
        j.a.component = a.id;
        j.b.component = b.id;
        j.value = value;
        j.rest = rest;
        REQUIRE(car.execute(std::make_unique<AddJointCommand>(j)).empty());
    };
    join(c[0], c[1], JointKind::Revolute, 1.0, 0.25);
    join(c[0], c[2], JointKind::Slider, 7.0, 0.0);
    REQUIRE(car.execute(std::make_unique<ResetJointsCommand>()).empty());
    CHECK(car.joints()[0].value == 0.25);
    CHECK(car.joints()[1].value == 0.0);
    CHECK(car.undoLabel() == "Reset joints");
    REQUIRE(car.undo());
    CHECK(car.joints()[0].value == 1.0);
    CHECK(car.joints()[1].value == 7.0);

    // Just one, by name.
    REQUIRE(run(car, "joint.reset", {{"joint", std::string("Joint2")}}).empty());
    CHECK(car.joints()[0].value == 1.0);
    CHECK(car.joints()[1].value == 0.0);
    REQUIRE(run(car, "joint.reset").empty());
    const auto again = run(car, "joint.reset");
    REQUIRE(again.size() == 1);
    CHECK(again.front().code == "joint.at_rest");
}
