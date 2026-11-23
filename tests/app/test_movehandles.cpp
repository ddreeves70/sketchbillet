// Tests for the sb.movehandles plugin: arrows on the picked body, and dragging
// them slides it, as one undo step.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/plugin/PluginHost.h"
#include "sb/plugins/Modeling.h"
#include "sb/plugins/MoveHandles.h"
#include "sb/plugins/Viewer.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using Catch::Approx;

namespace {

viewer::IViewer* peekedViewer = nullptr;
void peekViewer(plugin::PluginContext& ctx) {
    ctx.onStart([&ctx] { peekedViewer = ctx.service<viewer::IViewer>(); });
}

// SketchBillet without the desktop app's window: the viewer's service and the move handles.
struct SketchBillet {
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();
    std::unique_ptr<sketch::ISketchSolver> solver = sketch::makePlaneGcsSolver();
    plugin::PluginHost host{kernel.get(), solver.get()};
    DocId doc;
    viewer::IViewer* viewer = nullptr;

    SketchBillet() {
        host.addBuiltin(plugins::modelingPlugin());
        host.addBuiltin(plugins::viewerPlugin());
        host.addBuiltin(plugins::moveHandlesPlugin());
        host.addBuiltin({R"({"id": "test.peek", "name": "Peek", "version": "1", "requires": ["sb.viewer"]})",
                         &peekViewer});
        host.loadAll();
        REQUIRE(host.find("sb.movehandles")->state == plugin::PluginState::Loaded);
        viewer = peekedViewer;
        REQUIRE(viewer);
        doc = host.documents().add(std::make_unique<model::Document>());
    }

    const model::Document& design() { return *host.documents().find(doc); }

    FeatureId run(std::string_view name, const model::Args& args) {
        REQUIRE(host.documents().run(doc, name, args).empty());
        return design().featureAt(design().featureCount() - 1).id();
    }

    void pick(std::vector<FeatureId> ids) { host.selection().set(doc, std::move(ids)); }

    const model::BoxFeature& box(const FeatureId& id) {
        return static_cast<const model::BoxFeature&>(*design().find(id));
    }
};

}  // namespace

TEST_CASE("the handles are arrows along x, y and z from the body's middle, and a square on the ground",
          "[movehandles]") {
    const auto h = plugins::moveHandles({{0, 0, 0}, {40, 20, 10}});
    REQUIRE(h.size() == 4);
    CHECK(h[0].id == "x");
    CHECK(h[1].id == "y");
    CHECK(h[2].id == "z");
    CHECK(h[3].id == "xy");
    CHECK(h[3].kind == viewer::HandleKind::Square);
    for (const auto& a : h) {
        CHECK(a.at.x == 20);
        CHECK(a.at.y == 10);
        CHECK(a.at.z == 5);
    }
    CHECK(h[0].length == plugins::kArrowPixels);  // the same size on the screen, whatever the zoom
    CHECK(h[3].length == plugins::kSquarePixels);
    CHECK(h[0].r > h[0].g);  // x is red, y green, z blue, like the axes
    CHECK(h[1].g > h[1].r);
    CHECK(h[2].b > h[2].r);

    CHECK(plugins::snapSlide({4.4, -0.6, 2.5}, LengthUnit::Millimeter).x == 4);
    CHECK(plugins::snapSlide({4.4, -0.6, 2.5}, LengthUnit::Millimeter).y == -1);
    CHECK(plugins::snapSlide({0.4, 0, 0}, LengthUnit::Centimeter).x == 0);
    CHECK(plugins::snapSlide({3, 0, 0}, LengthUnit::Inch).x == Approx(2.54));  // 0.1 in steps
}

TEST_CASE("handles show on one picked body, and not on a sketch, nothing or two bodies", "[movehandles]") {
    SketchBillet k;
    const auto box = k.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
    const auto other =
        k.run("feature.add_box", {{"corner", kernel::Vec3{100, 0, 0}}, {"size", kernel::Vec3{5, 5, 5}}});
    const auto sketch = k.run("feature.add_sketch", {});
    CHECK(k.viewer->handles().empty());
    k.pick({box});
    REQUIRE(k.viewer->handles().size() == 4);
    CHECK(k.viewer->handles()[0].at.x == Approx(20));
    k.pick({box, other});
    CHECK(k.viewer->handles().empty());
    k.pick({sketch});
    CHECK(k.viewer->handles().empty());
    k.pick({});
    CHECK(k.viewer->handles().empty());
}

