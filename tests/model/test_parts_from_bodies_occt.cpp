// Make Parts from Bodies: a part's bodies become the parts of a new assembly,
// each placed where it was. A body and its linked copies make one part, and a
// body the steps only look at comes along for reference.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <numbers>

#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/AssemblyMass.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/model/Material.h"
#include "sb/model/Overlaps.h"
#include "sb/model/PartsFromBodies.h"
#include "sb/model/PartsList.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;

namespace {

bool same(const kernel::Vec3& a, const kernel::Vec3& b) {
    return std::abs(a.x - b.x) < 1e-12 && std::abs(a.y - b.y) < 1e-12 && std::abs(a.z - b.z) < 1e-12;
}

// A toy car as one part with several bodies: an 80 x 40 x 10 mm body; a wheel
// (r 12, 6 mm wide) and two linked copies of it, one slid 60 mm, one turned
// 90 degrees about z then slid; and a 5 mm tall cab sketched on the body's top.
struct Toy {
    Document doc{DocKind::Part, "Toy"};
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();
    std::unique_ptr<sketch::ISketchSolver> solver = sketch::makePlaneGcsSolver();

    void run(std::string_view name, Args args) {
        auto made = CommandRegistry::builtin().create(name, args, doc);
        REQUIRE(made.ok());
        const auto issues = doc.execute(std::move(made.command));
        INFO(name << ": " << (issues.empty() ? std::string() : issues.front().message));
        REQUIRE(issues.empty());
    }
    FeatureId last() const { return doc.featureAt(doc.featureCount() - 1).id(); }
    FeatureId named(std::string_view name) const {
        for (std::size_t i = 0; i < doc.featureCount(); ++i)
            if (doc.featureAt(i).name() == name) return doc.featureAt(i).id();
        FAIL("no step " << name);
        return {};
    }
    RebuildReport rebuild() { return doc.regenerate(*kernel, solver.get()); }

    Toy() {
        run("feature.add_box", {{"size", kernel::Vec3{80, 40, 10}}, {"name", std::string("Body")}});
        const auto body = last();
        run("feature.add_cylinder", {{"radius", 12.0},
                                     {"height", 6.0},
                                     {"base", kernel::Vec3{10, 0, 12}},
                                     {"axis", kernel::Vec3{0, -1, 0}},
                                     {"name", std::string("Wheel")}});
        const auto wheel = last();
        run("feature.add_move", {{"body", wheel},
                                 {"copy", true},
                                 {"by", kernel::Vec3{60, 0, 0}},
                                 {"name", std::string("Wheel2")}});
        run("feature.add_move", {{"body", wheel},
                                 {"copy", true},
                                 {"turn", 90.0},
                                 {"about", kernel::Vec3{10, 0, 12}},
                                 {"by", kernel::Vec3{0, 60, 0}},
                                 {"name", std::string("Wheel3")}});
        // The cab, sketched on the body's top face, so it only looks at the body.
        const auto report = rebuild();
        const auto& names = *report.result(body)->names;
        GeometryRef top;
        for (std::size_t i = 0; i < names.elements.faces.size(); ++i) {
            const auto& f = names.elements.faces[i];
            if (f.geom == kernel::GeomKind::Plane && same(f.direction, {0, 0, 1}))
                top = referTo(names, kernel::TopoKind::Face, static_cast<int>(i));
        }
        run("feature.add_sketch", {{"body", body}, {"face", top}});
        const auto sketch = last();
        run("sketch.add_rectangle",
            {{"sketch", sketch}, {"from", sketch::Point2{20, 10}}, {"to", sketch::Point2{50, 30}}});
        run("feature.add_extrude", {{"sketch", sketch},
                                    {"distance", 5.0},
                                    {"operation", std::string("new")},
                                    {"name", std::string("Cab")}});
    }
};

}  // namespace

