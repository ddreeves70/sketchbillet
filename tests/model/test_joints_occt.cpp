// Joints in an assembly, with real geometry: each joint places its second
// component so the two origins meet, pointing at each other; a revolute joint
// turns by exactly its angle about its axis and a slider slides by exactly its
// distance; limits hold; components are placed along chains of joints; and a
// joint that closes a loop is checked, never placed wrongly.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <memory>
#include <numbers>

#include "sb/kernel_occt/OcctKernel.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/CommandRegistry.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"

using namespace sb;
using namespace sb::model;
using Catch::Approx;
using kernel::Placement;
using kernel::TopoKind;
using kernel::Vec3;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTight = 1e-6;  // mm: the design asks origins to meet within this

std::unique_ptr<Document> boxPart(const std::string& name, Vec3 size) {
    auto part = std::make_unique<Document>(DocKind::Part, name);
    REQUIRE(
        part->execute(std::make_unique<AddFeatureCommand>(std::make_unique<BoxFeature>("Box1", Vec3{}, size)))
            .empty());
    return part;
}

double distance(const kernel::Point3& a, const kernel::Point3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

double dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// An assembly with the real kernel, and ways to name what's on its parts.
struct Assembly {
    Document doc{DocKind::Part, "Box"};
    std::unique_ptr<kernel::IKernel> kernel = kernel::makeOcctKernel();

    Assembly() { REQUIRE(doc.execute(std::make_unique<MakeAssemblyCommand>()).empty()); }

    ComponentId insert(std::unique_ptr<Document> part, kernel::Placement at = {}) {
        auto cmd = std::make_unique<InsertPartCommand>(std::move(part), 1, at);
        auto* raw = cmd.get();
        REQUIRE(doc.execute(std::move(cmd)).empty());
        return raw->added().front();
    }

    RebuildReport build() const { return doc.regenerate(*kernel); }

    // A side of a joint: the element of a component's step (its first, Box1, unless
    // said) that `pick` likes best.
    JointOrigin side(const ComponentId& c, TopoKind kind,
                     const std::function<bool(const kernel::ElementInfo&)>& pick,
                     OriginAt at = OriginAt::Middle, std::size_t step = 0) {
        const auto report = build();
        const auto* comp = doc.findComponent(c);
        const auto* part = doc.findPart(comp->part);
        const FeatureId body = part->featureAt(step).id();
        const auto* r = report.part(comp->part)->result(body);
        REQUIRE((r && r->names));
        const auto& list = r->names->elements.of(kind);
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (!pick(list[i])) continue;
            return {c, body, referTo(*r->names, kind, static_cast<int>(i)), at};
        }
        FAIL("nothing on the part matches");
        return {};
    }

    // A straight edge's end that's (or, placed, lands) nearest `point` in the
    // assembly: Start or End, whichever it is.
    JointOrigin endAt(const ComponentId& c, const std::function<bool(const kernel::ElementInfo&)>& pick,
                      const kernel::Point3& point) {
        JointOrigin o = side(c, TopoKind::Edge, pick, OriginAt::Start);
        const auto report = build();
        const auto* comp = doc.findComponent(c);
        const auto* r = report.part(comp->part)->result(o.body);
        const auto& elements = r->names->elements;
        const auto found = resolve(*o.element, *r->names);
        const auto& ends = elements.edgeVertices.at(static_cast<std::size_t>(found.index));
        REQUIRE(ends.size() == 2);
        auto at = [&](int end) {
            return kernel::apply(
                *report.placementOf(c),
                elements.vertices.at(static_cast<std::size_t>(ends[static_cast<std::size_t>(end)])).center);
        };
        o.at = distance(at(0), point) <= distance(at(1), point) ? OriginAt::Start : OriginAt::End;
        return o;
    }

    JointId join(JointKind kind, JointOrigin a, JointOrigin b, double value = 0, bool flip = false) {
        Joint j;
        j.kind = kind;
        j.a = std::move(a);
        j.b = std::move(b);
        j.value = value;
        j.flip = flip;
        auto cmd = std::make_unique<AddJointCommand>(j);
        auto* raw = cmd.get();
        const auto issues = doc.execute(std::move(cmd));
        INFO((issues.empty() ? std::string() : issues.front().message));
        REQUIRE(issues.empty());
        return raw->joint().id;
    }

    Diagnostics setValue(const JointId& id, double value) {
        Joint j = *doc.findJoint(id);
        j.value = value;
        return doc.execute(std::make_unique<SetJointCommand>(j));
    }
};

