// Tests for the Measure tool (sb.measure): Tools > Measure, the Measure panel,
// and the line it draws between the nearest points; and the assembly checks,
// Check for Overlaps and Balance Point.

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>

#include <cmath>
#include <memory>

#include "AppSettings.h"
#include "AppUi.h"
#include "Shell.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/model/Naming.h"
#include "sb/plugin/PluginHost.h"
#include "sb/plugins/Measure.h"
#include "sb/plugins/Modeling.h"
#include "sb/plugins/Viewer.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;

namespace {

void settle() {
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

struct App {
    QTemporaryDir dir;
    QSettings settings{dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat};
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();
    std::unique_ptr<sketch::ISketchSolver> solver = sketch::makePlaneGcsSolver();
    app::AppSettings pluginSettings{settings};
    app::AppUi ui;
    plugin::PluginHost host{kernel.get(), solver.get()};
    std::unique_ptr<app::Shell> shell;

    App() {
        host.provide<ui::UiRegistry>(ui);
        host.setSettings(pluginSettings);
        host.addBuiltin(plugins::modelingPlugin());
        host.addBuiltin(plugins::viewerPlugin());
        host.addBuiltin(plugins::measurePlugin());
        host.loadAll();
        shell = std::make_unique<app::Shell>(host, ui, settings);
        shell->askUnsaved = [](const QString&) { return app::Shell::Unsaved::Discard; };
        shell->show();
        settle();
        // A 60 x 30 x 4 mm plate.
        REQUIRE(host.documents()
                    .run(shell->activeDesign(), "feature.add_box", {{"size", kernel::Vec3{60, 30, 4}}})
                    .empty());
        settle();
    }

    DocId design() { return shell->activeDesign(); }
    QWidget* panel() { return shell->panelWidget(QStringLiteral("sb.measure.panel")); }
    QString text() {
        auto* label = panel()->findChild<QLabel*>(QStringLiteral("sb.measure.result"));
        REQUIRE(label);
        return label->text();
    }
    const plugin::Overlay* overlay(const char* name) {
        return host.overlays().find(std::string("sb.measure.") + name);
    }
    // The plate's face, edge or point whose middle is at `p`, as a click picks it.
    plugin::PickedElement pick(kernel::TopoKind kind, kernel::Vec3 p) {
        const auto& doc = *host.documents().find(design());
        const auto id = doc.featureAt(0).id();
        const auto& names = *host.documents().lastRebuild(design())->result(id)->names;
        const auto& list = names.elements.of(kind);
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& c = list[i].center;
            if (std::abs(c.x - p.x) < 1e-9 && std::abs(c.y - p.y) < 1e-9 && std::abs(c.z - p.z) < 1e-9)
                return {id, model::referTo(names, kind, static_cast<int>(i))};
        }
        FAIL("nothing at " << p.x << ", " << p.y << ", " << p.z);
        return {};
    }
    void picked(std::vector<plugin::PickedElement> picks) {
        host.selection().setElements(design(), std::move(picks));
        settle();
    }
};

}  // namespace