TEST_CASE("Make Parts from Bodies puts each body in its own part, where it was",
          "[model][parts_from_bodies][occt]") {
    Toy toy;
    const auto before = toy.rebuild();
    REQUIRE(before.bodies().size() == 5);  // Body, Wheel, Wheel2, Wheel3, Cab
    CHECK(finishedBodySteps(toy.doc) == before.bodies());

    auto made = partsFromBodies(toy.doc);
    REQUIRE(made.problems.empty());
    REQUIRE(made.parts.size() == 3);
    CHECK(made.parts[0].part->name() == "Body");
    CHECK(made.parts[1].part->name() == "Wheel");
    CHECK(made.parts[2].part->name() == "Cab");
    // The wheel and its two linked copies: one part, three components.
    CHECK(made.parts[1].bodies ==
          std::vector<FeatureId>{toy.named("Wheel"), toy.named("Wheel2"), toy.named("Wheel3")});
    REQUIRE(made.parts[1].at.size() == 3);
    CHECK(kernel::samePlacement(made.parts[1].at[0], kernel::Placement{}, 1e-12));
    CHECK(same(made.parts[1].at[1].origin, {60, 0, 0}));
    // Turned 90 degrees about the line x = 10, y = 0: the origin goes to (10, -10), then up 60 in y.
    CHECK(made.parts[1].at[2].origin.x == Approx(10));
    CHECK(made.parts[1].at[2].origin.y == Approx(50));
    CHECK(made.parts[1].at[2].xAxis.y == Approx(1));
    // The cab's part has the body too, kept for reference, so the sketch finds its face.
    const auto& cab = *made.parts[2].part;
    REQUIRE(cab.featureCount() == 3);
    CHECK(cab.featureAt(0).name() == "Body");
    CHECK(cab.featureAt(0).reference());
    CHECK_FALSE(cab.featureAt(2).reference());
    CHECK_FALSE(cab.canUndo());  // a part starts as it is
    const auto cabBuilt = cab.regenerate(*toy.kernel, toy.solver.get());
    CHECK(cabBuilt.bodies() == std::vector<FeatureId>{cab.featureAt(2).id()});
    CHECK(cabBuilt.referenceBodies() == std::vector<FeatureId>{cab.featureAt(0).id()});
    // The design isn't changed.
    CHECK(toy.doc.featureCount() == 6);

    Document car(DocKind::Part, "Car");
    REQUIRE(car.execute(std::make_unique<MakePartsFromBodiesCommand>(std::move(made.parts))).empty());
    CHECK(car.undoLabel() == "Make parts from bodies");
    REQUIRE(car.kind() == DocKind::Assembly);
    REQUIRE(car.components().size() == 5);
    CHECK(car.components()[0].name == "Body 1");
    CHECK(car.components()[0].grounded);
    CHECK(car.components()[1].name == "Wheel 1");
    CHECK(car.components()[3].name == "Wheel 3");
    CHECK_FALSE(car.components()[3].grounded);
    CHECK(car.components()[4].name == "Cab 1");

    // Property: the assembly weighs the same, and balances at the same point, as the
    // design's bodies did; so every component is where its body was.
    const auto report = car.regenerate(*toy.kernel, toy.solver.get());
    double grams = 0;
    kernel::Vec3 moment{};
    for (const auto& id : before.bodies()) {
        const auto m = bodyMass(*toy.kernel, *before.shapeOf(id), toy.doc.materialOf(id));
        grams += m.grams;
        moment = {moment.x + m.grams * m.center.x, moment.y + m.grams * m.center.y,
                  moment.z + m.grams * m.center.z};
    }
    const auto mass = assemblyMass(car, report, *toy.kernel);
    CHECK(mass.grams == Approx(grams));
    CHECK(mass.center.x == Approx(moment.x / grams));
    CHECK(mass.center.y == Approx(moment.y / grams));
    CHECK(mass.center.z == Approx(moment.z / grams));
    // Each wheel on its own too: the turned one balances where Wheel3 did.
    const auto wheel3 = bodyMass(*toy.kernel, *before.shapeOf(toy.named("Wheel3")), defaultMaterial());
    const auto* built = report.part(car.components()[3].part);
    const auto inPart = bodyMass(*toy.kernel, *built->shapeOf(built->bodies().front()), defaultMaterial());
    const auto at = kernel::apply(*report.placementOf(car.components()[3].id), inPart.center);
    CHECK(at.x == Approx(wheel3.center.x));
    CHECK(at.y == Approx(wheel3.center.y));
    CHECK(at.z == Approx(wheel3.center.z));
    // The parts list counts the wheel three times, and the reference body not at all.
    const auto rows = partsList(car, report, *toy.kernel);
    REQUIRE(rows.size() == 3);
    CHECK(rows[1].count == 3);
    CHECK(rows[2].bodies == 1);

    // Undo leaves the new design empty again.
    REQUIRE(car.undo());
    CHECK(car.kind() == DocKind::Part);
    CHECK(car.components().empty());
    REQUIRE(car.redo());
    CHECK(car.components().size() == 5);
}

TEST_CASE("Make Parts from Bodies uses just the picked bodies, and says why it can't",
          "[model][parts_from_bodies][occt]") {
    Toy toy;
    // A linked copy picked without its original is a part of its own, with the
    // original along for reference.
    auto made = partsFromBodies(toy.doc, {toy.named("Wheel2"), toy.named("Body")});
    REQUIRE(made.problems.empty());
    REQUIRE(made.parts.size() == 2);
    CHECK(made.parts[0].part->name() == "Body");  // in design order
    const auto& copy = *made.parts[1].part;
    CHECK(copy.name() == "Wheel2");
    REQUIRE(copy.featureCount() == 2);
    CHECK(copy.featureAt(0).reference());
    CHECK(made.parts[1].at.size() == 1);

    auto refused = partsFromBodies(toy.doc, {toy.named("Sketch1")});
    REQUIRE(refused.problems.size() == 1);
    CHECK(refused.problems.front().code == "parts.not_body");
    CHECK(
        refused.problems.front().message ==
        "Sketch1 isn't one of Toy's finished bodies (a later step uses it, or it's off or for reference), so "
        "it can't be a part. Pick finished bodies, or leave them out to use all of them.");
    CHECK(refused.parts.empty());

    Document empty(DocKind::Part, "Empty");
    auto none = partsFromBodies(empty);
    REQUIRE(none.problems.size() == 1);
    CHECK(none.problems.front().code == "parts.no_bodies");

    // The parts go in a new, empty design only.
    Document busy(DocKind::Part, "Busy");
    REQUIRE(busy.execute(std::make_unique<AddFeatureCommand>(
                             std::make_unique<BoxFeature>("Box1", kernel::Vec3{}, kernel::Vec3{1, 1, 1})))
                .empty());
    auto problems =
        busy.execute(std::make_unique<MakePartsFromBodiesCommand>(partsFromBodies(toy.doc).parts));
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().code == "assembly.not_empty");
    CHECK(busy.kind() == DocKind::Part);
}