auto face = [](double nx, double ny, double nz, double cz) {
    return [=](const kernel::ElementInfo& e) {
        return e.geom == kernel::GeomKind::Plane && std::abs(e.direction.x - nx) < 1e-9 &&
               std::abs(e.direction.y - ny) < 1e-9 && std::abs(e.direction.z - nz) < 1e-9 &&
               std::abs(e.center.z - cz) < 1e-9;
    };
};

// A straight edge along x, at y and z.
auto edgeAlongX = [](double y, double z) {
    return [=](const kernel::ElementInfo& e) {
        return e.geom == kernel::GeomKind::Line && std::abs(std::abs(e.direction.x) - 1) < 1e-9 &&
               std::abs(e.center.y - y) < 1e-9 && std::abs(e.center.z - z) < 1e-9;
    };
};

// Every joint that placed something: its two origins meet, pointing at each other.
void checkJointsMeet(const Assembly& box, const RebuildReport& report) {
    for (const auto& j : box.doc.joints()) {
        const auto* r = report.joint(j.id);
        REQUIRE(r);
        if (!r->placed) continue;
        REQUIRE((r->aFrame && r->bFrame));
        CHECK(distance(r->aFrame->origin, r->bFrame->origin) == Approx(0).margin(kTight));
        CHECK(dot(r->aFrame->zAxis, r->bFrame->zAxis) == Approx(j.flip ? 1.0 : -1.0).margin(1e-9));
    }
}

}  // namespace

TEST_CASE("a rigid joint puts the lid's bottom on the box's top, the two faces meeting",
          "[model][joints][occt]") {
    Assembly box;
    const auto base = box.insert(boxPart("Base", {40, 40, 10}));
    const auto lid = box.insert(boxPart("Lid", {40, 40, 4}), Placement{{100, 0, 0}});
    box.join(JointKind::Rigid, box.side(base, TopoKind::Face, face(0, 0, 1, 10)),
             box.side(lid, TopoKind::Face, face(0, 0, -1, 0)));
    const auto report = box.build();
    checkJointsMeet(box, report);
    REQUIRE(report.placementOf(lid));
    // The lid's bottom center (20, 20, 0) goes to the box's top center (20, 20, 10).
    CHECK(kernel::samePlacement(*report.placementOf(lid), Placement{{0, 0, 10}}, 1e-9));
    CHECK(report.placedByJoint(lid));
    CHECK_FALSE(report.placedByJoint(base));
    CHECK(kernel::samePlacement(*report.placementOf(base), Placement{}, 0));  // grounded: never moves
    // The lid's own placement is kept, for when the joint goes.
    CHECK(box.doc.findComponent(lid)->placement.origin.x == 100);
}

