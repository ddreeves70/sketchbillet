// Incremental rebuilds: given the last rebuild, only the steps that changed, and
// the steps that use them, are built again. Counted with the fake kernel, whose
// `calls` goes up once per kernel operation. (test_incremental_occt.cpp checks
// that an incremental rebuild always matches a full one, with real geometry.)

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "FakeKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"

using namespace sb;
using namespace sb::model;

namespace {

struct Design {
    Document doc;
    test::FakeKernel kernel;

    void run(std::string_view name, Args args) {
        auto made = CommandRegistry::builtin().create(name, args, doc);
        INFO(name << ": " << (made.ok() ? std::string() : made.issues.front().message));
        REQUIRE(made.ok());
        const auto issues = doc.execute(std::move(made.command));
        INFO(name << ": " << (issues.empty() ? std::string() : issues.front().message));
        REQUIRE(issues.empty());
    }
    FeatureId id(const std::string& name) const {
        for (std::size_t i = 0; i < doc.featureCount(); ++i)
            if (doc.featureAt(i).name() == name) return doc.featureAt(i).id();
        FAIL("no step " << name);
        return {};
    }
    std::vector<std::string> names(const std::vector<FeatureId>& ids) const {
        std::vector<std::string> out;
        for (const auto& i : ids) out.push_back(doc.find(i)->name());
        return out;
    }
    // Box1 cut by Tool, and Peg on its own.
    Design() {
        run("feature.add_box", {{"size", kernel::Vec3{40, 30, 20}}});
        run("feature.add_box", {{"size", kernel::Vec3{10, 10, 10}}, {"name", std::string("Tool")}});
        run("feature.add_boolean",
            {{"operation", std::string("cut")}, {"a", id("Box1")}, {"b", id("Tool")}, {"name", std::string("Cut")}});
        run("feature.add_cylinder", {{"radius", 2.0}, {"height", 5.0}, {"name", std::string("Peg")}});
    }
    double volume(const RebuildReport& r, const std::string& name) {
        const auto* shape = r.shapeOf(id(name));
        REQUIRE(shape);
        return kernel.massProps(*shape).volume;
    }
};

}  // namespace

TEST_CASE("A rebuild with nothing changed builds nothing", "[model][incremental]") {
    Design d;
    const auto first = d.doc.regenerate(d.kernel);
    CHECK(first.reused().empty());  // no previous rebuild: everything is built
    const int built = d.kernel.calls;
    CHECK(built == 4);

    const auto again = d.doc.regenerate(d.kernel, nullptr, &first);
    CHECK(d.kernel.calls == built);
    CHECK(d.names(again.reused()) == std::vector<std::string>{"Box1", "Tool", "Cut", "Peg"});
    CHECK(again.ok());
    CHECK(d.volume(again, "Cut") == 40 * 30 * 20 - 10 * 10 * 10);
    CHECK(again.bodies() == first.bodies());
}

TEST_CASE("A changed step builds again, with the steps that use it, and nothing else", "[model][incremental]") {
    Design d;
    auto report = d.doc.regenerate(d.kernel);
    d.run("feature.set_value", {{"feature", d.id("Tool")}, {"setting", std::string("size_x")}, {"value", 20.0}});
    const int before = d.kernel.calls;
    report = d.doc.regenerate(d.kernel, nullptr, &report);
    CHECK(d.kernel.calls - before == 2);  // Tool and Cut
    CHECK(d.names(report.reused()) == std::vector<std::string>{"Box1", "Peg"});
    CHECK(d.volume(report, "Cut") == 40 * 30 * 20 - 20 * 10 * 10);

    SECTION("undo is a change too") {
        d.doc.undo();
        report = d.doc.regenerate(d.kernel, nullptr, &report);
        CHECK(d.names(report.reused()) == std::vector<std::string>{"Box1", "Peg"});
        CHECK(d.volume(report, "Cut") == 40 * 30 * 20 - 10 * 10 * 10);
    }
    SECTION("a parameter change rebuilds the steps whose numbers use it") {
        d.run("parameter.set", {{"name", std::string("hole")}, {"value", std::string("5 mm")}});
        d.run("feature.set_value",
              {{"feature", d.id("Tool")}, {"setting", std::string("size_y")}, {"formula", std::string("hole * 2")}});
        report = d.doc.regenerate(d.kernel, nullptr, &report);
        d.run("parameter.set", {{"name", std::string("hole")}, {"value", std::string("4 mm")}});
        report = d.doc.regenerate(d.kernel, nullptr, &report);
        CHECK(d.names(report.reused()) == std::vector<std::string>{"Box1", "Peg"});
        CHECK(d.volume(report, "Cut") == 40 * 30 * 20 - 20 * 8 * 10);
    }
    SECTION("renaming a step builds it again, since its messages use its name") {
        d.run("feature.rename", {{"feature", d.id("Peg")}, {"name", std::string("Pin")}});
        report = d.doc.regenerate(d.kernel, nullptr, &report);
        CHECK(d.names(report.reused()) == std::vector<std::string>{"Box1", "Tool", "Cut"});
    }
}