TEST_CASE("Tools > Measure picks faces, edges and points, and shows what they measure", "[app][measure]") {
    App app;
    auto* measure = app.shell->action("sb.measure.measure");
    REQUIRE(measure);
    CHECK(measure->shortcut().toString() == QStringLiteral("Ctrl+Shift+I"));
    REQUIRE(app.panel());
    CHECK_FALSE(app.panel()->isVisible());  // closed until Measure is clicked

    measure->trigger();
    settle();
    CHECK(app.host.selection().pickMode() == plugin::PickMode::Measure);
    CHECK(measure->isChecked());
    CHECK(app.panel()->isVisible());
    CHECK(app.text() == QStringLiteral("Click a face, edge or point in the view to measure it."));
    CHECK_FALSE(app.overlay("line")->visible);

    // One face: its size, and what to do next.
    app.picked({app.pick(kernel::TopoKind::Face, {30, 15, 0})});
    CHECK(app.text() == QStringLiteral("Box1, flat face: area 1800.00 mm^2\n\n"
                                       "Click another one to measure between them."));
    CHECK_FALSE(app.overlay("line")->visible);

    // And a second: the distance, with a line between the nearest points.
    app.picked(
        {app.pick(kernel::TopoKind::Face, {30, 15, 0}), app.pick(kernel::TopoKind::Vertex, {60, 30, 4})});
    CHECK(app.text() == QString::fromUtf8("Box1, flat face: area 1800.00 mm^2\n"
                                          "Box1, point: at x 60, y 30, z 4 mm\n"
                                          "Distance: 4 mm (x 0, y 0, z 4 mm)"));
    const auto* line = app.overlay("line");
    REQUIRE(line);
    CHECK(line->visible);
    REQUIRE(line->points.size() == 2);
    CHECK(line->points[0].z == 0.0);
    CHECK(line->points[1].z == 4.0);
    REQUIRE(app.overlay("label")->texts.size() == 1);
    CHECK(app.overlay("label")->texts[0] == "4 mm");
    CHECK(app.overlay("ends")->visible);

    // Two faces that meet: they touch, so no line, and the angle between them.
    app.picked({app.pick(kernel::TopoKind::Face, {30, 0, 2}), app.pick(kernel::TopoKind::Face, {0, 15, 2})});
    CHECK(app.text().endsWith(QString::fromUtf8("Distance: 0 mm (they touch)\nAngle: 90°")));
    CHECK_FALSE(app.overlay("line")->visible);

    // A change to the design measures again.
    app.picked(
        {app.pick(kernel::TopoKind::Face, {30, 15, 0}), app.pick(kernel::TopoKind::Face, {30, 15, 4})});
    CHECK(app.text().contains(QStringLiteral("Distance: 4 mm")));

    // Done stops measuring: bodies are picked again, and the panel and line go away.
    auto* done = app.panel()->findChild<QPushButton*>(QStringLiteral("sb.measure.done"));
    REQUIRE(done);
    done->click();
    settle();
    CHECK(app.host.selection().pickMode() == plugin::PickMode::Bodies);
    CHECK_FALSE(measure->isChecked());
    CHECK_FALSE(app.panel()->isVisible());
    CHECK_FALSE(app.overlay("line")->visible);
    CHECK_FALSE(app.overlay("label")->visible);
}

TEST_CASE("Measure stops when another tool picks something else", "[app][measure]") {
    App app;
    app.shell->action("sb.measure.measure")->trigger();
    settle();
    app.picked(
        {app.pick(kernel::TopoKind::Face, {30, 15, 0}), app.pick(kernel::TopoKind::Face, {30, 15, 4})});
    REQUIRE(app.overlay("line")->visible);
    // Picking edges for a fillet, say.
    app.host.selection().setPickMode(plugin::PickMode::Edges);
    settle();
    CHECK_FALSE(app.panel()->isVisible());
    CHECK_FALSE(app.overlay("line")->visible);
    // And clicking Measure again while measuring stops it too.
    app.shell->action("sb.measure.measure")->trigger();
    settle();
    CHECK(app.panel()->isVisible());
    app.shell->action("sb.measure.measure")->trigger();
    settle();
    CHECK(app.host.selection().pickMode() == plugin::PickMode::Bodies);
    CHECK_FALSE(app.panel()->isVisible());
}

TEST_CASE("Measure's line and label", "[app][measure]") {
    model::Measurement m;
    auto o = plugins::measureOverlays(m, LengthUnit::Millimeter);
    CHECK_FALSE(o.line.visible);
    m.distance = 25.4;
    m.from = {0, 0, 0};
    m.to = {25.4, 0, 0};
    o = plugins::measureOverlays(m, LengthUnit::Inch);
    CHECK(o.line.visible);
    CHECK(o.ends.points.size() == 2);
    REQUIRE(o.label.points.size() == 1);
    CHECK(o.label.points[0].x == 12.7);
    CHECK(o.label.texts == std::vector<std::string>{"1 in"});
}

