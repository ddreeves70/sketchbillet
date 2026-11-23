// Tests for the shared selection (sb_plugin).

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include "sb/model/CommandRegistry.h"
#include "sb/plugin/DocumentList.h"
#include "sb/plugin/Events.h"
#include "sb/plugin/Selection.h"

using namespace sb;
using namespace sb::plugin;

namespace {

struct Fixture {
    EventBus bus;
    DocumentList docs{bus, model::CommandRegistry::builtin()};
    Selection selection{bus, docs};
    std::vector<SelectionChanged> heard;
    Subscription sub =
        bus.subscribe<SelectionChanged>([this](const SelectionChanged& e) { heard.push_back(e); });
    DocId doc = docs.add(std::make_unique<model::Document>());

    FeatureId addBox() {
        REQUIRE(docs.run(doc, "feature.add_box", {{"size", kernel::Vec3{1, 1, 1}}}).empty());
        const auto* d = docs.find(doc);
        return d->featureAt(d->featureCount() - 1).id();
    }
};

}  // namespace

TEST_CASE("selecting steps tells every listener once", "[plugin][selection]") {
    Fixture f;
    const auto a = f.addBox();
    const auto b = f.addBox();

    f.selection.set(f.doc, {b, a});
    REQUIRE(f.heard.size() == 1);
    CHECK(f.heard[0].doc == f.doc);
    CHECK(f.heard[0].features == std::vector<FeatureId>{b, a});  // picking order kept
    CHECK(f.selection.contains(a));

    f.selection.set(f.doc, {b, a});  // the same again: no event
    CHECK(f.heard.size() == 1);

    f.selection.clear();
    REQUIRE(f.heard.size() == 2);
    CHECK(f.heard[1].doc.isNil());
    CHECK(f.selection.features().empty());
}

TEST_CASE("only real steps of the design can be selected", "[plugin][selection]") {
    Fixture f;
    const auto a = f.addBox();
    f.selection.set(f.doc, {a, FeatureId::generate(), a});
    CHECK(f.selection.features() == std::vector<FeatureId>{a});  // unknown and repeated ids dropped
    f.selection.set(DocId::generate(), {a});                     // not an open design
    CHECK(f.selection.features().empty());
    CHECK(f.selection.doc().isNil());
}

TEST_CASE("deleting or undoing a selected step drops it from the selection", "[plugin][selection]") {
    Fixture f;
    const auto a = f.addBox();
    const auto b = f.addBox();
    f.selection.set(f.doc, {a, b});

    REQUIRE(f.docs.undo(f.doc));  // b is undone away
    CHECK(f.selection.features() == std::vector<FeatureId>{a});
    REQUIRE(f.docs.run(f.doc, "feature.delete", {{"feature", a}}).empty());
    CHECK(f.selection.features().empty());
    CHECK(f.heard.back().features.empty());
}

TEST_CASE("closing the design clears the selection", "[plugin][selection]") {
    Fixture f;
    const auto a = f.addBox();
    f.selection.set(f.doc, {a});
    f.docs.close(f.doc);
    CHECK(f.selection.features().empty());
    CHECK(f.selection.doc().isNil());
}

TEST_CASE("the design itself can be picked, for its settings; picking steps un-picks it",
          "[plugin][selection]") {
    Fixture f;
    const auto a = f.addBox();
    f.selection.set(f.doc, {a});
    f.selection.pickDesign(f.doc);
    CHECK(f.selection.designPicked());
    CHECK(f.selection.doc() == f.doc);
    CHECK(f.selection.features().empty());
    CHECK(f.heard.back().design);
    const auto heard = f.heard.size();
    f.selection.pickDesign(f.doc);  // no change, no event
    CHECK(f.heard.size() == heard);

    // A change to the design keeps it picked.
    REQUIRE(f.docs.run(f.doc, "feature.delete", {{"feature", a}}).empty());
    CHECK(f.selection.designPicked());

    f.selection.set(f.doc, {f.addBox()});
    CHECK_FALSE(f.selection.designPicked());
    CHECK_FALSE(f.heard.back().design);

    f.selection.pickDesign(DocId::generate());  // not an open design
    CHECK_FALSE(f.selection.designPicked());
    CHECK(f.selection.doc().isNil());

    f.selection.pickDesign(f.doc);
    f.docs.close(f.doc);
    CHECK_FALSE(f.selection.designPicked());
}