TEST_CASE("a revolute joint turns its component by exactly its angle about the hinge, within its limits",
          "[model][joints][occt]") {
    Assembly box;
    const auto base = box.insert(boxPart("Base", {40, 40, 10}));
    const auto lid = box.insert(boxPart("Lid", {40, 40, 4}));
    // The hinge: the box's top back edge, and the lid's bottom back edge.
    const auto hinge = box.join(JointKind::Revolute, box.side(base, TopoKind::Edge, edgeAlongX(40, 10)),
                                box.side(lid, TopoKind::Edge, edgeAlongX(40, 0)));
    const auto closed = box.build();
    checkJointsMeet(box, closed);
    const Placement at0 = *closed.placementOf(lid);
    const auto axis = *closed.joint(hinge)->aFrame;  // a point on the hinge, and its direction

    // A corner of the lid, as the hinge turns: it stays the same distance from
    // the hinge's axis, and turns about it by exactly the joint's value.
    const kernel::Point3 corner{0, 0, 4};
    auto radial = [&](const Placement& p) {
        const auto q = kernel::apply(p, corner);
        Vec3 d{q.x - axis.origin.x, q.y - axis.origin.y, q.z - axis.origin.z};
        const double along = dot(d, axis.zAxis);
        return Vec3{d.x - along * axis.zAxis.x, d.y - along * axis.zAxis.y, d.z - along * axis.zAxis.z};
    };
    const Vec3 r0 = radial(at0);
    for (const double degrees : {30.0, 90.0, 135.0}) {
        REQUIRE(box.setValue(hinge, degrees * kPi / 180).empty());
        const auto turned = box.build();
        checkJointsMeet(box, turned);
        const Vec3 r = radial(*turned.placementOf(lid));
        const double n0 = std::sqrt(dot(r0, r0)), n = std::sqrt(dot(r, r));
        CHECK(n == Approx(n0).margin(kTight));
        CHECK(std::acos(std::clamp(dot(r0, r) / (n0 * n), -1.0, 1.0)) * 180 / kPi ==
              Approx(degrees).margin(1e-6));
        // The hinge itself doesn't move.
        CHECK(distance(turned.joint(hinge)->bFrame->origin, axis.origin) == Approx(0).margin(kTight));
    }

    // Limits: from 0 to 110 degrees.
    Joint j = *box.doc.findJoint(hinge);
    j.value = 0;
    j.lowest = 0;
    j.highest = 110 * kPi / 180;
    REQUIRE(box.doc.execute(std::make_unique<SetJointCommand>(j)).empty());
    const auto beyond = box.setValue(hinge, 120 * kPi / 180);
    REQUIRE(beyond.size() == 1);
    CHECK(beyond.front().code == "joint.out_of_limits");
    CHECK(beyond.front().message ==
          "Joint1 turns from 0\xC2\xB0 to 110\xC2\xB0, so it can't be 120\xC2\xB0. Pick a "
          "value between them, or change its limits.");
    CHECK(box.doc.findJoint(hinge)->value == 0);
}

TEST_CASE("a slider slides its component by exactly its distance along the joint", "[model][joints][occt]") {
    Assembly box;
    const auto base = box.insert(boxPart("Base", {40, 40, 10}));
    const auto lid = box.insert(boxPart("Lid", {40, 40, 4}));
    const auto slide = box.join(JointKind::Slider, box.side(base, TopoKind::Face, face(0, 0, 1, 10)),
                                box.side(lid, TopoKind::Face, face(0, 0, -1, 0)));
    for (const double mm : {0.0, 5.0, 12.5}) {
        REQUIRE(box.setValue(slide, mm).empty());
        const auto report = box.build();
        CHECK(report.placementOf(lid)->origin.z == Approx(10 + mm).margin(kTight));
        CHECK(report.placementOf(lid)->origin.x == Approx(0).margin(kTight));
    }
}

TEST_CASE("components are placed along a chain of joints, from the grounded one, either way round",
          "[model][joints][occt]") {
    Assembly arm;
    const auto base = arm.insert(boxPart("Base", {40, 40, 10}));
    const auto link = arm.insert(boxPart("Link", {10, 10, 50}));
    const auto grip = arm.insert(boxPart("Grip", {10, 10, 5}));
    // Grip on Link is added first; Link on Base after: the chain is still walked
    // from Base.
    arm.join(JointKind::Rigid, arm.side(link, TopoKind::Face, face(0, 0, 1, 50)),
             arm.side(grip, TopoKind::Face, face(0, 0, -1, 0)));
    // This one names Base second: Base is grounded, so it places Link instead.
    arm.join(JointKind::Rigid, arm.side(link, TopoKind::Face, face(0, 0, -1, 0)),
             arm.side(base, TopoKind::Face, face(0, 0, 1, 10)));
    const auto report = arm.build();
    checkJointsMeet(arm, report);
    for (const auto& j : arm.doc.joints()) CHECK(report.joint(j.id)->placed);
    // Link's bottom center (5, 5, 0) on Base's top center (20, 20, 10); Grip's on Link's top.
    CHECK(kernel::apply(*report.placementOf(link), {5, 5, 0}).z == Approx(10).margin(kTight));
    CHECK(kernel::apply(*report.placementOf(link), {5, 5, 0}).x == Approx(20).margin(kTight));
    CHECK(kernel::apply(*report.placementOf(grip), {5, 5, 0}).z == Approx(60).margin(kTight));
    CHECK(kernel::samePlacement(*report.placementOf(base), Placement{}, 0));
}

