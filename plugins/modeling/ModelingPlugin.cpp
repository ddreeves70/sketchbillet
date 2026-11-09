#include "sb/plugins/Modeling.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#include "ModelingIcons.h"
#include "sb/kernel/Types.h"
#include "sb/model/CommandRegistry.h"
#include "sb/plugin/Actions.h"
#include "sb/plugin/Events.h"
#include "sb/plugin/Overlays.h"

// sb.modeling adds SketchBillet's modeling commands through the same PluginContext an
// outside plugin uses. The commands themselves are still defined in sb_model
// (CommandRegistry::builtin()); they move here when feature types become an
// extension point, because saving and opening them lives in sb_io for now.

namespace sb::plugins {

namespace {

constexpr const char* kManifest = R"({
  "id": "sb.modeling",
  "name": "Modeling",
  "version": "0.1.0",
  "description": "Boxes, cylinders and Booleans, plus deleting and renaming steps.",
  "tier": "foundation"
})";

// Draws the active design's construction planes (Plane steps): a square on each,
// round the point of it nearest the middle of the design's bodies and big enough
// to show beside them, with its name; the picked ones in orange.
void drawPlanes(plugin::PluginContext& ctx) {
    plugin::Overlay plain, picked, names;
    plain.color = {183, 121, 31, 200};
    picked.color = {230, 120, 20, 255};
    picked.size = 2.5;
    names.shape = plugin::OverlayShape::Labels;
    names.color = {140, 90, 20, 255};
    names.size = 12;
    const auto& docs = ctx.documents();
    const DocId active = docs.active();
    const auto* design = docs.find(active);
    const auto* report = design ? docs.lastRebuild(active) : nullptr;
    if (design && report) {
        // The middle of the bodies, and how big they are.
        kernel::Vec3 middle{};
        double half = 30;  // mm
        if (const auto* k = docs.kernel()) {
            bool any = false;
            kernel::Vec3 lo{}, hi{};
            for (const auto& id : report->bodies()) {
                const auto* shape = report->shapeOf(id);
                if (!shape) continue;
                const auto b = k->bounds(*shape);
                if (!any) {
                    lo = b.min;
                    hi = b.max;
                    any = true;
                }
                lo = {std::min(lo.x, b.min.x), std::min(lo.y, b.min.y), std::min(lo.z, b.min.z)};
                hi = {std::max(hi.x, b.max.x), std::max(hi.y, b.max.y), std::max(hi.z, b.max.z)};
            }
            if (any) {
                middle = {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
                const double dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
                half = std::max(half, 0.4 * std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        }
        const auto& sel = ctx.selection();
        for (std::size_t i = 0; i < design->featureCount(); ++i) {
            const auto& f = design->featureAt(i);
            const auto* r = report->result(f.id());
            if (f.type() != model::FeatureType::Plane || !r || !r->plane) continue;
            const auto& a = *r->plane;
            // The point of the plane nearest the middle, in the plane's own x and y.
            const kernel::Vec3 d{middle.x - a.origin.x, middle.y - a.origin.y, middle.z - a.origin.z};
            const double cu = d.x * a.x.x + d.y * a.x.y + d.z * a.x.z;
            const double cv = d.x * a.y.x + d.y * a.y.y + d.z * a.y.z;
            auto at = [&a, cu, cv](double u, double v) {
                u += cu;
                v += cv;
                return kernel::Vec3{a.origin.x + u * a.x.x + v * a.y.x, a.origin.y + u * a.x.y + v * a.y.y,
                                    a.origin.z + u * a.x.z + v * a.y.z};
            };
            const double kHalf = half;
            const kernel::Vec3 corners[] = {at(-kHalf, -kHalf), at(kHalf, -kHalf), at(kHalf, kHalf),
                                            at(-kHalf, kHalf)};
            const bool isPicked =
                sel.doc() == active &&
                std::find(sel.features().begin(), sel.features().end(), f.id()) != sel.features().end();
            auto& out = isPicked ? picked : plain;
            for (int k = 0; k < 4; ++k) {
                out.points.push_back(corners[k]);
                out.points.push_back(corners[(k + 1) % 4]);
            }
            names.points.push_back(at(-kHalf + 2, kHalf - 2));
            names.texts.push_back(f.name());
        }
    }
    // Only what there is: a design with no planes draws nothing.
    auto put = [&ctx](const char* name, plugin::Overlay o) {
        if (o.points.empty()) {
            ctx.overlays().remove(name);
        } else {
            ctx.overlays().set(name, std::move(o));
        }
    };
    put("planes", std::move(plain));
    put("planes.picked", std::move(picked));
    put("planes.names", std::move(names));
}

void registerModeling(plugin::PluginContext& ctx) {
    // Construction planes are drawn in the view after every rebuild and pick.
    ctx.listen<plugin::RebuildFinished>([&ctx](const auto&) { drawPlanes(ctx); });
    ctx.listen<plugin::SelectionChanged>([&ctx](const auto&) { drawPlanes(ctx); });
    ctx.listen<plugin::ActiveDocumentChanged>([&ctx](const auto&) { drawPlanes(ctx); });

    const auto& builtin = model::CommandRegistry::builtin();
    for (const auto* info : builtin.commands()) {
        ctx.addCommand(*info, *builtin.factory(info->name));
    }

    // Menu items and toolbar buttons. New shapes start at a sensible size; the
    // student changes the numbers in the Properties panel.
    using plugin::ActionSpec;
    ctx.addAction(ActionSpec{.id = "sb.modeling.box",
                             .text = "Box",
                             .menu = "Create",
                             .command = "feature.add_box",
                             .args = {{"size", kernel::Vec3{20, 20, 20}}},
                             .shortcut = "Ctrl+Shift+B",
                             .toolbar = true,
                             .icon = std::string(icons::modeling::box_svg),
                             .tip = "Add a 20 mm box. Change its size in Properties."});
    ctx.addAction(ActionSpec{.id = "sb.modeling.cylinder",
                             .text = "Cylinder",
                             .menu = "Create",
                             .command = "feature.add_cylinder",
                             .args = {{"radius", 10.0}, {"height", 20.0}},
                             .shortcut = "Ctrl+Shift+C",
                             .toolbar = true,
                             .icon = std::string(icons::modeling::cylinder_svg),
                             .tip = "Add a cylinder 20 mm across and 20 mm tall. Change it in Properties."});
    struct Combine {
        const char* id;
        const char* text;
        const char* op;
        const char* tip;
        std::string_view icon;
    };
    for (const auto& c :
         {Combine{"sb.modeling.union", "Union", "union", "Join the two selected bodies into one.",
                  icons::modeling::union_svg},
          Combine{"sb.modeling.cut", "Cut", "cut", "Cut the second selected body out of the first.",
                  icons::modeling::cut_svg},
          Combine{"sb.modeling.intersect", "Intersect", "intersect",
                  "Keep only where the two selected bodies overlap.", icons::modeling::intersect_svg}}) {
        ctx.addAction(ActionSpec{.id = c.id,
                                 .text = c.text,
                                 .menu = "Combine",
                                 .command = "feature.add_boolean",
                                 .args = {{"operation", std::string(c.op)}},
                                 .fromSelection = {"a", "b"},
                                 .toolbar = true,
                                 .icon = std::string(c.icon),
                                 .tip = c.tip});
    }
    ctx.addAction(ActionSpec{.id = "sb.modeling.move",
                             .text = "Move",
                             .menu = "Modify",
                             .command = "feature.add_move",
                             .fromSelection = {"body"},
                             .shortcut = "Ctrl+M",
                             .toolbar = true,
                             .icon = std::string(icons::modeling::move_svg),
                             .tip = "Move or turn the picked body. Then type how far in Properties."});
    // Extrude: the one sketch picked, swept 10 mm out of its plane.
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.extrude",
        .text = "Extrude",
        .menu = "Create",
        .command = "feature.add_extrude",
        .args = {{"distance", 10.0}},
        .fromSelection = {"sketch"},
        .shortcut = "Ctrl+Shift+E",
        .toolbar = true,
        .icon = std::string(icons::modeling::extrude_svg),
        .tip = "Make a 10 mm solid from the picked sketch's closed shapes. Change how far in "
               "Properties.",
        .enabled = [&ctx] {
            const auto& sel = ctx.selection();
            const auto* doc = ctx.documents().find(sel.doc());
            const auto* f = doc && sel.features().size() == 1 ? doc->find(sel.features()[0]) : nullptr;
            return f && f->type() == model::FeatureType::Sketch;
        }});
    // Revolve: the one sketch picked, turned all the way round its y axis (or its
    // only helper line).
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.revolve",
        .text = "Revolve",
        .menu = "Create",
        .command = "feature.add_revolve",
        .fromSelection = {"sketch"},
        .toolbar = true,
        .icon = std::string(icons::modeling::revolve_svg),
        .tip = "Turn the picked sketch's closed shapes all the way round a line, like a lathe: its y axis, "
               "or its helper line. Change the axis and angle in Properties.",
        .enabled = [&ctx] {
            const auto& sel = ctx.selection();
            const auto* doc = ctx.documents().find(sel.doc());
            const auto* f = doc && sel.features().size() == 1 ? doc->find(sel.features()[0]) : nullptr;
            return f && f->type() == model::FeatureType::Sketch;
        }});
    // Hole: M3 clearance holes, through, at every point of the one sketch picked
    // (drawn on a face of the body); or one hole 10 mm in from two picked edges.
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.hole",
        .text = "Hole",
        .menu = "Create",
        .command = "feature.add_hole",
        .args = {{"size", std::string("M3")}},
        .fromSelection = {"sketch"},
        .toolbar = true,
        .icon = std::string(icons::modeling::hole_svg),
        .tip =
            "Drill an M3 hole through the body at every point of the picked sketch. Change the size, depth "
            "and kind in Properties.",
        .enabled = [&ctx] {
            const auto& sel = ctx.selection();
            const auto* doc = ctx.documents().find(sel.doc());
            const auto* f = doc && sel.features().size() == 1 ? doc->find(sel.features()[0]) : nullptr;
            return f && f->type() == model::FeatureType::Sketch;
        }});
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.hole_edges",
        .text = "Hole from two edges",
        .menu = "Create",
        .command = "feature.add_hole",
        .args = {{"size", std::string("M3")}, {"distances", sketch::Point2{10, 10}}},
        .fromEdges = "edges",
        .fromEdgesBody = "body",
        .tip =
            "Drill an M3 hole 10 mm in from two picked edges of a flat face. With no edges picked, click two "
            "edges first. Change how far in from each in Properties."});
    // Patterns of the picked steps: their holes, joins and cuts again; or, for
    // one step that makes a body of its own, copies of the body.
    const auto patternable = [&ctx] {
        const auto& sel = ctx.selection();
        const auto* doc = ctx.documents().find(sel.doc());
        if (!doc || sel.features().empty()) return false;
        for (const auto& id : sel.features()) {
            const auto* f = doc->find(id);
            if (!f || f->type() == model::FeatureType::Sketch) return false;
        }
        return true;
    };
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.pattern_linear",
        .text = "Linear pattern",
        .menu = "Create",
        .command = "feature.add_pattern",
        .args = {{"kind", std::string("linear")}},
        .fromSelectionList = "steps",
        .toolbar = true,
        .buttonText = "Row",
        .icon = std::string(icons::modeling::pattern_linear_svg),
        .tip =
            "Repeat the picked hole, join or cut 3 times along x, 10 mm apart; or copy a picked body. Change "
            "how many, which way and how far in Properties.",
        .enabled = patternable});
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.pattern_circular",
        .text = "Circular pattern",
        .menu = "Create",
        .command = "feature.add_pattern",
        .args = {{"kind", std::string("circular")}},
        .fromSelectionList = "steps",
        .toolbar = true,
        .buttonText = "Circle",
        .icon = std::string(icons::modeling::pattern_circular_svg),
        .tip =
            "Repeat the picked hole, join or cut 6 times round the z axis; or copy a picked body. Change how "
            "many and the axis in Properties.",
        .enabled = patternable});
    // Mirror: the picked steps' holes, joins and cuts, or a picked body, across
    // the YZ plane (left and right swap).
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.mirror",
        .text = "Mirror",
        .menu = "Create",
        .command = "feature.add_mirror",
        .fromSelectionList = "steps",
        .toolbar = true,
        .icon = std::string(icons::modeling::mirror_svg),
        .tip =
            "Mirror the picked hole, join or cut, or a picked body, across the YZ plane, so left and right "
            "match. Change the plane in Properties.",
        .enabled = patternable});
    // Fillet and Chamfer: the picked edges of one body. With none picked, the
    // first click starts picking edges.
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.fillet",
        .text = "Fillet",
        .menu = "Modify",
        .command = "feature.add_fillet",
        .args = {{"radius", 1.0}},
        .fromEdges = "edges",
        .fromEdgesBody = "body",
        .toolbar = true,
        .icon = std::string(icons::modeling::fillet_svg),
        .tip = "Round the picked edges by 1 mm. Change the radius in Properties. With no edges picked, "
               "click the edges first."});
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.chamfer",
        .text = "Chamfer",
        .menu = "Modify",
        .command = "feature.add_chamfer",
        .args = {{"distance", 1.0}},
        .fromEdges = "edges",
        .fromEdgesBody = "body",
        .toolbar = true,
        .icon = std::string(icons::modeling::chamfer_svg),
        .tip = "Bevel the picked edges by 1 mm. Change the distance in Properties. With no edges picked, "
               "click the edges first."});
    // What clicking a body picks: the body (the usual) or one of its edges.
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.pick_edges",
        .text = "Pick edges",
        .menu = "Edit",
        .tip = "Click edges of bodies instead of whole bodies, for a fillet or chamfer.",
        .run =
            [&ctx] {
                auto& sel = ctx.selection();
                sel.setPickMode(sel.pickMode() == plugin::PickMode::Edges ? plugin::PickMode::Bodies
                                                                          : plugin::PickMode::Edges);
            },
        .checked = [&ctx] { return ctx.selection().pickMode() == plugin::PickMode::Edges; }});
    ctx.addAction(ActionSpec{.id = "sb.modeling.duplicate",
                             .text = "Duplicate step",
                             .menu = "Edit",
                             .command = "feature.duplicate",
                             .fromSelection = {"feature"},
                             .shortcut = "Ctrl+D",
                             .tip = "Copy the selected step, and the steps it uses, as new steps."});
    ctx.addAction(ActionSpec{.id = "sb.modeling.turn_off",
                             .text = "Turn off",
                             .menu = "Edit",
                             .command = "feature.suppress",
                             .args = {{"off", true}},
                             .fromSelectionList = "steps",
                             .tip = "Turn the picked steps off, so they aren't built. Nothing is deleted."});
    ctx.addAction(ActionSpec{.id = "sb.modeling.turn_on",
                             .text = "Turn on",
                             .menu = "Edit",
                             .command = "feature.suppress",
                             .args = {{"off", false}},
                             .fromSelectionList = "steps",
                             .tip = "Turn the picked steps back on."});
    ctx.addAction(ActionSpec{.id = "sb.modeling.new_folder",
                             .text = "New folder",
                             .menu = "Edit",
                             .command = "folder.add",
                             .fromSelectionList = "steps",
                             .tip = "Put the picked steps in a folder, to keep the feature tree tidy."});
    ctx.addAction(ActionSpec{.id = "sb.modeling.delete",
                             .text = "Delete step",
                             .menu = "Edit",
                             .command = "feature.delete",
                             .fromSelection = {"feature"},
                             .shortcut = "Delete",
                             .tip = "Delete the selected step."});
}

}  // namespace

plugin::BuiltinPlugin modelingPlugin() {
    return {kManifest, &registerModeling};
}

}  // namespace sb::plugins