TEST_CASE("dragging an arrow previews the box moved in whole mm, and dropping slides it as one step",
          "[movehandles]") {
    SketchBillet k;
    const auto box = k.run("feature.add_box", {{"size", kernel::Vec3{40, 20, 10}}});
    k.pick({box});
    const auto before = k.design().undoLabel();
    auto* user = k.viewer->handleUser();
    REQUIRE(user);
    user->dragged("x", {2.3, 0, 0});
    // Only the drawing moves while dragging, so it's quick however big the body is.
    REQUIRE(k.viewer->preview());
    CHECK(k.viewer->preview()->body == box);
    CHECK(k.viewer->preview()->motion.offset.x == 2);
    CHECK(k.box(box).corner().x == 0);
    CHECK(k.design().undoLabel() == before);
    user->dragged("x", {6.8, 0, 0});
    CHECK(k.viewer->preview()->motion.offset.x == 7);
    CHECK(k.viewer->handles()[0].at.x == Approx(27));  // the handles follow the drawing
    user->dragged("x", {7.1, 0, 0});                   // still 7
    user->dropped("x", {7.1, 0, 0});
    CHECK_FALSE(k.viewer->preview());
    CHECK(k.box(box).corner().x == 7);
    CHECK(k.design().undoLabel() == "Move Box1");
    CHECK(k.viewer->handles()[0].at.x == Approx(27));  // where it is now

    // A second drag is its own undo step.
    user->dragged("xy", {1, 2, 0});
    user->dropped("xy", {1, 2, 0});
    CHECK(k.box(box).corner().y == 2);
    REQUIRE(k.host.documents().undo(k.doc));
    CHECK(k.box(box).corner().x == 7);
    CHECK(k.box(box).corner().y == 0);
    REQUIRE(k.host.documents().undo(k.doc));  // the whole first drag
    CHECK(k.box(box).corner().x == 0);

    // A drag that ends where it started changes nothing.
    user->dragged("x", {0.3, 0, 0});
    user->dropped("x", {0.3, 0, 0});
    CHECK(k.design().undoLabel() == before);
}

TEST_CASE("dragging another body adds a Move step on dropping, which is then what's picked",
          "[movehandles]") {
    SketchBillet k;
    const auto a = k.run("feature.add_box", {{"size", kernel::Vec3{10, 10, 10}}});
    const auto b = k.run("feature.add_cylinder", {{"radius", 2.0}, {"height", 20.0}});
    const auto cut = k.run("feature.add_boolean", {{"operation", std::string("cut")}, {"a", a}, {"b", b}});
    k.pick({cut});
    REQUIRE(k.viewer->handles().size() == 4);
    auto* user = k.viewer->handleUser();
    user->dragged("z", {0, 0, 3});
    CHECK(k.design().featureCount() == 3);  // not yet
    CHECK(k.viewer->preview()->body == cut);
    user->dragged("z", {0, 0, 5});
    user->dropped("z", {0, 0, 5});
    REQUIRE(k.design().featureCount() == 4);
    const auto& move = k.design().featureAt(3);
    CHECK(move.type() == model::FeatureType::Move);
    CHECK(static_cast<const model::MoveFeature&>(move).motion().offset.z == 5);
    CHECK(k.host.selection().features() == std::vector<FeatureId>{move.id()});
    REQUIRE(k.viewer->handles().size() == 4);  // on the Move now
    REQUIRE(k.host.documents().undo(k.doc));
    CHECK(k.design().featureCount() == 3);  // the drag, Move step and all, was one step
}