TEST_CASE("Check for Overlaps shows where parts overlap in red, and checks again as parts move",
          "[app][measure][assembly]") {
    App app;
    auto& docs = app.host.documents();
    // An assembly instead of the plate: a 20 x 20 x 10 mm base, and a 10 mm cube
    // with a corner 5 x 5 x 5 mm into it.
    REQUIRE(docs.undo(app.design()));
    REQUIRE(docs.run(app.design(), "assembly.new", {}).empty());
    auto part = [](const char* name, kernel::Vec3 size) {
        auto p = std::make_unique<model::Document>(model::DocKind::Part, name);
        REQUIRE(p->execute(std::make_unique<model::AddFeatureCommand>(
                               std::make_unique<model::BoxFeature>("Box1", kernel::Vec3{}, size)))
                    .empty());
        return p;
    };
    REQUIRE(
        docs.execute(app.design(), std::make_unique<model::InsertPartCommand>(part("Base", {20, 20, 10}), 1))
            .empty());
    kernel::Placement corner;
    corner.origin = {15, 15, 5};
    REQUIRE(docs.execute(app.design(),
                         std::make_unique<model::InsertPartCommand>(part("Cube", {10, 10, 10}), 1, corner))
                .empty());
    settle();
    const auto base = docs.find(app.design())->components()[0].id;
    const auto cube = docs.find(app.design())->components()[1].id;

    auto* overlaps = app.shell->action("sb.measure.overlaps");
    auto* balance = app.shell->action("sb.measure.balance");
    REQUIRE(overlaps);
    REQUIRE(balance);
    CHECK(overlaps->isEnabled());
    overlaps->trigger();
    settle();
    CHECK(overlaps->isChecked());
    auto* panel = app.shell->panelWidget(QStringLiteral("sb.measure.checks.panel"));
    REQUIRE(panel);
    CHECK(panel->isVisible());
    auto text = [&] { return panel->findChild<QLabel*>(QStringLiteral("sb.measure.checks.result"))->text(); };
    CHECK(
        text() ==
        QStringLiteral(
            "Base 1 and Cube 1 overlap by 125.00 mm\u00B3, around x 17.5, y 17.5, z 7.5 mm. Parts can't "
            "be in the same place, so they won't go together. Move one of them, or change one of "
            "their parts so they fit.\nWhere they overlap is red in the view.\nIf two are meant to overlap, "
            "like a pin pushed into a hole a bit smaller than it, pick both and click Press fit."));
    // In red, over the parts: a 5 mm cube, in triangles, inside 15 to 20 mm.
    const auto* red = app.overlay("overlaps");
    REQUIRE(red);
    CHECK(red->visible);
    CHECK(red->shape == plugin::OverlayShape::Triangles);
    CHECK(red->overBodies);
    REQUIRE(red->points.size() >= 36);  // 6 squares, 2 triangles each
    CHECK(red->points.size() % 3 == 0);
    for (const auto& p : red->points) {
        CHECK(p.x >= 15 - 1e-4);
        CHECK(p.x <= 20 + 1e-4);
        CHECK(p.z >= 5 - 1e-4);
        CHECK(p.z <= 10 + 1e-4);
    }

    // Moved up onto the base, it only touches: checked again, and fine.
    // Meant to overlap? Pick both, and mark them as a press fit: no red then.
    auto* pressFit = app.shell->action("sb.measure.press_fit");
    REQUIRE(pressFit);
    CHECK_FALSE(pressFit->isEnabled());  // pick two first
    app.host.selection().setComponents(app.design(), {base, cube});
    settle();
    CHECK(pressFit->isEnabled());
    CHECK_FALSE(pressFit->isChecked());
    pressFit->trigger();
    settle();
    CHECK(pressFit->isChecked());
    CHECK(docs.find(app.design())->findPressFit(cube, base));
    CHECK(docs.find(app.design())->undoLabel() == "Mark press fit");
    CHECK(text() ==
          QStringLiteral("Untitled: no components overlap, but for 1 press fit (2 components, 1 pair "
                         "close enough to check).\nBase 1 and Cube 1 overlap by 125.00 mm\u00B3, "
                         "around x 17.5, y 17.5, z 7.5 mm: they're a press fit, so that's meant."));
    CHECK_FALSE(app.overlay("overlaps")->visible);
    pressFit->trigger();  // not after all
    settle();
    CHECK_FALSE(pressFit->isChecked());
    CHECK(docs.find(app.design())->pressFits().empty());
    CHECK(app.overlay("overlaps")->visible);
    app.host.selection().clear();
    settle();

    kernel::Placement onTop;
    onTop.origin = {15, 15, 10};
    REQUIRE(docs.execute(app.design(), std::make_unique<model::PlaceComponentCommand>(cube, onTop)).empty());
    settle();
    CHECK(text() ==
          QStringLiteral("Untitled: no components overlap (2 components, 1 pair close enough to check)."));
    CHECK_FALSE(app.overlay("overlaps")->visible);

    // Balance Point too: the mass, and the dot. The base is 4.96 g with its
    // middle at 10, 10, 5 mm; the cube 1.24 g at 20, 20, 15 mm.
    balance->trigger();
    settle();
    CHECK(balance->isChecked());
    CHECK(text() ==
          QStringLiteral("Untitled: no components overlap (2 components, 1 pair close enough to check).\n\n"
                         "Mass: 6.20 g, so it weighs 0.061 N.\n"
                         "Balance point (the dot): x 12, y 12, z 7 mm. It balances there, like on a pin."));
    const auto* dot = app.overlay("balance.dot");
    REQUIRE(dot);
    CHECK(dot->visible);
    CHECK(dot->overBodies);
    REQUIRE(dot->points.size() == 1);
    CHECK(std::abs(dot->points[0].x - 12) < 1e-6);
    CHECK(std::abs(dot->points[0].z - 7) < 1e-6);
    CHECK(app.overlay("balance.ring")->visible);

    // Done: both off, the panel and the dot gone.
    panel->findChild<QPushButton*>(QStringLiteral("sb.measure.checks.done"))->click();
    settle();
    CHECK_FALSE(overlaps->isChecked());
    CHECK_FALSE(balance->isChecked());
    CHECK_FALSE(panel->isVisible());
    CHECK_FALSE(app.overlay("balance.dot")->visible);
}