TEST_CASE("a joint that closes a loop is accepted if its sides meet, and explained if not",
          "[model][joints][occt]") {
    Assembly box;
    const auto base = box.insert(boxPart("Base", {40, 40, 10}));
    const auto lid = box.insert(boxPart("Lid", {40, 40, 4}));
    // Two hinges on one axis: the back edge, picked at its middle, then at its end.
    const auto first = box.join(JointKind::Revolute, box.side(base, TopoKind::Edge, edgeAlongX(40, 10)),
                                box.side(lid, TopoKind::Edge, edgeAlongX(40, 0)));
    REQUIRE(box.setValue(first, 60 * kPi / 180).empty());
    const auto second = box.join(JointKind::Revolute, box.endAt(base, edgeAlongX(40, 10), {0, 40, 10}),
                                 box.endAt(lid, edgeAlongX(40, 0), {0, 40, 10}));
    auto report = box.build();
    CHECK(report.joint(first)->placed);
    CHECK(report.joint(second)->loop);
    INFO((report.joint(second)->issues.empty() ? std::string()
                                               : report.joint(second)->issues.front().message));
    CHECK(report.joint(second)->issues.empty());  // its sides meet: nothing to say

    // A rigid joint from the lid's top to the box's top can't agree: it's a loop
    // whose sides are apart, so it moves nothing, and says why.
    const auto third = box.join(JointKind::Rigid, box.side(base, TopoKind::Face, face(0, 0, 1, 10)),
                                box.side(lid, TopoKind::Face, face(0, 0, 1, 4)));
    report = box.build();
    const auto* r = report.joint(third);
    REQUIRE(r->issues.size() == 1);
    CHECK(r->issues.front().code == "joint.loop");
    CHECK(r->issues.front().severity == Severity::Warning);
    CHECK(r->issues.front().message.rfind(
              "Joint3 makes a loop (Base 1, Lid 1, Base 1), and its two sides are ", 0) == 0);
    CHECK(r->issues.front().message.find("Turn Joint3 off, or change a value in the loop") !=
          std::string::npos);
    CHECK(report.joint(first)->placed);  // the first hinge still places the lid
    checkJointsMeet(box, report);

    // Turned off, it's not checked at all.
    Joint j = *box.doc.findJoint(third);
    j.off = true;
    REQUIRE(box.doc.execute(std::make_unique<SetJointCommand>(j)).empty());
    CHECK(box.build().joint(third)->issues.empty());
}

TEST_CASE("a joint whose origin is gone after a change says so, and places nothing",
          "[model][joints][occt]") {
    Assembly box;
    const auto base = box.insert(boxPart("Base", {40, 40, 10}));
    const auto lid = box.insert(boxPart("Lid", {40, 40, 4}), Placement{{100, 0, 0}});
    box.join(JointKind::Rigid, box.side(base, TopoKind::Face, face(0, 0, 1, 10)),
             box.side(lid, TopoKind::Face, face(0, 0, -1, 0)));
    // Base's box step is deleted, so the body the joint sits on is gone.
    const DocId basePart = box.doc.findComponent(base)->part;
    const FeatureId boxStep = box.doc.findPart(basePart)->featureAt(0).id();
    auto made = CommandRegistry::builtin().create("feature.delete", {{"feature", boxStep}},
                                                  *box.doc.findPart(basePart));
    REQUIRE(made.ok());
    REQUIRE(box.doc.execute(std::make_unique<EditPartCommand>(basePart, std::move(made.command))).empty());
    const auto report = box.build();
    const auto* r = report.joint(box.doc.joints().front().id);
    REQUIRE(r->issues.size() == 1);
    CHECK(r->issues.front().code == "joint.origin_missing");
    CHECK(r->issues.front().message ==
          "Joint1 can't find the body it sits on in Base: a change to Base took it away, so it doesn't place "
          "anything. Choose Joint1's origins again (joint.set joint=\"Joint1\" a=... b=...).");
    CHECK_FALSE(r->placed);
    CHECK(report.placementOf(lid)->origin.x == 100);  // where it was put
}

