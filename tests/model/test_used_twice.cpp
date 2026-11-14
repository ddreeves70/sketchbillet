// A body used by two steps: the second starts again from it as it was, and
// makes a second body in the same place. Bug: a student picked Box1 (already
// cut by Cut1) for a second cut, and got two overlapping boxes with one hole
// each and no word why. Now the step gets a warning saying what to use, the
// steps that pick a body for you use the body as it is now
// (Document::latestBody()), and so does the app (tests/app/test_shell.cpp).

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
        REQUIRE(made.ok());
        REQUIRE(doc.execute(std::move(made.command)).empty());
    }
    FeatureId id(const std::string& name) const {
        for (std::size_t i = 0; i < doc.featureCount(); ++i)
            if (doc.featureAt(i).name() == name) return doc.featureAt(i).id();
        FAIL("no step " << name);
        return {};
    }
    // Box1, Cylinder1, Cut1 = Box1 cut Cylinder1, Cylinder2.
    Design() {
        run("feature.add_box", {{"size", kernel::Vec3{60, 30, 4}}});
        run("feature.add_cylinder", {{"base", kernel::Vec3{9, 15, -1}}, {"radius", 2.5}, {"height", 6.0}});
        run("feature.add_boolean",
            {{"operation", std::string("cut")}, {"a", id("Box1")}, {"b", id("Cylinder1")}});
        run("feature.add_cylinder", {{"base", kernel::Vec3{51, 15, -1}}, {"radius", 2.5}, {"height", 6.0}});
    }
    Diagnostics warnings(const RebuildReport& r, const std::string& step) {
        Diagnostics out;
        for (const auto& d : r.result(id(step))->issues)
            if (d.code == "feature.used_twice") out.push_back(d);
        return out;
    }
};

}  // namespace

TEST_CASE("A step that starts again from a body another step changed gets a warning", "[model][used_twice]") {
    Design d;
    // The slip: Box1 instead of Cut1.
    d.run("feature.add_boolean",
          {{"operation", std::string("cut")}, {"a", d.id("Box1")}, {"b", d.id("Cylinder2")}});
    auto report = d.doc.regenerate(d.kernel);
    CHECK(report.ok());                  // a warning, not a failure
    CHECK(report.bodies().size() == 2);  // Cut1 and Cut2, in the same place
    auto w = d.warnings(report, "Cut2");
    REQUIRE(w.size() == 1);
    CHECK(w.front().severity == Severity::Warning);
    CHECK(w.front().message ==
          "Cut2 uses Box1, but Cut1 already changed Box1. So Cut2 starts again from Box1 as it was before "
          "Cut1, "
          "and makes a second body in the same place. To build on Cut1, use Cut1 instead of Box1.");
    CHECK(d.warnings(report, "Cut1").empty());

    // Rebuilt from the last rebuild, it's still there once.
    report = d.doc.regenerate(d.kernel, nullptr, &report);
    CHECK(d.warnings(report, "Cut2").size() == 1);
    // Turning Cut1 off leaves Box1 to Cut2 alone: no warning.
    d.run("feature.suppress", {{"steps", std::vector{d.id("Cut1")}}});
    report = d.doc.regenerate(d.kernel, nullptr, &report);
    CHECK(d.warnings(report, "Cut2").empty());
}

TEST_CASE("The right way, and bodies only looked at, get no warning", "[model][used_twice]") {
    Design d;
    d.run("feature.add_boolean",
          {{"operation", std::string("cut")}, {"a", d.id("Cut1")}, {"b", d.id("Cylinder2")}});
    // A linked copy of the result only looks at it; joining the two uses each once.
    d.run("feature.add_move", {{"body", d.id("Cut2")}, {"by", kernel::Vec3{0, 40, 0}}, {"copy", true}});
    d.run("feature.add_boolean",
          {{"operation", std::string("union")}, {"a", d.id("Cut2")}, {"b", d.id("Copy1")}});
    const auto report = d.doc.regenerate(d.kernel);
    for (const char* step : {"Cut1", "Cut2", "Copy1", "Union1"}) {
        INFO(step);
        CHECK(d.warnings(report, step).empty());
    }
}

TEST_CASE("latestBody gives the step a body has become", "[model][used_twice]") {
    Design d;
    CHECK(d.doc.latestBody(d.id("Box1")) == d.id("Cut1"));
    CHECK(d.doc.latestBody(d.id("Cylinder1")) == d.id("Cut1"));  // used up as a tool, too
    CHECK(d.doc.latestBody(d.id("Cut1")) == d.id("Cut1"));
    CHECK(d.doc.latestBody(d.id("Cylinder2")) == d.id("Cylinder2"));
    d.run("feature.add_move", {{"body", d.id("Cut1")}, {"by", kernel::Vec3{0, 0, 5}}});
    CHECK(d.doc.latestBody(d.id("Box1")) == d.id("Move1"));  // through every step since
    // A linked copy only looks: it doesn't change Move1.
    d.run("feature.add_move", {{"body", d.id("Move1")}, {"by", kernel::Vec3{0, 40, 0}}, {"copy", true}});
    CHECK(d.doc.latestBody(d.id("Box1")) == d.id("Move1"));
    // Steps turned off, or rolled back, don't count.
    d.run("feature.suppress", {{"steps", std::vector{d.id("Move1")}}});
    CHECK(d.doc.latestBody(d.id("Box1")) == d.id("Cut1"));
    d.run("feature.suppress", {{"steps", std::vector{d.id("Move1")}}, {"off", false}});
    d.run("timeline.rollback", {{"after", d.id("Cut1")}});
    CHECK(d.doc.latestBody(d.id("Box1")) == d.id("Cut1"));
    const auto stray = FeatureId::generate();
    CHECK(d.doc.latestBody(stray) == stray);  // not in the design: itself
}
