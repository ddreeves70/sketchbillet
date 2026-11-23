// Tests for the list of open designs (sb_plugin).

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "../model/FakeKernel.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/CommandRegistry.h"
#include "sb/plugin/DocumentList.h"
#include "sb/plugin/Events.h"

using namespace sb;
using namespace sb::plugin;

namespace {

// Records every event, as "Type:detail".
struct Recorder {
    explicit Recorder(EventBus& bus) {
        subs.push_back(bus.subscribe<DocumentOpened>([this](const auto&) { log.push_back("Opened"); }));
        subs.push_back(bus.subscribe<DocumentClosed>([this](const auto&) { log.push_back("Closed"); }));
        subs.push_back(bus.subscribe<ActiveDocumentChanged>([this](const ActiveDocumentChanged& e) {
            log.push_back(e.doc.isNil() ? "Active:none" : "Active");
        }));
        subs.push_back(bus.subscribe<DocumentChanged>([this](const DocumentChanged& e) {
            const char* kind = e.kind == ChangeKind::Executed ? "Executed"
                               : e.kind == ChangeKind::Undone ? "Undone"
                                                              : "Redone";
            log.push_back(std::string("Changed:") + kind + ":" + e.label);
        }));
        subs.push_back(bus.subscribe<RebuildFinished>(
            [this](const RebuildFinished& e) { log.push_back(e.ok ? "Rebuilt:ok" : "Rebuilt:problem"); }));
    }
    std::vector<std::string> log;
    std::vector<Subscription> subs;
};

}  // namespace

TEST_CASE("changes go through the list and every listener hears about them", "[plugin][documents]") {
    EventBus bus;
    test::FakeKernel kernel;
    DocumentList docs(bus, model::CommandRegistry::builtin(), &kernel);
    Recorder rec(bus);

    const DocId id = docs.add(std::make_unique<model::Document>());
    CHECK(rec.log == std::vector<std::string>{"Opened", "Rebuilt:ok", "Active"});
    CHECK(docs.active() == id);
    rec.log.clear();

    REQUIRE(docs.run(id, "feature.add_box", {{"size", kernel::Vec3{10, 20, 30}}}).empty());
    CHECK(rec.log == std::vector<std::string>{"Changed:Executed:Add Box1", "Rebuilt:ok"});

    // The rebuild result is there for listeners to read.
    const auto* report = docs.lastRebuild(id);
    REQUIRE(report);
    const auto box = docs.find(id)->featureAt(0).id();
    CHECK(kernel.massProps(*report->shapeOf(box)).volume == 6000.0);  // 10 * 20 * 30 mm^3

    rec.log.clear();
    CHECK(docs.undo(id));
    CHECK(docs.redo(id));
    CHECK(rec.log == std::vector<std::string>{"Changed:Undone:Add Box1", "Rebuilt:ok",
                                              "Changed:Redone:Add Box1", "Rebuilt:ok"});
}

TEST_CASE("a failed command changes nothing and sends nothing", "[plugin][documents]") {
    EventBus bus;
    DocumentList docs(bus, model::CommandRegistry::builtin());
    const DocId id = docs.add(std::make_unique<model::Document>());
    Recorder rec(bus);

    const auto issues = docs.run(id, "feature.add_box", {});  // size is required
    CHECK_FALSE(issues.empty());
    CHECK(docs.find(id)->featureCount() == 0);
    CHECK(rec.log.empty());
    CHECK_FALSE(docs.undo(id));  // nothing to undo
}

TEST_CASE("without a kernel, designs are not rebuilt", "[plugin][documents]") {
    EventBus bus;
    DocumentList docs(bus, model::CommandRegistry::builtin());
    Recorder rec(bus);
    const DocId id = docs.add(std::make_unique<model::Document>());
    CHECK(docs.lastRebuild(id) == nullptr);
    CHECK(rec.log == std::vector<std::string>{"Opened", "Active"});
}

TEST_CASE("closing designs moves the active one along", "[plugin][documents]") {
    EventBus bus;
    DocumentList docs(bus, model::CommandRegistry::builtin());
    const DocId a = docs.add(std::make_unique<model::Document>());
    const DocId b = docs.add(std::make_unique<model::Document>());
    CHECK(docs.active() == a);  // the first one stays active
    CHECK(docs.ids() == std::vector<DocId>{a, b});

    Recorder rec(bus);
    CHECK(docs.close(a));
    CHECK(docs.active() == b);
    CHECK(docs.close(b));
    CHECK(docs.active().isNil());
    CHECK(rec.log == std::vector<std::string>{"Closed", "Active", "Closed", "Active:none"});
    CHECK_FALSE(docs.close(b));
}