TEST_CASE("Escape during a drag puts the drawing back; the design never changed", "[movehandles]") {
    SketchBillet k;
    const auto a = k.run("feature.add_box", {{"size", kernel::Vec3{10, 10, 10}}});
    const auto b = k.run("feature.add_cylinder", {{"radius", 2.0}, {"height", 20.0}});
    const auto cut = k.run("feature.add_boolean", {{"operation", std::string("cut")}, {"a", a}, {"b", b}});
    k.pick({cut});
    auto* user = k.viewer->handleUser();
    user->dragged("x", {4, 0, 0});
    user->dragged("x", {9, 0, 0});
    CHECK(k.viewer->preview());
    user->cancelled();
    CHECK_FALSE(k.viewer->preview());
    CHECK(k.design().featureCount() == 3);
    CHECK(k.host.selection().features() == std::vector<FeatureId>{cut});
    REQUIRE(k.viewer->handles().size() == 4);
    CHECK(k.viewer->handles()[0].at.x == Approx(5));  // back on the body

    k.pick({a});
    user->dragged("y", {0, 6, 0});
    user->cancelled();
    CHECK(k.box(a).corner().y == 0);
    CHECK_FALSE(k.design().canRedo());  // nothing was done, so nothing to redo
    // Dropping after Escape does nothing.
    user->dropped("y", {0, 6, 0});
    CHECK(k.box(a).corner().y == 0);
}

TEST_CASE("a free component has handles; dragging previews it and dropping places it, as one step",
          "[movehandles][assembly]") {
    SketchBillet k;
    auto& docs = k.host.documents();
    REQUIRE(docs.run(k.doc, "assembly.new", {}).empty());
    auto wheel = std::make_unique<model::Document>(model::DocKind::Part, "Wheel");
    REQUIRE(wheel
                ->execute(std::make_unique<model::AddFeatureCommand>(
                    std::make_unique<model::BoxFeature>("Box1", kernel::Vec3{}, kernel::Vec3{10, 20, 4})))
                .empty());
    // Wheel 1 (grounded, the first), and Wheel 2 at x 50, turned a quarter about z.
    REQUIRE(docs.execute(k.doc, std::make_unique<model::InsertPartCommand>(std::move(wheel), 1)).empty());
    const auto wheelPart = k.design().components()[0].part;
    REQUIRE(docs.execute(k.doc, std::make_unique<model::AddComponentsCommand>(
                                    wheelPart, 1, kernel::Placement{{50, 0, 0}, {0, 1, 0}, {0, 0, 1}}))
                .empty());
    const auto w1 = k.design().components()[0].id;
    const auto w2 = k.design().components()[1].id;

    // Grounded: none. Free: around it, where it's placed (turned, 20 by 10 mm).
    k.host.selection().setComponents(k.doc, {w1});
    CHECK(k.viewer->handles().empty());
    k.host.selection().setComponents(k.doc, {w2});
    REQUIRE(k.viewer->handles().size() == 4);
    CHECK(k.viewer->handles()[0].at.x == Approx(40));  // x from 30 to 50
    CHECK(k.viewer->handles()[0].at.y == Approx(5));   // y from 0 to 10
    CHECK(k.viewer->handles()[0].at.z == Approx(2));

    // Dragging only draws it moved.
    const auto label = k.design().undoLabel();
    auto* user = k.viewer->handleUser();
    REQUIRE(user);
    user->dragged("x", {5.4, 0, 0});
    REQUIRE(k.viewer->preview());
    CHECK(k.viewer->preview()->component == w2);
    CHECK(k.viewer->preview()->body.isNil());
    CHECK(k.viewer->preview()->motion.offset.x == 5);
    CHECK(k.viewer->handles()[0].at.x == Approx(45));  // the handles follow
    CHECK(k.design().findComponent(w2)->placement.origin.x == 50);
    CHECK(k.design().undoLabel() == label);
    user->dragged("xy", {7, -3, 0});
    user->dropped("xy", {7, -3, 0});
    CHECK_FALSE(k.viewer->preview());
    const auto& placed = k.design().findComponent(w2)->placement;
    CHECK(placed.origin.x == 57);
    CHECK(placed.origin.y == -3);
    CHECK(placed.xAxis.y == Approx(1));  // still facing the same way
    CHECK(k.design().undoLabel() == "Move Wheel 2");
    CHECK(k.host.selection().components() == std::vector{w2});
    REQUIRE(k.viewer->handles().size() == 4);
    CHECK(k.viewer->handles()[0].at.x == Approx(47));
    REQUIRE(docs.undo(k.doc));  // the whole drag
    CHECK(k.design().findComponent(w2)->placement.origin.x == 50);

    // Escape: nothing changes.
    user->dragged("z", {0, 0, 9});
    user->cancelled();
    CHECK_FALSE(k.viewer->preview());
    CHECK(k.design().findComponent(w2)->placement.origin.z == 0);
    CHECK(k.viewer->handles()[0].at.z == Approx(2));
}
