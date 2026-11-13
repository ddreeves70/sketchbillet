// The print check on real designs: a thin wall made from a sketch is found,
// and the check follows a change to the sketch.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/model/PrintCheck.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;

namespace {

struct Design {
    Document doc;
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();
    std::unique_ptr<sketch::ISketchSolver> solver = sketch::makePlaneGcsSolver();

    FeatureId run(std::string_view name, Args args) {
        auto made = CommandRegistry::builtin().create(name, args, doc);
        INFO(name << ": " << (made.ok() ? std::string() : made.issues.front().message));
        REQUIRE(made.ok());
        REQUIRE(doc.execute(std::move(made.command)).empty());
        return doc.featureAt(doc.featureCount() - 1).id();
    }
    PrintCheckResult check(const FeatureId& step) {
        const auto report = doc.regenerate(*kernel, solver.get());
        return checkPrint(doc, report, *kernel, step, {});
    }
};

}  // namespace

TEST_CASE("the print check finds a thin wall between a hole and the edge", "[model][print][occt]") {
    Design d;
    // A 30 x 20 x 5 mm block with a 10 mm hole whose edge is 0.6 mm from the side.
    const auto block = d.run("feature.add_box", {{"size", kernel::Vec3{30, 20, 5}}});
    const auto pin = d.run("feature.add_cylinder", {{"radius", 5.0}, {"height", 9.0}, {"base", kernel::Vec3{15, 5.6, -2}}});
    const auto cut = d.run("feature.add_boolean", {{"operation", std::string("cut")}, {"a", block}, {"b", pin}});
    auto r = d.check(cut);
    CHECK_FALSE(r.ready());
    REQUIRE(r.issues.size() == 1);
    CHECK(r.issues[0].code == "print.thin");
    // The mesh's flat sides cut the hole a little short of round, so it's close to 0.6 mm.
    CHECK(r.thinnest == Approx(0.6).margin(0.06));
    CHECK(r.thinnestAt.y < 1.5);  // at the front, by the hole

    // Move the hole back from the edge: now it's fine.
    REQUIRE(d.doc.execute(std::make_unique<SetNumberCommand>(pin, "base_y", 10.0)).empty());
    r = d.check(cut);
    CHECK(r.ready());
    CHECK(r.thinnest == Approx(5).margin(0.06));  // from the hole's wall to the front, or top to bottom
}

TEST_CASE("the print check says when a step didn't build", "[model][print][occt]") {
    Design d;
    const auto box = d.run("feature.add_box", {{"size", kernel::Vec3{10, 10, 10}}});
    const auto r = d.check(FeatureId::generate());
    REQUIRE(r.issues.size() == 1);
    CHECK(r.issues[0].code == "print.nothing");
    CHECK(d.check(box).ready());
}