TEST_CASE("working on a design that isn't open explains the problem", "[plugin][documents]") {
    EventBus bus;
    DocumentList docs(bus, model::CommandRegistry::builtin());
    const auto issues = docs.run(DocId::generate(), "feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "document.not_open");
    CHECK_FALSE(docs.setActive(DocId::generate()));
}

TEST_CASE("a listener may close the design that just changed", "[plugin][documents]") {
    EventBus bus;
    test::FakeKernel kernel;
    DocumentList docs(bus, model::CommandRegistry::builtin(), &kernel);
    const DocId id = docs.add(std::make_unique<model::Document>());
    auto closer = bus.subscribe<DocumentChanged>([&](const DocumentChanged& e) { docs.close(e.doc); });
    CHECK(docs.run(id, "feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}}).empty());
    CHECK(docs.find(id) == nullptr);
}

namespace {

// An assembly, Car, with an inside part (Wheel, one box, two components) and a
// linked part (Axle).
std::unique_ptr<model::Document> car() {
    auto doc = std::make_unique<model::Document>(model::DocKind::Part, "Car");
    REQUIRE(doc->execute(std::make_unique<model::MakeAssemblyCommand>()).empty());
    auto wheel = std::make_unique<model::Document>(model::DocKind::Part, "Wheel");
    auto box = model::CommandRegistry::builtin().create("feature.add_box", {{"size", kernel::Vec3{2, 3, 4}}},
                                                        *wheel);
    REQUIRE(wheel->execute(std::move(box.command)).empty());
    REQUIRE(doc->execute(std::make_unique<model::InsertPartCommand>(std::move(wheel), 2)).empty());
    model::PartLink link;
    link.path = "/designs/Axle.sb";
    REQUIRE(doc->execute(std::make_unique<model::InsertPartCommand>(
                             std::make_unique<model::Document>(model::DocKind::Part, "Axle"), 1,
                             kernel::Placement{}, kernel::Vec3{}, link))
                .empty());
    doc->markSaved();
    return doc;
}

}  // namespace

TEST_CASE("editing an inside part makes it the active design, and its changes steps of the assembly",
          "[plugin][documents][edit]") {
    EventBus bus;
    test::FakeKernel kernel;
    DocumentList docs(bus, model::CommandRegistry::builtin(), &kernel);
    const DocId id = docs.add(car());
    const auto* assembly = docs.find(id);
    const DocId wheel = assembly->findPart("Wheel")->id();
    const auto wheel2 = assembly->components()[1].id;
    Recorder rec(bus);

    REQUIRE(docs.editPart(id, wheel2).empty());
    CHECK(rec.log == std::vector<std::string>{"Active"});
    CHECK(docs.active() == wheel);
    CHECK(docs.activeDesign() == id);
    CHECK(docs.editedPart(id) == wheel);
    CHECK(docs.editedComponent(id) == wheel2);
    CHECK(docs.assemblyOf(wheel) == id);
    CHECK(docs.find(wheel) == assembly->findPart(wheel));
    REQUIRE(docs.lastRebuild(wheel));
    CHECK(docs.lastRebuild(wheel) == docs.lastRebuild(id)->part(wheel));
    CHECK(docs.ids() == std::vector<DocId>{id});

    rec.log.clear();
    REQUIRE(docs.run(wheel, "feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}}).empty());
    CHECK(rec.log == std::vector<std::string>{"Changed:Executed:Add Box2 in Wheel", "Rebuilt:ok"});
    CHECK(docs.find(wheel)->featureCount() == 2);
    CHECK(docs.lastRebuild(wheel)->bodies().size() == 2);
    CHECK(docs.find(id)->isModified());

    // Undo from either: it's the assembly's history.
    REQUIRE(docs.undo(wheel));
    CHECK(docs.find(wheel)->featureCount() == 1);
    REQUIRE(docs.redo(id));
    CHECK(docs.find(wheel)->featureCount() == 2);
    CHECK(docs.markSaved(wheel));
    CHECK_FALSE(docs.find(id)->isModified());

    rec.log.clear();
    CHECK(docs.finishEditing(id));
    CHECK(rec.log == std::vector<std::string>{"Active"});
    CHECK(docs.active() == id);
    CHECK_FALSE(docs.find(wheel));
    CHECK_FALSE(docs.finishEditing(id));
    CHECK(docs.run(wheel, "feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}}).front().code ==
          "document.not_open");
}

TEST_CASE("a linked part isn't edited in the assembly; editing stops when the part goes",
          "[plugin][documents][edit]") {
    EventBus bus;
    test::FakeKernel kernel;
    DocumentList docs(bus, model::CommandRegistry::builtin(), &kernel);
    const DocId id = docs.add(car());
    const auto* assembly = docs.find(id);
    const auto wheel1 = assembly->components()[0].id;
    const auto wheel2 = assembly->components()[1].id;
    const auto axle = assembly->components()[2].id;

    auto linked = docs.editPart(id, axle);
    REQUIRE(linked.size() == 1);
    CHECK(linked.front().code == "part.linked");
    CHECK(linked.front().message.find("Axle.sb") != std::string::npos);
    CHECK(docs.active() == id);
    CHECK(docs.editPart(id, model::ComponentId::generate()).front().code == "assembly.no_component");

    REQUIRE(docs.editPart(id, wheel2).empty());
    Recorder rec(bus);
    // Deleting the component picked to edit the part stops editing it.
    REQUIRE(docs.run(id, "component.delete", {{"component", "Wheel 2"}}).empty());
    CHECK(docs.active() == id);
    CHECK(rec.log.front() == "Active");
    REQUIRE(docs.undo(id));
    CHECK(docs.active() == id);  // it doesn't start again by itself

    REQUIRE(docs.editPart(id, wheel1).empty());
    // Closing the assembly ends it too.
    REQUIRE(docs.close(id));
    CHECK(docs.active().isNil());
}

TEST_CASE("switching designs keeps a part being edited for when the assembly comes back",
          "[plugin][documents][edit]") {
    EventBus bus;
    DocumentList docs(bus, model::CommandRegistry::builtin());
    const DocId id = docs.add(car());
    const DocId other = docs.add(std::make_unique<model::Document>());
    const DocId wheel = docs.find(id)->findPart("Wheel")->id();
    REQUIRE(docs.editPart(id, docs.find(id)->components()[0].id).empty());
    CHECK(docs.active() == wheel);
    REQUIRE(docs.setActive(other));
    CHECK(docs.active() == other);
    CHECK(docs.editedPart(id) == wheel);
    // Picking the part being edited picks its assembly.
    REQUIRE(docs.setActive(wheel));
    CHECK(docs.activeDesign() == id);
    CHECK(docs.active() == wheel);
}
