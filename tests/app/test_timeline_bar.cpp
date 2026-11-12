// Tests for the timeline bar (sb.timeline): what it shows, the buttons, picking
// steps, and dragging the marker and steps.

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QMouseEvent>
#include <QScrollArea>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>

#include <memory>

#include "AppSettings.h"
#include "AppUi.h"
#include "Shell.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/Document.h"
#include "sb/plugin/PluginHost.h"
#include "sb/plugins/FeatureTree.h"
#include "sb/plugins/Modeling.h"
#include "sb/plugins/Timeline.h"
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
    QWidget* strip = nullptr;

    App() {
        host.provide<ui::UiRegistry>(ui);
        host.setSettings(pluginSettings);
        host.addBuiltin(plugins::modelingPlugin());
        host.addBuiltin(plugins::featureTreePlugin());
        host.addBuiltin(plugins::timelinePlugin());
        host.addBuiltin(plugins::viewerPlugin());
        host.loadAll();
        shell = std::make_unique<app::Shell>(host, ui, settings);
        shell->askUnsaved = [](const QString&) { return app::Shell::Unsaved::Discard; };
        shell->resize(1280, 800);
        shell->show();
        settle();
        auto* panel = shell->panelWidget(QStringLiteral("sb.timeline.panel"));
        REQUIRE(panel);
        CHECK(panel->isVisible());  // open from the start, along the bottom
        strip = panel->findChild<QWidget*>(QStringLiteral("sb.timeline.strip"));
        REQUIRE(strip);
        // Box1, a fillet on its upright edges, then Peg: a separate cylinder.
        run("feature.add_box", {{"size", kernel::Vec3{40, 30, 20}}});
        run("feature.add_cylinder", {{"radius", 4.0},
                                     {"height", 10.0},
                                     {"base", kernel::Vec3{60, 0, 0}},
                                     {"name", std::string("Peg")}});
        run("feature.add_box", {{"size", kernel::Vec3{5, 5, 5}},
                                {"corner", kernel::Vec3{80, 0, 0}},
                                {"name", std::string("Pin")}});
        run("feature.add_boolean", {{"operation", std::string("union")},
                                    {"a", id("Box1")},
                                    {"b", id("Pin")},
                                    {"name", std::string("Joined")}});
        settle();
    }

    const model::Document& doc() { return *host.documents().find(shell->activeDesign()); }
    void run(std::string_view name, model::Args args) {
        const auto issues = host.documents().run(shell->activeDesign(), name, args);
        INFO(name << ": " << (issues.empty() ? std::string() : issues.front().message));
        REQUIRE(issues.empty());
    }
    FeatureId id(const std::string& name) {
        for (std::size_t i = 0; i < doc().featureCount(); ++i)
            if (doc().featureAt(i).name() == name) return doc().featureAt(i).id();
        FAIL("no step " << name);
        return {};
    }
    QStringList steps() const { return strip->property("steps").toStringList(); }
    QStringList states() const { return strip->property("states").toStringList(); }
    int marker() const { return strip->property("marker").toInt(); }
    QRect chip(int i) const { return strip->property("chipRects").toList().at(i).toRect(); }
    // The middle of the gap with this many steps before it: where the marker sits.
    QPoint gap(int g) const {
        const QRect a = chip(0);
        const int step = chip(1).left() - a.left();
        return {a.left() - (step - a.width()) / 2 + g * step, a.center().y()};
    }
    void click(const char* button) {
        auto* b = shell->panelWidget(QStringLiteral("sb.timeline.panel"))->findChild<QToolButton*>(button);
        REQUIRE(b);
        REQUIRE(b->isEnabled());
        b->click();
        settle();
    }
    void mouse(QEvent::Type type, QPoint at, Qt::MouseButtons held,
               Qt::KeyboardModifiers keys = Qt::NoModifier) {
        QMouseEvent e(type, at, strip->mapToGlobal(at),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held, keys);
        QApplication::sendEvent(strip, &e);
    }
    void drag(QPoint from, QPoint to) {
        mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
        mouse(QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
        mouse(QEvent::MouseMove, to, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, to, Qt::NoButton);
        settle();
    }
    void tap(int i, Qt::KeyboardModifiers keys = Qt::NoModifier) {
        mouse(QEvent::MouseButtonPress, chip(i).center(), Qt::LeftButton, keys);
        mouse(QEvent::MouseButtonRelease, chip(i).center(), Qt::NoButton, keys);
        settle();
    }
};

}  // namespace