TEST_CASE("the assembly checks are only for assemblies", "[app][measure][assembly]") {
    App app;  // a part
    CHECK_FALSE(app.shell->action("sb.measure.overlaps")->isEnabled());
    CHECK_FALSE(app.shell->action("sb.measure.balance")->isEnabled());
}

TEST_CASE("in an assembly, Measure is on the Assemble tab and measures between components",
          "[app][measure][assembly]") {
    App app;
    auto& docs = app.host.documents();
    // An assembly: a 20 x 20 x 10 mm base, and a 10 mm cube 10 mm to its right.
    REQUIRE(docs.undo(app.design()));
    REQUIRE(docs.run(app.design(), "assembly.new", {}).empty());
    auto part = [](const char* name, kernel::Vec3 size) {
        auto p = std::make_unique<model::Document>(model::DocKind::Part, name);
        REQUIRE(p->execute(std::make_unique<model::AddFeatureCommand>(
                               std::make_unique<model::BoxFeature>("Box1", kernel::Vec3{}, size)))
                    .empty());
        return p;
    };
    REQUIRE(
        docs.execute(app.design(), std::make_unique<model::InsertPartCommand>(part("Base", {20, 20, 10}), 1))
            .empty());
    kernel::Placement right;
    right.origin = {30, 0, 0};
    REQUIRE(docs.execute(app.design(),
                         std::make_unique<model::InsertPartCommand>(part("Cube", {10, 10, 10}), 1, right))
                .empty());
    settle();
    const auto& design = *docs.find(app.design());
    // A component's face whose middle is at `p` in its own part, as a click picks it.
    auto face = [&](std::size_t which, kernel::Vec3 p) {
        const auto& c = design.components()[which];
        const auto* built = docs.lastRebuild(app.design())->part(c.part);
        REQUIRE(built);
        const auto body = built->bodies().front();
        const auto& names = *built->result(body)->names;
        const auto& faces = names.elements.faces;
        for (std::size_t i = 0; i < faces.size(); ++i) {
            const auto& f = faces[i].center;
            if (std::abs(f.x - p.x) < 1e-9 && std::abs(f.y - p.y) < 1e-9 && std::abs(f.z - p.z) < 1e-9)
                return plugin::PickedElement{
                    body, model::referTo(names, kernel::TopoKind::Face, static_cast<int>(i)), c.id};
        }
        FAIL("no face at " << p.x << ", " << p.y << ", " << p.z);
        return plugin::PickedElement{};
    };

    auto* measure = app.shell->action("sb.measure.measure");
    REQUIRE(measure);
    CHECK(measure->isEnabled());
    measure->trigger();
    settle();
    CHECK(app.host.selection().pickMode() == plugin::PickMode::Measure);
    // Base's right side (x 20) and the cube's left side, at x 30 where it's placed.
    app.picked({face(0, {20, 10, 5}), face(1, {0, 5, 5})});
    CHECK(app.text() == QString::fromUtf8("Base 1, Box1, flat face: area 200.00 mm^2\n"
                                          "Cube 1, Box1, flat face: area 100.00 mm^2\n"
                                          "Distance: 10 mm (x 10, y 0, z 0 mm)\n"
                                          "Angle: 0° (they're parallel)"));
    const auto* line = app.overlay("line");
    REQUIRE(line);
    CHECK(line->visible);
    REQUIRE(line->points.size() == 2);
    CHECK(line->points[0].x == 20.0);
    CHECK(line->points[1].x == 30.0);
}
