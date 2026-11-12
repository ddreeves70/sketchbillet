// Incremental rebuilds with real geometry: after every kind of change, a rebuild
// that reuses the last one's results must match a full rebuild exactly.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/model/Naming.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;
using kernel::TopoKind;

namespace {

struct Design {
    Document doc;
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();
    std::unique_ptr<sketch::ISketchSolver> solver = sketch::makePlaneGcsSolver();

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
    FeatureId last() const { return doc.featureAt(doc.featureCount() - 1).id(); }
    RebuildReport full() { return doc.regenerate(*kernel, solver.get()); }
    // The first face, or the edges, of a step's body that `want` picks.
    std::vector<GeometryRef> pick(const FeatureId& body, TopoKind kind,
                                  const std::function<bool(const kernel::ElementInfo&)>& want) {
        const auto report = full();
        const auto& names = *report.result(body)->names;
        const auto& list = kind == TopoKind::Face ? names.elements.faces : names.elements.edges;
        std::vector<GeometryRef> out;
        for (std::size_t i = 0; i < list.size(); ++i)
            if (want(list[i])) out.push_back(referTo(names, kind, static_cast<int>(i)));
        REQUIRE_FALSE(out.empty());
        return out;
    }

    // A 60 x 40 mm plate, `thick` thick, rounded at its corners, with a hole
    // repeated three times along it; and a ring on its own.
    Design() {
        run("parameter.set", {{"name", std::string("thick")}, {"value", std::string("8 mm")}});
        run("feature.add_sketch", {{"plane", std::string("xy")}});
        const auto plate = last();
        run("sketch.add_rectangle", {{"sketch", plate}, {"from", sketch::Point2{0, 0}}, {"to", sketch::Point2{60, 40}}});
        run("sketch.constrain", {{"sketch", plate}, {"kind", std::string("fix")}, {"on", std::string("Line1.start")}});
        run("sketch.constrain", {{"sketch", plate},
                                 {"kind", std::string("length")},
                                 {"on", std::string("Line1")},
                                 {"value", 60.0},
                                 {"name", std::string("Width")}});
        run("feature.add_extrude", {{"sketch", plate}, {"distance", 8.0}});
        run("feature.set_value",
            {{"feature", id("Extrude1")}, {"setting", std::string("distance")}, {"formula", std::string("thick")}});
        const auto upright = pick(id("Extrude1"), TopoKind::Edge,
                                  [](const kernel::ElementInfo& e) { return std::abs(e.direction.z) > 0.999; });
        run("feature.add_fillet", {{"body", id("Extrude1")}, {"edges", upright}, {"radius", 4.0}});
        const auto top =
            pick(id("Fillet1"), TopoKind::Face, [](const kernel::ElementInfo& f) { return f.geom == kernel::GeomKind::Plane && f.direction.z > 0.999; });
        run("feature.add_sketch", {{"body", id("Fillet1")}, {"face", top.front()}});
        run("sketch.add_point", {{"sketch", last()}, {"at", sketch::Point2{15, 20}}});
        run("feature.add_hole", {{"sketch", last()}});
        run("feature.add_pattern", {{"steps", std::vector{id("Hole1")}}, {"count", 3}, {"spacing", 15.0}});
        run("feature.add_sketch", {{"plane", std::string("xz")}});
        run("sketch.add_rectangle",
            {{"sketch", last()}, {"from", sketch::Point2{100, 0}}, {"to", sketch::Point2{110, 10}}});
        run("feature.add_revolve", {{"sketch", last()}, {"axis", std::string("y")}, {"name", std::string("Ring")}});
    }
};

// Checks that two rebuilds of the same design say exactly the same things.
void same(Design& d, const RebuildReport& a, const RebuildReport& b) {
    CHECK(a.bodies() == b.bodies());
    CHECK(a.ok() == b.ok());
    for (std::size_t i = 0; i < d.doc.featureCount(); ++i) {
        const auto& f = d.doc.featureAt(i);
        INFO("step " << f.name());
        const auto* x = a.result(f.id());
        const auto* y = b.result(f.id());
        REQUIRE(x);
        REQUIRE(y);
        CHECK(x->status == y->status);
        REQUIRE(x->issues.size() == y->issues.size());
        for (std::size_t k = 0; k < x->issues.size(); ++k) {
            CHECK(x->issues[k].code == y->issues[k].code);
            CHECK(x->issues[k].message == y->issues[k].message);
        }
        REQUIRE(x->shape.valid() == y->shape.valid());
        if (x->shape.valid()) {
            const auto mx = d.kernel->massProps(x->shape), my = d.kernel->massProps(y->shape);
            CHECK(mx.volume == Approx(my.volume).epsilon(1e-12));
            CHECK(mx.area == Approx(my.area).epsilon(1e-12));
        }
        REQUIRE(bool(x->names) == bool(y->names));
        if (x->names) {
            CHECK(x->names->faces == y->names->faces);
            CHECK(x->names->edges == y->names->edges);
            CHECK(x->names->vertices == y->names->vertices);
        }
        REQUIRE(bool(x->sketch) == bool(y->sketch));
        if (x->sketch) CHECK(x->sketch->ok() == y->sketch->ok());
        CHECK(x->plane.has_value() == y->plane.has_value());
    }
}

}  // namespace