TEST_CASE("joints are checked, changed, deleted, and go with their components, each one undo step",
          "[model][joints][occt]") {
    Assembly box;
    const auto base = box.insert(boxPart("Base", {40, 40, 10}));
    const auto lid = box.insert(boxPart("Lid", {40, 40, 4}));
    const auto top = box.side(base, TopoKind::Face, face(0, 0, 1, 10));

    Joint self;
    self.a = top;
    self.b = top;
    const auto same = box.doc.execute(std::make_unique<AddJointCommand>(self));
    REQUIRE(same.size() == 1);
    CHECK(same.front().code == "joint.same_component");

    Joint rigid;
    rigid.a = top;
    rigid.b = box.side(lid, TopoKind::Face, face(0, 0, -1, 0));
    rigid.value = 3;
    CHECK(box.doc.execute(std::make_unique<AddJointCommand>(rigid)).front().code == "joint.rigid_value");

    const auto id = box.join(JointKind::Revolute, top, rigid.b);
    CHECK(box.doc.findJoint(id)->name == "Joint1");
    CHECK(box.doc.undoLabel() == "Add Joint1");

    // A drag's changes are one undo step.
    Joint j = *box.doc.findJoint(id);
    j.value = 0.1;
    REQUIRE(box.doc.execute(std::make_unique<SetJointCommand>(j)).empty());
    j.value = 0.2;
    REQUIRE(box.doc.execute(std::make_unique<SetJointCommand>(j, true)).empty());
    CHECK(box.doc.findJoint(id)->value == 0.2);
    REQUIRE(box.doc.undo());
    CHECK(box.doc.findJoint(id)->value == 0);
    CHECK(box.doc.undoLabel() == "Add Joint1");

    // Deleting the lid takes its joint too; undo puts both back.
    REQUIRE(box.doc.execute(std::make_unique<DeleteComponentCommand>(lid)).empty());
    CHECK(box.doc.joints().empty());
    REQUIRE(box.doc.undo());
    REQUIRE(box.doc.joints().size() == 1);
    CHECK(box.doc.joints().front().id == id);

    REQUIRE(box.doc.execute(std::make_unique<DeleteJointCommand>(id)).empty());
    CHECK(box.doc.undoLabel() == "Delete Joint1");
    CHECK(box.doc.joints().empty());
    CHECK_FALSE(box.build().placedByJoint(lid));
    REQUIRE(box.doc.undo());
    CHECK(box.build().placedByJoint(lid));
}

TEST_CASE("a peg's end rim joined to a hole's rim stands the peg on the hole, facing out of the plate",
          "[model][joints][occt]") {
    Assembly kit;
    auto run = [](Document& d, std::string_view name, Args args) {
        auto made = CommandRegistry::builtin().create(name, args, d);
        REQUIRE(made.ok());
        REQUIRE(d.execute(std::move(made.command)).empty());
    };
    auto plate = std::make_unique<Document>(DocKind::Part, "Plate");
    run(*plate, "feature.add_box", {{"size", Vec3{40, 40, 5}}});
    run(*plate, "feature.add_cylinder", {{"base", Vec3{20, 20, -1}}, {"radius", 3.0}, {"height", 7.0}});
    run(*plate, "feature.add_boolean",
        {{"operation", std::string("cut")},
         {"a", plate->featureAt(0).id()},
         {"b", plate->featureAt(1).id()}});
    auto peg = std::make_unique<Document>(DocKind::Part, "Peg");
    run(*peg, "feature.add_cylinder", {{"radius", 2.9}, {"height", 20.0}});
    const auto plateC = kit.insert(std::move(plate));
    const auto pegC = kit.insert(std::move(peg), Placement{{80, 0, 0}});

    // The hole's top rim (on the plate's top face), and the peg's bottom rim.
    auto rimAt = [](double x, double y, double z) {
        return [=](const kernel::ElementInfo& e) {
            return e.geom == kernel::GeomKind::Circle && std::abs(e.center.x - x) < 1e-6 &&
                   std::abs(e.center.y - y) < 1e-6 && std::abs(e.center.z - z) < 1e-6;
        };
    };
    // On the plate's built body: its last step, the cut.
    const JointOrigin hole = kit.side(plateC, TopoKind::Edge, rimAt(20, 20, 5), OriginAt::Middle, 2);
    kit.join(JointKind::Rigid, hole, kit.side(pegC, TopoKind::Edge, rimAt(0, 0, 0)));
    const auto report = kit.build();
    checkJointsMeet(kit, report);
    const auto& at = *report.placementOf(pegC);
    // The peg's bottom center on the hole's top center, standing straight up.
    const auto bottom = kernel::apply(at, {0, 0, 0});
    CHECK(bottom.x == Approx(20).margin(kTight));
    CHECK(bottom.y == Approx(20).margin(kTight));
    CHECK(bottom.z == Approx(5).margin(kTight));
    CHECK(at.zAxis.z == Approx(1).margin(1e-9));
}