TEST_CASE("Turning a step off and on, and rolling back, reuse what they can", "[model][incremental]") {
    Design d;
    auto report = d.doc.regenerate(d.kernel);
    d.run("feature.suppress", {{"steps", std::vector<FeatureId>{d.id("Tool")}}});
    report = d.doc.regenerate(d.kernel, nullptr, &report);
    CHECK(report.result(d.id("Cut"))->status == FeatureStatus::Blocked);
    CHECK(d.names(report.reused()) == std::vector<std::string>{"Box1", "Peg"});

    d.run("feature.suppress", {{"steps", std::vector<FeatureId>{d.id("Tool")}}, {"off", false}});
    const int before = d.kernel.calls;
    report = d.doc.regenerate(d.kernel, nullptr, &report);
    CHECK(d.kernel.calls - before == 2);  // Tool and Cut, which was blocked last time
    CHECK(report.ok());

    d.run("timeline.rollback", {{"after", d.id("Box1")}});
    report = d.doc.regenerate(d.kernel, nullptr, &report);
    CHECK(d.names(report.reused()) == std::vector<std::string>{"Box1"});
    CHECK(report.result(d.id("Peg"))->status == FeatureStatus::RolledBack);
    d.run("timeline.rollback", {{"to", std::string("end")}});
    report = d.doc.regenerate(d.kernel, nullptr, &report);
    // The rolled back steps hadn't changed, but weren't built last time.
    CHECK(d.names(report.reused()) == std::vector<std::string>{"Box1"});
    CHECK(report.ok());
}

TEST_CASE("A rebuild made another way, or for a deleted step, isn't reused", "[model][incremental]") {
    Design d;
    const auto first = d.doc.regenerate(d.kernel);

    SECTION("another kernel") {
        test::FakeKernel other;
        const auto again = d.doc.regenerate(other, nullptr, &first);
        CHECK(again.reused().empty());
        CHECK(other.calls == 4);
    }
    SECTION("another length unit: messages show lengths in it") {
        d.run("document.set_units", {{"unit", std::string("in")}});
        CHECK(d.doc.regenerate(d.kernel, nullptr, &first).reused().empty());
    }
    SECTION("a step deleted and put back with undo is built again") {
        d.run("feature.delete", {{"feature", d.id("Peg")}});
        auto report = d.doc.regenerate(d.kernel, nullptr, &first);
        d.doc.undo();
        report = d.doc.regenerate(d.kernel, nullptr, &report);
        CHECK(d.names(report.reused()) == std::vector<std::string>{"Box1", "Tool", "Cut"});
    }
    SECTION("another design: revisions are never shared") {
        Design e;
        CHECK(e.doc.regenerate(e.kernel, nullptr, &first).reused().empty());
    }
}

TEST_CASE("Every change gives a step a new revision, never used before", "[model][incremental]") {
    Design d;
    const auto box = d.id("Box1");
    const auto r1 = d.doc.revision(box);
    CHECK(r1 > 0);
    d.run("feature.rename", {{"feature", box}, {"name", std::string("Base")}});
    const auto r2 = d.doc.revision(box);
    CHECK(r2 > r1);
    d.doc.undo();
    CHECK(d.doc.revision(box) > r2);  // undo doesn't hand back an old number
    CHECK(d.doc.revision(FeatureId::generate()) == 0);
}
