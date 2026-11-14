// Tests for the Measure tool (sb.measure): Tools > Measure, the Measure panel,
// and the line it draws between the nearest points.

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