TEST_CASE("an assembly's components can be picked; steps and deleted components drop them",
          "[plugin][selection][assembly]") {
    Fixture f;
    REQUIRE(f.docs.run(f.doc, "assembly.new", {}).empty());
    REQUIRE(f.docs.run(f.doc, "component.new", {{"name", std::string("Wheel")}}).empty());
    REQUIRE(f.docs.run(f.doc, "component.add", {{"part", std::string("Wheel")}, {"count", std::int64_t{2}}})
                .empty());
    const auto components = f.docs.find(f.doc)->components();
    REQUIRE(components.size() == 3);
    const auto wheel1 = components[0].id;
    const auto wheel3 = components[2].id;

    f.selection.setComponents(f.doc, {wheel3, model::ComponentId::generate(), wheel1, wheel3});
    CHECK(f.selection.components() == std::vector{wheel3, wheel1});  // in order, without strangers or repeats
    CHECK(f.selection.containsComponent(wheel1));
    CHECK(f.selection.doc() == f.doc);
    REQUIRE(f.heard.size() == 1);
    CHECK(f.heard.back().components == std::vector{wheel3, wheel1});
    CHECK(f.heard.back().features.empty());
    f.selection.setComponents(f.doc, {wheel3, wheel1});  // the same: nothing to tell
    CHECK(f.heard.size() == 1);

    // Deleting one drops it; undo doesn't bring it back into the pick.
    REQUIRE(f.docs.run(f.doc, "component.delete", {{"component", std::string("Wheel 3")}}).empty());
    CHECK(f.selection.components() == std::vector{wheel1});

    // Picking the design's settings, or nothing, un-picks components.
    f.selection.pickDesign(f.doc);
    CHECK(f.selection.components().empty());
    f.selection.setComponents(f.doc, {wheel1});
    f.selection.clear();
    CHECK(f.selection.components().empty());
    CHECK(f.selection.doc().isNil());
}

TEST_CASE("an assembly's joints can be picked, in place of components; deleted ones drop out",
          "[plugin][selection][assembly][joints]") {
    Fixture f;
    REQUIRE(f.docs.run(f.doc, "assembly.new", {}).empty());
    REQUIRE(f.docs.run(f.doc, "component.new", {{"name", std::string("Wheel")}}).empty());
    REQUIRE(f.docs.run(f.doc, "component.add", {{"part", std::string("Wheel")}, {"count", std::int64_t{2}}})
                .empty());
    const auto components = f.docs.find(f.doc)->components();
    auto side = [](const model::Component& c) {
        model::JointOrigin o;
        o.component = c.id;
        return o;
    };
    REQUIRE(f.docs.run(f.doc, "joint.add", {{"a", side(components[0])}, {"b", side(components[1])}}).empty());
    REQUIRE(f.docs.run(f.doc, "joint.add", {{"a", side(components[0])}, {"b", side(components[2])}}).empty());
    const auto joint1 = f.docs.find(f.doc)->joints()[0].id;
    const auto joint2 = f.docs.find(f.doc)->joints()[1].id;

    f.selection.setComponents(f.doc, {components[1].id});
    f.selection.setJoints(f.doc, {joint2, JointId::generate(), joint1});
    CHECK(f.selection.joints() == std::vector{joint2, joint1});
    CHECK(f.selection.components().empty());  // one kind at a time
    CHECK(f.selection.containsJoint(joint1));
    CHECK(f.heard.back().joints == std::vector{joint2, joint1});
    const auto heard = f.heard.size();
    f.selection.setJoints(f.doc, {joint2, joint1});  // the same: nothing to tell
    CHECK(f.heard.size() == heard);

    REQUIRE(f.docs.run(f.doc, "joint.delete", {{"joint", std::string("Joint2")}}).empty());
    CHECK(f.selection.joints() == std::vector{joint1});

    f.selection.setComponents(f.doc, {components[0].id});
    CHECK(f.selection.joints().empty());
    f.selection.setJoints(f.doc, {joint1});
    f.selection.set(f.doc, {});
    CHECK(f.selection.joints().empty());
}
