// Measuring a design: one face, edge or point, or two, with real geometry.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <numbers>
#include <string>

#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Document.h"
#include "sb/model/Measure.h"
#include "sb/model/Naming.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;

namespace {

// The keychain tag with a hole at each end: x 9 and x 51, 5 mm across.
struct Tag {
    Document doc;
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();
    RebuildReport report;

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
    Tag() {
        run("feature.add_box", {{"name", std::string("Plate")}, {"size", kernel::Vec3{60, 30, 4}}});
        for (const double x : {9.0, 51.0}) {
            run("feature.add_cylinder",
                {{"base", kernel::Vec3{x, 15, -1}}, {"radius", 2.5}, {"height", 6.0}});
        }
        run("feature.add_boolean", {{"name", std::string("Half")},
                                    {"operation", std::string("cut")},
                                    {"a", id("Plate")},
                                    {"b", id("Cylinder1")}});
        run("feature.add_boolean", {{"name", std::string("Tag")},
                                    {"operation", std::string("cut")},
                                    {"a", id("Half")},
                                    {"b", id("Cylinder2")}});
        report = doc.regenerate(*kernel);
        REQUIRE(report.ok());
    }
    // A face, edge or point of the tag, by where its middle is.
    MeasurePick pick(kernel::TopoKind kind, kernel::Vec3 p) const {
        const auto& names = *report.result(id("Tag"))->names;
        const auto& list = names.elements.of(kind);
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& c = list[i].center;
            if (std::abs(c.x - p.x) < 1e-6 && std::abs(c.y - p.y) < 1e-6 && std::abs(c.z - p.z) < 1e-6)
                return {id("Tag"), referTo(names, kind, static_cast<int>(i))};
        }
        FAIL("nothing at " << p.x << ", " << p.y << ", " << p.z);
        return {};
    }
    Measurement measure(std::vector<MeasurePick> picks) const {
        return model::measure(doc, report, *kernel, picks);
    }
    std::vector<std::string> lines(std::vector<MeasurePick> picks) const {
        return describe(measure(std::move(picks)), doc.lengthUnit());
    }
};

using K = kernel::TopoKind;

}  // namespace

TEST_CASE("One face, edge or point: its size", "[model][measure][occt]") {
    Tag t;
    CHECK(t.lines({t.pick(K::Face, {0, 15, 2})}) ==
          std::vector<std::string>{"Tag, flat face: area 120.00 mm^2"});
    CHECK(t.lines({t.pick(K::Edge, {9, 15, 4})}) ==
          std::vector<std::string>{"Tag, round edge: radius 2.5 mm (5 mm across), 15.71 mm long"});
    CHECK(t.lines({t.pick(K::Edge, {30, 0, 0})}) ==
          std::vector<std::string>{"Tag, straight edge: 60 mm long"});
    CHECK(t.lines({t.pick(K::Vertex, {60, 30, 4})}) ==
          std::vector<std::string>{"Tag, point: at x 60, y 30, z 4 mm"});
    const auto m = t.measure({t.pick(K::Edge, {9, 15, 4})});
    CHECK(m.ok());
    CHECK_FALSE(m.distance);
    CHECK_FALSE(m.angle);
}

TEST_CASE("Two faces: the distance, and the angle", "[model][measure][occt]") {
    Tag t;
    // The two ends: parallel, 60 mm apart.
    auto m = t.measure({t.pick(K::Face, {0, 15, 2}), t.pick(K::Face, {60, 15, 2})});
    REQUIRE(m.ok());
    CHECK(*m.distance == Approx(60));
    CHECK(*m.apart == Approx(60));
    CHECK(*m.angle == Approx(0));
    CHECK(m.from.x == Approx(0));
    CHECK(m.to.x == Approx(60));
    CHECK(describe(m, LengthUnit::Millimeter) ==
          std::vector<std::string>{"Tag, flat face: area 120.00 mm^2", "Tag, flat face: area 120.00 mm^2",
                                   "Distance: 60 mm (x 60, y 0, z 0 mm)", "Angle: 0° (they're parallel)"});
    // The front and the left end: square, and touching.
    m = t.measure({t.pick(K::Face, {30, 0, 2}), t.pick(K::Face, {0, 15, 2})});
    CHECK(*m.distance == Approx(0).margin(1e-9));
    CHECK(*m.angle == Approx(std::numbers::pi / 2));
    CHECK_FALSE(m.apart);
    CHECK(describe(m, LengthUnit::Millimeter).at(2) == "Distance: 0 mm (they touch)");
    CHECK(describe(m, LengthUnit::Millimeter).at(3) == "Angle: 90°");
}