TEST_CASE("An incremental rebuild matches a full one, whatever changed", "[model][incremental][occt]") {
    Design d;
    auto report = d.full();
    REQUIRE(report.ok());
    std::size_t reused = 0;
    auto check = [&](const char* what) {
        INFO("after: " << what);
        report = d.doc.regenerate(*d.kernel, d.solver.get(), &report);
        reused += report.reused().size();
        same(d, report, d.full());
    };
    check("nothing");
    CHECK(report.reused().size() == d.doc.featureCount());

    d.run("sketch.set_value", {{"sketch", d.id("Sketch1")}, {"constraint", std::string("Width")}, {"value", 70.0}});
    check("an upstream sketch dimension");
    CHECK(report.reused() == std::vector{d.id("Sketch3"), d.id("Ring")});

    d.run("parameter.set", {{"name", std::string("thick")}, {"value", std::string("10 mm")}});
    check("a parameter the extrude uses");
    d.run("feature.suppress", {{"steps", std::vector{d.id("Fillet1")}}});
    check("turning the fillet off, which blocks what's sketched on it");
    d.run("feature.suppress", {{"steps", std::vector{d.id("Fillet1")}}, {"off", false}});
    check("turning it back on");
    d.run("timeline.rollback", {{"after", d.id("Extrude1")}});
    check("rolling back");
    d.run("feature.add_cylinder",
          {{"radius", 3.0}, {"height", 20.0}, {"base", kernel::Vec3{-20, 0, 0}}, {"name", std::string("Peg")}});
    check("a step added at the marker");
    d.run("timeline.rollback", {{"to", std::string("end")}});
    check("rolling forward to the end");
    d.run("timeline.move", {{"feature", d.id("Sketch3")}, {"before", d.id("Sketch1")}});
    check("moving a step");
    d.run("feature.rename", {{"feature", d.id("Hole1")}, {"name", std::string("Drill")}});
    check("renaming a step");
    d.run("feature.delete", {{"feature", d.id("Pattern1")}});
    check("deleting a step");
    d.doc.undo();
    check("undo");
    d.doc.redo();
    check("redo");
    d.doc.undo();
    check("undo again");
    d.run("document.set_units", {{"unit", std::string("in")}});
    check("another length unit");
    d.run("sketch.set_value", {{"sketch", d.id("Sketch1")}, {"constraint", std::string("Width")}, {"value", 20.0}});
    check("a plate too small for the holes, which miss");
    d.doc.undo();
    check("undo of that");
    CHECK(reused > 0);
}
