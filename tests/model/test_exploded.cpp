// Exploded views of an assembly: their steps, the commands that change them,
// and how far each component moves.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <memory>

#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;

namespace {

struct Car {
    Document doc{DocKind::Part, "Car"};
    ComponentId body, wheel1, wheel2;

    Car() {
        REQUIRE(doc.execute(std::make_unique<MakeAssemblyCommand>()).empty());
        auto insert = [&](const char* name) {
            auto part = std::make_unique<Document>(DocKind::Part, name);
            REQUIRE(part->execute(std::make_unique<AddFeatureCommand>(std::make_unique<BoxFeature>(
                                      "Box1", kernel::Vec3{}, kernel::Vec3{10, 10, 10})))
                        .empty());
            auto cmd = std::make_unique<InsertPartCommand>(std::move(part), 1);
            auto* raw = cmd.get();
            REQUIRE(doc.execute(std::move(cmd)).empty());
            return raw->added().front();
        };
        body = insert("Body");
        wheel1 = insert("Wheel");
        wheel2 = insert("Spare");
    }
};

}  // namespace

TEST_CASE("an exploded view slides components step by step, and a component in two steps moves by both",
          "[model][assembly][explode]") {
    Car car;
    auto add = std::make_unique<AddExplodedViewCommand>(ExplodedView{});
    auto* added = add.get();
    REQUIRE(car.doc.execute(std::move(add)).empty());
    CHECK(added->view().name == "Exploded 1");
    CHECK_FALSE(added->view().id.isNil());
    CHECK(car.doc.undoLabel() == "Add Exploded 1");
    REQUIRE(car.doc.explodedViews().size() == 1);
    CHECK(car.doc.findExplodedView(std::string_view("Exploded 1")) == &car.doc.explodedViews()[0]);
    CHECK(car.doc.uniqueExplodedViewName() == "Exploded 2");

    // Both wheels 20 mm to the front (a direction of any length), then wheel 2
    // 15 mm to the right.
    ExplodedView v = car.doc.explodedViews()[0];
    v.steps.push_back({{}, {car.wheel1, car.wheel2}, {0, -5, 0}, 20});
    v.steps.push_back({{}, {car.wheel2}, {1, 0, 0}, 15});
    auto set = std::make_unique<SetExplodedViewCommand>(v);
    auto* changed = set.get();
    REQUIRE(car.doc.execute(std::move(set)).empty());
    CHECK(car.doc.undoLabel() == "Change Exploded 1");
    const auto& view = car.doc.explodedViews()[0];
    REQUIRE(view.steps.size() == 2);
    CHECK_FALSE(view.steps[0].id.isNil());
    CHECK(view.steps[0].id == changed->view().steps[0].id);
    CHECK(view.steps[0].id != view.steps[1].id);

    const auto body = explodePath(view, car.body);
    REQUIRE(body.size() == 1);  // it doesn't move
    CHECK(body[0].x == 0);
    const auto one = explodePath(view, car.wheel1);
    REQUIRE(one.size() == 2);
    CHECK(one[1].y == Approx(-20));
    CHECK(one[1].x == 0);
    const auto two = explodePath(view, car.wheel2);
    REQUIRE(two.size() == 3);
    CHECK(two[1].y == Approx(-20));
    CHECK(two[2].x == Approx(15));
    CHECK(two[2].y == Approx(-20));

    // Exploding never moves the components themselves.
    CHECK(car.doc.findComponent(car.wheel1)->placement.origin.y == 0);

    // Undo and redo keep the steps' ids.
    const auto firstStep = view.steps[0].id;
    REQUIRE(car.doc.undo());
    CHECK(car.doc.explodedViews()[0].steps.empty());
    REQUIRE(car.doc.redo());
    CHECK(car.doc.explodedViews()[0].steps[0].id == firstStep);
}

TEST_CASE("an exploded view's name, steps and components are checked", "[model][assembly][explode]") {
    Car car;
    REQUIRE(car.doc.execute(std::make_unique<AddExplodedViewCommand>(ExplodedView{{}, "Apart", {}})).empty());
    auto taken = car.doc.execute(std::make_unique<AddExplodedViewCommand>(ExplodedView{{}, "Apart", {}}));
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].code == "explode.name");
    CHECK(taken[0].message == "There's an exploded view called Apart already. Give this one another name.");

    ExplodedView v = car.doc.explodedViews()[0];
    v.steps.push_back({{}, {car.wheel1}, {0, 0, 0}, 10});
    auto bad = car.doc.execute(std::make_unique<SetExplodedViewCommand>(v));
    REQUIRE(bad.size() == 1);
    CHECK(bad[0].code == "explode.direction");
    CHECK(bad[0].message == "Step 1 of Apart needs a direction to slide in, like 0,0,1 for up.");
    v.steps[0] = {{}, {ComponentId::generate()}, {0, 0, 1}, 10};
    CHECK(car.doc.execute(std::make_unique<SetExplodedViewCommand>(v))[0].code == "assembly.no_component");
    v.steps[0] = {{}, {car.wheel1}, {0, 0, 1}, std::numeric_limits<double>::infinity()};
    CHECK(car.doc.execute(std::make_unique<SetExplodedViewCommand>(v))[0].code == "explode.distance");
    v.name.clear();
    CHECK(car.doc.execute(std::make_unique<SetExplodedViewCommand>(v))[0].code == "explode.name");

    // In a part: no exploded views.
    Document part(DocKind::Part, "Plate");
    CHECK(part.execute(std::make_unique<AddExplodedViewCommand>(ExplodedView{}))[0].code ==
          "assembly.not_assembly");
}

TEST_CASE("deleting a component takes it out of exploded views' steps; undo puts it back",
          "[model][assembly][explode]") {
    Car car;
    ExplodedView v{{}, "Apart", {{{}, {car.wheel1, car.wheel2}, {0, 0, 1}, 10}}};
    REQUIRE(car.doc.execute(std::make_unique<AddExplodedViewCommand>(v)).empty());
    REQUIRE(car.doc.execute(std::make_unique<DeleteComponentCommand>(car.wheel1)).empty());
    REQUIRE(car.doc.explodedViews()[0].steps[0].components.size() == 1);
    CHECK(car.doc.explodedViews()[0].steps[0].components[0] == car.wheel2);
    REQUIRE(car.doc.undo());
    REQUIRE(car.doc.explodedViews()[0].steps[0].components.size() == 2);
    CHECK(car.doc.explodedViews()[0].steps[0].components[0] == car.wheel1);

    // Deleting the view, and undoing it.
    const auto id = car.doc.explodedViews()[0].id;
    REQUIRE(car.doc.execute(std::make_unique<DeleteExplodedViewCommand>(id)).empty());
    CHECK(car.doc.undoLabel() == "Delete Apart");
    CHECK(car.doc.explodedViews().empty());
    CHECK(car.doc.execute(std::make_unique<DeleteExplodedViewCommand>(id))[0].code == "explode.missing");
    REQUIRE(car.doc.undo());
    CHECK(car.doc.findExplodedView(id));
}