TEST_CASE("The timeline bar shows the steps in order, and the marker at the end", "[app][timeline]") {
    App app;
    CHECK(app.steps() == QStringList{"Box1", "Peg", "Pin", "Joined"});
    CHECK(app.states() == QStringList{"ok", "ok", "ok", "ok"});
    CHECK(app.marker() == 4);
    auto* panel = app.shell->panelWidget(QStringLiteral("sb.timeline.panel"));
    CHECK_FALSE(panel->findChild<QToolButton*>(QStringLiteral("sb.timeline.forward"))->isEnabled());
    CHECK_FALSE(panel->findChild<QToolButton*>(QStringLiteral("sb.timeline.end"))->isEnabled());
}

TEST_CASE("The timeline bar's buttons roll back and forward, one undo step each", "[app][timeline]") {
    App app;
    app.click("sb.timeline.back");
    CHECK(app.doc().markerIndex() == 3);
    CHECK(app.marker() == 3);
    CHECK(app.states() == QStringList{"ok", "ok", "ok", "rolled back"});
    CHECK(app.doc().undoLabel() == "Roll back to after Pin");
    app.click("sb.timeline.start");
    CHECK(app.marker() == 0);
    CHECK(app.states() == QStringList{"rolled back", "rolled back", "rolled back", "rolled back"});
    app.click("sb.timeline.forward");
    CHECK(app.marker() == 1);
    app.click("sb.timeline.end");
    CHECK(app.marker() == 4);
    CHECK_FALSE(app.doc().rolledBack());
    app.shell->undoAction()->trigger();
    settle();
    CHECK(app.marker() == 1);
}

TEST_CASE("Clicking a step in the timeline picks it", "[app][timeline]") {
    App app;
    app.tap(1);
    CHECK(app.host.selection().features() == std::vector{app.id("Peg")});
    CHECK(app.strip->property("picked").toStringList() == QStringList{"Peg"});
    app.tap(0, Qt::ControlModifier);
    CHECK(app.host.selection().features() == std::vector{app.id("Peg"), app.id("Box1")});
    app.tap(1, Qt::ControlModifier);  // Ctrl-click again takes it off
    CHECK(app.host.selection().features() == std::vector{app.id("Box1")});
}

TEST_CASE("Dragging the timeline's marker rolls back to where it's dropped", "[app][timeline]") {
    App app;
    app.drag(app.gap(4), app.gap(1));
    CHECK(app.doc().markerIndex() == 1);
    CHECK(app.marker() == 1);
    CHECK(app.states() == QStringList{"ok", "rolled back", "rolled back", "rolled back"});
    // Then a new step goes in at the marker.
    app.run(
        "feature.add_box",
        {{"size", kernel::Vec3{3, 3, 3}}, {"corner", kernel::Vec3{0, 50, 0}}, {"name", std::string("Tab")}});
    settle();
    CHECK(app.steps() == QStringList{"Box1", "Tab", "Peg", "Pin", "Joined"});
    CHECK(app.marker() == 2);
    // Back to the end.
    app.drag(app.gap(2), app.gap(5));
    CHECK_FALSE(app.doc().rolledBack());
}

TEST_CASE("Dragging a step in the timeline moves it, if the steps it uses come first", "[app][timeline]") {
    App app;
    // Peg to the end: nothing uses it.
    app.drag(app.chip(1).center(), app.gap(4));
    CHECK(app.steps() == QStringList{"Box1", "Pin", "Joined", "Peg"});
    CHECK(app.doc().undoLabel() == "Move Peg in the timeline");
    // Back to the start.
    app.drag(app.chip(3).center(), app.gap(0));
    CHECK(app.steps() == QStringList{"Peg", "Box1", "Pin", "Joined"});
    // Pin after Joined, which uses it: not allowed, and it says why.
    app.drag(app.chip(2).center(), app.gap(4));
    CHECK(app.steps() == QStringList{"Peg", "Box1", "Pin", "Joined"});
    REQUIRE_FALSE(app.shell->lastProblems().empty());
    CHECK(app.shell->lastProblems()[0].message ==
          "Joined uses Pin, so Pin has to come before Joined. Put it somewhere before Joined.");
    // Dropped next to itself, it stays put and nothing is said.
    const auto before = app.doc().undoLabel();
    app.drag(app.chip(1).center(), app.gap(2));
    CHECK(app.steps() == QStringList{"Peg", "Box1", "Pin", "Joined"});
    CHECK(app.doc().undoLabel() == before);
}