TEST_CASE("Edges: straight across when parallel, and the centers of round ones", "[model][measure][occt]") {
    Tag t;
    // The front bottom edge and the back top edge: 30 mm across and 4 mm up.
    auto m = t.measure({t.pick(K::Edge, {30, 0, 0}), t.pick(K::Edge, {30, 30, 4})});
    CHECK(*m.distance == Approx(std::hypot(30, 4)));
    CHECK(*m.apart == Approx(std::hypot(30, 4)));
    // The front bottom edge and the left top edge: square to each other.
    m = t.measure({t.pick(K::Edge, {30, 0, 0}), t.pick(K::Edge, {0, 15, 4})});
    CHECK(*m.angle == Approx(std::numbers::pi / 2));
    // The two holes' top rims: 37 mm apart rim to rim, 42 mm center to center.
    m = t.measure({t.pick(K::Edge, {9, 15, 4}), t.pick(K::Edge, {51, 15, 4})});
    REQUIRE(m.ok());
    CHECK(*m.distance == Approx(37));
    CHECK(*m.centers == Approx(42));
    CHECK_FALSE(m.angle);
    CHECK(describe(m, LengthUnit::Millimeter).back() == "Centers: 42 mm apart");
    // A top rim and the other hole's bottom rim: centers are straight across, not slanted.
    m = t.measure({t.pick(K::Edge, {9, 15, 4}), t.pick(K::Edge, {51, 15, 0})});
    CHECK(*m.centers == Approx(42));
    // A rim and a straight edge: no centers.
    CHECK_FALSE(t.measure({t.pick(K::Edge, {9, 15, 4}), t.pick(K::Edge, {30, 0, 0})}).centers);
}

TEST_CASE("Points, in inches too", "[model][measure][occt]") {
    Tag t;
    const auto m = t.measure({t.pick(K::Vertex, {0, 0, 0}), t.pick(K::Vertex, {60, 30, 4})});
    CHECK(*m.distance == Approx(std::sqrt(60.0 * 60 + 30 * 30 + 4 * 4)));
    CHECK(describe(m, LengthUnit::Millimeter).back() == "Distance: 67.2 mm (x 60, y 30, z 4 mm)");
    CHECK(describe(m, LengthUnit::Inch).back() == "Distance: 2.646 in (x 2.362, y 1.181, z 0.157 in)");
}

TEST_CASE("Measuring tells the student what went wrong", "[model][measure][occt]") {
    Tag t;
    auto m = t.measure({});
    CHECK_FALSE(m.ok());
    CHECK(m.issues.front().code == "measure.count");
    const auto p = t.pick(K::Face, {0, 15, 2});
    CHECK(t.measure({p, p, p}).issues.front().code == "measure.count");
    // A step with no body.
    t.run("feature.add_sketch", {});
    t.report = t.doc.regenerate(*t.kernel);
    m = t.measure({{t.id("Sketch1"), p.ref}});
    CHECK(m.issues.front().code == "measure.no_body");
    CHECK(m.issues.front().message ==
          "Sketch1 has no body to measure. Pick a step that built, or fix it first.");
    // A face that's gone: it doesn't look like anything left on the body.
    MeasurePick gone = p;
    gone.ref.name = "nothing";
    gone.ref.signature.geom = kernel::GeomKind::Sphere;
    m = t.measure({gone});
    CHECK(m.issues.front().code == "measure.missing");
    CHECK(m.elements.empty());
}
