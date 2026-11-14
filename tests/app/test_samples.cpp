// Tests for File > Open Sample: the sample designs the build makes from the
// reference parts (cmake/SbSamples.cmake), listed by group and opened unsaved.

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>

#include <memory>

#include "AppSettings.h"
#include "AppUi.h"
#include "Shell.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/Document.h"
#include "sb/plugin/PluginHost.h"
#include "sb/plugins/Modeling.h"
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
        host.loadAll();
        shell = std::make_unique<app::Shell>(host, ui, settings);
        shell->askUnsaved = [](const QString&) { return app::Shell::Unsaved::Discard; };
        shell->show();
        settle();
    }
    QMenu* samples() { return shell->findChild<QMenu*>(QStringLiteral("sb.shell.samples")); }
    const model::Document& doc() { return *host.documents().find(shell->activeDesign()); }
};

}  // namespace

#ifdef SB_TEST_SAMPLES

TEST_CASE("File > Open Sample lists the reference parts by group", "[app][samples]") {
    App app;
    app.shell->setSamplesFolder(SB_TEST_SAMPLES);
    auto* menu = app.samples();
    REQUIRE(menu);
    QStringList groups;
    int count = 0;
    for (auto* a : menu->actions()) {
        REQUIRE(a->menu());
        groups << a->text();
        count += static_cast<int>(a->menu()->actions().size());
    }
    CHECK(groups == QStringList{"Brackets", "Enclosures", "Knobs", "Mounts"});
    CHECK(count == 20);
    // In each group's own order, each with what it is as its tip.
    const auto brackets = menu->actions().front()->menu()->actions();
    CHECK(brackets.front()->text() == QStringLiteral("L bracket"));
    CHECK(brackets.front()->objectName() == QStringLiteral("sb.shell.sample.l-bracket"));
    CHECK(brackets.front()->toolTip().startsWith(QStringLiteral("A 60 mm wide angle bracket")));
}

TEST_CASE("Opening a sample makes a new design that builds and isn't saved anywhere", "[app][samples]") {
    App app;
    app.shell->setSamplesFolder(SB_TEST_SAMPLES);
    auto* bracket = app.shell->findChild<QAction*>(QStringLiteral("sb.shell.sample.l-bracket"));
    REQUIRE(bracket);
    bracket->trigger();
    settle();
    CHECK(app.doc().name() == "L bracket");
    CHECK(app.doc().featureCount() == 10);
    CHECK(app.shell->activePath().empty());  // Save asks where to keep it
    CHECK_FALSE(app.doc().isModified());
    const auto* report = app.host.documents().lastRebuild(app.shell->activeDesign());
    REQUIRE(report);
    CHECK(report->ok());
    CHECK(report->bodies().size() == 1);

    // Every sample opens and builds.
    for (auto* group : app.samples()->actions()) {
        for (auto* sample : group->menu()->actions()) {
            INFO(sample->text().toStdString());
            sample->trigger();
            settle();
            CHECK(app.doc().name() == sample->text().toStdString());
            const auto* r = app.host.documents().lastRebuild(app.shell->activeDesign());
            REQUIRE(r);
            CHECK(r->ok());
        }
    }
}

#endif

TEST_CASE("With no samples, File > Open Sample says so", "[app][samples]") {
    App app;
    app.shell->setSamplesFolder({});
    REQUIRE(app.samples());
    REQUIRE(app.samples()->actions().size() == 1);
    CHECK(app.samples()->actions().front()->text() == QStringLiteral("No samples found"));
    CHECK_FALSE(app.samples()->actions().front()->isEnabled());
    const auto problems = app.shell->openSample(QStringLiteral("l-bracket"));
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().code == "sample.missing");
}
